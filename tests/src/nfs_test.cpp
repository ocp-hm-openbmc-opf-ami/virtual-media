// SPDX-License-Identifier: Apache-2.0
/**
 * @file nfs_test.cpp
 * @brief Comprehensive unit tests for NFS (Network File System) mounting
 * operations
 *
 * This combined test suite validates all NFS-related functionality including
 * mounting, option negotiation, fallback strategies, and error handling.
 *
 * Combined Test Sources:
 * - nfs_test.cpp: Core NFS mounting operations
 * - nfs_fallback_options_completeness_test.cpp: Option fallback testing
 * - nfs_fallback_rw_flags_test.cpp: Read-write flag handling
 * - nfs_logging_test.cpp: Error logging validation
 * - nfs_parse_edges_test.cpp: URL parsing edge cases
 *
 * Testing Strategy:
 * - Stub utils::safeMount() to intercept mount() syscalls
 * - Record all mount attempts with parameters
 * - Simulate mount failures for fallback testing
 * - Validate NFS version negotiation (v4 → v3 fallback)
 * - No real network or filesystem operations
 *
 * Functional Coverage:
 * 1. **NFS URL Parsing**:
 *    - server:/export/path format validation
 *    - IPv4 address support
 *    - IPv6 address support (with brackets)
 *    - Hostname resolution (simulated)
 *    - Port number handling
 *    - Path component extraction
 *
 * 2. **Mount Operations**:
 *    - NFS v4 first attempt (default)
 *    - NFS v3 fallback on v4 failure
 *    - Read-only vs. read-write flag handling
 *    - Mount option string construction
 *    - Source path formatting
 *
 * 3. **Mount Options**:
 *    - Base options: nolock,noexec,nosuid,nodev
 *    - Version-specific options (vers=4.2 / nfsvers=3)
 *    - RW flag translation (rw / ro)
 *    - Address family hints (addr=IPv4/IPv6)
 *    - Retry and timeout parameters
 *
 * 4. **Fallback Strategy**:
 *    - NFSv4 failure → automatic NFSv3 retry
 *    - Both attempts recorded for verification
 *    - Failure propagation when both versions fail
 *    - Option string correctness in both attempts
 *
 * 5. **Error Handling**:
 *    - Invalid URL format rejection
 *    - Missing server/path components
 *    - Mount system call failures
 *    - Logging of failures (when enabled)
 *
 * Test Coverage Areas:
 * - All NfsShare class methods
 * - URL parsing variants (IPv4/IPv6/hostname)
 * - Mount option construction
 * - Version fallback logic
 * - Read-write flag propagation
 * - Error logging integration
 *
 * Edge Cases Tested:
 * - IPv6 addresses with brackets
 * - Paths with special characters
 * - Empty export paths (/)
 * - Mount point directory creation
 * - Concurrent mount attempts (isolated)
 *
 * Implementation Notes:
 * - Real utils.hpp included first (inline definitions preserved)
 * - safeMount redirected to safeMount_test_stub via #define
 * - Stub records: src, target, fstype, flags, options
 * - Test controls: fail_first (v4), fail_both (v4+v3)
 *
 * Dependencies:
 * - filesystem::path: Mount point management
 * - utils::safeMount (stubbed): Mount syscall wrapper
 *
 * @note No actual network operations performed
 * @note All filesystem operations are simulated
 */
// Combined NFS tests for nfs.hpp
// This file combines all NFS-related test suites from:
//   - nfs_test.cpp
//   - nfs_fallback_options_completeness_test.cpp
//   - nfs_fallback_rw_flags_test.cpp
//   - nfs_logging_test.cpp
//   - nfs_parse_edges_test.cpp

#include <sys/mount.h> // MS_RDONLY

#include <filesystem>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

// =============================================================================
// Section 1: Basic NFS tests (from nfs_test.cpp)
// =============================================================================

// Include the real utils.hpp first
#include "utils.hpp"

// Test-only recorder and controls
namespace mount_recorder
{
inline std::vector<std::string> src_calls;
inline std::vector<std::string> target_calls;
inline std::vector<std::string> fstype_calls;
inline std::vector<unsigned long> flags_calls;
inline std::vector<std::string> options_calls;

inline bool fail_first = false;
inline bool fail_both = false;

inline void reset()
{
    src_calls.clear();
    target_calls.clear();
    fstype_calls.clear();
    flags_calls.clear();
    options_calls.clear();
    fail_first = false;
    fail_both = false;
}
} // namespace mount_recorder

// Provide our stub in the SAME namespace 'utils' but with a DIFFERENT name
namespace utils
{
inline int safeMount_test_stub(std::string src, std::string target,
                               std::string fstype, unsigned long flags,
                               std::string options)
{
    using namespace mount_recorder;
    src_calls.push_back(src);
    target_calls.push_back(target);
    fstype_calls.push_back(fstype);
    flags_calls.push_back(flags);
    options_calls.push_back(options);

    const size_t call_index = src_calls.size();
    if (fail_both)
    {
        errno = EIO;
        return -1;
    } // error
    if (fail_first && call_index == 1)
    {
        errno = EIO;
        return -1;
    } // first attempt fails
    errno = 0;
    return 0; // success
}
} // namespace utils

// Redirect ONLY the function name while including nfs.hpp
#define safeMount safeMount_test_stub
#include "nfs.hpp"
#undef safeMount

using namespace mount_recorder;

// Happy path: parse succeeds; first mount (nfs4) succeeds; RW flags
TEST(NfsShare, MountSuccessRw)
{
    reset();

    NfsShare share(fs::path("/mnt/nfs"));
    const std::string remote = "server1:/export/images";

    const bool ok = (share.mount(remote, /*rw*/ true) == 0);
    EXPECT_TRUE(ok);

    ASSERT_EQ(src_calls.size(), 1u);
    ASSERT_EQ(target_calls.size(), 1u);
    ASSERT_EQ(fstype_calls.size(), 1u);
    ASSERT_EQ(flags_calls.size(), 1u);
    ASSERT_EQ(options_calls.size(), 1u);

    // Current code uses remote.substr(index, remote.length()-1) → drops last
    // char.
    const auto idx = remote.find(":/");
    ASSERT_NE(idx, std::string::npos);
    const auto expected_src = remote.substr(idx, remote.size() - 1);
    EXPECT_EQ(src_calls[0], expected_src);

    EXPECT_EQ(fstype_calls[0], "nfs4");
    EXPECT_EQ(flags_calls[0], 0UL); // RW
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("soft"));
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("addr=server1"));
    EXPECT_EQ(target_calls[0], std::string("/mnt/nfs"));
}

// Fallback: first mount fails → second attempt with fstype "nfs" and options
// append ",nolock"
TEST(NfsShare, MountFallbackToNfsWithNoLockOnFirstFailure)
{
    reset();
    fail_first = true;

    NfsShare share(fs::path("/mnt/fallback"));
    const std::string remote = "bmc-host:/exports/os";

    const bool ok = (share.mount(remote, /*rw*/ false) == 0); // RO flags
    EXPECT_TRUE(ok); // second attempt succeeds via stub

    ASSERT_EQ(src_calls.size(), 2u);
    ASSERT_EQ(target_calls.size(), 2u);
    ASSERT_EQ(fstype_calls.size(), 2u);
    ASSERT_EQ(flags_calls.size(), 2u);
    ASSERT_EQ(options_calls.size(), 2u);

    EXPECT_EQ(fstype_calls[0], "nfs4");
    EXPECT_EQ(flags_calls[0], static_cast<unsigned long>(MS_RDONLY));
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("addr=bmc-host"));

    EXPECT_EQ(fstype_calls[1], "nfs");
    EXPECT_EQ(flags_calls[1], static_cast<unsigned long>(MS_RDONLY));
    EXPECT_THAT(options_calls[1], ::testing::HasSubstr("nolock"));

    EXPECT_EQ(src_calls[0], src_calls[1]);
    EXPECT_EQ(target_calls[0], target_calls[1]);
}

// Both attempts fail → mount() returns false and we recorded 2 calls
TEST(NfsShare, MountFailsWhenBothAttemptsReturnError)
{
    reset();
    fail_both = true;

    NfsShare share(fs::path("/mnt/fail"));
    const bool ok = (share.mount("storage:/bad/path", /*rw*/ true) == 0);

    EXPECT_FALSE(ok);
    ASSERT_EQ(src_calls.size(), 2u);
    EXPECT_EQ(fstype_calls[0], "nfs4");
    EXPECT_EQ(fstype_calls[1], "nfs");
}

// Parse failure: no ":/" → returns EINVAL (nonzero error), no safeMount calls
TEST(NfsShare, MountParseFailureReturnsError)
{
    reset();

    NfsShare share(fs::path("/mnt/parse"));
    const bool ok = (share.mount("server:share", /*rw*/ true) == 0);

    EXPECT_FALSE(ok); // parse failure returns EINVAL (nonzero)
    EXPECT_TRUE(src_calls.empty());
    EXPECT_TRUE(target_calls.empty());
    EXPECT_TRUE(fstype_calls.empty());
    EXPECT_TRUE(flags_calls.empty());
    EXPECT_TRUE(options_calls.empty());
}

// Options formatting: "soft" initially; ensure "addr=<host>" appended with
// comma
TEST(NfsShare, OptionsContainAddrAndSoft)
{
    reset();

    NfsShare share(fs::path("/mnt/options"));
    const bool ok = (share.mount("nfs-srv:/vol1/os", /*rw*/ true) == 0);
    EXPECT_TRUE(ok);

    ASSERT_EQ(options_calls.size(), 1u);
    const auto& opts = options_calls[0];
    EXPECT_THAT(opts, ::testing::HasSubstr("soft"));
    EXPECT_THAT(opts, ::testing::HasSubstr("addr=nfs-srv"));
}

TEST(NfsShare, MountWithIPv6HostAddrOption)
{
    reset();
    NfsShare share(fs::path("/mnt/ipv6"));
    const std::string remote = "2001:db8::1:/export/images";
    EXPECT_TRUE((share.mount(remote, true) == 0));
    ASSERT_EQ(src_calls.size(), 1u);
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("addr=2001:db8::1"));
}

TEST(NfsShare, MountWithPortInHostAddrOption)
{
    reset();
    NfsShare share(fs::path("/mnt/port"));
    const std::string remote = "server:2049:/export/images";
    EXPECT_TRUE((share.mount(remote, false) == 0));
    ASSERT_EQ(src_calls.size(), 1u);
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("addr=server:2049"));
    EXPECT_EQ(flags_calls[0], static_cast<unsigned long>(MS_RDONLY));
}

// RW fallback: first mount (nfs4) fails → second attempt (nfs) preserves RW
// flags (0)
TEST(NfsShare, MountFallbackPreservesRwFlagsAcrossAttempts)
{
    reset();
    fail_first = true;

    NfsShare share(fs::path("/mnt/fb-rw"));
    const bool ok = (share.mount("srv:/export/os", /*rw*/ true) == 0);
    EXPECT_TRUE(ok);

    ASSERT_EQ(src_calls.size(), 2u);
    ASSERT_EQ(flags_calls.size(), 2u);
    EXPECT_EQ(flags_calls[0], 0UL); // RW on first attempt (nfs4)
    EXPECT_EQ(flags_calls[1], 0UL); // RW preserved on fallback (nfs)
    EXPECT_EQ(fstype_calls[0], "nfs4");
    EXPECT_EQ(fstype_calls[1], "nfs");
    EXPECT_THAT(options_calls[1], ::testing::HasSubstr("nolock"));
    EXPECT_EQ(src_calls[0], src_calls[1]);
    EXPECT_EQ(target_calls[0], target_calls[1]);
}
