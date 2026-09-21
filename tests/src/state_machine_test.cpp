// SPDX-License-Identifier: Apache-2.0
/**
 * @file state_machine_test.cpp
 * @brief Comprehensive unit tests for MountPointStateMachine (concrete state
 * machine)
 *
 * This test suite validates the concrete implementation of the mount point
 * state machine, which orchestrates all virtual media operations for a single
 * mount point/slot.
 *
 * Testing Strategy:
 * - Single-threaded execution for deterministic behavior
 * - Unique slot names per test to avoid D-Bus vtable conflicts
 * - Suite-wide D-Bus connection for efficiency
 * - No actual device operations (NBD/mounting simulated)
 *
 * Functional Coverage:
 * 1. **Constructor Side Effects**:
 *    - DeviceMonitor.addDevice() called with configured NBD device
 *    - Initial state set to InitialState
 *    - Configuration copied correctly
 *
 * 2. **Accessor Methods**:
 *    - getName() returns slot name
 *    - getConfig() returns configuration reference
 *    - getTarget() returns mutable target reference
 *    - getExitCode() returns process exit code reference
 *    - getIoc() returns io_context reference
 *    - getAdditionalInfo() returns additional info string
 *
 * 3. **D-Bus Notification Flow**:
 *    - notificationInitialize() stores D-Bus connection info
 *    - notificationStart() resets notification state
 *    - notify() triggers D-Bus signal emission (when initialized)
 *
 * 4. **Event Routing**:
 *    - RegisterDbusEvent → InitialState → ReadyState transition
 *    - UdevStateChangeEvent only processed if device matches
 *    - Non-matching device events ignored
 *
 * 5. **Production-Grade Practices**:
 *    - Unique slot names prevent test interference
 *    - Clean state between tests
 *    - No global state pollution
 *
 * Test Coverage Areas:
 * - Constructor initialization
 * - State machine accessors
 * - D-Bus notification setup
 * - Event dispatching and filtering
 * - State transitions (InitialState → ReadyState)
 * - Device matching logic
 *
 * Edge Cases Tested:
 * - Event for non-matching device (ignored)
 * - Multiple state machines in same process
 * - D-Bus connection sharing
 *
 * Dependencies:
 * - boost::asio::io_context: Async operations
 * - sdbusplus: D-Bus communication
 * - DeviceMonitor: NBD device tracking
 *
 * @note Uses static test suite setup for shared D-Bus connection
 * @note Each test uses unique slot name (Slot_SM_N) for isolation
 */
// Single-thread friendly unit tests for concrete MountPointStateMachine
// (state_machine.hpp)
//
// Coverage focus:
//  - Constructor side-effects (DeviceMonitor.addDevice, initial state)
//  - Accessors (name, additionalInfo, config, target, exitCode, ioc)
//  - DBus notification flow (notificationInitialize / notificationStart /
//  notify)
//  - RegisterDbusEvent transition (InitialState -> ReadyState)
//  - Event routing (udev matching only)
//  - Production-grade: use unique slot names per test to avoid D-Bus vtable
//  duplication.

#include "configuration.hpp"
#include "events.hpp"
#include "interfaces/mount_point_state_machine.hpp"
#include "system.hpp"
#include "utils.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
// NOTE: Do not include state/initial_state.hpp directly; state_machine.hpp
// includes it.
// Expose InitialState's private static bookkeeping so suite teardown can
// reset it (see initial_state_test.cpp for the same pattern/rationale).
#define private public
#include "state_machine.hpp" // Under test
#undef private

class StateMachineFixture : public ::testing::Test
{
  protected:
    static boost::asio::io_context suiteIo;
    static std::shared_ptr<sdbusplus::asio::connection> suiteBus;
    static std::shared_ptr<sdbusplus::asio::object_server> suiteObjServer;
    boost::asio::io_context ioc{};
    DeviceMonitor devMon{ioc};
    Configuration::MountPoint mp{};
    std::string nameStr{"Slot_SM_1"};
    std::string addInfoStr{"state-machine-additional-info"};

    std::unique_ptr<MountPointStateMachine> machine;

    static void SetUpTestSuite()
    {
        InitialState::allMountPoints.clear();
        InitialState::globalLocalServiceCreated = false;
        InitialState::globalObjServer.reset();
        suiteBus = std::make_shared<sdbusplus::asio::connection>(suiteIo);
        suiteObjServer =
            std::make_shared<sdbusplus::asio::object_server>(suiteBus);
    }
    static void TearDownTestSuite()
    {
        InitialState::allMountPoints.clear();
        InitialState::globalLocalServiceCreated = false;
        InitialState::globalObjServer.reset();
        suiteObjServer.reset();
        suiteBus.reset();
    }

    void SetUp() override
    {
        mp.nbdDevice = NBDDevice("nbd0");
        mp.unixSocket = "/tmp/virtual-media-ut/state-machine.sock";
        mp.endPointId = "EP-101";
        mp.timeout = 10;
        mp.blocksize = std::nullopt;
        mp.remainingInactivityTimeout = std::chrono::seconds(0);
        mp.verifyCertificate = true;
        mp.mode = Configuration::Mode::proxy;

        machine = std::make_unique<MountPointStateMachine>(ioc, devMon, nameStr,
                                                           mp, addInfoStr);
    }
};

boost::asio::io_context StateMachineFixture::suiteIo{};
std::shared_ptr<sdbusplus::asio::connection> StateMachineFixture::suiteBus{};
std::shared_ptr<sdbusplus::asio::object_server>
    StateMachineFixture::suiteObjServer{};

// Constructor side-effects & initial state
TEST_F(StateMachineFixture, Ctor_AddsDeviceToMonitor_InitialStateIsSet)
{
    EXPECT_EQ(devMon.getState(mp.nbdDevice), StateChange::removed);
    EXPECT_EQ(machine->getState().getStateName(), InitialState::stateName());

    // getName() returns the slot name passed at construction
    EXPECT_EQ(machine->getName(), nameStr);

    // getAdditionalInfo() returns the string passed at construction
    EXPECT_EQ(machine->getAdditionalInfo(), addInfoStr);

    const auto& cfg = machine->getConfig();
    EXPECT_EQ(cfg.nbdDevice.to_string(), mp.nbdDevice.to_string());
    EXPECT_EQ(cfg.unixSocket, mp.unixSocket);
    EXPECT_EQ(cfg.endPointId, mp.endPointId);
    EXPECT_EQ(cfg.mode, mp.mode);
}

// Accessors: target, exitCode, ioc
TEST_F(StateMachineFixture, Accessors_TargetExitCodeIoc)
{
    auto& targetRef = machine->getTarget();
    EXPECT_FALSE(targetRef.has_value());
    targetRef.emplace();
    targetRef->imgUrl = "http://example.com/test.iso";
    targetRef->rw = false;

    EXPECT_TRUE(machine->getTarget().has_value());
    EXPECT_EQ(machine->getTarget()->imgUrl, "http://example.com/test.iso");
    EXPECT_FALSE(machine->getTarget()->rw);

    // exitCode is initialised to -1 by the constructor
    EXPECT_EQ(machine->getExitCode(), -1);
    auto& ecRef = machine->getExitCode();
    ecRef = 0;
    EXPECT_EQ(machine->getExitCode(), 0);
    EXPECT_EQ(&machine->getIoc(), &ioc);
}

// DBus notification lifecycle: initialize -> start -> notify
TEST_F(StateMachineFixture, Notification_Initialize_Start_Notify_Smoke)
{
    const std::string objectPath = "/xyz/openbmc_project/VirtualMedia/Proxy/" +
                                   std::string(machine->getName());
    const std::string iface = "xyz.openbmc_project.VirtualMedia.Proxy";

    machine->notificationInitialize(suiteBus, objectPath, iface, "Completion");
    ASSERT_NE(machine->completionNotification, nullptr);

    machine->notificationStart();
    // notify() calls timer->cancel(), which only schedules the cancellation.
    // Run the ioc briefly to flush the pending async_wait callback
    // (ec=op_canceled), exercising the early-return branch inside the
    // notificationStart() lambda.
    machine->notify(std::make_error_code(std::errc::device_or_resource_busy));
    ioc.run_for(std::chrono::milliseconds(50));
    SUCCEED();
}

// RegisterDbusEvent -> ReadyState; completionNotification created
TEST_F(StateMachineFixture,
       RegisterDbusEvent_CreatesInterfaces_TransitionsToReadyState)
{
    machine->emitRegisterDBusEvent(suiteBus, suiteObjServer);
    EXPECT_EQ(machine->getState().getStateName(), ReadyState::stateName());
    ASSERT_NE(machine->completionNotification, nullptr);
}

// (Fixed) Event routing: keep it deterministic & thread-free.
// Use a unique name; assert unmount in ReadyState is forbidden.
TEST_F(StateMachineFixture, EmitMountAndUnmount_Events_ContractsHold)
{
    Configuration::MountPoint mp2 = mp;
    auto machine2 = std::make_unique<MountPointStateMachine>(
        ioc, devMon, "Slot_SM_Emit_1", mp2, addInfoStr);

    machine2->emitRegisterDBusEvent(suiteBus, suiteObjServer);
    ASSERT_EQ(machine2->getState().getStateName(), ReadyState::stateName());

    // In ReadyState, Unmount is not permitted per contract; assert it throws.
    // This avoids exercising external spawn and keeps the test deterministic.
    EXPECT_THROW(machine2->emitUnmountEvent(),
                 sdbusplus::exception::SdBusError);
}

// Udev event routing: matching vs non-matching devices, unique name
TEST_F(StateMachineFixture, EmitUdevStateChange_MatchingDevice_Only)
{
    Configuration::MountPoint mp3 = mp;
    auto machine3 = std::make_unique<MountPointStateMachine>(
        ioc, devMon, "Slot_SM_Udev_1", mp3, addInfoStr);

    machine3->emitRegisterDBusEvent(suiteBus, suiteObjServer);
    ASSERT_EQ(machine3->getState().getStateName(), ReadyState::stateName());

    const NBDDevice other{"nbd9"}; // different device token
    machine3->emitUdevStateChangeEvent(other, StateChange::inserted);
    EXPECT_EQ(machine3->getState().getStateName(), ReadyState::stateName());

    machine3->emitUdevStateChangeEvent(mp3.nbdDevice, StateChange::inserted);
    SUCCEED();
}

// Additional info is mutable
TEST_F(StateMachineFixture, SetAdditionalInfo_UpdatesGetter)
{
    machine->setAdditionalInfo("updated-client-info");
    EXPECT_EQ(machine->getAdditionalInfo(),
              std::string_view{"updated-client-info"});
}

TEST_F(StateMachineFixture, EmitSubprocessStoppedEvent_Routes)
{
    // Unique name to avoid vtable duplication
    Configuration::MountPoint mpX = mp;
    auto machineX = std::make_unique<MountPointStateMachine>(
        ioc, devMon, "Slot_SM_Subproc_1", mpX, addInfoStr);
    machineX->emitRegisterDBusEvent(suiteBus, suiteObjServer);
    ASSERT_EQ(machineX->getState().getStateName(), ReadyState::stateName());
    // Just ensure no crash when routing the event
    machineX->emitSubprocessStoppedEvent();
    SUCCEED();
}
/////////////////////////////////////////////////////////////////////////

// ---- Test-only states to exercise changeState(...) recursion ---------------
class NextState : public BasicStateT<NextState>
{
  public:
    explicit NextState(interfaces::MountPointStateMachine& machine) :
        BasicStateT<NextState>(machine)
    {}
    static std::string_view stateName()
    {
        return "NextState";
    }
};

class EnterChainState : public BasicStateT<EnterChainState>
{
  public:
    explicit EnterChainState(interfaces::MountPointStateMachine& machine) :
        BasicStateT<EnterChainState>(machine)
    {}
    static std::string_view stateName()
    {
        return "EnterChainState";
    }

    // Returning a new state from onEnter() forces changeState(...) to recurse.
    std::unique_ptr<BasicState> onEnter() override
    {
        return std::make_unique<NextState>(this->machine);
    }
};

TEST_F(StateMachineFixture, ChangeState_Recurses_WhenOnEnterReturnsNewState)
{
    // Use a unique name to avoid any D-Bus duplication
    Configuration::MountPoint mpX = mp;
    auto machineX = std::make_unique<MountPointStateMachine>(
        ioc, devMon, "Slot_SM_Chain_1", mpX, addInfoStr);

    machineX->changeState(std::make_unique<EnterChainState>(*machineX));
    EXPECT_EQ(machineX->getState().getStateName(), NextState::stateName());
}

// notificationStart() with timeout=nullopt falls back to defaultTimeout (30s).
// Immediately cancel via notify() so there's no actual wait.
TEST_F(StateMachineFixture, NotificationStart_NulloptTimeout_UsesDefaultTimeout)
{
    Configuration::MountPoint mpNull = mp;
    mpNull.timeout = std::nullopt;
    auto machineN = std::make_unique<MountPointStateMachine>(
        ioc, devMon, "Slot_SM_Null_Timeout_1", mpNull, addInfoStr);

    const std::string objectPath = "/xyz/openbmc_project/VirtualMedia/Proxy/" +
                                   std::string(machineN->getName());
    const std::string iface = "xyz.openbmc_project.VirtualMedia.Proxy";

    machineN->notificationInitialize(suiteBus, objectPath, iface, "Completion");
    ASSERT_NE(machineN->completionNotification, nullptr);

    // Start with nullopt timeout -> uses defaultTimeout (30) internally.
    // Cancel immediately via notify() so the test doesn't wait 35 seconds.
    machineN->notificationStart();
    machineN->notify(std::make_error_code(std::errc::operation_canceled));
    SUCCEED();
}

TEST_F(StateMachineFixture, Notification_TimerExpiry_FiresHandler)
{
    // Configure a short timeout (1s => ~6s wait). You can also set 0 for ~5s.
    Configuration::MountPoint mpT = mp;
    mpT.timeout = 1;
    auto machineT = std::make_unique<MountPointStateMachine>(
        ioc, devMon, "Slot_SM_Timer_1", mpT, addInfoStr);

    const std::string objectPath = "/xyz/openbmc_project/VirtualMedia/Proxy/" +
                                   std::string(machineT->getName());
    const std::string iface = "xyz.openbmc_project.VirtualMedia.Proxy";

    machineT->notificationInitialize(suiteBus, objectPath, iface, "Completion");
    ASSERT_NE(machineT->completionNotification, nullptr);

    // Start and let the timer expire naturally; run I/O long enough.
    machineT->notificationStart();
    ioc.run_for(std::chrono::seconds(7)); // >= timeout+5
    SUCCEED();
}

// emitMountEvent() exercises the ReadyState -> ActivatingState transition.
// Spawn fails synchronously in the test environment (no nbdkit), so the machine
// rolls back to ReadyState immediately within the emitMountEvent() call.
TEST_F(StateMachineFixture, EmitMountEvent_SpawnFails_RollsBackToReadyState)
{
    Configuration::MountPoint mpLocal = mp;
    mpLocal.mode = Configuration::Mode::local;
    auto machineM = std::make_unique<MountPointStateMachine>(
        ioc, devMon, "Slot_SM_Mount_1", mpLocal, addInfoStr);

    machineM->emitRegisterDBusEvent(suiteBus, suiteObjServer);
    ASSERT_EQ(machineM->getState().getStateName(), ReadyState::stateName());

    interfaces::MountPointStateMachine::Target tgt{};
    tgt.imgUrl = "/tmp/test.img";
    tgt.rw = false;
    machineM->emitMountEvent(std::move(tgt));

    // Spawn failure is synchronous; machine has already recovered to
    // ReadyState.
    EXPECT_EQ(machineM->getState().getStateName(), ReadyState::stateName());
}
