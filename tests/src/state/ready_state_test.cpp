// SPDX-License-Identifier: Apache-2.0
/**
 * @file ready_state_test.cpp
 * @brief Comprehensive unit tests for ReadyState (idle/ready for mount
 * operations)
 *
 * This test suite validates the ready state of the virtual media state machine,
 * representing the idle state when virtual media is configured and ready to
 * accept mount requests but not currently mounted.
 *
 * Testing Strategy:
 * - Mock MountPointStateMachine for isolated interface testing
 * - Validate state transitions and notification flows
 * - Test constructor variants (default and error-code based)
 * - Verify CRTP pattern implementation
 *
 * Functional Coverage:
 * 1. **Constructors**:
 *    - Default constructor → notify() with default error_code
 *    - Error-code constructor → notify() with make_error_code(ec)
 *
 * 2. **State Entry (onEnter)**:
 *    - Target cleared (reset to std::nullopt)
 *    - Inactivity timeout set to 0s (disabled)
 *    - Returns nullptr (no async operations)
 *
 * 3. **Event Handling**:
 *    - MountEvent → notificationStart(), set target if present, →
 * ActivatingState
 *    - UnmountEvent → throws sdbusplus::exception::SdBusError(EPERM)
 *    - Other events → returns nullptr (no state change)
 *
 * 4. **CRTP Identity**:
 *    - getStateName() returns "ReadyState"
 *    - get_if<ReadyState>() returns valid pointer when in ready state
 *
 * Test Coverage Areas:
 * - All constructor variants
 * - onEnter() side effects and return value
 * - All handleEvent() method variants
 * - Notification flow (notificationStart)
 * - Target manipulation
 * - Error handling for invalid operations
 *
 * Edge Cases Tested:
 * - MountEvent with empty optional target
 * - MountEvent with populated target
 * - UnmountEvent while already unmounted (EPERM)
 * - Unsupported event types
 * - State identity verification
 *
 * Dependencies:
 * - MockMountPointStateMachine: Interface isolation
 * - boost::asio::io_context: Context for async operations
 * - sdbusplus::exception: D-Bus error handling
 *
 * @note ReadyState is a terminal resting state - always ready for next mount
 */
// Unit tests for ReadyState (state/ready_state.hpp)
//
// Targets behaviors from ready_state.hpp:
//  - ctor(): notify(default error_code)
//  - ctor(ec,msg): notify(make_error_code(ec))
//  - onEnter(): clear target, set inactivity timeout to 0s, return nullptr
//  - handleEvent(MountEvent): notificationStart(), set target if present,
//  transition to ActivatingState
//  - handleEvent(UnmountEvent): throws sdbusplus::exception::SdBusError(EPERM)
//  - handleEvent(AnyOther): return nullptr
//
// Also verifies CRTP identity: getStateName() and get_if<T>() from
// BasicStateT<T>.

#include "events.hpp"
#include "interfaces/mount_point_state_machine.hpp" // interface & Target
#include "state/activating_state.hpp"
#include "state/basic_state.hpp"
#include "state/ready_state.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/exception.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <system_error>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// -----------------------------------------------------------------------------
// Local gmock for interfaces::MountPointStateMachine
// NOTE: The interface declares ONE notify(const std::error_code& ec = {})
// method.
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// Test fixture
// -----------------------------------------------------------------------------
class ReadyStateFixture : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc{};
    Configuration::MountPoint mp{};
    std::optional<interfaces::MountPointStateMachine::Target> target{};
    int exitCode{42};
    std::string nameStr{"Slot_2"};
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

    static interfaces::MountPointStateMachine::Target makeTarget(
        std::string url, bool rw)
    {
        interfaces::MountPointStateMachine::Target t{};
        t.imgUrl = std::move(url);
        t.rw = rw;
        return t;
    }
};

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

// 1) Construction: default (no error) calls notify() with a "no-error" code;
// error member stays nullopt
TEST_F(ReadyStateFixture, ConstructionWithoutErrorInvokesNotifyDefaultErrorCode)
{
    EXPECT_CALL(machine, notify(::testing::Truly([](const std::error_code& e) {
                    return !e;
                } // value()==0 => no error
                                                 )))
        .Times(1);

    ReadyState s{machine};

    // Default constructor must not populate the error member
    EXPECT_FALSE(s.error.has_value());
}

// 2) Construction: with error calls notify(make_error_code(ec)) and stores
// error
TEST_F(ReadyStateFixture, ConstructionWithErrorInvokesNotifyWithErrorCode)
{
    const std::errc ec = std::errc::permission_denied;
    const std::string msg = "no access";

    EXPECT_CALL(machine,
                notify(::testing::Truly([ec](const std::error_code& e) {
                    return e == std::make_error_code(ec);
                })))
        .Times(1);

    ReadyState s{machine, ec, msg};

    // Verify the error is stored in the state
    ASSERT_TRUE(s.error.has_value());
    EXPECT_EQ(s.error->code, ec);
    EXPECT_EQ(s.error->message, msg);
}

// 3) onEnter(): clears target and zeroes inactivity timeout; returns nullptr
TEST_F(ReadyStateFixture, OnEnterClearsTargetAndInactivityTimeout)
{
    // Arrange: a non-empty target and non-zero timeout
    target = makeTarget("/tmp/image.iso", /*rw=*/false);
    mp.remainingInactivityTimeout = std::chrono::seconds(123);

    ReadyState s{machine};
    auto next = s.onEnter();

    EXPECT_EQ(next, nullptr);
    EXPECT_FALSE(machine.getTarget().has_value()); // cleared to nullopt
    EXPECT_EQ(machine.getConfig().remainingInactivityTimeout,
              std::chrono::seconds{0});
}

// 4) handleEvent(MountEvent) with target: sets target & transitions to
// ActivatingState
TEST_F(ReadyStateFixture,
       HandleEventMountWithTargetTransitionsToActivatingState)
{
    ReadyState s{machine};

    auto tgt = makeTarget("https://example.com/os.iso", /*rw=*/true);
    MountEvent ev{std::make_optional(std::move(tgt))};

    EXPECT_CALL(machine, notificationStart()).Times(1);

    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ActivatingState::stateName());

    // Target should be set on the machine
    ASSERT_TRUE(machine.getTarget().has_value());
    EXPECT_EQ(machine.getTarget()->imgUrl, "https://example.com/os.iso");
    EXPECT_TRUE(machine.getTarget()->rw);
}

// 5) handleEvent(MountEvent) without target: only transition
TEST_F(ReadyStateFixture,
       HandleEventMountWithoutTargetTransitionsAndDoesNotModifyTarget)
{
    ReadyState s{machine};
    MountEvent ev{std::nullopt}; // no target set

    EXPECT_CALL(machine, notificationStart()).Times(1);

    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ActivatingState::stateName());

    // Target remains unchanged (nullopt)
    EXPECT_FALSE(machine.getTarget().has_value());
}

// 6) handleEvent(UnmountEvent): throws SdBusError(EPERM)
TEST_F(ReadyStateFixture, HandleEventUnmountThrowsSdBusErrorPerm)
{
    ReadyState s{machine};
    UnmountEvent ev{};
    EXPECT_THROW((void)s.handleEvent(std::move(ev)),
                 sdbusplus::exception::SdBusError);
}

// 7) handleEvent(Unsupported): returns nullptr (and logs error)
TEST_F(ReadyStateFixture, HandleEventUnsupportedReturnsNullptr)
{
    ReadyState s{machine};
    SubprocessStoppedEvent ev{}; // any unsupported event type
    auto next = s.handleEvent(std::move(ev));
    EXPECT_EQ(next, nullptr);
}

// 8) CRTP identity: stateName/get_if work
TEST_F(ReadyStateFixture, StateNameAndGetIf)
{
    ReadyState s{machine};
    EXPECT_EQ(s.getStateName(), ReadyState::stateName());

    auto* asReady = s.get_if<ReadyState>();
    EXPECT_EQ(asReady, &s);

    auto* asActivating = s.get_if<ActivatingState>();
    EXPECT_EQ(asActivating, nullptr);
}
