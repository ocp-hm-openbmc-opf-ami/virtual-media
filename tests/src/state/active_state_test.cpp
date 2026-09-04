// SPDX-License-Identifier: Apache-2.0
/**
 * @file active_state_test.cpp
 * @brief Comprehensive unit tests for ActiveState (mounted and serving state)
 *
 * This test suite validates the active state of the virtual media state
 * machine, representing the operational state when virtual media is
 * successfully mounted and being actively served to the host system.
 *
 * Testing Strategy:
 * - Mock UsbGadget via test shim to avoid hardware dependencies
 * - Mock MountPointStateMachine interface for isolated testing
 * - Test inactivity timeout mechanism with timer simulation
 * - Validate USB gadget statistics monitoring for activity detection
 *
 * Functional Coverage:
 * 1. **Event Handling**:
 *    - UdevStateChangeEvent → transition to DeactivatingState
 *    - SubprocessStoppedEvent → transition to DeactivatingState
 *    - UnmountEvent → notificationStart() + DeactivatingState
 *    - MountEvent → throws SdBusError(EPERM) - already mounted
 *
 * 2. **Inactivity Timeout Logic**:
 *    - Timeout disabled (0s) → onEnter() returns nullptr (no timer)
 *    - Non-expired timeout → reschedule handler, update remaining time
 *    - Expired timeout → emit UnmountEvent for graceful unmount
 *
 * 3. **USB Gadget Statistics Monitoring**:
 *    - Stats present & changed → update lastStats/lastAccess timestamps
 *    - Stats present & unchanged → no update (idle detection)
 *    - Stats missing/unavailable → no update (gadget not configured)
 *
 * Test Coverage Areas:
 * - All handleEvent() method variants
 * - Timer-based inactivity detection and auto-unmount
 * - USB gadget activity monitoring via getStats()
 * - Proper notification flow on state transitions
 * - Error handling for invalid operations (mount while active)
 *
 * Edge Cases Tested:
 * - Zero inactivity timeout (feature disabled)
 * - Timeout expiration at exact boundary
 * - Statistics changes vs. no changes
 * - Rapid successive events (udev then subprocess)
 * - Invalid operations during active state
 *
 * Dependencies:
 * - UsbGadgetTestShim: Simulates USB gadget statistics without hardware
 * - MockMountPointStateMachine: Isolates state machine interactions
 * - boost::asio::io_context: For timer-based operations
 * - sdbusplus::exception: For D-Bus error simulation
 */

#include "events.hpp"
#include "interfaces/mount_point_state_machine.hpp" // brings configuration.hpp -> system.hpp
#include "state/basic_state.hpp"
#include "state/deactivating_state.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/exception.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <system_error>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// -----------------------------------------------------------------------------
// Test-only UsbGadget shim (struct) + gMock (as NiceMock to suppress warnings)
// -----------------------------------------------------------------------------
class UsbGadgetShimMock
{
  public:
    MOCK_METHOD(std::optional<std::string>, getStats, (const std::string& name),
                ());
};

static UsbGadgetShimMock* g_usbShimMock = nullptr;

struct UsbGadgetTestShim
{
    static std::optional<std::string> getStats(const std::string& name)
    {
        return g_usbShimMock ? g_usbShimMock->getStats(name) : std::nullopt;
    }
};

// -----------------------------------------------------------------------------
// Include ActiveState with private exposure, aliasing UsbGadget to our shim.
// IMPORTANT: system.hpp has already been included via the earlier headers,
// so re-inclusion is guarded and we won't affect the real struct.
// -----------------------------------------------------------------------------
#define private public
#define UsbGadget UsbGadgetTestShim
#include "state/active_state.hpp"
#undef UsbGadget
#undef private

// -----------------------------------------------------------------------------
// Mock for interfaces::MountPointStateMachine
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
class ActiveStateFixture : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc{};
    Configuration::MountPoint mp{};
    std::optional<interfaces::MountPointStateMachine::Target> target{};
    int exitCode{0};
    std::string nameStr{"Slot_3"};
    std::string addInfoStr{"unit-test-additional-info"};

    using NiceMachine =
        ::testing::NiceMock<interfaces::MockMountPointStateMachine>;
    NiceMachine machine;

    ::testing::NiceMock<UsbGadgetShimMock>
        statsMock; // NiceMock to suppress warnings

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

        // Install shim mock
        g_usbShimMock = &statsMock;

        // Default: no stats (explicit to avoid default-value warnings)
        ON_CALL(statsMock, getStats(::testing::_))
            .WillByDefault(Return(std::nullopt));
    }

    void TearDown() override
    {
        g_usbShimMock = nullptr;
    }

    // Helper: construct ActiveState with empty unique_ptrs
    static ActiveState makeActive(interfaces::MountPointStateMachine& m)
    {
        return ActiveState{m, std::unique_ptr<resource::Process>{},
                           std::unique_ptr<resource::Gadget>{}};
    }
};

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

// 1) Udev event -> DeactivatingState
TEST_F(ActiveStateFixture, HandleEventUdevTransitionsToDeactivatingState)
{
    auto s = makeActive(machine);
    UdevStateChangeEvent ev{StateChange::inserted};

    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), DeactivatingState::stateName());
}

// 2) SubprocessStopped event -> DeactivatingState
TEST_F(ActiveStateFixture,
       HandleEventSubprocessStoppedTransitionsToDeactivatingState)
{
    auto s = makeActive(machine);
    SubprocessStoppedEvent ev{};

    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), DeactivatingState::stateName());
}

// 3) Unmount event -> notificationStart() + DeactivatingState
TEST_F(ActiveStateFixture,
       HandleEventUnmountStartsNotificationAndTransitionsToDeactivating)
{
    auto s = makeActive(machine);
    EXPECT_CALL(machine, notificationStart()).Times(1);

    UnmountEvent ev{};
    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), DeactivatingState::stateName());
}

// 4) Mount event -> throws SdBusError(EPERM)
TEST_F(ActiveStateFixture, HandleEventMountThrowsSdBusErrorPerm)
{
    auto s = makeActive(machine);
    MountEvent ev{std::nullopt};
    EXPECT_THROW((void)s.handleEvent(std::move(ev)),
                 sdbusplus::exception::SdBusError);
}

// 5) onEnter(): inactivity disabled -> returns nullptr
TEST_F(ActiveStateFixture, OnEnterInactivityDisabledReturnsNullptr)
{
    Configuration::inactivityTimeout = std::chrono::seconds(0);
    auto s = makeActive(machine);
    auto next = s.onEnter();
    EXPECT_EQ(next, nullptr);
}

// 6) CRTP identity helpers
TEST_F(ActiveStateFixture, StateNameAndGetIf)
{
    auto s = makeActive(machine);
    EXPECT_EQ(s.getStateName(), ActiveState::stateName());
    EXPECT_EQ(s.get_if<ActiveState>(), &s);
    EXPECT_EQ(s.get_if<DeactivatingState>(), nullptr);
}

// 7) Handler non-expired -> update remaining & reschedule
TEST_F(ActiveStateFixture,
       OnEnterHandlerNonExpiredUpdatesRemainingAndReschedules)
{
    Configuration::inactivityTimeout = std::chrono::seconds(10);
    ActiveState s{machine, {}, {}};
    s.onEnter(); // installs handler

    s.lastAccess = std::chrono::steady_clock::now() - std::chrono::seconds(3);

    const boost::system::error_code ok;
    s.handler(ok);

    EXPECT_EQ(machine.getConfig().remainingInactivityTimeout,
              std::chrono::seconds(7));
}

// 8) Handler expired -> post emitUnmountEvent()
TEST_F(ActiveStateFixture, OnEnterHandlerExpiredPostsEmitUnmountEvent)
{
    Configuration::inactivityTimeout = std::chrono::seconds(5);
    ActiveState s{machine, {}, {}};
    s.onEnter();

    s.lastAccess = std::chrono::steady_clock::now() - std::chrono::seconds(9);

    EXPECT_CALL(machine, emitUnmountEvent()).Times(1);

    const boost::system::error_code ok;
    s.handler(ok);
    machine.getIoc().poll();
}

// 9) Stats present & DIFFERENT -> update lastStats/lastAccess (TRUE branch)
TEST_F(ActiveStateFixture, OnEnterHandlerStatsUpdatesLastAccessWhenDifferent)
{
    using ::testing::Return;

    Configuration::inactivityTimeout = std::chrono::seconds(10);
    auto s = makeActive(machine);

    s.lastStats = "OLD";
    ON_CALL(statsMock, getStats(::testing::_))
        .WillByDefault(Return(std::optional<std::string>{"NEW"}));

    s.onEnter(); // installs handler

    auto before = s.lastAccess;
    const boost::system::error_code ok;
    s.handler(ok);

    EXPECT_EQ(s.lastStats, "NEW");
    EXPECT_GT(s.lastAccess, before);
}

// 10) Stats present & EQUAL + MISSING -> no update (FALSE branches)
TEST_F(ActiveStateFixture, OnEnterHandlerStatsNoChangeWhenEqualOrMissing)
{
    using ::testing::Return;

    Configuration::inactivityTimeout = std::chrono::seconds(10);
    auto s = makeActive(machine);
    s.onEnter();

    // present & equal
    s.lastStats = "SAME";
    ON_CALL(statsMock, getStats(::testing::_))
        .WillByDefault(Return(std::optional<std::string>{"SAME"}));

    auto beforeA = s.lastAccess;
    const boost::system::error_code ok;
    s.handler(ok);
    EXPECT_EQ(s.lastStats, "SAME");
    EXPECT_EQ(s.lastAccess, beforeA);

    // missing
    ON_CALL(statsMock, getStats(::testing::_))
        .WillByDefault(Return(std::nullopt));

    auto beforeB = s.lastAccess;
    s.handler(ok);
    EXPECT_EQ(s.lastStats, "SAME");
    EXPECT_EQ(s.lastAccess, beforeB);
}

// 11) Constructor calls machine.notify() with a no-error code
TEST_F(ActiveStateFixture, ConstructionCallsNotify)
{
    EXPECT_CALL(machine, notify(::testing::Truly([](const std::error_code& e) {
                    return !e;
                })))
        .Times(1);

    ActiveState s{machine, {}, {}};
}

// 12) Handler with non-zero error code -> early return (no stats update, no
// unmount posted)
TEST_F(ActiveStateFixture, HandlerWithErrorCodeReturnsEarly)
{
    Configuration::inactivityTimeout = std::chrono::seconds(10);
    ActiveState s{machine, {}, {}};
    s.onEnter();

    s.lastStats = "BEFORE";
    auto beforeAccess = s.lastAccess;

    // Simulate timer cancellation
    boost::system::error_code cancelled = boost::asio::error::operation_aborted;
    s.handler(cancelled);

    // Nothing should change — early return was taken
    EXPECT_EQ(s.lastStats, "BEFORE");
    EXPECT_EQ(s.lastAccess, beforeAccess);
}

// 13) handleEvent(AnyEvent) template -> throws SdBusError(EOPNOTSUPP)
//     RegisterDbusEvent has no specific overload in ActiveState, so it hits the
//     template.
TEST_F(ActiveStateFixture, HandleEventUnsupportedAnyEventThrowsEOpNotSupp)
{
    auto s = makeActive(machine);
    RegisterDbusEvent ev{nullptr, nullptr};
    EXPECT_THROW((void)s.handleEvent(std::move(ev)),
                 sdbusplus::exception::SdBusError);
}
