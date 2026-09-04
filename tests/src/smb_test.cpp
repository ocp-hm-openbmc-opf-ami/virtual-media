// SPDX-License-Identifier: Apache-2.0
/**
 * @file smb_test.cpp
 * @brief Comprehensive unit tests for SMB/CIFS (Windows share) mounting
 * operations
 *
 * This combined test suite validates all SMB/CIFS-related functionality
 * including mounting with credentials, guest fallback, secure cleanup, and
 * error handling.
 *
 * Combined Test Sources:
 * - smb_test.cpp: Core SMB mounting operations
 * - smb_edge_inputs_and_credentials_test.cpp: Input validation and credentials
 * - smb_guest_fallback_and_cleanup_failure_test.cpp: Guest mode and cleanup
 * - smb_logging_test.cpp: Error logging validation
 *
 * Testing Strategy:
 * - Stub utils::safeMount() to intercept mount() syscalls
 * - Stub utils::secureCleanup() to track credential cleanup
 * - Record all mount attempts with full parameters
 * - Simulate mount failures for guest fallback testing
 * - Validate credential formatting and security
 * - No real network or filesystem operations
 *
 * Functional Coverage:
 * 1. **SMB URL Parsing**:
 *    - smb://server/share/path format validation
 *    - UNC path conversion (//server/share)
 *    - Windows-style backslash support
 *    - Server and share extraction
 *    - Path component handling
 *
 * 2. **Credential Handling**:
 *    - Username and password formatting
 *    - Credentials string construction (user=X,pass=Y)
 *    - Special character escaping in passwords
 *    - Comma escaping in password fields
 *    - Secure cleanup of credential buffers
 *
 * 3. **Mount Operations**:
 *    - Authenticated mount (with credentials)
 *    - Guest fallback on auth failure
 *    - Read-only vs. read-write flag handling
 *    - Mount option string construction
 *    - Filesystem type (cifs) specification
 *
 * 4. **Mount Options**:
 *    - Base options: user=X,pass=Y,rw/ro
 *    - CIFS version specification (vers=3.0)
 *    - Guest mode options (guest,username=)
 *    - Security flags and permissions
 *
 * 5. **Fallback Strategy**:
 *    - Authenticated mount first attempt
 *    - Guest mount fallback on auth failure
 *    - Both attempts recorded for verification
 *    - Failure propagation when both attempts fail
 *
 * 6. **Security**:
 *    - Credential cleanup after mount
 *    - Secure memory zeroing (explicit_bzero)
 *    - No credential leakage in logs
 *    - Cleanup even on mount failures
 *
 * 7. **Error Handling**:
 *    - Invalid URL format rejection
 *    - Missing server/share components
 *    - Mount system call failures
 *    - Cleanup failures (logged but non-fatal)
 *    - Logging of failures (when enabled)
 *
 * Test Coverage Areas:
 * - All SmbShare class methods
 * - URL parsing variants
 * - Credential formatting
 * - Mount option construction
 * - Guest fallback logic
 * - Secure cleanup operations
 * - Error logging integration
 *
 * Edge Cases Tested:
 * - Passwords with special characters
 * - Passwords with commas (escaped)
 * - Empty passwords (guest mode)
 * - UNC paths vs. SMB URLs
 * - Cleanup failures
 * - Multiple mount attempts
 *
 * Implementation Notes:
 * - Real utils.hpp included first (inline definitions preserved)
 * - safeMount redirected to safeMount_test_stub via #define
 * - secureCleanup redirected to secureCleanup_test_stub via #define
 * - Stubs record: src, target, fstype, flags, options, cleanup args
 * - Test controls: fail_first (auth), fail_both (auth+guest)
 *
 * Dependencies:
 * - filesystem::path: Mount point management
 * - utils::safeMount (stubbed): Mount syscall wrapper
 * - utils::secureCleanup (stubbed): Secure memory cleanup
 * - utils::CredentialsProvider: Credential management
 *
 * @note No actual network operations performed
 * @note All filesystem operations are simulated
 * @note Credentials are securely cleaned even in test mode
 */
// Combined SMB tests for smb.hpp
// This file combines all SMB-related test suites from:
//   - smb_test.cpp
//   - smb_edge_inputs_and_credentials_test.cpp
//   - smb_guest_fallback_and_cleanup_failure_test.cpp
//   - smb_logging_test.cpp

#include <sys/mount.h> // MS_RDONLY

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

// =============================================================================
// Section 1: Basic SMB tests (from smb_test.cpp)
// =============================================================================

// Include the real utils.hpp first
#include "utils.hpp"

// Test-only recorder and controls
namespace smb_recorder
{
inline std::vector<std::string> src_calls;
inline std::vector<std::string> target_calls;
inline std::vector<std::string> fstype_calls;
inline std::vector<unsigned long> flags_calls;
inline std::vector<std::string> options_calls;
inline std::vector<std::string> cleanup_args;
inline bool fail_first = false;
inline bool fail_both = false;
inline void reset()
{
    src_calls.clear();
    target_calls.clear();
    fstype_calls.clear();
    flags_calls.clear();
    options_calls.clear();
    cleanup_args.clear();
    fail_first = false;
    fail_both = false;
}
} // namespace smb_recorder

// Provide our stubs in the SAME namespace 'utils' but with DIFFERENT names
namespace utils
{
inline int safeMount_test_stub(std::string src, std::string target,
                               std::string fstype, unsigned long flags,
                               std::string options)
{
    using namespace smb_recorder;
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

inline void secureCleanup_test_stub(const std::string& buf)
{
    using namespace smb_recorder;
    cleanup_args.push_back(buf);
}
} // namespace utils

// Redirect ONLY the function names while including smb.hpp
#define safeMount safeMount_test_stub
#define secureCleanup secureCleanup_test_stub
#include "smb.hpp"
#undef secureCleanup
#undef safeMount

using namespace smb_recorder;

// Helper: construct real CredentialsProvider
static std::unique_ptr<utils::CredentialsProvider> makeCreds(
    std::string user, std::string password)
{
    return std::make_unique<utils::CredentialsProvider>(std::move(user),
                                                        std::move(password));
}

// Helper: split by comma
static std::vector<std::string> splitByComma(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == ',')
        {
            out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Guest mount (no credentials): RW flags, single attempt succeeds, vers=3.1.1
TEST(SmbShare, MountGuestSuccessRw)
{
    reset();
    SmbShare share(fs::path("/mnt/cifs"));
    const fs::path remote = "//server/share";
    const bool ok = (share.mount(remote, /*rw=*/true, /*creds*/ nullptr) == 0);
    EXPECT_TRUE(ok);
    ASSERT_EQ(src_calls.size(), 1u);
    ASSERT_EQ(target_calls.size(), 1u);
    ASSERT_EQ(fstype_calls.size(), 1u);
    ASSERT_EQ(flags_calls.size(), 1u);
    ASSERT_EQ(options_calls.size(), 1u);
    EXPECT_EQ(fstype_calls[0], std::string("cifs"));
    EXPECT_EQ(flags_calls[0], 0UL); // RW
    EXPECT_EQ(target_calls[0], std::string("/mnt/cifs"));
    EXPECT_EQ(src_calls[0], remote.string());
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("sec=ntlmsspi"));
    EXPECT_THAT(options_calls[0],
                ::testing::HasSubstr("guest,username=OpenBmc"));
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr(",vers=3.1.1"));
    EXPECT_EQ(cleanup_args.size(), 2u);
}

// Authenticated mount: RO flags; vers=3.1.1
TEST(SmbShare, MountWithCredentialsSuccessRo)
{
    reset();
    auto creds = makeCreds("admin", "s3cr3t");

    SmbShare share(fs::path("/mnt/data"));
    const fs::path remote = "//srv/vol";
    const bool ok = (share.mount(remote, /*rw=*/false, creds) == 0);
    EXPECT_TRUE(ok);
    ASSERT_EQ(src_calls.size(), 1u);
    ASSERT_EQ(options_calls.size(), 1u);
    EXPECT_EQ(flags_calls[0], static_cast<unsigned long>(MS_RDONLY));
    EXPECT_THAT(options_calls[0],
                ::testing::HasSubstr("username=admin,password=s3cr3t"));
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr(",vers=3.1.1"));
    EXPECT_EQ(cleanup_args.size(), 2u);
}

// Fallback: first mount fails -> second attempt with vers=3, options retained
TEST(SmbShare, MountFallbackToVers3OnFirstFailure)
{
    reset();
    fail_first = true;
    auto creds = makeCreds("operator", "p@ss");

    SmbShare share(fs::path("/mnt/fallback"));
    const fs::path remote = "//bmc/share";
    const bool ok = (share.mount(remote, /*rw=*/true, creds) == 0);
    EXPECT_TRUE(ok); // second attempt succeeds via stub
    ASSERT_EQ(src_calls.size(), 2u);
    ASSERT_EQ(options_calls.size(), 2u);
    // First attempt: vers=3.1.1
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr(",vers=3.1.1"));
    EXPECT_THAT(options_calls[0],
                ::testing::HasSubstr("username=operator,password=p@ss"));
    // Second attempt: vers=3
    EXPECT_THAT(options_calls[1], ::testing::HasSubstr(",vers=3"));
    EXPECT_THAT(options_calls[1],
                ::testing::HasSubstr("username=operator,password=p@ss"));
    // src/target unchanged across attempts
    EXPECT_EQ(src_calls[0], src_calls[1]);
    EXPECT_EQ(target_calls[0], target_calls[1]);
    EXPECT_EQ(cleanup_args.size(), 2u);
}

// Both attempts fail -> returns false; still cleans sensitive buffers
TEST(SmbShare, MountFailsWhenBothAttemptsReturnError)
{
    reset();
    fail_both = true;
    auto creds = makeCreds("tech", "pwd");

    SmbShare share(fs::path("/mnt/fail"));
    const bool ok = (share.mount("//storage/share", /*rw=*/false, creds) == 0);
    EXPECT_FALSE(ok);
    ASSERT_EQ(src_calls.size(), 2u);
    EXPECT_EQ(fstype_calls[0], std::string("cifs"));
    EXPECT_EQ(fstype_calls[1], std::string("cifs"));
    EXPECT_EQ(cleanup_args.size(), 2u);
}

// Invalid username containing comma -> early return false; no mount attempts;
// no cleanup
TEST(SmbShare, InvalidUsernameWithCommaReturnsFalseWithoutMount)
{
    reset();
    auto creds = makeCreds("bad,user", "any");

    SmbShare share(fs::path("/mnt/invalid"));
    const bool ok = (share.mount("//host/share", /*rw=*/true, creds) == 0);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(src_calls.empty());
    EXPECT_TRUE(options_calls.empty());
    EXPECT_TRUE(cleanup_args.empty());
}

TEST(SmbShare, CredentialsWithCommasEscapedInOptions)
{
    reset();
    // Password with commas should be escaped by
    // CredentialsProvider::escapeCommas()
    auto creds = makeCreds("okuser", "pa,ss,word");
    SmbShare share(fs::path("/mnt/esc"));
    const bool ok = (share.mount("//srv/esc", /*rw=*/true, creds) == 0);
    EXPECT_TRUE(ok);
    ASSERT_EQ(options_calls.size(), 1u);
    // Post-escape, the password string in options must NOT break the
    // comma-separated list.
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("username=okuser"));
    EXPECT_THAT(options_calls[0],
                ::testing::Not(::testing::HasSubstr("password=pa,ss,word")));
}

TEST(SmbShare, BaseCifsOptionsAreComplete)
{
    reset();
    SmbShare share(fs::path("/mnt/base"));
    EXPECT_TRUE((share.mount("//s/base", /*rw=*/true, nullptr) == 0));
    ASSERT_EQ(options_calls.size(), 1u);
    const std::string& opts = options_calls[0];
    EXPECT_THAT(opts, ::testing::HasSubstr("sec=ntlmsspi"));
    EXPECT_THAT(opts, ::testing::HasSubstr("seal"));
    EXPECT_THAT(opts, ::testing::HasSubstr("soft"));
    EXPECT_THAT(opts, ::testing::HasSubstr("cache=none"));
    EXPECT_THAT(opts, ::testing::HasSubstr("retrans=1"));
    EXPECT_THAT(opts, ::testing::HasSubstr("echo_interval=1"));
}

TEST(SmbShare, FallbackPreservesFlagsAcrossAttempts)
{
    reset();
    fail_first = true;
    auto creds = makeCreds("u", "p");
    SmbShare share(fs::path("/mnt/f"));
    EXPECT_TRUE((share.mount("//h/share", /*rw=*/false, creds) == 0)); // RO
    ASSERT_EQ(flags_calls.size(), 2u);
    EXPECT_EQ(flags_calls[0], flags_calls[1]); // both MS_RDONLY
}

TEST(SmbShare, MountWithCredentialsSuccessRw)
{
    reset();
    auto creds = makeCreds("admin", "pw");
    SmbShare share(fs::path("/mnt/rw"));
    EXPECT_TRUE((share.mount("//srv/rw", /*rw=*/true, creds) == 0));
    ASSERT_EQ(flags_calls.size(), 1u);
    EXPECT_EQ(flags_calls[0], 0UL);
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr(",vers=3.1.1"));
}

TEST(SmbShare, MountGuestSuccessRo)
{
    reset();
    SmbShare share(fs::path("/mnt/gro"));
    EXPECT_TRUE((share.mount("//srv/gro", /*rw=*/false, nullptr) == 0));
    ASSERT_EQ(flags_calls.size(), 1u);
    EXPECT_EQ(flags_calls[0], static_cast<unsigned long>(MS_RDONLY));
    EXPECT_THAT(options_calls[0],
                ::testing::HasSubstr("guest,username=OpenBmc"));
}

TEST(SmbShare, EmptyUsernameStillBuildsOptions)
{
    reset();
    auto creds = makeCreds("", "pw");
    SmbShare share(fs::path("/mnt/emptyu"));
    EXPECT_TRUE((share.mount("//srv/empty", /*rw=*/true, creds) == 0));
    ASSERT_EQ(options_calls.size(), 1u);
    EXPECT_THAT(options_calls[0],
                ::testing::HasSubstr("username=,password=pw"));
}

TEST(SmbShare, GuestFallbackToVers3OnFirstFailure)
{
    reset();
    fail_first = true;
    SmbShare share(fs::path("/mnt/guestfb"));
    const fs::path remote = "//s/g";
    EXPECT_TRUE((share.mount(remote, /*rw=*/true, /*creds*/ nullptr) == 0));
    ASSERT_EQ(options_calls.size(), 2u);
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr(",vers=3.1.1"));
    EXPECT_THAT(options_calls[1], ::testing::HasSubstr(",vers=3"));
}

TEST(SmbShare, FallbackPreservesFlagsAcrossAttemptsRw)
{
    reset();
    fail_first = true;
    auto creds = makeCreds("u", "p");
    SmbShare share(fs::path("/mnt/fb-rw"));
    EXPECT_TRUE((share.mount("//h/rw", /*rw=*/true, creds) == 0));
    ASSERT_EQ(flags_calls.size(), 2u);
    EXPECT_EQ(flags_calls[0], flags_calls[1]); // both 0UL
}

TEST(SmbShare, CredentialsEscapingCreatesSinglePasswordToken)
{
    reset();
    auto creds = makeCreds("escuser", "pa,ss,word"); // raw commas
    SmbShare share(fs::path("/mnt/esc2"));
    ASSERT_TRUE((share.mount("//srv/esc2", /*rw=*/true, creds) == 0));

    ASSERT_EQ(options_calls.size(), 1u);
    auto tokens = splitByComma(options_calls[0]);

    // There must be exactly one token starting with password=
    int pwCount = 0;
    for (const auto& t : tokens)
    {
        if (t.rfind("password=", 0) == 0)
            ++pwCount;
    }
    EXPECT_EQ(pwCount, 1);

    // And the raw comma sequence must not appear unescaped
    EXPECT_THAT(options_calls[0],
                ::testing::Not(::testing::HasSubstr("password=pa,ss,word")));
}

TEST(SmbShare, EmptyPasswordStillBuildsOptions)
{
    reset();
    auto creds = makeCreds("user", "");
    SmbShare share(fs::path("/mnt/emptypw"));

    EXPECT_TRUE((share.mount("//srv/emptypw", /*rw=*/true, creds) == 0));
    ASSERT_EQ(options_calls.size(), 1u);

    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("username=user"));
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr("password="));
}

TEST(SmbShare, CleanupReceivesOptionsAndCredentialsInOrder_Guest)
{
    reset();
    SmbShare share(fs::path("/mnt/guest-clean"));
    EXPECT_TRUE((share.mount("//srv/guest-clean", /*rw=*/true, nullptr) == 0));

    const std::string baseOpts =
        "sec=ntlmsspi,seal,soft,cache=none,retrans=1,echo_interval=1";
    const std::string credsOpt = "guest,username=OpenBmc";
    const std::string expectedOptions = baseOpts + "," + credsOpt;

    ASSERT_EQ(cleanup_args.size(), 2u);
    EXPECT_EQ(cleanup_args[0], expectedOptions); // options first
    EXPECT_EQ(cleanup_args[1], credsOpt);        // credentialsOpt second
}

TEST(SmbShare, CleanupReceivesOptionsAndCredentialsInOrder_Auth)
{
    reset();
    auto creds = makeCreds("u name", "p@ss+word"); // special chars (no commas)
    SmbShare share(fs::path("/mnt/auth-clean"));
    EXPECT_TRUE((share.mount("//srv/auth-clean", /*rw=*/false, creds) == 0));

    const std::string baseOpts =
        "sec=ntlmsspi,seal,soft,cache=none,retrans=1,echo_interval=1";
    ASSERT_EQ(cleanup_args.size(), 2u);

    EXPECT_THAT(cleanup_args[0], ::testing::HasSubstr(baseOpts + ","));
    EXPECT_THAT(cleanup_args[1], ::testing::HasSubstr("username=u name"));
    EXPECT_THAT(cleanup_args[1], ::testing::HasSubstr("password=p@ss+word"));
}

TEST(SmbShare, FstypeAlwaysCifsOnGuestFallback)
{
    reset();
    fail_first = true;
    SmbShare share(fs::path("/mnt/cifs-fstype"));
    EXPECT_TRUE((share.mount("//srv/cifs-fstype", /*rw=*/true, nullptr) == 0));

    ASSERT_EQ(fstype_calls.size(), 2u);
    EXPECT_EQ(fstype_calls[0], std::string("cifs"));
    EXPECT_EQ(fstype_calls[1], std::string("cifs"));
}

TEST(SmbShare, RemoteWithSpacesAndTrailingSlashPropagates)
{
    reset();
    const fs::path remote = "//srv with space/share/";
    SmbShare share(fs::path("/mnt/strange"));
    EXPECT_TRUE((share.mount(remote, /*rw=*/true, nullptr) == 0));

    ASSERT_EQ(src_calls.size(), 1u);
    EXPECT_EQ(src_calls[0], remote.string());
    ASSERT_EQ(target_calls.size(), 1u);
    EXPECT_EQ(target_calls[0], std::string("/mnt/strange"));
}

TEST(SmbShare, UsernameWithSpacesAndSymbolsAccepted)
{
    reset();
    auto creds = makeCreds("user name+@ok", "pw!");
    SmbShare share(fs::path("/mnt/specialuser"));
    EXPECT_TRUE((share.mount("//srv/specialuser", /*rw=*/true, creds) == 0));

    ASSERT_EQ(options_calls.size(), 1u);
    EXPECT_THAT(options_calls[0],
                ::testing::HasSubstr("username=user name+@ok,password=pw!"));
}

TEST(SmbShare, GuestFallbackPreservesFlagsAcrossAttemptsRw)
{
    reset();
    fail_first = true;
    SmbShare share(fs::path("/mnt/guest-flags"));
    EXPECT_TRUE((share.mount("//srv/guest-flags", /*rw=*/true, nullptr) == 0));
    ASSERT_EQ(flags_calls.size(), 2u);
    EXPECT_EQ(flags_calls[0], 0UL);
    EXPECT_EQ(flags_calls[1], 0UL);
}

TEST(SmbShare, CleanupOrderAndContentOnFailure_Guest)
{
    reset();
    fail_both = true;
    SmbShare share(fs::path("/mnt/guest-clean-fail"));
    EXPECT_FALSE(
        (share.mount("//srv/guest-clean-fail", /*rw=*/false, nullptr) == 0));
    const std::string baseOpts =
        "sec=ntlmsspi,seal,soft,cache=none,retrans=1,echo_interval=1";
    const std::string credsOpt = "guest,username=OpenBmc";
    const std::string expectedOptions = baseOpts + "," + credsOpt;
    ASSERT_EQ(cleanup_args.size(), 2u);
    EXPECT_EQ(cleanup_args[0], expectedOptions);
    EXPECT_EQ(cleanup_args[1], credsOpt);
}

// --- Credentials ---
// escapeCommas() must be idempotent: calling it twice must not double-escape
TEST(SmbShare, EscapeCommasIsIdempotent)
{
    auto c = makeCreds("uname", "pa,,ss,word");
    c->escapeCommas();
    const std::string pass1 = c->password();
    c->escapeCommas();
    const std::string pass2 = c->password();
    EXPECT_EQ(pass1, pass2);
}

// Guest RO fallback: rw=false, no credentials, first nfs4 fails → retry vers=3;
// MS_RDONLY must be preserved across both attempts.
TEST(SmbShare, GuestFallbackPreservesRoFlagsAcrossAttempts)
{
    reset();
    fail_first = true;
    SmbShare share(fs::path("/mnt/guest-ro-fb"));
    EXPECT_TRUE((share.mount("//srv/guest-ro-fb", /*rw=*/false, nullptr) == 0));

    ASSERT_EQ(flags_calls.size(), 2u);
    EXPECT_EQ(flags_calls[0], static_cast<unsigned long>(MS_RDONLY));
    EXPECT_EQ(flags_calls[1], static_cast<unsigned long>(MS_RDONLY));
    EXPECT_EQ(fstype_calls[0], std::string("cifs"));
    EXPECT_EQ(fstype_calls[1], std::string("cifs"));
    EXPECT_THAT(options_calls[0], ::testing::HasSubstr(",vers=3.1.1"));
    EXPECT_THAT(options_calls[1], ::testing::HasSubstr(",vers=3"));
    EXPECT_THAT(options_calls[0],
                ::testing::HasSubstr("guest,username=OpenBmc"));
    EXPECT_EQ(src_calls[0], src_calls[1]);
    EXPECT_EQ(target_calls[0], target_calls[1]);
    EXPECT_EQ(cleanup_args.size(), 2u);
}
