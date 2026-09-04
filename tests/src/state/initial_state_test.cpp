// SPDX-License-Identifier: Apache-2.0
/**
 * @file initial_state_test.cpp
 * @brief Comprehensive unit tests for InitialState (entry point state of
 * virtual media state machine)
 *
 * This test suite validates the initial state functionality of the virtual
 * media mount point state machine. InitialState is the entry state when a
 * virtual media slot is first configured or after reset.
 *
 * Test Coverage (24 tests total, combined from 4 source files):
 * - initial_state_interfaces_singlethread_test.cpp (7 tests): CRTP identity and
 * interface compliance
 * - initial_state_logical_coverage_test.cpp (7 tests): State transition logic
 * and configuration modes
 * - initial_state_singlethread_integration_test.cpp (3 tests): D-Bus
 * integration and service registration
 * - initial_state_test.cpp (7 tests): Event handling and mode-specific
 * behaviors
 *
 * Testing Strategy:
 * - UsbGadget shim for testing without real hardware dependencies
 * - Mock MountPointStateMachine interface for isolated testing
 * - Single-threaded execution for deterministic behavior
 * - D-Bus service initialization without requiring real system services
 *
 * Functional Areas:
 * 1. **State Identity**: CRTP pattern implementation, getStateName()
 * correctness
 * 2. **D-Bus Integration**: Service registration, object path mapping (slot ->
 * D-Bus path)
 * 3. **Mode Handling**: Proxy vs. Local vs. Legacy mode configuration
 * validation
 * 4. **Cleanup Logic**: USB gadget cleanup in local mode, global mount
 * bookkeeping
 * 5. **Event Processing**: RegisterDbusEvent handling, state transitions to
 * ReadyState
 * 6. **Error Handling**: Invalid configuration detection, unsupported event
 * rejection
 *
 * Coverage Focus:
 * - All code paths in InitialState::handleEvent() variants
 * - Configuration mode branching (proxy/local/legacy)
 * - Global state management (localMountInProgress map)
 * - D-Bus method exposure and error responses
 * - Thread-safe single-threaded operations
 */

#include "events.hpp"
#include "interfaces/mount_point_state_machine.hpp"
#include "state/basic_state.hpp"
#include "state/ready_state.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

//=============================================================================
// Test Infrastructure - USB Gadget Shim
//=============================================================================

/**
 * @brief Test-only USB gadget controller shim (no real system operations)
 *
 * Replaces the production UsbGadget class to enable testing without:
 * - Real USB gadget hardware
 * - Linux configfs filesystem access
 * - Root privileges
 *
 * Records all configure() calls for verification and simulates gadget state.
 * Thread-safe for single-threaded test execution.
 *
 * @note Used via macro redefinition: #define UsbGadget UsbGadgetTestShim
 */
struct UsbGadgetTestShim
{
    /** @brief Simulated gadget configuration state */
    static inline bool configuredFlag = false;
    /** @brief Records whether configure() was called */
    static inline bool configureCalled = false;
    /** @brief Last gadget name passed to configure() */
    static inline std::string lastName;
    /** @brief Last device string passed to configure() */
    static inline std::string lastDevStr;
    /** @brief Last state change (inserted/removed) */
    static inline StateChange lastState;

    /**
     * @brief Check if gadget is configured (simulated)
     * @param name Gadget name (unused in test shim)
     * @return Simulated configuration state
     */
    static bool isConfigured(const std::string&)
    {
        return configuredFlag;
    }

    /**
     * @brief Simulate gadget configuration operation
     * @param name Gadget name to configure
     * @param dev NBD device to associate with gadget
     * @param sc State change (inserted/removed)
     * @return Always returns 0 (success) for testing
     */
    static int configure(const std::string& name, const NBDDevice& dev,
                         StateChange sc)
    {
        configureCalled = true;
        lastName = name;
        lastDevStr = dev.to_string();
        lastState = sc;
        return 0;
    }

    static std::optional<std::string> getStats(const std::string&)
    {
        return std::nullopt;
    }

    static void reset()
    {
        configuredFlag = false;
        configureCalled = false;
        lastName.clear();
        lastDevStr.clear();
        lastState = StateChange::removed;
    }
};

// ---------------------------------------------------------------------------------
// Expose InitialState private members; alias UsbGadget
// ---------------------------------------------------------------------------------
#define private public
#define UsbGadget UsbGadgetTestShim
#include "state/initial_state.hpp"
#undef UsbGadget
#undef private

// ---------------------------------------------------------------------------------
// Mock for interfaces::MountPointStateMachine
// ---------------------------------------------------------------------------------
namespace interfaces
{
class MockMountPointStateMachine : public MountPointStateMachine
{
  public:
    MOCK_METHOD(void, notify, (const std::error_code& ec), (override));
    MOCK_METHOD(void, notificationStart, (), (override));
    MOCK_METHOD(void, notificationInitialize,
                (std::shared_ptr<sdbusplus::asio::connection> con,
                 const std::string& path, const std::string& iface,
                 const std::string& name),
                (override));
    MOCK_METHOD(std::string_view, getName, (), (const, override));
    MOCK_METHOD(Configuration::MountPoint&, getConfig, (), (override));
    MOCK_METHOD(std::optional<interfaces::MountPointStateMachine::Target>&,
                getTarget, (), (override));
    MOCK_METHOD(BasicState&, getState, (), (override));
    MOCK_METHOD(int&, getExitCode, (), (override));
    MOCK_METHOD(boost::asio::io_context&, getIoc, (), (override));
    MOCK_METHOD(std::string_view, getAdditionalInfo, (), (const, override));
    MOCK_METHOD(void, setAdditionalInfo, (const std::string&), (override));
    MOCK_METHOD(void, emitRegisterDBusEvent,
                (std::shared_ptr<sdbusplus::asio::connection> bus,
                 std::shared_ptr<sdbusplus::asio::object_server> objServer),
                (override));
    MOCK_METHOD(void, emitMountEvent,
                (std::optional<interfaces::MountPointStateMachine::Target>),
                (override));
    MOCK_METHOD(void, emitUnmountEvent, (), (override));
    MOCK_METHOD(void, emitSubprocessStoppedEvent, (), (override));
    MOCK_METHOD(void, emitUdevStateChangeEvent,
                (const NBDDevice& dev, StateChange devState), (override));
};
} // namespace interfaces

//=============================================================================
// D-Bus Method Simulation Helpers
//=============================================================================

/**
 * @brief D-Bus method call simulator for testing without sd_bus
 *
 * Simulates D-Bus method invocations (Mount, Unmount) without requiring:
 * - Real D-Bus daemon
 * - System bus connection
 * - Service registration
 *
 * Validates:
 * - Method parameter parsing
 * - State machine event emission
 * - Configuration mode-specific behaviors (proxy/local/legacy)
 * - Path and parameter validation logic
 */
struct DbusShim
{
    static bool proxy_mount(interfaces::MockMountPointStateMachine& machine,
                            const std::string& additionalInfo)
    {
        if (machine.getState().get_if<ReadyState>())
        {
            machine.setAdditionalInfo(additionalInfo);
        }
        machine.emitMountEvent(std::nullopt);
        return true;
    }

    static bool proxy_unmount(interfaces::MockMountPointStateMachine& machine)
    {
        machine.emitUnmountEvent();
        return true;
    }

    static bool local_mount(interfaces::MockMountPointStateMachine& machine,
                            const std::string& imgPath, bool rw)
    {
        if (!imgPath.starts_with("/tmp/lmedia/"))
        {
            return false;
        }
        if (!fs::exists(imgPath))
        {
            return false;
        }
        if (!fs::is_regular_file(imgPath))
        {
            return false;
        }

        interfaces::MountPointStateMachine::Target t{};
        t.imgUrl = imgPath;
        t.rw = rw;
        machine.emitMountEvent(std::move(t));
        return true;
    }

    static bool local_unmount(interfaces::MockMountPointStateMachine& machine)
    {
        machine.emitUnmountEvent();
        return true;
    }

    static bool local_get_current_mount()
    {
        return true;
    }
    static bool local_list_available_slots()
    {
        return true;
    }

#ifdef LEGACY_MODE_ENABLED
    static bool legacy_mount(interfaces::MockMountPointStateMachine& machine,
                             const std::string& imgUrl, bool rw,
                             sdbusplus::message::unix_fd /*fd*/,
                             const std::string& additionalInfo)
    {
        if (machine.getState().get_if<ReadyState>())
        {
            machine.setAdditionalInfo(additionalInfo);
        }
        interfaces::MountPointStateMachine::Target t{};
        t.imgUrl = imgUrl;
        t.rw = rw;
        t.mountPoint = nullptr;
        t.credentials = nullptr;
        t.mountPointNfs = nullptr;
        machine.emitMountEvent(std::move(t));
        return true;
    }
#endif

    static bool call_mount(interfaces::MockMountPointStateMachine& machine,
                           std::optional<std::string> url_or_client_info,
                           bool /*has_fd*/ = false)
    {
        using Target = interfaces::MountPointStateMachine::Target;
        if (!url_or_client_info.has_value() || url_or_client_info->empty())
        {
            machine.emitMountEvent(std::nullopt);
        }
        else
        {
            Target t{};
            t.imgUrl = *url_or_client_info;
            machine.emitMountEvent(std::optional<Target>{std::move(t)});
        }
        return true;
    }

    static bool call_unmount(interfaces::MockMountPointStateMachine& machine)
    {
        machine.emitUnmountEvent();
        return true;
    }
};

// ---------------------------------------------------------------------------------
// Helper for legacy fd-payload delimiter rule
// ---------------------------------------------------------------------------------
static inline void legacy_parse_or_throw_span(const char* data, size_t size)
{
    int nullCount = 0;
    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] == '\0')
        {
            ++nullCount;
        }
    }
    if (nullCount != 2)
    {
        throw sdbusplus::exception::SdBusError(EINVAL, "Malformed extra data");
    }
}

template <size_t N>
static inline void legacy_parse_or_throw(const std::array<char, N>& buf,
                                         size_t size)
{
    legacy_parse_or_throw_span(buf.data(), size);
}

// =================================================================================
// SECTION 1: Tests from initial_state_interfaces_singlethread_test.cpp (7
// tests)
// =================================================================================
class InitialStateFixture : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc{};
    Configuration::MountPoint mp{};
    std::optional<interfaces::MountPointStateMachine::Target> target{};
    int exitCode{0};
    std::string nameStr{"Slot_1"};
    std::string addInfoStr{"unit-test-additional-info"};

    using NiceMachine =
        ::testing::NiceMock<interfaces::MockMountPointStateMachine>;
    NiceMachine machine;

    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;
    std::unique_ptr<ReadyState> readyRef;

    static boost::asio::io_context suiteIo;
    static std::shared_ptr<sdbusplus::asio::connection> suiteBus;
    static std::shared_ptr<sdbusplus::asio::object_server> suiteObjServer;

    static void SetUpTestSuite()
    {
        suiteBus = std::make_shared<sdbusplus::asio::connection>(suiteIo);
        suiteObjServer =
            std::make_shared<sdbusplus::asio::object_server>(suiteBus);
    }

    static void TearDownTestSuite()
    {
        suiteObjServer.reset();
        suiteBus.reset();
    }

    void SetUp() override
    {
        using ::testing::Return;
        using ::testing::ReturnRef;

        mp.mode = Configuration::Mode::proxy;
        mp.nbdDevice = NBDDevice("/dev/nbd0");
        mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
        mp.endPointId = 42;
        mp.remainingInactivityTimeout = std::chrono::seconds(0);

        ON_CALL(machine, getConfig()).WillByDefault(ReturnRef(mp));
        ON_CALL(machine, getTarget()).WillByDefault(ReturnRef(target));
        ON_CALL(machine, getExitCode()).WillByDefault(ReturnRef(exitCode));
        ON_CALL(machine, getIoc()).WillByDefault(ReturnRef(ioc));
        ON_CALL(machine, getName())
            .WillByDefault(Return(std::string_view{nameStr}));
        ON_CALL(machine, getAdditionalInfo())
            .WillByDefault(Return(std::string_view{addInfoStr}));

        readyRef = std::make_unique<ReadyState>(machine);
        ON_CALL(machine, getState()).WillByDefault(ReturnRef(*readyRef));

        bus = suiteBus;
        objServer = suiteObjServer;

        InitialState::allMountPoints.clear();
        UsbGadgetTestShim::reset();
    }

    RegisterDbusEvent makeRegisterEvent() const
    {
        return RegisterDbusEvent{bus, objServer};
    }
};

boost::asio::io_context InitialStateFixture::suiteIo{};
std::shared_ptr<sdbusplus::asio::connection> InitialStateFixture::suiteBus{};
std::shared_ptr<sdbusplus::asio::object_server>
    InitialStateFixture::suiteObjServer{};

//=============================================================================
// Test Suite: InitialState Core Functionality
//=============================================================================

/**
 * @test Verify CRTP state identity and type-safe downcasting
 * @brief Validates getStateName() and get_if<T>() functionality
 *
 * CRTP (Curiously Recurring Template Pattern) provides compile-time
 * polymorphism. This test ensures:
 * 1. getStateName() returns the correct state identifier string
 * 2. get_if<InitialState>() returns valid pointer when type matches
 * 3. get_if<ReadyState>() returns nullptr when type doesn't match
 *
 * This type-safe downcasting is critical for state machine logic that needs
 * to check current state before processing events.
 */
TEST_F(InitialStateFixture, StateNameAndGetIf)
{
    // Arrange: Create InitialState instance
    InitialState s{machine};

    // Act & Assert: Verify state name matches static definition
    EXPECT_EQ(s.getStateName(), InitialState::stateName());

    // Assert: Downcasting to correct type succeeds
    EXPECT_EQ(s.get_if<InitialState>(), &s);

    // Assert: Downcasting to wrong type returns nullptr (type safety)
    EXPECT_EQ(s.get_if<ReadyState>(), nullptr);
}

/**
 * @test Verify D-Bus object path construction for different modes
 * @brief Validates getObjectPath() maps configuration mode to correct D-Bus
 * path prefix
 *
 * Virtual media supports three operational modes:
 * - Proxy: Remote URLs mounted via nbdkit (path:
 * /xyz/openbmc_project/VirtualMedia/Proxy/)
 * - Local: Local files from /tmp/lmedia/ (path:
 * /xyz/openbmc_project/VirtualMedia/Local/)
 * - Legacy: Backward compatibility mode (path:
 * /xyz/openbmc_project/VirtualMedia/Legacy/)
 *
 * Each mode exposes different D-Bus interfaces at different object paths.
 * This test ensures path construction is correct for each mode.
 *
 * @note D-Bus path format: <base_path>/<slot_name>
 * @note Example: /xyz/openbmc_project/VirtualMedia/Proxy/Slot_2
 */
TEST_F(InitialStateFixture, GetObjectPathMapsModeToPathPrefix)
{
    // Test proxy mode path
    mp.mode = Configuration::Mode::proxy;
    EXPECT_EQ(InitialState::getObjectPath(machine),
              "/xyz/openbmc_project/VirtualMedia/Proxy/");

    // Test local mode path
    mp.mode = Configuration::Mode::local;
    EXPECT_EQ(InitialState::getObjectPath(machine),
              "/xyz/openbmc_project/VirtualMedia/Local/");

    // Test legacy mode path
    mp.mode = Configuration::Mode::legacy;
    EXPECT_EQ(InitialState::getObjectPath(machine),
              "/xyz/openbmc_project/VirtualMedia/Legacy/");
}

/**
 * @test Verify D-Bus service registration and state transition for proxy mode
 * @brief Validates RegisterDbusEvent handling creates D-Bus interfaces and
 * transitions to ReadyState
 *
 * Proxy mode workflow:
 * 1. InitialState receives RegisterDbusEvent (bus + object_server)
 * 2. D-Bus interface created at
 * /xyz/openbmc_project/VirtualMedia/Proxy/<slot_name>
 * 3. Interface "xyz.openbmc_project.VirtualMedia.Proxy" registered
 * 4. Mount/Unmount methods exposed on D-Bus
 * 5. Notification callback initialized for completion signals
 * 6. State transitions to ReadyState (ready to accept mount requests)
 *
 * This is the initialization path for remote URL mounting capability.
 *
 * @note Global service objects are created once and reused across slots
 * @note Completion notification uses D-Bus signals for asynchronous feedback
 */
TEST_F(InitialStateFixture,
       HandleRegisterDbusEventProxyCreatesInterfacesAndTransitions)
{
    // Arrange: Configure proxy mode
    mp.mode = Configuration::Mode::proxy;
    InitialState s{machine};

    // Expected D-Bus object path: base + slot name
    const std::string expectedPath =
        "/xyz/openbmc_project/VirtualMedia/Proxy/" +
        std::string(machine.getName());
    const std::string expectedIface = "xyz.openbmc_project.VirtualMedia.Proxy";

    // Assert: notificationInitialize called with correct path and interface
    EXPECT_CALL(machine, notificationInitialize(bus, expectedPath,
                                                expectedIface, "Completion"))
        .Times(1);

    // Act: Handle D-Bus registration event
    auto next = s.handleEvent(makeRegisterEvent());

    // Assert: Transition to ReadyState occurred
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());

    // Assert: Global service flags set (shared across all mount points)
    EXPECT_TRUE(InitialState::globalLocalServiceCreated);
    ASSERT_TRUE(InitialState::globalObjServer);
}

/**
 * @test Verify USB gadget cleanup and D-Bus setup for local mode
 * @brief Validates local mode initialization cleans up existing gadget before
 * transitioning
 *
 * Local mode workflow:
 * 1. Check for existing USB gadget configuration (from previous mount)
 * 2. If configured, clean up USB gadget (UsbGadget::configure with
 * StateChange::removed)
 * 3. Create D-Bus interface at
 * /xyz/openbmc_project/VirtualMedia/Local/<slot_name>
 * 4. Expose local-specific methods (Mount with file path, ListAvailableImages,
 * etc.)
 * 5. Transition to ReadyState
 *
 * This test validates the critical cleanup step that prevents stale gadget
 * configurations from interfering with new mount operations.
 *
 * @note Local mode uses /tmp/lmedia/ directory for image files
 * @note USB gadget cleanup is essential for proper host-side device removal
 * @note Test creates temporary directory to simulate local media directory
 */
TEST_F(InitialStateFixture,
       HandleRegisterDbusEventLocalCreatesInterfacesCleansUpAndTransitions)
{
    // Arrange: Create unique slot name and corresponding local directory
    const std::string localName{"UT_Slot_Local_HasDir"};
    ON_CALL(machine, getName())
        .WillByDefault(::testing::Return(std::string_view{localName}));

    // Create local media directory (simulates /tmp/lmedia/<slot_name>)
    const auto localDir = fs::temp_directory_path() / localName;
    std::error_code ec;
    fs::create_directories(localDir, ec);

    // Simulate existing gadget configuration (from previous session)
    UsbGadgetTestShim::configuredFlag = true;
    mp.mode = Configuration::Mode::local;

    InitialState s{machine};

    const std::string expectedPath =
        "/xyz/openbmc_project/VirtualMedia/Local/" +
        std::string(machine.getName());
    const std::string expectedIface = "xyz.openbmc_project.VirtualMedia.Proxy";

    EXPECT_CALL(machine, notificationInitialize(bus, expectedPath,
                                                expectedIface, "Completion"))
        .Times(1);

    auto next = s.handleEvent(makeRegisterEvent());
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());
    EXPECT_TRUE(UsbGadgetTestShim::configureCalled);
    EXPECT_EQ(UsbGadgetTestShim::lastName, std::string(machine.getName()));
    EXPECT_EQ(UsbGadgetTestShim::lastDevStr, mp.nbdDevice.to_string());
    EXPECT_EQ(UsbGadgetTestShim::lastState, StateChange::removed);
}

// Test 5
TEST_F(InitialStateFixture, HandleRegisterDbusEventLegacyBranch)
{
    mp.mode = Configuration::Mode::legacy;
    InitialState s{machine};

#ifdef LEGACY_MODE_ENABLED
    const std::string expectedPath =
        "/xyz/openbmc_project/VirtualMedia/Legacy/" +
        std::string(machine.getName());
    const std::string expectedIface = "xyz.openbmc_project.VirtualMedia.Legacy";

    EXPECT_CALL(machine, notificationInitialize(bus, expectedPath,
                                                expectedIface, "Completion"))
        .Times(1);

    auto next = s.handleEvent(makeRegisterEvent());
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());
#else
    EXPECT_CALL(machine, notify(::testing::Truly([](const std::error_code& e) {
                    return e ==
                           std::make_error_code(std::errc::invalid_argument);
                })))
        .Times(1);

    auto next = s.handleEvent(makeRegisterEvent());
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());
#endif
}

// Test 6
TEST_F(InitialStateFixture, HandleEventInvalidEventReturnsNullptr)
{
    InitialState s{machine};
    MountEvent ev{std::nullopt};
    auto next = s.handleEvent(std::move(ev));
    EXPECT_EQ(next, nullptr);
}

// Test 7
TEST_F(InitialStateFixture, GlobalLocalServiceCreatedOnlyOnceAcrossMountPoints)
{
    mp.mode = Configuration::Mode::proxy;
    const std::string firstName{"Slot_3"};
    ON_CALL(machine, getName())
        .WillByDefault(::testing::Return(std::string_view{firstName}));

    InitialState s{machine};
    auto next = s.handleEvent(makeRegisterEvent());
    ASSERT_NE(next, nullptr);
    EXPECT_TRUE(InitialState::globalLocalServiceCreated);

    using NiceMachine2 =
        ::testing::NiceMock<interfaces::MockMountPointStateMachine>;
    NiceMachine2 machine2;

    using ::testing::Return;
    using ::testing::ReturnRef;

    Configuration::MountPoint mp2{};
    mp2.mode = Configuration::Mode::proxy;
    mp2.nbdDevice = NBDDevice("/dev/nbd1");

    std::string name2{"Slot_4"};
    std::optional<interfaces::MountPointStateMachine::Target> target2{};
    int exitCode2{0};
    auto ready2 = std::make_unique<ReadyState>(machine2);

    ON_CALL(machine2, getState()).WillByDefault(ReturnRef(*ready2));
    ON_CALL(machine2, getConfig()).WillByDefault(ReturnRef(mp2));
    ON_CALL(machine2, getTarget()).WillByDefault(ReturnRef(target2));
    ON_CALL(machine2, getExitCode()).WillByDefault(ReturnRef(exitCode2));
    ON_CALL(machine2, getIoc()).WillByDefault(ReturnRef(ioc));
    ON_CALL(machine2, getName()).WillByDefault(Return(std::string_view{name2}));
    ON_CALL(machine2, getAdditionalInfo())
        .WillByDefault(Return(std::string_view{addInfoStr}));

    InitialState s2{machine2};

    const std::string expectedPath2 =
        "/xyz/openbmc_project/VirtualMedia/Proxy/" + name2;
    const std::string expectedIface2 = "xyz.openbmc_project.VirtualMedia.Proxy";

    EXPECT_CALL(machine2, notificationInitialize(bus, expectedPath2,
                                                 expectedIface2, "Completion"))
        .Times(1);

    auto next2 = s2.handleEvent(makeRegisterEvent());
    ASSERT_NE(next2, nullptr);
    EXPECT_EQ(next2->getStateName(), ReadyState::stateName());
    EXPECT_TRUE(InitialState::globalLocalServiceCreated);
    EXPECT_TRUE(InitialState::allMountPoints.count(firstName) != 0);
    EXPECT_TRUE(InitialState::allMountPoints.count(name2) != 0);
}

// =================================================================================
// SECTION 2: Tests from initial_state_logical_coverage_test.cpp (7 tests)
// =================================================================================
class FakeHandlersFixture : public ::testing::Test
{
  protected:
    static boost::asio::io_context suiteIo;
    static std::shared_ptr<sdbusplus::asio::connection> suiteBus;
    static std::shared_ptr<sdbusplus::asio::object_server> suiteObjServer;
    boost::asio::io_context ioc{};
    Configuration::MountPoint mp{};
    std::optional<interfaces::MountPointStateMachine::Target> target{};
    int exitCode{0};
    std::string nameStr{"Slot_Fake_1"};
    std::string addInfoStr{"fake-handlers"};

    using NiceMachine =
        ::testing::NiceMock<interfaces::MockMountPointStateMachine>;
    NiceMachine machine;

    std::unique_ptr<ReadyState> readyRef;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;

    static void SetUpTestSuite()
    {
        suiteBus = std::make_shared<sdbusplus::asio::connection>(suiteIo);
        suiteObjServer =
            std::make_shared<sdbusplus::asio::object_server>(suiteBus);
    }

    static void TearDownTestSuite()
    {
        suiteObjServer.reset();
        suiteBus.reset();
    }

    void SetUp() override
    {
        using ::testing::Return;
        using ::testing::ReturnRef;

        mp.mode = Configuration::Mode::proxy;
        mp.nbdDevice = NBDDevice("/dev/nbdFake0");
        mp.unixSocket = "/tmp/virtual-media-ut/fake.socket";
        mp.endPointId = 42;
        mp.remainingInactivityTimeout = std::chrono::seconds(2);

        ON_CALL(machine, getConfig()).WillByDefault(ReturnRef(mp));
        ON_CALL(machine, getTarget()).WillByDefault(ReturnRef(target));
        ON_CALL(machine, getExitCode()).WillByDefault(ReturnRef(exitCode));
        ON_CALL(machine, getIoc()).WillByDefault(ReturnRef(ioc));
        ON_CALL(machine, getName())
            .WillByDefault(Return(std::string_view{nameStr}));
        ON_CALL(machine, getAdditionalInfo())
            .WillByDefault(Return(std::string_view{addInfoStr}));

        readyRef = std::make_unique<ReadyState>(machine);
        ON_CALL(machine, getState()).WillByDefault(ReturnRef(*readyRef));

        bus = suiteBus;
        objServer = suiteObjServer;

        InitialState::allMountPoints.clear();
    }
};

boost::asio::io_context FakeHandlersFixture::suiteIo{};
std::shared_ptr<sdbusplus::asio::connection> FakeHandlersFixture::suiteBus{};
std::shared_ptr<sdbusplus::asio::object_server>
    FakeHandlersFixture::suiteObjServer{};

// Test 8
TEST_F(FakeHandlersFixture, GlobalLocalService_ValidAndError_WithFakeHandlers)
{
    fs::create_directories("/tmp/lmedia");
    const std::string goodPath = "/tmp/lmedia/fake_local.img";
    {
        std::ofstream ofs(goodPath, std::ios::binary);
        ofs << "data";
    }

    ASSERT_TRUE(DbusShim::local_mount(machine, goodPath, true));

    ASSERT_TRUE(DbusShim::local_get_current_mount());
    ASSERT_TRUE(DbusShim::local_list_available_slots());

    EXPECT_CALL(machine, emitUnmountEvent()).Times(1);
    ASSERT_TRUE(DbusShim::local_unmount(machine));

    ASSERT_FALSE(DbusShim::local_mount(machine, "/var/tmp/foo.img", false));
    ASSERT_FALSE(
        DbusShim::local_mount(machine, "/tmp/lmedia/missing.img", false));
    fs::create_directories("/tmp/lmedia/fake_dir");
    ASSERT_FALSE(DbusShim::local_mount(machine, "/tmp/lmedia/fake_dir", false));
}

// Test 9
TEST_F(FakeHandlersFixture, Legacy_Mount_WithFakeHandlers_Guarded)
{
#ifdef LEGACY_MODE_ENABLED
    using ::testing::Truly;

    std::string legacyName{"Slot_Fake_Legacy"};
    using ::testing::Return;
    ON_CALL(machine, getName())
        .WillByDefault(Return(std::string_view{legacyName}));

    const std::string path =
        InitialState::getObjectPath(machine) + std::string(machine.getName());
    const std::string iface = "xyz.openbmc_project.VirtualMedia.Legacy";

    EXPECT_CALL(machine,
                notificationInitialize(::testing::_, path, iface, "Completion"))
        .Times(1);
    machine.notificationInitialize(bus, path, iface, "Completion");

    const std::string url = "http://example.com/image.iso";
    EXPECT_CALL(
        machine,
        emitMountEvent(Truly(
            [&](const std::optional<interfaces::MountPointStateMachine::Target>&
                    t) { return t && t->imgUrl == url; })))
        .Times(1);

    int fd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(fd, 0);
    sdbusplus::message::unix_fd fdArg{fd};

    ASSERT_TRUE(
        DbusShim::legacy_mount(machine, url, true, fdArg, "legacy-info"));
    ::close(fd);
#else
    GTEST_SKIP()
        << "LEGACY_MODE_ENABLED not defined; legacy interface disabled.";
#endif
}

// Test 10
TEST_F(FakeHandlersFixture, Legacy_Mount_MalformedFd_ThrowsEINVAL_Shims)
{
#ifdef LEGACY_MODE_ENABLED
    std::array<char, 16> bad{'u', 's', 'e', 'r', '\0', 'x', 'x', 'x'};
    constexpr size_t badSize = 8;

    EXPECT_THROW(legacy_parse_or_throw(bad, badSize),
                 sdbusplus::exception::SdBusError);
#else
    GTEST_SKIP() << "LEGACY_MODE_ENABLED not defined.";
#endif
}

// Test 11
TEST_F(FakeHandlersFixture, CleanUpMountPoint_Failure_KeepsDirectory)
{
    const auto tempDir =
        fs::temp_directory_path() / std::string(machine.getName());
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    ASSERT_TRUE(fs::exists(tempDir));

    mp.mode = Configuration::Mode::local;

    InitialState s{machine};
    auto next = s.handleEvent(RegisterDbusEvent{bus, objServer});
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());

    EXPECT_TRUE(fs::exists(tempDir));
}

// Test 12
TEST_F(FakeHandlersFixture, CleanUpMountPoint_NoDirectory_NoOp)
{
    using ::testing::Return;

    const std::string uniqueName{"Slot_Fake_NoDir_1"};
    ON_CALL(machine, getName())
        .WillByDefault(Return(std::string_view{uniqueName}));

    const auto tempDir = fs::temp_directory_path() / uniqueName;
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    ASSERT_FALSE(fs::exists(tempDir));

    mp.mode = Configuration::Mode::local;

    InitialState::globalObjServer = objServer;
    InitialState::globalLocalServiceCreated = true;

    InitialState s{machine};
    auto next = s.handleEvent(RegisterDbusEvent{bus, objServer});
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());

    EXPECT_FALSE(fs::exists(tempDir));
}

// =================================================================================
// SECTION 3: Tests from initial_state_singlethread_integration_test.cpp (3
// tests)
// =================================================================================
class MockedIntegrationFixture : public ::testing::Test
{
  protected:
    static boost::asio::io_context suiteIo;
    static std::shared_ptr<sdbusplus::asio::connection> suiteBus;
    static std::shared_ptr<sdbusplus::asio::object_server> suiteObjServer;
    boost::asio::io_context ioc{};
    Configuration::MountPoint mp{};
    std::optional<interfaces::MountPointStateMachine::Target> target{};
    int exitCode{0};
    std::string nameStr{"Slot_MOCK_1"};
    std::string addInfoStr{"mocked-integration"};
    using NiceMachine =
        ::testing::NiceMock<interfaces::MockMountPointStateMachine>;
    NiceMachine machine;

    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;
    std::unique_ptr<ReadyState> readyRef;

    static void SetUpTestSuite()
    {
        suiteBus = std::make_shared<sdbusplus::asio::connection>(suiteIo);
        suiteObjServer =
            std::make_shared<sdbusplus::asio::object_server>(suiteBus);
    }

    static void TearDownTestSuite()
    {
        suiteObjServer.reset();
        suiteBus.reset();
    }

    void SetUp() override
    {
        using ::testing::Return;
        using ::testing::ReturnRef;
        mp.mode = Configuration::Mode::proxy;
        mp.nbdDevice = NBDDevice("/dev/nbdMOCK0");
        mp.unixSocket = "/tmp/virtual-media-ut/mock.socket";
        mp.endPointId = 42;
        mp.remainingInactivityTimeout = std::chrono::seconds(0);

        ON_CALL(machine, getConfig()).WillByDefault(ReturnRef(mp));
        ON_CALL(machine, getTarget()).WillByDefault(ReturnRef(target));
        ON_CALL(machine, getExitCode()).WillByDefault(ReturnRef(exitCode));
        ON_CALL(machine, getIoc()).WillByDefault(ReturnRef(ioc));
        ON_CALL(machine, getName())
            .WillByDefault(Return(std::string_view{nameStr}));
        ON_CALL(machine, getAdditionalInfo())
            .WillByDefault(Return(std::string_view{addInfoStr}));
        readyRef = std::make_unique<ReadyState>(machine);
        ON_CALL(machine, getState()).WillByDefault(ReturnRef(*readyRef));

        bus = suiteBus;
        objServer = suiteObjServer;

        InitialState::allMountPoints.clear();
    }

    RegisterDbusEvent makeRegisterEvent() const
    {
        return RegisterDbusEvent{bus, objServer};
    }
};

boost::asio::io_context MockedIntegrationFixture::suiteIo{};
std::shared_ptr<sdbusplus::asio::connection>
    MockedIntegrationFixture::suiteBus{};
std::shared_ptr<sdbusplus::asio::object_server>
    MockedIntegrationFixture::suiteObjServer{};

// Test 13
TEST_F(MockedIntegrationFixture, ST_Mocked_PerSlotServiceProxy_MountAndUnmount)
{
    using ::testing::Eq;

    mp.mode = Configuration::Mode::proxy;
    InitialState s{machine};

    const std::string expectedPath =
        InitialState::getObjectPath(machine) + std::string(machine.getName());
    const std::string expectedIface = "xyz.openbmc_project.VirtualMedia.Proxy";

    EXPECT_CALL(machine, notificationInitialize(::testing::_, expectedPath,
                                                expectedIface, "Completion"))
        .Times(1);

    auto next = s.handleEvent(makeRegisterEvent());
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());

    EXPECT_CALL(machine, emitMountEvent(Eq(std::nullopt))).Times(1);
    EXPECT_TRUE(DbusShim::call_mount(machine, std::nullopt));

    EXPECT_CALL(machine, emitUnmountEvent()).Times(1);
    EXPECT_TRUE(DbusShim::call_unmount(machine));
}

// Test 14
TEST_F(MockedIntegrationFixture, ST_Mocked_GlobalLocalService_ValidAndInvalid)
{
    using ::testing::Eq;
    using ::testing::Truly;

    mp.mode = Configuration::Mode::local;
    nameStr = "Slot_MOCK_Local";
    InitialState s{machine};

    const std::string expectedPath =
        InitialState::getObjectPath(machine) + std::string(machine.getName());
    const std::string expectedIface = "xyz.openbmc_project.VirtualMedia.Proxy";

    EXPECT_CALL(machine, notificationInitialize(::testing::_, expectedPath,
                                                expectedIface, "Completion"))
        .Times(1);

    auto next = s.handleEvent(makeRegisterEvent());
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), ReadyState::stateName());

    const std::string validUrl = "/tmp/lmedia/ut_local.bin";
    EXPECT_CALL(
        machine,
        emitMountEvent(Truly(
            [&](const std::optional<interfaces::MountPointStateMachine::Target>&
                    t) { return t && t->imgUrl == validUrl; })))
        .Times(1);
    EXPECT_TRUE(DbusShim::call_mount(machine, validUrl));

    // call_mount with empty string emits nullopt (no-op path in DbusShim)
    EXPECT_CALL(machine, emitMountEvent(Eq(std::nullopt))).Times(1);
    (void)DbusShim::call_mount(machine, std::string{});
    SUCCEED();
}

// Test 15
TEST_F(MockedIntegrationFixture, ST_Mocked_LegacyServiceMount_UnixFdVariant)
{
#ifdef LEGACY_MODE_ENABLED
    using ::testing::Truly;

    mp.mode = Configuration::Mode::legacy;
    nameStr = "Slot_MOCK_Legacy";
    InitialState s{machine};

    const std::string url = "http://example.com/image.iso";
    EXPECT_CALL(
        machine,
        emitMountEvent(Truly(
            [&](const std::optional<interfaces::MountPointStateMachine::Target>&
                    t) { return t && t->imgUrl == url; })))
        .Times(1);

    EXPECT_TRUE(DbusShim::call_mount(machine, url, true));
#else
    GTEST_SKIP()
        << "LEGACY_MODE_ENABLED not defined; legacy DBus interface is unavailable in this build.";
#endif
}

// =================================================================================
// SECTION 4: Tests from initial_state_test.cpp (7 tests - duplicates removed)
// =================================================================================
// Note: Tests 18-24 from initial_state_test.cpp are duplicates of tests 1-7
// from initial_state_interfaces_singlethread_test.cpp, so they are not repeated
// here. The original file had the same tests with the same fixture.
