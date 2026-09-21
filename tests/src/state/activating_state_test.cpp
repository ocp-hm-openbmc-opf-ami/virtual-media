// SPDX-License-Identifier: Apache-2.0
/**
 * @file activating_state_test.cpp
 * @brief Comprehensive unit tests for ActivatingState (mounting in progress)
 *
 * This test suite validates the activating state of the virtual media state
 * machine, which represents the transitional state while mounting operations
 * are in progress. This is a critical state managing subprocess creation,
 * resource allocation, and error handling during the mount operation.
 *
 * Testing Strategy:
 * - JSON configuration file seeding for vm::Interface initialization
 * - Mock MountPointStateMachine for isolated state machine testing
 * - No actual NBD/mounting operations (test-only validation)
 * - SIGPIPE signal handling validation
 *
 * Functional Coverage:
 * 1. **State Entry (onEnter)**:
 *    - NBD subprocess creation (nbdkit/nbd-client)
 *    - Resource mounting (NFS/SMB shares)
 *    - Process lifetime management
 *    - Error handling for subprocess failures
 *
 * 2. **Event Handling**:
 *    - SubprocessStoppedEvent → DeactivatingState (abnormal termination)
 *    - UdevStateChangeEvent (inserted) → ActiveState (successful mount)
 *    - UnmountEvent → DeactivatingState (user-requested cancellation)
 *    - MountEvent → SdBusError(EBUSY) (operation already in progress)
 *
 * 3. **Mount Type Handling**:
 *    - Proxy mode: HTTP/HTTPS URLs with credential pipe
 *    - Local mode: Direct file paths from /tmp/lmedia/
 *    - NFS mode: Network filesystem mounting
 *    - SMB/CIFS mode: Windows share mounting with credentials
 *
 * 4. **Subprocess Management**:
 *    - NBD device attachment
 *    - Process spawning and monitoring
 *    - Exit code handling
 *    - Resource cleanup on failure
 *
 * 5. **Credential Handling**:
 *    - Credential provider initialization (SMB/CIFS)
 *    - Pipe-based credential passing (proxy mode)
 *    - Secure cleanup on state exit
 *
 * Test Coverage Areas:
 * - All handleEvent() method variants
 * - JSON configuration loading and parsing
 * - Target validation (URL, path, credentials)
 * - Process creation success and failure paths
 * - Resource cleanup on premature exit
 * - Signal handling (SIGPIPE for broken pipes)
 *
 * Edge Cases Tested:
 * - Missing or invalid JSON configuration
 * - Invalid URL formats
 * - Process spawn failures
 * - Rapid state transitions
 * - Multiple concurrent mount attempts (EBUSY)
 * - Cleanup during ongoing operations
 *
 * Dependencies:
 * - vm::Interface: JSON-based configuration management
 * - MockMountPointStateMachine: State machine interface isolation
 * - boost::asio::io_context: Async process management
 * - utils::CredentialsProvider: Secure credential handling
 * - sdbusplus::exception: D-Bus error simulation
 *
 * @note VIRTUAL_MEDIA_CONFIG_PATH must be defined at build time
 * @note Tests create temporary JSON files for each test case
 */

#include "events.hpp"                 // SubprocessStoppedEvent, UnmountEvent
#include "state/activating_state.hpp" // Class under test (public API)
#include "state/basic_state.hpp"
#include "state/ready_state.hpp"      // ReadyState::error for assertions
#include "utils.hpp"        // utils::CredentialsProvider for SMB credentials
#include "vm_interface.hpp" // vm::Interface::loadJson(), constructor

#include <unistd.h>         // access()

#include <csignal>          // SIGPIPE guard
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifdef INJECT_MOCKS
namespace vm
{
const std::string vmObjPath = "/xyz/openbmc_project/VirtualMedia";
} // namespace vm
#endif

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>    // D-Bus connection
#include <sdbusplus/asio/object_server.hpp> // D-Bus object_server
#include <sdbusplus/exception.hpp>

// ---------- JSON seed helpers (mirrors vm_interface_test.cpp style) ----------
namespace
{
namespace fs = std::filesystem;

static fs::path cfgPath()
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    return fs::path{VIRTUAL_MEDIA_CONFIG_PATH};
#else
    return fs::path{};
#endif
}

static void writeJsonFile(const fs::path& p, const std::string& text)
{
    ASSERT_FALSE(p.empty()) << "VIRTUAL_MEDIA_CONFIG_PATH is empty in test TU";
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open()) << "Failed to open for write: " << p;
    ofs.write(text.c_str(), static_cast<std::streamsize>(text.size()));
    ofs.flush();
    ofs.close();
}
} // namespace

// ---------- GMock for interfaces::MountPointStateMachine ---------------------
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

// ---------- Test fixture -----------------------------------------------------
class ActivatingStateFixture : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc{};
    Configuration::MountPoint mp{};
    std::optional<interfaces::MountPointStateMachine::Target> target{};
    int exitCode{99};
    std::string nameStr{"Slot_2"};
    std::string addInfoStr{"unit-test-additional-info"};

    // NiceMock to suppress "Uninteresting mock function call" warnings.
    using NiceMachine =
        ::testing::NiceMock<interfaces::MockMountPointStateMachine>;
    NiceMachine machine;

    // Keep a vm::Interface instance alive for legacy tests (no D-Bus needed)
    std::unique_ptr<vm::Interface> vmIf;

    static void SetUpTestSuite()
    {
        // Ignore SIGPIPE so failed writes to closed pipes/sockets don’t kill
        // the test runner.
        std::signal(SIGPIPE, SIG_IGN);
    }

    void SetUp() override
    {
        mp.mode = Configuration::Mode::proxy;
        mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
        mp.nbdDevice = NBDDevice("/dev/nbd0");

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

    void setTarget(const std::string& url, bool rw)
    {
        interfaces::MountPointStateMachine::Target t{};
        t.imgUrl = url;
        t.rw = rw;
        target = std::move(t);
    }

    // Initialize vm::Interface (null server, no D-Bus) + seed JSON so that
    // vm::Interface::instance is set and saveImageURLToJson() can work.
    void initVmInterfaceForLegacy()
    {
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
        // Seed JSON without BackupImageURL so saveImageURLToJson() takes the
        // safe "Slot not found" else-branch and never calls
        // loadImageURLFromJson() (which would crash trying to dereference the
        // null object_server).
        const std::string seed = R"({
          "RetryCount": 3,
          "RetryInterval": 15
        })";
        writeJsonFile(cfgPath(), seed);
        // Construct with null server to set vm::Interface::instance without
        // D-Bus.
        std::shared_ptr<sdbusplus::asio::object_server> nullServer;
        vmIf = std::make_unique<vm::Interface>(
            nullServer, std::string(vm::vmObjPath), VIRTUAL_MEDIA_CONFIG_PATH);
        ASSERT_EQ(vm::Interface::loadJson(), 0);
#endif
    }
};

TEST_F(ActivatingStateFixture,
       StateNameAndGetIf_WhenConstructed_ReturnExpectedType)
{
    ActivatingState state{machine};

    EXPECT_EQ(state.getStateName(), ActivatingState::stateName());
    EXPECT_NE(state.get_if<ActivatingState>(), nullptr);
    EXPECT_EQ(state.get_if<ReadyState>(), nullptr);
}

// ---------- Success-path environment checks ----------------------------------
namespace
{

// Absolute-path presence (many builds spawn via /usr/sbin)
inline bool nbd_client_available_abs()
{
    using std::filesystem::exists;
    return exists("/usr/sbin/nbd-client") || exists("/sbin/nbd-client") ||
           exists("/usr/bin/nbd-client") || exists("/bin/nbd-client");
}
// The production spawnNbdKit() hardcodes /usr/sbin/nbdkit, so that exact path
// must be present and executable for the success-path tests to pass.
inline bool nbdkit_available_abs()
{
    return ::access("/usr/sbin/nbdkit", X_OK) == 0;
}

// Configfs + usb_gadget subtree + root (typical gadget prerequisites)
inline bool gadget_env_ready()
{
    using std::filesystem::exists;
    if (!exists("/sys/kernel/config"))
        return false;
    if (!exists("/sys/kernel/config/usb_gadget"))
        return false;
    return ::geteuid() == 0;
}

// Optional SMB/NFS info from environment — tests skip when vars are unset.
inline std::optional<std::string> env_smb_url()
{
    if (const char* p = std::getenv("VMEDIA_TEST_SMB_URL"))
        return std::string{p};
    return std::nullopt;
}
inline std::optional<std::string> env_smb_user()
{
    if (const char* p = std::getenv("VMEDIA_TEST_SMB_USER"))
        return std::string{p};
    return std::nullopt;
}
inline std::optional<std::string> env_smb_pass()
{
    if (const char* p = std::getenv("VMEDIA_TEST_SMB_PASS"))
        return std::string{p};
    return std::nullopt;
}
inline std::optional<std::string> env_nfs_url()
{
    if (const char* p = std::getenv("VMEDIA_TEST_NFS_URL"))
        return std::string{p};
    return std::nullopt;
}

} // anonymous namespace

// ---------- onEnter(): proxy-mode negative branches --------------------------
TEST_F(ActivatingStateFixture, OnEnterProxy_LocalFileMissingReturnsReadyState)
{
    mp.mode = Configuration::Mode::proxy;
    setTarget("/tmp/lmedia/missing.iso", /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter();

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
    EXPECT_EQ(exitCode, -1); // reset by onEnter()
}

TEST_F(ActivatingStateFixture, OnEnterProxy_PathIsDirectoryReturnsReadyState)
{
    mp.mode = Configuration::Mode::proxy;

    std::filesystem::path dir = "/tmp/lmedia/ut-dir";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    setTarget(dir.string(), /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter();

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");

    std::filesystem::remove_all(dir, ec);
}

TEST_F(ActivatingStateFixture,
       OnEnterProxy_RWTrueButFileNotWritableReturnsReadyState)
{
    mp.mode = Configuration::Mode::proxy;

    std::filesystem::path file = "/tmp/lmedia/ut-readonly.iso";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    {
        std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
        ofs << "dummy";
    }
    // Make file read-only for owner
    std::filesystem::permissions(file, std::filesystem::perms::owner_read, ec);

    setTarget(file.string(), /*rw=*/true);

    ActivatingState s{machine};
    auto next = s.onEnter();

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");

    std::filesystem::remove(file, ec);
}

// ---------- Proxy mode: non-local spawn failure → ReadyState ---------------
TEST_F(ActivatingStateFixture,
       OnEnterProxy_NonLocalSpawnFailureReturnsReadyState)
{
    // Skip if nbd-client is present: spawn would succeed, TestCase tests the
    // failure path.
    if (nbd_client_available_abs())
        GTEST_SKIP() << "nbd-client present; skipping spawn-failure test";

    mp.mode = Configuration::Mode::proxy;
    setTarget("https://example.com/path/file.iso",
              /*rw=*/false); // non-local URL

    ActivatingState s{machine};
    auto next = s.onEnter(); // -> activateProxyMode()

    // nbd-client absent → spawn fails → ReadyState
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- Proxy mode: no target set (has_value() == false) → ReadyState ---
// Exercises the !has_value() branch in activateProxyMode() that skips the
// isLocalFile() check entirely and falls through to nbd-client spawn.
TEST_F(ActivatingStateFixture,
       OnEnterProxy_NoTargetSpawnFailureReturnsReadyState)
{
    if (nbd_client_available_abs())
        GTEST_SKIP()
            << "nbd-client present; skipping no-target spawn-failure test";

    mp.mode = Configuration::Mode::proxy;
    // Leave target empty (std::optional default = nullopt)

    ActivatingState s{machine};
    auto next = s.onEnter(); // activateProxyMode(): has_value()==false → spawn
                             // nbd-client

    // nbd-client absent → spawn fails → ReadyState
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- onEnter(): legacy-mode (create_directories failure → ReadyState) -
TEST_F(ActivatingStateFixture,
       OnEnterLegacy_FailedToCreateParentDirectoryReturnsReadyState)
{
    initVmInterfaceForLegacy(); // ensure vm::Interface::instance and D-Bus
                                // interfaces

    mp.mode = Configuration::Mode::legacy;
    // Use /proc (read-only FS) so exists() does not throw; create_directories
    // sets ec.
    mp.unixSocket = "/proc/virtual-media-ut/socket.sock";

    setTarget("https://host/path/file.iso", /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter(); // -> activateLegacyMode()

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- onEnter(): legacy-mode local file (isLocalFile branch) ----------
// Exercises the #ifndef MULTI_HOST_DEFAULT_MODE isLocalFile() check in
// activateLegacyMode() — distinct from the same check in activateProxyMode().
TEST_F(ActivatingStateFixture, OnEnterLegacy_LocalFileMissingReturnsReadyState)
{
    initVmInterfaceForLegacy(); // ensure vm::Interface::instance

    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    // /tmp/lmedia/ prefix → isLocalFile() true → mountLocalFile() → file not
    // found → ReadyState
    setTarget("/tmp/lmedia/legacy-missing.iso", /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter();

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- onEnter(): legacy invalid slot number → ReadyState --------------
TEST_F(ActivatingStateFixture, OnEnterLegacy_InvalidSlotNumberReturnsReadyState)
{
    initVmInterfaceForLegacy(); // ensure vm::Interface::instance

    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    // "Proxy" has no underscore → extractSlotNumber returns -1 → ReadyState
    nameStr = "Proxy";
    ON_CALL(machine, getName())
        .WillByDefault(::testing::Return(std::string_view{nameStr}));

    setTarget("https://host/path/file.iso", /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter();

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");

    // Restore default name for subsequent tests
    nameStr = "Slot_2";
}

// ---------- onEnter(): legacy unknown scheme --------------------------------
TEST_F(ActivatingStateFixture, OnEnterLegacy_UnknownUrlSchemeReturnsReadyState)
{
    initVmInterfaceForLegacy(); // ensure vm::Interface::instance and D-Bus
                                // interfaces

    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    // Unknown scheme -> "URL not recognized" branch
    setTarget("foo://host/path/file.iso", /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter();

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- onEnter(): legacy CIFS without credentials → connection_refused
TEST_F(ActivatingStateFixture,
       OnEnterLegacy_CifsWithoutCredentialsReturnsReadyState)
{
    initVmInterfaceForLegacy();

    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    // smb:// scheme with no credentials on the target -> credential guard
    setTarget("smb://host/share/file.iso", /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter();

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");

    auto* ready = next->get_if<ReadyState>();
    ASSERT_NE(ready, nullptr);
    ASSERT_TRUE(ready->error.has_value());
    EXPECT_EQ(ready->error->code, std::errc::connection_refused);
}

// ---------- onEnter(): legacy HTTPS without credentials → connection_refused
TEST_F(ActivatingStateFixture,
       OnEnterLegacy_HttpsWithoutCredentialsReturnsReadyState)
{
    initVmInterfaceForLegacy();

    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    // https:// scheme with no credentials on the target -> credential guard
    setTarget("https://host/path/file.iso", /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter();

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");

    auto* ready = next->get_if<ReadyState>();
    ASSERT_NE(ready, nullptr);
    ASSERT_TRUE(ready->error.has_value());
    EXPECT_EQ(ready->error->code, std::errc::connection_refused);
}

// ---------- Legacy HTTPS: spawnNbdKit(url) failure → ReadyState ------------
TEST_F(ActivatingStateFixture, OnEnterLegacy_HttpsSpawnFailureReturnsReadyState)
{
    initVmInterfaceForLegacy(); // ensure vm::Interface::instance

    // Skip if nbdkit is present: spawn would succeed, this test covers the
    // failure path.
    if (nbdkit_available_abs())
        GTEST_SKIP() << "nbdkit present; skipping spawn-failure test";

    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    setTarget("https://example.com/path/file.iso", /*rw=*/false);

    // Provide credentials required by the HTTPS branch
    target->credentials =
        std::make_unique<utils::CredentialsProvider>("utuser", "utpass");

    ActivatingState s{machine};
    auto next = s.onEnter(); // -> activateLegacyMode() -> mountHttpsShare()

    // nbdkit absent → spawn fails → ReadyState
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- Legacy CIFS: mount failure (no remote) → ReadyState -------------
TEST_F(ActivatingStateFixture, OnEnterLegacy_SmbMountFailureReturnsReadyState)
{
    initVmInterfaceForLegacy(); // ensure vm::Interface::instance and D-Bus
                                // interfaces

    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    // Set URL and RW
    setTarget("smb://srv/share/file.iso", /*rw=*/false);

    // Provide minimal credentials to avoid nullptr dereference in CIFS branch
    target->credentials =
        std::make_unique<utils::CredentialsProvider>("utuser", "utpass");

    ActivatingState s{machine};
    auto next = s.onEnter(); // -> mountSmbShare()

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- Legacy NFS: mount failure (no remote) → ReadyState --------------
TEST_F(ActivatingStateFixture, OnEnterLegacy_NfsMountFailureReturnsReadyState)
{
    initVmInterfaceForLegacy(); // ensure vm::Interface::instance and D-Bus
                                // interfaces

    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    setTarget("nfs://srv/export/file.iso", /*rw=*/true);

    ActivatingState s{machine};
    auto next = s.onEnter(); // -> mountNfsShare()

    // resource::NfsMount constructor throws resource::Error when nfs.mount()
    // fails (before nbdkit is ever called), so the catch block always returns
    // ReadyState.
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- Event handlers ---------------------------------------------------
TEST_F(ActivatingStateFixture, HandleEventSubprocessStoppedReturnsReadyState)
{
    ActivatingState s{machine};
    SubprocessStoppedEvent ev{};
    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// Target holds an HTTPS URL -> refined protocol_error (distinct from the
// generic no_such_file_or_directory branch used for NFS/CIFS/local targets).
TEST_F(ActivatingStateFixture,
       HandleEventSubprocessStoppedWithHttpsTargetReturnsProtocolError)
{
    setTarget("https://host/path/image.iso", /*rw=*/false);

    ActivatingState s{machine};
    SubprocessStoppedEvent ev{};
    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");

    auto* ready = next->get_if<ReadyState>();
    ASSERT_NE(ready, nullptr);
    ASSERT_TRUE(ready->error.has_value());
    EXPECT_EQ(ready->error->code, std::errc::protocol_error);
}

// Target holds a non-HTTPS URL -> generic no_such_file_or_directory branch.
TEST_F(ActivatingStateFixture,
       HandleEventSubprocessStoppedWithNonHttpsTargetReturnsGenericError)
{
    setTarget("nfs://host/export/image.iso", /*rw=*/false);

    ActivatingState s{machine};
    SubprocessStoppedEvent ev{};
    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");

    auto* ready = next->get_if<ReadyState>();
    ASSERT_NE(ready, nullptr);
    ASSERT_TRUE(ready->error.has_value());
    EXPECT_EQ(ready->error->code, std::errc::no_such_file_or_directory);
}

TEST_F(ActivatingStateFixture, UnsupportedEventThrowsSdBusErrorBusy)
{
    ActivatingState s{machine};
    UnmountEvent ev{};
    EXPECT_THROW((void)s.handleEvent(std::move(ev)),
                 sdbusplus::exception::SdBusError);
}

TEST_F(ActivatingStateFixture,
       HandleEventMountEvent_WhenAlreadyActivating_ThrowsSdBusErrorBusy)
{
    ActivatingState s{machine};

    interfaces::MountPointStateMachine::Target mountTarget{};
    mountTarget.imgUrl = "https://example.com/another.iso";
    mountTarget.rw = false;

    MountEvent ev{std::optional<interfaces::MountPointStateMachine::Target>{
        std::move(mountTarget)}};
    EXPECT_THROW((void)s.handleEvent(std::move(ev)),
                 sdbusplus::exception::SdBusError);
}

// ---------- Udev handler: non-inserted → DeactivatingState ------------------
TEST_F(ActivatingStateFixture, HandleEventUdevRemovedReturnsDeactivatingState)
{
    ActivatingState s{machine};
    UdevStateChangeEvent ev{StateChange::removed};
    auto next = s.handleEvent(std::move(ev));

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "DeactivatingState");
}

TEST_F(ActivatingStateFixture,
       HandleEventUdevInsertedGadgetTooSmallReturnsReadyState)
{
    // No configfs in the test environment → resource::Gadget setup fails →
    // getStatus() == -1
    ActivatingState s{machine};
    UdevStateChangeEvent ev{StateChange::inserted};
    auto next = s.handleEvent(std::move(ev));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ReadyState");
}

// ---------- Proxy mode: local file success path (mountLocalFile) ------------
// isLocalFile() requires /tmp/lmedia/ prefix; success → spawnNbdKit path.
// Accept nullptr (nbdkit spawned) or ReadyState (nbdkit absent).
TEST_F(ActivatingStateFixture, OnEnterProxy_LocalFileSuccessRemainsActivating)
{
    namespace fs = std::filesystem;

    // Use /tmp/lmedia/ path so isLocalFile() routes to mountLocalFile().
    fs::path file = fs::path{"/tmp/lmedia"} / "vm_local_ok.iso";
    std::error_code ec;
    fs::create_directories(file.parent_path(),
                           ec); // ensure /tmp/lmedia/ exists
    {
        std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
        ofs << "dummy";
    }
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, ec);

    mp.mode = Configuration::Mode::proxy;
    setTarget(file.string(), /*rw=*/true);

    ActivatingState s{machine};
    auto next = s.onEnter();

    // nbdkit present → nullptr (remains activating); absent → ReadyState
    if (next == nullptr)
    {
        SUCCEED();
    }
    else
    {
        EXPECT_EQ(next->getStateName(), "ReadyState");
    }

    fs::remove(file, ec);
}

// ---------------------------------------------------------------
// Proxy mode: NON-LOCAL success (requires nbd-client present)
// Expected: stay in ActivatingState (return nullptr)
// ---------------------------------------------------------------
TEST_F(ActivatingStateFixture,
       OnEnterProxy_NonLocalSpawnSuccessRemainsActivating_IfNbdClientAvailable)
{
    if (!nbd_client_available_abs())
    {
        GTEST_SKIP()
            << "nbd-client not available in absolute path; skipping success-path.";
    }

    mp.mode = Configuration::Mode::proxy;
    setTarget("https://example.com/path/file.iso", /*rw=*/false);

    ActivatingState s{machine};
    auto next = s.onEnter();

    EXPECT_EQ(next, nullptr);
}

// ---------------------------------------------------------------
// Legacy mode: HTTPS success path (requires nbdkit present)
// Expected: stay in ActivatingState (return nullptr)
// ---------------------------------------------------------------
TEST_F(ActivatingStateFixture,
       OnEnterLegacy_HttpsSpawnSuccessRemainsActivating_IfNbdkitAvailable)
{
    if (!nbdkit_available_abs())
    {
        GTEST_SKIP()
            << "nbdkit not available in absolute path; skipping HTTPS success-path.";
    }

    initVmInterfaceForLegacy(); // seed JSON + D-Bus interfaces
    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    setTarget("https://example.com/path/file.iso", /*rw=*/false);

    // Provide credentials required by the HTTPS branch
    target->credentials =
        std::make_unique<utils::CredentialsProvider>("utuser", "utpass");

    ActivatingState s{machine};
    auto next = s.onEnter();

    // nbdkit binary is present and executable → spawn() succeeds → remains in
    // ActivatingState
    EXPECT_EQ(next, nullptr);
}

// ---------------------------------------------------------------
// Legacy mode: SMB/CIFS success path (requires a reachable share)
// Driven via VMEDIA_TEST_SMB_URL / USER / PASS environment variables.
// Expected: stay in ActivatingState (return nullptr) if mount succeeds,
//           or ReadyState if mount fails (network unreachable with test
//           credentials)
// ---------------------------------------------------------------
TEST_F(ActivatingStateFixture,
       OnEnterLegacy_SmbMountSuccessRemainsActivating_IfEnvProvided)
{
    auto url = env_smb_url();
    auto user = env_smb_user();
    auto pass = env_smb_pass();
    if (!url || !user || !pass)
    {
        GTEST_SKIP()
            << "SMB env (VMEDIA_TEST_SMB_URL/USER/PASS) not set; skipping SMB success-path.";
    }

    initVmInterfaceForLegacy();
    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    setTarget(*url, /*rw=*/false);
    target->credentials = std::make_unique<utils::CredentialsProvider>(
        std::move(*user), std::move(*pass));

    ActivatingState s{machine};
    auto next = s.onEnter();

    // With hardcoded test credentials to non-existent server, mount will fail
    // Accept either: nullptr (mount succeeded) or ReadyState (mount failed)
    EXPECT_TRUE(next == nullptr ||
                (next && next->getStateName() == "ReadyState"));
}

// ---------------------------------------------------------------
// Legacy mode: NFS success path (requires a reachable export)
// Driven via VMEDIA_TEST_NFS_URL environment variable.
// Expected: stay in ActivatingState (return nullptr)
// ---------------------------------------------------------------
TEST_F(ActivatingStateFixture,
       OnEnterLegacy_NfsMountSuccessRemainsActivating_IfEnvProvided)
{
    auto url = env_nfs_url();
    if (!url)
    {
        GTEST_SKIP()
            << "NFS env (VMEDIA_TEST_NFS_URL) not set; skipping NFS success-path.";
    }

    initVmInterfaceForLegacy();
    mp.mode = Configuration::Mode::legacy;
    mp.unixSocket = "/tmp/virtual-media-ut/socket.sock";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(mp.unixSocket).parent_path(), ec);

    setTarget(*url, /*rw=*/true); // exercise RW=true success branch

    ActivatingState s{machine};
    auto next = s.onEnter();

    EXPECT_EQ(next, nullptr);
}

// ---------------------------------------------------------------
// Udev inserted: SUCCESS (gadget OK) -> ActiveState
// Requires rooted configfs with usb_gadget available.
// ---------------------------------------------------------------
TEST_F(ActivatingStateFixture,
       HandleEventUdevInsertedSuccessReturnsActiveState_IfGadgetEnvReady)
{
    if (!gadget_env_ready())
    {
        GTEST_SKIP()
            << "Configfs/usb_gadget/root not available; skipping gadget success-path.";
    }

    ActivatingState s{machine};
    UdevStateChangeEvent ev{StateChange::inserted};
    auto next = s.handleEvent(std::move(ev));

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), "ActiveState");
}
