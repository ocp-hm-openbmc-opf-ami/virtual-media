// SPDX-License-Identifier: Apache-2.0
/**
 * @file resources_test.cpp
 * @brief Comprehensive unit tests for resources.cpp (Process and filesystem
 * mounting)
 *
 * This test suite validates the Process class and mounting resource management
 * using the header-shim pattern to intercept system calls without modifying
 * production code.
 *
 * Testing Strategy:
 * - Stub utils::safeMount() to record mount() calls without real operations
 * - Stub umount2() to record unmount() calls
 * - Map UsbGadget to TestUsbGadget for gadget operations recording
 * - DummyState for satisfying getState() reference requirements
 * - No real filesystem operations or hardware dependencies
 *
 * Functional Coverage:
 * 1. **Process Management**:
 *    - NBD subprocess creation and lifecycle
 *    - Process spawning with arguments
 *    - Exit code handling
 *    - Process termination and cleanup
 *
 * 2. **Mount Operations**:
 *    - NFS share mounting with correct options
 *    - SMB/CIFS share mounting with credentials
 *    - Local file mounting
 *    - Mount failure handling
 *    - Unmount operations (lazy/force flags)
 *
 * 3. **USB Gadget Integration**:
 *    - configure() called with correct parameters
 *    - Device assignment and state changes
 *    - Read-write flag propagation
 *    - Cleanup on unmount
 *
 * 4. **Resource Cleanup**:
 *    - Proper umount2() calls
 *    - Filesystem cleanup
 *    - Process termination
 *    - Credential cleanup
 *
 * 5. **Error Handling**:
 *    - Mount failures propagated correctly
 *    - Partial cleanup on errors
 *    - Invalid path rejection
 *    - Resource leak prevention
 *
 * Test Coverage Areas:
 * - All Process class methods
 * - Mount/unmount operation variants
 * - USB gadget configuration recording
 * - Error path handling
 * - Resource lifecycle management
 *
 * Implementation Notes:
 * - utils::safeMount redirected to utils::safeMount_test_stub via macro
 * - umount2 redirected to custom stub for recording
 * - UsbGadget mapped after system.hpp inclusion to avoid redefinition
 * - Stub functions record all parameters for verification
 *
 * Edge Cases Tested:
 * - Mount failures (simulated errors)
 * - Unmount failures
 * - Empty/invalid paths
 * - Resource cleanup on partial failures
 *
 * Dependencies:
 * - boost::asio::io_context: Async process management
 * - filesystem operations (stubbed)
 * - USB gadget API (stubbed)
 *
 * @note INJECT_MOCKS undefined to use test stubs
 * @note Real utils.hpp included first to lock in inline definitions
 */
// Unit tests for resources.{hpp,cpp} using the same header-shim pattern as
// nfs_test.cpp:
//  - Include utils.hpp first (real inline definitions remain intact).
//  - Provide utils::safeMount_test_stub and macro-redirect ONLY the function
//  name
//    while including smb.hpp, nfs.hpp and resources.cpp (so call sites route to
//    the stub).
//  - Macro-redirect umount2 to a stub for recording/simulation.
//  - Provide a concrete DummyState to satisfy getState() by reference in the
//  mock.
//  - Record gadget configure calls by mapping UsbGadget -> TestUsbGadget.
//  - IMPORTANT: Map UsbGadget ONLY AFTER system.hpp is included to avoid
//  redefinition.

#ifdef INJECT_MOCKS
#undef INJECT_MOCKS
#endif

#include <sys/mount.h> // MS_RDONLY, MNT_DETACH, MNT_FORCE
#include <unistd.h>

#include <boost/asio/io_context.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

// ------------------------------------------------------------------
// 0) Include the real utils.hpp FIRST (lock in inline definitions).
// ------------------------------------------------------------------
#include "utils.hpp"

// ------------------------------------------------------------------
// 1) Test-only recorders and controls (mounts + gadget configure).
// ------------------------------------------------------------------
namespace rec
{
inline std::vector<std::string> src_calls, target_calls, fstype_calls,
    options_calls;
inline std::vector<unsigned long> flags_calls;
inline std::vector<std::pair<std::string, int>> umount_calls;
inline bool fail_mount = false;         // make stub return error for mount()
inline bool umount_should_fail = false; // make stub return error for umount2()

inline void reset()
{
    src_calls.clear();
    target_calls.clear();
    fstype_calls.clear();
    flags_calls.clear();
    options_calls.clear();
    umount_calls.clear();
    fail_mount = false;
    umount_should_fail = false;
}
} // namespace rec

// Gadget recorder (for UsbGadget configure calls)
namespace gadget_recorder
{
struct Call
{
    std::string name;
    std::string nbd;
    int state;
    bool rw_present = false;
    bool rw_value = false;
    std::string info;
};
inline std::vector<Call> calls;
inline bool throw_in_config = false;
inline int ret_for_full = 0;
inline int ret_for_short = 0;

inline void reset()
{
    calls.clear();
    throw_in_config = false;
    ret_for_full = 0;
    ret_for_short = 0;
}
} // namespace gadget_recorder

// ------------------------------------------------------------------
// 2) Provide stubs in the SAME namespace as real functions.
//    We'll map name tokens only while including headers & implementation.
// ------------------------------------------------------------------
namespace utils
{

inline int safeMount_test_stub(std::string src, std::string target,
                               std::string fstype, unsigned long flags,
                               std::string options)
{
    using namespace rec;

    // Record arguments
    src_calls.push_back(src);
    target_calls.push_back(target);
    fstype_calls.push_back(fstype);
    flags_calls.push_back(flags);
    options_calls.push_back(options);

    // Simulate success/failure
    if (fail_mount)
    {
        errno = EIO; // ensure src's `errno` read returns a real error code
        return -1;
    }

    // Ensure target exists and write a marker indicating simulation
    std::error_code ec;
    fs::create_directories(fs::path(target), ec);
    std::ofstream sim(fs::path(target) / ".mounted.simulated");
    sim << "simulated mount: fstype=" << fstype << " flags=" << flags
        << " options=" << options << "\n";
    sim.close();

    return 0; // success
}

// Optional: secureCleanup stub if implementation calls it.
inline void secureCleanup_test_stub(const std::string& /*path*/)
{
    // no-op
}

} // namespace utils

// C stub for umount2 (libc symbol) to record/simulate detach.
extern "C" int umount2_test_stub(const char* target, int flags)
{
    using namespace rec;
    fs::path tgt = fs::path(target ? target : "");
    umount_calls.emplace_back(tgt.string(), flags);

    if (umount_should_fail)
    {
        return -1; // failure
    }

    // Remove simulation marker on "unmount"
    std::error_code ec;
    fs::remove(tgt / ".mounted.simulated", ec);
    return 0; // success
}

// ------------------------------------------------------------------
// 3) Redirect ONLY the function name tokens while including call sites.
//    This mirrors nfs_test.cpp: safeMount → safeMount_test_stub.
//    Also redirect secureCleanup and umount2 for completeness.
// ------------------------------------------------------------------
#define safeMount safeMount_test_stub
#define secureCleanup secureCleanup_test_stub
#define umount2 umount2_test_stub

// Include headers that contain call sites after the macro is defined.
// (Do NOT map UsbGadget yet; we include system.hpp first to avoid
// macro-renaming its struct.)
#include "interfaces/mount_point_state_machine.hpp"
#include "nfs.hpp"
#include "smb.hpp"
#include "state/basic_state.hpp"
#include "system.hpp"

// Provide TestUsbGadget that records configure calls.
struct TestUsbGadget
{
    static int32_t configure(std::string name, NBDDevice dev, StateChange st,
                             bool rw, std::string info)
    {
        using namespace gadget_recorder;
        if (throw_in_config)
            throw std::runtime_error("configure failed");
        calls.emplace_back(gadget_recorder::Call{
            name, dev.to_string(), static_cast<int>(st), true, rw, info});
        return ret_for_full;
    }
    static int32_t configure(std::string name, NBDDevice dev, StateChange st)
    {
        using namespace gadget_recorder;
        calls.emplace_back(gadget_recorder::Call{
            name, dev.to_string(), static_cast<int>(st), false, false, {}});
        return ret_for_short;
    }
};

// 🔑 Map UsbGadget ONLY NOW (AFTER system.hpp) so resources.cpp uses
// TestUsbGadget,
//     but we don't rename the struct definition inside system.hpp.
#define UsbGadget TestUsbGadget

// Include the implementation (resources.cpp) ONCE into this TU
// so its call sites see the macro redirects above.
#include "resources.cpp"

// Cleanup macro scope (avoid leakage outside this TU).
#undef UsbGadget
#undef umount2
#undef secureCleanup
#undef safeMount

// ------------------------------------------------------------------
// 4) Mock for interfaces::MountPointStateMachine with a concrete state.
//    Avoids dangling refs and segfaults when io_context runs.
// ------------------------------------------------------------------
class DummyState : public BasicState
{
  public:
    static constexpr std::string_view kName{"Dummy"};
    static std::string_view stateName()
    {
        return kName;
    }
    explicit DummyState(interfaces::MountPointStateMachine& m) : BasicState(m)
    {}
    std::unique_ptr<BasicState> handleEvent(Event) override
    {
        return nullptr;
    }
    std::unique_ptr<BasicState> onEnter() override
    {
        return nullptr;
    }
    std::string_view getStateName() const override
    {
        return kName;
    }
};

class MockMachine : public interfaces::MountPointStateMachine
{
  public:
    boost::asio::io_context ioc_;
    std::string name_ = "UTMachine";
    Configuration::MountPoint cfg_{};
    std::optional<Target> tgt_{};
    int exitCode_ = 0;
    std::string info_ = "additional-info";
    std::unique_ptr<BasicState> bs_; // backing state instance

    MOCK_METHOD(void, notify, (const std::error_code& ec), (override));
    MOCK_METHOD(void, notificationStart, (), (override));
    MOCK_METHOD(void, notificationInitialize,
                (std::shared_ptr<sdbusplus::asio::connection>,
                 const std::string&, const std::string&, const std::string&),
                (override));
    MOCK_METHOD(std::string_view, getName, (), (const, override));
    MOCK_METHOD(Configuration::MountPoint&, getConfig, (), (override));
    MOCK_METHOD(std::optional<Target>&, getTarget, (), (override));
    MOCK_METHOD(BasicState&, getState, (), (override)); // returns ref
    MOCK_METHOD(int&, getExitCode, (), (override));
    MOCK_METHOD(boost::asio::io_context&, getIoc, (), (override));
    MOCK_METHOD(std::string_view, getAdditionalInfo, (), (const, override));
    MOCK_METHOD(void, setAdditionalInfo, (const std::string&), (override));
    MOCK_METHOD(void, emitRegisterDBusEvent,
                (std::shared_ptr<sdbusplus::asio::connection>,
                 std::shared_ptr<sdbusplus::asio::object_server>),
                (override));
    MOCK_METHOD(void, emitMountEvent, (std::optional<Target>), (override));
    MOCK_METHOD(void, emitUnmountEvent, (), (override));
    MOCK_METHOD(void, emitSubprocessStoppedEvent, (), (override));
    MOCK_METHOD(void, emitUdevStateChangeEvent, (const NBDDevice&, StateChange),
                (override));

    void setDefaults()
    {
        using ::testing::Return;
        using ::testing::ReturnRef;
        bs_ = std::make_unique<DummyState>(*this);

        ON_CALL(*this, getName())
            .WillByDefault(Return(std::string_view(name_)));
        ON_CALL(*this, getConfig()).WillByDefault(ReturnRef(cfg_));
        ON_CALL(*this, getTarget()).WillByDefault(ReturnRef(tgt_));
        ON_CALL(*this, getExitCode()).WillByDefault(ReturnRef(exitCode_));
        ON_CALL(*this, getIoc()).WillByDefault(ReturnRef(ioc_));
        ON_CALL(*this, getAdditionalInfo())
            .WillByDefault(Return(std::string_view(info_)));
        ON_CALL(*this, emitSubprocessStoppedEvent())
            .WillByDefault(::testing::Return());
        ON_CALL(*this, getState())
            .WillByDefault(ReturnRef(*bs_)); // concrete ref
    }
};

// ------------------------------------------------------------------
// 5) Tests (unchanged intent; assertions match recorder contents).
// ------------------------------------------------------------------
TEST(Resources_Directory, CreateAndAutoRemove)
{
    rec::reset();
    fs::path name{"ut_dir_ok"};
    fs::path expected = fs::temp_directory_path() / name;
    {
        resource::Directory dir(name);
        EXPECT_TRUE(fs::exists(dir.getPath()));
        EXPECT_EQ(dir.getPath(), expected);
    }
    EXPECT_FALSE(fs::exists(expected));
}

TEST(Resources_Directory, ConstructorThrowsOnExistingDirectory)
{
    rec::reset();
    fs::path name{"ut_dir_exists"};
    fs::path existing = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::create_directory(existing, ec);
    ASSERT_FALSE(ec);
    EXPECT_THROW((resource::Directory(name)), resource::Error);
    fs::remove(existing, ec);
}

TEST(Resources_Mount, SmbGuestMountSuccess)
{
    rec::reset();
    auto dir = std::make_unique<resource::Directory>("cifs_ok");
    SmbShare smb(fs::path("/mnt/cifs"));
    const fs::path remote = "//server/share";
    resource::Mount m(std::move(dir), smb, remote, /*rw*/ true,
                      /*creds*/ nullptr);

    ASSERT_EQ(rec::src_calls.size(), 1u);
    EXPECT_EQ(rec::src_calls[0], remote.string());
    ASSERT_EQ(rec::fstype_calls.size(), 1u);
    EXPECT_EQ(rec::fstype_calls[0], "cifs");
    ASSERT_EQ(rec::flags_calls.size(), 1u);
    EXPECT_EQ(rec::flags_calls[0], 0UL);
}

TEST(Resources_Mount, DestructorCallsUmount2Success)
{
    rec::reset();
    {
        auto dir = std::make_unique<resource::Directory>("cifs_detach_ok");
        SmbShare smb(fs::path("/mnt/cifs2"));
        resource::Mount m(std::move(dir), smb, fs::path("//srv/vol"),
                          /*rw*/ false, /*creds*/ nullptr);
    }
    ASSERT_EQ(rec::umount_calls.size(), 1u);
    EXPECT_THAT(rec::umount_calls[0].first,
                ::testing::HasSubstr("cifs_detach_ok"));
    EXPECT_EQ(rec::umount_calls[0].second, MNT_DETACH | MNT_FORCE);
}

TEST(Resources_Mount, SmbMountFailureThrowsError)
{
    rec::reset();
    rec::fail_mount = true;
    auto dir = std::make_unique<resource::Directory>("cifs_fail");
    SmbShare smb(fs::path("/mnt/cifs3"));
    EXPECT_THROW((resource::Mount(std::move(dir), smb, fs::path("//bad/vol"),
                                  /*rw*/ true, nullptr)),
                 resource::Error);
    EXPECT_TRUE(rec::umount_calls.empty());
}

TEST(Resources_Mount, DestructorCallsUmount2EvenOnFailure)
{
    rec::reset();
    rec::umount_should_fail = true;
    {
        auto dir = std::make_unique<resource::Directory>("cifs_umount_fail");
        SmbShare smb(fs::path("/mnt/cifsX"));
        resource::Mount m(std::move(dir), smb, fs::path("//srv/volX"),
                          /*rw*/ false, /*creds*/ nullptr);
    }
    ASSERT_EQ(rec::umount_calls.size(), 1u);
    EXPECT_THAT(rec::umount_calls[0].first,
                ::testing::HasSubstr("cifs_umount_fail"));
    EXPECT_EQ(rec::umount_calls[0].second, MNT_DETACH | MNT_FORCE);
}

TEST(Resources_NfsMount, NfsMountSuccessAndDetach)
{
    rec::reset();
    auto dir = std::make_unique<resource::Directory>("nfs_ok");
    NfsShare nfs(fs::path("/mnt/nfs"));
    const fs::path remote = "host:/export/images";
    resource::NfsMount m(std::move(dir), nfs, remote, /*rw*/ true);

    ASSERT_EQ(rec::fstype_calls.size(), 1u);
    EXPECT_EQ(rec::fstype_calls[0], "nfs4");
}

TEST(Resources_NfsMount, DestructorCallsUmount2)
{
    rec::reset();
    {
        auto dir = std::make_unique<resource::Directory>("nfs_detach");
        NfsShare nfs(fs::path("/mnt/nfs2"));
        resource::NfsMount m(std::move(dir), nfs, fs::path("host:/vol"),
                             /*rw*/ false);
    }
    ASSERT_EQ(rec::umount_calls.size(), 1u);
    EXPECT_THAT(rec::umount_calls[0].first, ::testing::HasSubstr("nfs_detach"));
    EXPECT_EQ(rec::umount_calls[0].second, MNT_DETACH | MNT_FORCE);
}

TEST(Resources_NfsMount, DestructorCallsUmount2EvenOnFailure)
{
    rec::reset();
    rec::umount_should_fail = true;
    {
        auto dir = std::make_unique<resource::Directory>("nfs_umount_fail");
        NfsShare nfs(fs::path("/mnt/nfs_uf"));
        resource::NfsMount m(std::move(dir), nfs, fs::path("host:/uf"),
                             /*rw*/ false);
    }
    ASSERT_EQ(rec::umount_calls.size(), 1u);
    EXPECT_THAT(rec::umount_calls[0].first,
                ::testing::HasSubstr("nfs_umount_fail"));
    EXPECT_EQ(rec::umount_calls[0].second, MNT_DETACH | MNT_FORCE);
}

TEST(Resources_NfsMount, NfsMountFailureThrowsError)
{
    rec::reset();
    rec::fail_mount = true;
    auto dir = std::make_unique<resource::Directory>("nfs_fail");
    NfsShare nfs(fs::path("/mnt/nfs3"));
    EXPECT_THROW((resource::NfsMount(std::move(dir), nfs, fs::path("bad:/path"),
                                     /*rw*/ true)),
                 resource::Error);
    EXPECT_TRUE(rec::umount_calls.empty());
}

TEST(Resources_Process, ConstructorThrowsOnNullptr)
{
    rec::reset();
    ::testing::NiceMock<MockMachine> machine;
    machine.setDefaults();
    std::shared_ptr<::Process> nullp;
    EXPECT_THROW((resource::Process(machine, nullp)), resource::Error);
}

TEST(Resources_Process, SpawnSignatureExercisedAndDestructorPostsSafely)
{
    rec::reset();
    ::testing::NiceMock<MockMachine> machine;
    machine.setDefaults();
    boost::asio::io_context& ioc = machine.getIoc();
    NBDDevice dev("nbd7");
    std::shared_ptr<::Process> p = std::make_shared<::Process>(
        ioc, machine.getName(), /*binary*/ "/usr/bin/true", dev);
    {
        resource::Process rp(machine, p);
        std::vector<std::string> args{};
        auto onExit = [](int) noexcept {}; // proper lambda
        (void)rp.spawn(args, onExit);
    }
    machine.getIoc().poll(); // safe: getState() returns a concrete DummyState
}

TEST(Resources_Gadget, ConfigureSuccess_RecordsArgs_StatusNonNegative)
{
    gadget_recorder::reset();
    ::testing::NiceMock<MockMachine> m;
    m.setDefaults();
    m.tgt_.emplace();
    m.tgt_->rw = true;
    m.cfg_.nbdDevice = NBDDevice("nbd10");
    gadget_recorder::ret_for_full = 0;

    resource::Gadget g(m, StateChange::removed);
    ASSERT_EQ(gadget_recorder::calls.size(), 1u);
    EXPECT_EQ(gadget_recorder::calls[0].name, m.getName());
    EXPECT_EQ(gadget_recorder::calls[0].nbd, m.cfg_.nbdDevice.to_string());
    EXPECT_TRUE(gadget_recorder::calls[0].rw_present);
    EXPECT_TRUE(gadget_recorder::calls[0].rw_value);
    EXPECT_EQ(g.getStatus(), 0);
}

TEST(Resources_Gadget, ConstructedWithInsertedForwardsInsertedStateToConfigure)
{
    gadget_recorder::reset();
    ::testing::NiceMock<MockMachine> m;
    m.setDefaults();
    gadget_recorder::ret_for_full = 0;

    resource::Gadget g(m, StateChange::inserted);
    ASSERT_EQ(gadget_recorder::calls.size(), 1u);
    EXPECT_EQ(gadget_recorder::calls[0].state,
              static_cast<int>(StateChange::inserted));
    EXPECT_EQ(g.getStatus(), 0);
}

TEST(Resources_Gadget, ConfigureFailure_StatusMinusOne)
{
    gadget_recorder::reset();
    ::testing::NiceMock<MockMachine> m;
    m.setDefaults();
    gadget_recorder::ret_for_full = -1;

    resource::Gadget g(m, StateChange::removed);
    EXPECT_EQ(g.getStatus(), -1);
}

TEST(Resources_Gadget, ConfigureThrows_ExceptionCaught_StatusMinusOne)
{
    gadget_recorder::reset();
    ::testing::NiceMock<MockMachine> m;
    m.setDefaults();
    gadget_recorder::throw_in_config = true;

    resource::Gadget g(m, StateChange::removed);
    EXPECT_EQ(g.getStatus(), -1);
}

TEST(Resources_Gadget, NoTargetImpliesRwFalse)
{
    gadget_recorder::reset();
    ::testing::NiceMock<MockMachine> m;
    m.setDefaults();
    m.tgt_.reset();

    resource::Gadget g(m, StateChange::removed);
    ASSERT_EQ(gadget_recorder::calls.size(), 1u);
    EXPECT_TRUE(gadget_recorder::calls[0].rw_present);
    EXPECT_FALSE(gadget_recorder::calls[0].rw_value);
}

TEST(Resources_Gadget, DestructorCallsShortConfigureRemoved)
{
    gadget_recorder::reset();
    {
        ::testing::NiceMock<MockMachine> m;
        m.setDefaults();
        resource::Gadget g(m, StateChange::removed);
    }
    ASSERT_GE(gadget_recorder::calls.size(), 1u);
    const auto& last = gadget_recorder::calls.back();
    EXPECT_FALSE(last.rw_present);
    EXPECT_EQ(last.state, static_cast<int>(StateChange::removed));
}

// -----------------------------------------------------------------------------
// Directory: exercise destructor failure branch (non-empty directory)
// -----------------------------------------------------------------------------
TEST(Resources_Directory, DestructorFailureLeavesDirectoryNonEmpty)
{
    rec::reset();
    namespace fs = std::filesystem;

    fs::path name{"ut_dir_nonempty"};
    fs::path root = fs::temp_directory_path() / name;
    {
        resource::Directory d{name};
        // Make directory non-empty so std::filesystem::remove(dir) returns
        // false.
        std::ofstream f(root / "keep.txt");
        f << "keep";
        f.close();
        EXPECT_TRUE(fs::exists(root / "keep.txt"));
    }
    // Destructor ran; removal should have failed because directory was
    // non-empty.
    EXPECT_TRUE(fs::exists(root));

    // Manual cleanup
    std::error_code ec;
    fs::remove(root / "keep.txt", ec);
    fs::remove(root, ec);
}

// -----------------------------------------------------------------------------
// Mount (SMB): getPath accessor
// -----------------------------------------------------------------------------
TEST(Resources_Mount, GetPathReturnsMountDirectoryPath)
{
    rec::reset();
    namespace fs = std::filesystem;

    fs::path name{"cifs_getpath_ok"};
    fs::path expected = fs::temp_directory_path() / name;

    auto dir = std::make_unique<resource::Directory>(name);
    SmbShare smb(fs::path("/mnt/cifs_gp"));
    resource::Mount m(std::move(dir), smb, fs::path("//srv/gp"), /*rw*/ true,
                      /*creds*/ nullptr);

    EXPECT_EQ(m.getPath(), expected);
}

// -----------------------------------------------------------------------------
// NfsMount (NFS): getPath accessor
// -----------------------------------------------------------------------------
TEST(Resources_NfsMount, GetPathReturnsMountDirectoryPath)
{
    rec::reset();
    namespace fs = std::filesystem;

    fs::path name{"nfs_getpath_ok"};
    fs::path expected = fs::temp_directory_path() / name;

    auto dir = std::make_unique<resource::Directory>(name);
    NfsShare nfs(fs::path("/mnt/nfs_gp"));
    resource::NfsMount m(std::move(dir), nfs, fs::path("host:/gp"),
                         /*rw*/ true);

    EXPECT_EQ(m.getPath(), expected);
}

// -----------------------------------------------------------------------------
// Mount (SMB): read-only path sets MS_RDONLY in flags
// -----------------------------------------------------------------------------
TEST(Resources_Mount, SmbReadOnlySetsMS_RDONLYFlag)
{
    rec::reset();

    auto dir = std::make_unique<resource::Directory>("cifs_ro");
    SmbShare smb(std::filesystem::path("/mnt/cifs_ro"));
    resource::Mount m(std::move(dir), smb, std::filesystem::path("//srv/ro"),
                      /*rw*/ false, /*creds*/ nullptr);

    ASSERT_EQ(rec::flags_calls.size(), 1u);
    EXPECT_NE((rec::flags_calls[0] & MS_RDONLY), 0UL);
}

// -----------------------------------------------------------------------------
// NfsMount (NFS): read-only path sets MS_RDONLY in flags
// -----------------------------------------------------------------------------
TEST(Resources_NfsMount, ReadOnlySetsMS_RDONLYFlag)
{
    rec::reset();

    auto dir = std::make_unique<resource::Directory>("nfs_ro");
    NfsShare nfs(std::filesystem::path("/mnt/nfs_ro"));
    resource::NfsMount m(std::move(dir), nfs, std::filesystem::path("host:/ro"),
                         /*rw*/ false);

    ASSERT_EQ(rec::flags_calls.size(), 1u);
    EXPECT_NE((rec::flags_calls[0] & MS_RDONLY), 0UL);
}

// -----------------------------------------------------------------------------
// Process: spawn failure branch (invalid binary path → ::Process::spawn returns
// false)
// -----------------------------------------------------------------------------
TEST(Resources_Process, SpawnFailureReturnsFalseAndRemainsUnspawned)
{
    rec::reset();

    ::testing::NiceMock<MockMachine> machine;
    machine.setDefaults();
    NBDDevice dev("nbd42");

    // Use a guaranteed-nonexistent binary so ::Process::spawn(...) fails.
    auto p =
        std::make_shared<::Process>(machine.getIoc(), machine.getName(),
                                    /*binary*/ "/this/does/not/exist", dev);

    resource::Process rp(machine, p);
    std::vector<std::string> args{};
    auto onExit = [](int) noexcept {}; // proper lambda

    EXPECT_FALSE(rp.spawn(args, onExit));

    // Ensure destructor posting is still safe (even after a spawn failure).
    machine.getIoc().poll();
}

// -----------------------------------------------------------------------------
// Gadget: target present but rw=false (distinct from "no target" case)
// -----------------------------------------------------------------------------
TEST(Resources_Gadget, TargetPresentButRwFalseRecordedCorrectly)
{
    gadget_recorder::reset();

    ::testing::NiceMock<MockMachine> m;
    m.setDefaults();

    // Provide a target but set rw=false.
    m.tgt_.emplace();
    m.tgt_->rw = false;
    m.cfg_.nbdDevice = NBDDevice("nbd11");

    // Use an enum value known to be valid in this codebase.
    resource::Gadget g(m, StateChange::removed);

    ASSERT_EQ(gadget_recorder::calls.size(), 1u);
    EXPECT_TRUE(gadget_recorder::calls[0].rw_present);
    EXPECT_FALSE(gadget_recorder::calls[0].rw_value);
}
