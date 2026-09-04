// SPDX-License-Identifier: Apache-2.0
/**
 * @file credentials_destructor_test.cpp
 * @brief Security-critical test validating credential memory wiping in
 * Credentials destructor
 *
 * This test validates that the Credentials class properly wipes sensitive
 * credential data from memory when the object is destroyed. This is a critical
 * security requirement to prevent credential leakage via memory dumps, core
 * dumps, or swap files.
 *
 * Testing Strategy:
 * - **Spy Pattern**: Intercept explicit_bzero() calls using C linkage
 * - **Macro Substitution**: Replace explicit_bzero with spy BEFORE including
 * credentials.hpp
 * - **TU Isolation**: Test must be in separate translation unit for macro to
 * take effect
 * - **Prefix Matching**: Verify buffer contents before wiping
 * - **Call Counting**: Ensure destructor wipes both username AND password
 *
 * Security Requirements:
 * 1. Destructor MUST call explicit_bzero() on username buffer
 * 2. Destructor MUST call explicit_bzero() on password buffer
 * 3. Buffers must be wiped with correct sizes (full capacity)
 * 4. Wiping must occur even if asyncWrite() was never called
 * 5. Wiping must use explicit_bzero() to prevent compiler optimization
 *
 * Why explicit_bzero()?
 * - Standard memset() can be optimized away by compiler (dead store
 * elimination)
 * - explicit_bzero() guarantees memory is actually zeroed
 * - Critical for security-sensitive data like passwords
 * - Required by security best practices (OWASP, CWE-14)
 *
 * Test Mechanism:
 * 1. Define explicit_bzero_spy() with C linkage
 * 2. Spy captures buffer pointer and verifies contents before wiping
 * 3. Spy counts calls (should be exactly 2: user + pass)
 * 4. Macro redirect: #define explicit_bzero explicit_bzero_spy
 * 5. Include credentials.hpp (now uses spy instead of real function)
 * 6. Create Credentials object with large strings (1024/2048 bytes)
 * 7. Let object go out of scope → destructor triggered
 * 8. Verify spy was called twice with correct data
 *
 * Why Large Strings (1024/2048 bytes)?
 * - std::string uses SSO (Small String Optimization) for small strings
 * - Large strings force heap allocation with non-zero capacity()
 * - Ensures destructor has actual buffer to wipe
 * - Tests realistic production scenario (URLs can be long)
 *
 * Expected Behavior:
 * - First call: Wipes username buffer (1024+ bytes)
 * - Second call: Wipes password buffer (2048+ bytes)
 * - Buffer contents verified before wiping (prefix match)
 * - All bytes set to zero after wiping
 *
 * Test Assertions:
 * 1. g_bzero_calls == 2 (both buffers wiped)
 * 2. g_user_prefix_match == true (username verified before wipe)
 * 3. g_pass_prefix_match == true (password verified before wipe)
 *
 * Related Security Standards:
 * - CWE-14: Compiler Removal of Code to Clear Buffers
 * - CWE-244: Improper Clearing of Heap Memory Before Release
 * - OWASP: Sensitive Data Exposure
 * - NIST SP 800-88: Guidelines for Media Sanitization
 *
 * @note This test MUST be in its own translation unit (TU) for macro to work
 * @note Spy pattern allows testing without modifying production code
 * @note Failure of this test indicates CRITICAL security vulnerability
 */

#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

//=============================================================================
// Spy State (Translation Unit Local)
//=============================================================================

namespace
{
/** @brief Expected username for verification (set by test) */
std::string g_expected_user;

/** @brief Expected password for verification (set by test) */
std::string g_expected_pass;

/** @brief Counter for explicit_bzero calls (should be 2) */
int g_bzero_calls = 0;

/** @brief Flag indicating username buffer matched before wipe */
bool g_user_prefix_match = false;

/** @brief Flag indicating password buffer matched before wipe */
bool g_pass_prefix_match = false;
} // namespace

//=============================================================================
// Spy Function (C Linkage for Interception)
//=============================================================================

/**
 * @brief Spy replacement for explicit_bzero() to validate destructor behavior
 *
 * This function intercepts calls to explicit_bzero() during Credentials
 * destruction. It verifies that buffers contain expected data before wiping,
 * then performs the actual memory clearing.
 *
 * Call Sequence:
 * 1. First call (g_bzero_calls == 0): Username buffer
 * 2. Second call (g_bzero_calls == 1): Password buffer
 *
 * @param p Pointer to buffer to be wiped
 * @param n Size of buffer in bytes
 *
 * @note Must use C linkage to match libc function signature
 * @note Actually wipes memory to preserve semantics (std::memset)
 */
extern "C" void explicit_bzero_spy(void* p, size_t n)
{
    // First call: Verify username buffer
    if (g_bzero_calls == 0)
    {
        if (!g_expected_user.empty())
        {
            g_user_prefix_match = std::memcmp(p, g_expected_user.data(),
                                              g_expected_user.size()) == 0;
        }
        else
        {
            g_user_prefix_match = true; // Empty string case
        }
    }
    // Second call: Verify password buffer
    else if (g_bzero_calls == 1)
    {
        if (!g_expected_pass.empty())
        {
            g_pass_prefix_match = std::memcmp(p, g_expected_pass.data(),
                                              g_expected_pass.size()) == 0;
        }
        else
        {
            g_pass_prefix_match = true; // Empty string case
        }
    }

    // Perform actual wipe to preserve destructor semantics
    std::memset(p, 0, n);
    ++g_bzero_calls;
}

//=============================================================================
// Macro Redirection (MUST be before #include "credentials.hpp")
//=============================================================================

/**
 * @def explicit_bzero
 * @brief Redirect explicit_bzero to spy function for this translation unit
 *
 * This macro substitution allows interception of explicit_bzero() calls
 * without modifying the production code. The redirection only affects
 * this translation unit.
 */
#define explicit_bzero explicit_bzero_spy
#include "credentials.hpp" // Now uses spy instead of real explicit_bzero
#undef explicit_bzero

//=============================================================================
// Security Test Case
//=============================================================================

/**
 * @test CredentialsTest.DestructorWipesSensitiveBuffers
 * @brief Verify Credentials destructor securely wipes username and password
 * from memory
 *
 * This is a security-critical test that validates proper credential cleanup.
 * Failure indicates a serious security vulnerability (CWE-14, CWE-244).
 *
 * Test Scenario:
 * 1. Create Credentials object in limited scope
 * 2. Populate with large username (1024 bytes) and password (2048 bytes)
 * 3. Call asyncWrite() to initialize internal buffers
 * 4. Let object go out of scope → destructor runs
 * 5. Spy intercepts explicit_bzero() calls
 * 6. Verify both buffers were wiped
 *
 * Expected Behavior:
 * - Destructor calls explicit_bzero() exactly twice
 * - First call wipes username buffer (verified via prefix match)
 * - Second call wipes password buffer (verified via prefix match)
 * - All credential data zeroed before memory release
 *
 * Why This Matters:
 * - Prevents credential leakage via core dumps
 * - Prevents credential exposure in swap space
 * - Prevents credential recovery from heap memory
 * - Required for FIPS compliance and security certifications
 *
 * @note Large strings (1024/2048 bytes) force heap allocation
 * @note SSO (Small String Optimization) would prevent wiping for tiny strings
 * @note Test runs I/O loop to ensure asyncWrite completes initialization
 */
TEST(CredentialsTest, DestructorWipesSensitiveBuffers)
{
    using std::string;

    // Large strings to ensure heap allocation (avoid SSO)
    // Real-world credentials can be long (URLs, tokens, etc.)
    const string u(1024, 'U'); // Username: 1024 'U' characters
    const string p(2048, 'P'); // Password: 2048 'P' characters

    // Initialize spy state
    g_expected_user = u;
    g_expected_pass = p;
    g_bzero_calls = 0;
    g_user_prefix_match = false;
    g_pass_prefix_match = false;

    {
        // Limited scope for Credentials object
        boost::asio::io_context io;
        Credentials creds(io);

        // Populate internal user/pass buffers
        // asyncWrite() copies username/password into internal std::string
        // members Assignment happens synchronously before async I/O scheduling
        creds.asyncWrite(string(u), string(p),
                         [&](const boost::system::error_code&, std::size_t) {
                             // Callback not relevant for destructor test
                         });

        // Run I/O loop to complete initialization
        // Not strictly necessary for destructor test, but ensures clean state
        io.run();

        // Object goes out of scope here → destructor called
        // Destructor should call explicit_bzero_spy() twice
    }

    // Verify destructor behavior
    EXPECT_EQ(g_bzero_calls, 2)
        << "SECURITY FAILURE: Destructor must wipe both username and password buffers";

    EXPECT_TRUE(g_user_prefix_match)
        << "SECURITY FAILURE: Username buffer verification failed - buffer may not contain expected data";

    EXPECT_TRUE(g_pass_prefix_match)
        << "SECURITY FAILURE: Password buffer verification failed - buffer may not contain expected data";
}

// -----------------------------------------------------------------------------
// Destructor wipes even when asyncWrite() was never called (empty user/pass)
// -----------------------------------------------------------------------------
TEST(CredentialsTest, DestructorWipesEvenWithoutAsyncWrite)
{
    g_bzero_calls = 0;
    g_expected_user.clear();
    g_expected_pass.clear();
    g_user_prefix_match = false;
    g_pass_prefix_match = false;

    {
        boost::asio::io_context io;
        Credentials creds(io);
        // asyncWrite never called — user and pass remain default-constructed
    }

    // Destructor must still call explicit_bzero twice (once per buffer),
    // even when capacity() == 0 (no-op wipe is still correct and safe).
    EXPECT_EQ(g_bzero_calls, 2)
        << "SECURITY FAILURE: Destructor must wipe both buffers even when asyncWrite was never called";

    EXPECT_TRUE(g_user_prefix_match)
        << "Empty username buffer should match (spy allows empty string)";

    EXPECT_TRUE(g_pass_prefix_match)
        << "Empty password buffer should match (spy allows empty string)";
}
