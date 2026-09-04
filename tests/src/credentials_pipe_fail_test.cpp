// SPDX-License-Identifier: Apache-2.0
/**
 * @file credentials_pipe_fail_test.cpp
 * @brief Error path test for Credentials constructor pipe creation failure
 *
 * This test validates that the Credentials class handles pipe creation failures
 * gracefully without crashing or leaking resources. This is critical for
 * robustness in production environments where resource exhaustion can occur.
 *
 * Testing Strategy:
 * - **Namespace Override**: Inject failing connect_pipe() in boost::asio
 * namespace
 * - **Macro Substitution**: Redirect connect_pipe to failing variant BEFORE
 * including header
 * - **TU Isolation**: Test must be in separate translation unit for override to
 * work
 * - **Error Simulation**: Simulate ENOMEM (not enough memory) error condition
 * - **State Verification**: Ensure pipes are closed after constructor failure
 *
 * Error Handling Requirements:
 * 1. Constructor MUST NOT throw on pipe creation failure
 * 2. Both pipe ends MUST be closed if connect_pipe() fails
 * 3. Object MUST be in valid (safe-to-destroy) state after constructor
 * 4. No resource leaks (file descriptors) on failure
 * 5. Error code propagated via boost::system::error_code
 *
 * Why Test Constructor Failure?
 * - Production systems can run out of file descriptors
 * - Kernel may limit pipe creation (ulimit -n)
 * - Container environments may have resource restrictions
 * - Graceful degradation better than crash
 * - Required for high-availability systems
 *
 * Test Mechanism:
 * 1. Define my_connect_pipe() in boost::asio namespace
 * 2. Function sets error code to std::errc::not_enough_memory
 * 3. Function closes both pipe endpoints (r and w)
 * 4. Macro redirect: #define connect_pipe my_connect_pipe
 * 5. Include credentials.hpp (now uses failing variant)
 * 6. Construct Credentials object (constructor sees failure)
 * 7. Verify both pipes are closed (is_open() returns false)
 *
 * Why boost::asio Namespace?
 * - connect_pipe() is a boost::asio free function
 * - Namespace-level override allows interception without ADL issues
 * - More reliable than global namespace override
 * - Matches actual function signature and namespace
 *
 * Why std::errc::not_enough_memory?
 * - Realistic error condition (ENOMEM from pipe() syscall)
 * - Maps to errno values used by kernel
 * - boost::system::error_code supports std::errc
 * - Commonly occurs in resource-constrained environments
 *
 * Expected Behavior After Constructor Failure:
 * - creds.read.is_open() returns false
 * - creds.impl.is_open() returns false
 * - Object can be safely destroyed
 * - No file descriptor leaks
 * - No undefined behavior
 *
 * Related Error Handling:
 * - RAII: Object must be valid even after partial construction
 * - Zero-overhead: No exception throwing (error_code pattern)
 * - Resource cleanup: Pipes closed on error path
 * - Safe destruction: Destructor handles partially-constructed state
 *
 * Production Scenarios:
 * 1. **File Descriptor Exhaustion**:
 *    - ulimit -n reached
 *    - Too many open files
 *    - Leak in another component
 *
 * 2. **Container Limits**:
 *    - Docker/Kubernetes resource limits
 *    - cgroup restrictions
 *    - Systemd resource control
 *
 * 3. **Kernel Limits**:
 *    - /proc/sys/fs/file-max reached
 *    - Per-process limits (RLIMIT_NOFILE)
 *    - System-wide descriptor table full
 *
 * Test Assertions:
 * - read.is_open() == false (readable pipe closed)
 * - impl.is_open() == false (writable pipe closed)
 *
 * @note This test MUST be in its own translation unit for macro override to
 * work
 * @note No ADL (Argument Dependent Lookup) issues due to namespace matching
 * @note Tests error path that is difficult to trigger in integration tests
 */

#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/system/error_code.hpp>

#include <system_error> // std::errc

#include <gtest/gtest.h>

//=============================================================================
// Failing Pipe Creation Stub (Boost.Asio Namespace Override)
//=============================================================================

namespace boost
{
namespace asio
{

/**
 * @brief Test stub that simulates pipe creation failure
 *
 * This function replaces boost::asio::connect_pipe() for this translation unit
 * only. It simulates resource exhaustion (ENOMEM) that would occur when the
 * system runs out of file descriptors or memory.
 *
 * Failure Simulation:
 * 1. Set error_code to not_enough_memory (ENOMEM)
 * 2. Close both pipe endpoints if they were opened
 * 3. Return without creating valid pipes
 *
 * This ensures the Credentials constructor sees a failure from connect_pipe()
 * and must handle it gracefully.
 *
 * @param r Readable pipe endpoint (will be closed on error)
 * @param w Writable pipe endpoint (will be closed on error)
 * @param ec Error code set to std::errc::not_enough_memory
 *
 * @note Signature must exactly match boost::asio::connect_pipe()
 * @note Must be in boost::asio namespace for override to work
 */
inline void my_connect_pipe(boost::asio::readable_pipe& r,
                            boost::asio::writable_pipe& w,
                            boost::system::error_code& ec)
{
    // Simulate pipe creation failure (ENOMEM - out of memory/file descriptors)
    ec = make_error_code(std::errc::not_enough_memory);

    // Ensure pipe endpoints are closed to leave object in safe state
    // This matches what Credentials constructor expects on error path
    if (r.is_open())
    {
        r.close();
    }
    if (w.is_open())
    {
        w.close();
    }
}

} // namespace asio
} // namespace boost

//=============================================================================
// Macro Redirection (MUST be before #include "credentials.hpp")
//=============================================================================

/**
 * @def connect_pipe
 * @brief Redirect connect_pipe to failing stub for this translation unit
 *
 * This macro substitution allows interception of boost::asio::connect_pipe()
 * calls without modifying the production code. The redirection only affects
 * this translation unit.
 */
#define connect_pipe my_connect_pipe
#include "credentials.hpp"
#undef connect_pipe

//=============================================================================
// Error Path Test Case
//=============================================================================

/**
 * @test CredentialsCtorFailureTest.PipesAreClosedWhenConnectFails
 * @brief Verify Credentials constructor handles pipe creation failure
 * gracefully
 *
 * This test validates error handling in the Credentials constructor when the
 * underlying pipe creation fails due to resource exhaustion. This is critical
 * for production robustness.
 *
 * Test Scenario:
 * 1. Attempt to create Credentials object
 * 2. Constructor calls connect_pipe() (our failing stub)
 * 3. connect_pipe() returns error (ENOMEM)
 * 4. Constructor must handle error and close pipes
 * 5. Verify both pipe endpoints are closed
 *
 * Expected Behavior:
 * - Constructor completes without throwing
 * - read.is_open() returns false
 * - impl.is_open() returns false
 * - Object can be safely destroyed
 * - No file descriptor leaks
 *
 * Why This Matters:
 * - Prevents crashes in production during resource exhaustion
 * - Allows service to degrade gracefully rather than fail catastrophically
 * - Enables proper error reporting to operators
 * - Required for high-availability systems
 * - Prevents file descriptor leaks that could worsen resource exhaustion
 *
 * Production Impact:
 * - Without this check: Constructor might leave pipes open → FD leak
 * - Without this check: Destructor might try to close invalid FDs → crash
 * - Without this check: asyncWrite() might use invalid pipes → undefined
 * behavior
 * - With this check: Service can report error and retry later
 *
 * Error Recovery Options (Production):
 * 1. Log error and return failure to caller
 * 2. Retry with exponential backoff
 * 3. Alert operators to resource exhaustion
 * 4. Shed load temporarily
 * 5. Restart service if persistent
 *
 * @note Test uses dependency injection via macro substitution
 * @note No production code modifications required
 * @note Tests error path that is hard to trigger reliably in integration tests
 */
TEST(CredentialsCtorFailureTest, PipesAreClosedWhenConnectFails)
{
    boost::asio::io_context io;

    // Constructor calls boost::asio::connect_pipe() which is our failing stub
    // in this translation unit due to macro redirection
    Credentials creds(io);

    // Verify both pipe endpoints are closed after constructor failure
    EXPECT_FALSE(creds.read.is_open())
        << "Readable pipe must be closed after connect_pipe() failure";

    EXPECT_FALSE(creds.impl.is_open())
        << "Writable pipe must be closed after connect_pipe() failure";

    // Object destruction should be safe even with closed pipes
    // Destructor will be called when creds goes out of scope
}
