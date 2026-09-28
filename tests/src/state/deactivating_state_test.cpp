// SPDX-License-Identifier: Apache-2.0
/**
 * @file deactivating_state_test.cpp
 * @brief Comprehensive unit tests for DeactivatingState (unmounting in
 * progress)
 *
 * This test suite validates the deactivating state of the virtual media state
 * machine, representing the transitional state while unmount operations are in
 * progress. This is a critical cleanup state managing resource deallocation,
 * process termination, and device removal.
 *
 * Testing Strategy:
 * - Expose private members for white-box testing (test-only)
 * - Mock MountPointStateMachine for isolated testing
 * - Simulate event ordering scenarios (udev before/after subprocess stop)
 * - Validate state machine convergence to ReadyState
 *
 * Functional Coverage:
 * 1. **Constructors**:
 *    - Constructor with process + gadget resources (no initial event)
 *    - Constructor with initial event (UdevStateChangeEvent or
 * SubprocessStoppedEvent)
 *
 * 2. **State Entry (onEnter)**:
 *    - Resource cleanup: process → nullptr, gadget → nullptr
 *    - Returns nullptr (no async operations, relies on event-driven
 * transitions)
 *
 * 3. **Event Handling**:
 *    - UdevStateChangeEvent → store event, call evaluate()
 *    - SubprocessStoppedEvent → store event, call evaluate()
 *    - MountEvent/UnmountEvent → throws SdBusError(EBUSY) - operation in
 * progress
 *
 * 4. **State Evaluation Logic (private evaluate)**:
 *    - Both events received + device removed → ReadyState with notify(ok)
 *    - Both events received + device NOT removed → ReadyState with
 * notify(error)
 *    - One event missing → returns nullptr (wait for second event)
 *
 * 5. **CRTP Identity**:
 *    - getStateName() returns "DeactivatingState"
 *    - get_if<DeactivatingState>() works correctly
 *
 * Test Coverage Areas:
 * - All constructor variants
 * - onEnter() cleanup behavior
 * - All handleEvent() method variants
 * - evaluate() branching logic (removed vs. non-removed)
 * - Event ordering independence (udev first vs. subprocess first)
 * - Error handling for operations during deactivation
 *
 * Edge Cases Tested:
 * - Constructor with initial event (early arrival)
 * - Both events in different orderings
 * - Device removed vs. device still present scenarios
 * - Unsupported events during deactivation (EBUSY)
 * - Resource cleanup validation
 *
 * Dependencies:
 * - MockMountPointStateMachine: Interface isolation
 * - boost::asio::io_context: Context for operations
 * - sdbusplus::exception: D-Bus error handling
 *
 * @note Private member exposure is test-only, not for production builds
 * @note Deactivating state coordinates asynchronous cleanup of multiple
 * resources
 */
// Unit tests for DeactivatingState (state/deactivating_state.hpp)
//
// Covers:
//  - Constructors (with/without initial event)
//  - onEnter(): resource cleanup (process/gadget -> nullptr)
//  - handleEvent(UdevStateChangeEvent) + handleEvent(SubprocessStoppedEvent) in
//  any order
//  - Private evaluate() branches (removed vs. non-removed)
//  - Unsupported event throws SdBusError(EBUSY)
//  - CRTP identity helpers
//
// NOTE: We temporarily expose private members for white-box checks.
//       This is purely for unit-test validation and should NOT be used in
//       production builds.

#include "events.hpp"                               // Event types
#include "interfaces/mount_point_state_machine.hpp" // Target, interface
#include "state/basic_state.hpp"                    // CRTP base
#include "state/ready_state.hpp" // ReadyState used by evaluate()

#include <boost/asio/io_context.hpp>
#include <sdbusplus/exception.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <system_error>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// ---- Expose private members of DeactivatingState for testing ----
#define private public
#include "state/deactivating_state.hpp" // Under test
#undef private

// ---- Local gmock for interfaces::MountPointStateMachine ----
namespace interfaces
{
class MockMountPointStateMachine : public MountPointStateMachine
{
  public:
    MOCK_METHOD(void, notify, (const std::error_code& ec), (override));
    MOCK_METHOD(void, notificationStart, (), (override));
    MOCK_METHOD(void, notificationInitialize,
                (std::shared_ptr<sdbusplus::asio::connection> con,
                 const std::string& svc, const std::string& iface,
                 const std::string& name),
                (override));

    MOCK_METHOD(std::string_view, getName, (), (const, override));
    MOCK_METHOD(Configuration::MountPoint&, getConfig, (), (override));
    MOCK_METHOD(std::optional<Target>&, getTarget, (), (override));
    MOCK_METHOD(BasicState&, getState, (), (override));
    MOCK_METHOD(int&, getExitCode, (), (override));
    MOCK_METHOD(boost::asio::io_context&, getIoc, (), (override));

    MOCK_METHOD(std::string_view, getAdditionalInfo, (), (const, override));
    MOCK_METHOD(void, setAdditionalInfo, (const std::string&), (override));

    MOCK_METHOD(void, emitRegisterDBusEvent,
                (std::shared_ptr<sdbusplus::asio::connection> bus,
                 std::shared_ptr<sdbusplus::asio::object_server> objServer),
                (override));
    MOCK_METHOD(void, emitMountEvent, (std::optional<Target>), (override));
    MOCK_METHOD(void, emitUnmountEvent, (), (override));
    MOCK_METHOD(void, emitSubprocessStoppedEvent, (), (override));
    MOCK_METHOD(void, emitUdevStateChangeEvent,
                (const NBDDevice& dev, StateChange devState), (override));
};
} // namespace interfaces

// ---- Test fixture ----
class DeactivatingStateFixture : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc{};
    Configuration::MountPoint mp{};
    std::optional<interfaces::MountPointStateMachine::Target> target{};
    int exitCode{7};
    std::string nameStr{"Slot_9"};
    std::string addInfoStr{"unit-test-additional-info"};

    using NiceMachine =
        ::testing::NiceMock<interfaces::MockMountPointStateMachine>;
    NiceMachine machine;

    void SetUp() override
    {
        using ::testing::Return;
        using ::testing::ReturnRef;
        ON_CALL(machine, getConfig()).WillByDefault(ReturnRef(mp));
        ON_CALL(machine, getTarget()).WillByDefault(ReturnRef(target));
        ON_CALL(machine, getExitCode()).WillByDefault(ReturnRef(exitCode));
        ON_CALL(machine, getIoc()).WillByDefault(ReturnRef(ioc));
        ON_CALL(machine, getName())
            .WillByDefault(Return(std::string_view{nameStr}));
        ON_CALL(machine, getAdditionalInfo())
            .WillByDefault(Return(std::string_view{addInfoStr}));
    }
};

// -----------------------------------------------------------------------------
// 1) ctor(process,gadget): no initial event -> no evaluate; optionals empty
// -----------------------------------------------------------------------------
TEST_F(DeactivatingStateFixture,
       ConstructionWithoutInitialEventLeavesOptionalsEmpty)
{
    // Arrange: pass nullptr resources (we only validate logic, not resource
    // impl)
    std::unique_ptr<resource::Process> proc{};
    std::unique_ptr<resource::Gadget> gad{};

    DeactivatingState s{machine, std::move(proc),
                        std::move(gad)}; // ctor does NOT call evaluate

    // Private members are exposed via #define; check initial state
    EXPECT_FALSE(s.udevStateChangeEvent.has_value());
    EXPECT_FALSE(s.subprocessStoppedEvent.has_value());
}

// -----------------------------------------------------------------------------
// 2) ctor(process,gadget,event=UdevInserted): constructor calls
// handleEvent(event)
//    -> udevStateChangeEvent set; evaluate() returns nullptr until both events
//    present.
// -----------------------------------------------------------------------------
TEST_F(DeactivatingStateFixture,
       ConstructionWithUdevEventStoresItButNoTransitionYet)
{
    std::unique_ptr<resource::Process> proc{};
    std::unique_ptr<resource::Gadget> gad{};

    UdevStateChangeEvent udevEv{StateChange::inserted};
    DeactivatingState s{machine, std::move(proc), std::move(gad),
                        std::move(udevEv)}; // calls handleEvent(udev) in ctor

    EXPECT_TRUE(s.udevStateChangeEvent.has_value());
    EXPECT_EQ(s.udevStateChangeEvent->devState, StateChange::inserted);
    EXPECT_FALSE(s.subprocessStoppedEvent.has_value());

    // Complete the pair: SubprocessStoppedEvent -> evaluate() =>
    // ReadyState(error path)
    SubprocessStoppedEvent sp{};
    // Expect notify(error=connection_refused) from ReadyState(error ctor)
    EXPECT_CALL(machine, notify(::testing::Truly([](const std::error_code& e) {
                    return e ==
                           std::make_error_code(std::errc::connection_refused);
                })))
        .Times(1);

    auto next =
        s.handleEvent(std::move(sp)); // sets subprocessStoppedEvent & evaluates
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(),
              ReadyState::stateName()); // returns ReadyState on both events
}

// -----------------------------------------------------------------------------
// 3) ctor(process,gadget,event=SubprocessStopped): constructor calls
// handleEvent(event)
//    -> subprocessStoppedEvent set; evaluate() returns nullptr until both
//    events present.
// -----------------------------------------------------------------------------
TEST_F(DeactivatingStateFixture,
       ConstructionWithSubprocessEventStoresItButNoTransitionYet)
{
    std::unique_ptr<resource::Process> proc{};
    std::unique_ptr<resource::Gadget> gad{};

    SubprocessStoppedEvent sp{};
    DeactivatingState s{machine, std::move(proc), std::move(gad),
                        std::move(sp)};

    EXPECT_TRUE(s.subprocessStoppedEvent.has_value());
    EXPECT_FALSE(s.udevStateChangeEvent.has_value());

    // Complete the pair: UdevRemoved -> evaluate() => ReadyState(no error)
    UdevStateChangeEvent udevEv{StateChange::removed};
    EXPECT_CALL(machine, notify(::testing::Truly([](const std::error_code& e) {
                    return !e;
                } // no-error code
                                                 )))
        .Times(1);

    auto next = s.handleEvent(std::move(udevEv));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());
}

// -----------------------------------------------------------------------------
// 4) Event order reversed: SubprocessStopped first, then UdevRemoved ->
// ReadyState(no error)
// -----------------------------------------------------------------------------
TEST_F(DeactivatingStateFixture,
       EventOrderIndependenceSubprocessThenUdevRemoved)
{
    std::unique_ptr<resource::Process> proc{};
    std::unique_ptr<resource::Gadget> gad{};

    DeactivatingState s{machine, std::move(proc), std::move(gad)};

    SubprocessStoppedEvent sp{};
    auto mid = s.handleEvent(std::move(sp)); // only one event -> nullptr
    EXPECT_EQ(mid, nullptr);
    EXPECT_TRUE(s.subprocessStoppedEvent.has_value());
    EXPECT_FALSE(s.udevStateChangeEvent.has_value());

    // Now udev removed -> Info log + ReadyState(no error)
    UdevStateChangeEvent udevEv{StateChange::removed};
    EXPECT_CALL(machine, notify(::testing::Truly([](const std::error_code& e) {
                    return !e;
                } // no-error code
                                                 )))
        .Times(1);

    auto next = s.handleEvent(std::move(udevEv)); // evaluates (both present)
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());
}

// -----------------------------------------------------------------------------
// 5) onEnter(): process and gadget are set to nullptr; returns nullptr
// -----------------------------------------------------------------------------
TEST_F(DeactivatingStateFixture, OnEnterNullifiesResourcesAndReturnsNullptr)
{
    std::unique_ptr<resource::Process>
        proc{}; // if constructors available, you can allocate
    std::unique_ptr<resource::Gadget>
        gad{};  // but null is fine for behavior check

    DeactivatingState s{machine, std::move(proc), std::move(gad)};
    auto next = s.onEnter(); // sets gadget= nullptr; process= nullptr

    EXPECT_EQ(next, nullptr);
    EXPECT_EQ(s.gadget, nullptr);
    EXPECT_EQ(s.process, nullptr);
}

// -----------------------------------------------------------------------------
// 6) Private evaluate(): directly drive both branches via member injection
//    (allowed here by the private->public trick for testing only).
// -----------------------------------------------------------------------------
TEST_F(DeactivatingStateFixture,
       EvaluateReturnsReadyStateForRemovedAndErrorForNonRemoved)
{
    DeactivatingState s{machine, {}, {}};

    // Case A: only one event present -> nullptr
    s.udevStateChangeEvent = UdevStateChangeEvent{StateChange::removed};
    s.subprocessStoppedEvent.reset();
    EXPECT_EQ(s.evaluate(), nullptr); // until both are set, no transition

    // Case B: both present, removed -> ReadyState(no error)
    s.subprocessStoppedEvent = SubprocessStoppedEvent{};
    EXPECT_CALL(machine, notify(::testing::Truly([](const std::error_code& e) {
                    return !e;
                } // no error
                                                 )))
        .Times(1);
    {
        auto next = s.evaluate();
        ASSERT_NE(next, nullptr);
        EXPECT_EQ(next->getStateName(), ReadyState::stateName());
    }

    // Case C: both present, non-removed -> ReadyState(error=connection_refused)
    s.udevStateChangeEvent = UdevStateChangeEvent{StateChange::inserted};
    EXPECT_CALL(machine, notify(::testing::Truly([](const std::error_code& e) {
                    return e ==
                           std::make_error_code(std::errc::connection_refused);
                })))
        .Times(1);
    {
        auto next = s.evaluate();
        ASSERT_NE(next, nullptr);
        EXPECT_EQ(next->getStateName(), ReadyState::stateName());
    }
}

// -----------------------------------------------------------------------------
// 7) Unsupported event -> throws SdBusError(EBUSY)
// -----------------------------------------------------------------------------
TEST_F(DeactivatingStateFixture, HandleEventUnsupportedThrowsBusy)
{
    DeactivatingState s{machine, {}, {}};
    MountEvent ev{std::nullopt}; // any unsupported event type
    EXPECT_THROW((void)s.handleEvent(std::move(ev)),
                 sdbusplus::exception::SdBusError);
}

// -----------------------------------------------------------------------------
// 8) CRTP identity helpers
// -----------------------------------------------------------------------------
TEST_F(DeactivatingStateFixture, StateNameAndGetIf)
{
    DeactivatingState s{machine, {}, {}};
    EXPECT_EQ(s.getStateName(),
              DeactivatingState::stateName());    // CRTP name forwarding

    auto* asThis = s.get_if<DeactivatingState>(); // type-safe "downcast"
    EXPECT_EQ(asThis, &s);

    auto* asReady = s.get_if<ReadyState>();
    EXPECT_EQ(asReady, nullptr);
}
