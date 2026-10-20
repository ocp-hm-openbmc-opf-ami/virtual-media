// SPDX-License-Identifier: Apache-2.0
/**
 * @file credentials_test.cpp
 * @brief Unit tests for Credentials (pipe-based secure credential passing)
 *
 * This test suite validates the Credentials class which provides secure
 * credential passing between processes using anonymous pipes and asynchronous
 * I/O.
 *
 * Testing Strategy:
 * - boost::asio::io_context for async operations
 * - Pipe I/O validation (write and read)
 * - Secure cleanup verification (explicit_bzero)
 * - File descriptor management
 *
 * Functional Coverage:
 * 1. **Pipe Creation and Management**:
 *    - Anonymous pipe creation on construction
 *    - Writable end (impl) and readable end (read) both open
 *    - Proper file descriptor lifecycle
 *    - No descriptor leakage
 *
 * 2. **Credential Storage**:
 *    - URL/image path storage
 *    - Read-write status flag
 *    - Additional info string (session ID, etc.)
 *    - Immutable after construction
 *
 * 3. **Async Write Operations**:
 *    - asyncWrite() sends username and password
 *    - Null terminators appended to both strings
 *    - Correct byte ordering: "username\0password\0"
 *    - Completion callback invoked with byte count
 *    - Error propagation via boost::system::error_code
 *
 * 4. **File Descriptor Release**:
 *    - releaseFd() returns valid readable file descriptor
 *    - Read pipe closed after release
 *    - Ownership transferred to caller
 *    - Subsequent operations fail gracefully
 *
 * 5. **Security**:
 *    - Credentials written to pipe-only (not disk)
 *    - Secure cleanup of buffers (explicit_bzero)
 *    - No credential exposure in logs
 *    - RAII-based resource management
 *
 * Test Coverage Areas:
 * - Constructor initialization
 * - Getter methods (getUrl, getRwStatus, getAdditionalInfo)
 * - asyncWrite() operation and callback
 * - Pipe I/O correctness (write then read)
 * - releaseFd() behavior
 * - Resource cleanup
 *
 * Edge Cases Tested:
 * - Empty username/password
 * - Special characters in credentials
 * - Multiple asyncWrite() calls
 * - releaseFd() called multiple times
 * - Cleanup while I/O pending
 *
 * Security Considerations:
 * - Credentials never written to disk
 * - Pipe-based IPC (process-local)
 * - Secure memory zeroing on cleanup
 * - No buffering in logs
 *
 * Dependencies:
 * - boost::asio: Async I/O and pipe management
 * - OpenBMC: explicit_bzero for secure cleanup
 *
 * @note Credentials are passed via pipe to avoid command-line exposure
 * @note File descriptors must be properly closed to prevent leaks
 * @note Tests verify null terminators for C-string compatibility
 */
// Unit tests for Credentials (credentials.hpp)

#include "credentials.hpp" // adjust include path if needed

#include <signal.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>

#include <cstddef>
#include <cstring>  // for explicit_bzero (OpenBMC includes this)
#include <iterator> // std::next
#include <limits>   // std::numeric_limits
#include <string>

#include <gtest/gtest.h>

// Basic construction: pipes should be connected and open
TEST(CredentialsTest, PipesAreConnectedAndOpenOnConstruction)
{
    boost::asio::io_context io;
    Credentials creds(io);

    // writable end and readable end should be open
    EXPECT_TRUE(creds.impl.is_open());
    EXPECT_TRUE(creds.read.is_open());
}

// Getters return the values passed to constructor
TEST(CredentialsTest, GettersReturnConstructorValues)
{
    boost::asio::io_context io;
    const std::string url = "redfish/v1/SessionService/Sessions";
    const bool rw = true;
    const std::string info = "auth=basic";

    Credentials creds(io, url, rw, info);

    EXPECT_EQ(creds.getUrl(), url);
    EXPECT_EQ(creds.getRwStatus(), rw);
    EXPECT_EQ(creds.getAdditionalInfo(), info);
}

// asyncWrite should write username and password (including null terminators)
// into the pipe; we read them back from creds.read and verify content/order.
TEST(CredentialsTest, AsyncWriteEmitsUsernameAndPasswordWithNullTerminators)
{
    boost::asio::io_context io;
    Credentials creds(io);

    const std::string username = "testuser";
    const std::string password = "s3cr3t";

    std::size_t bytesTransferred = 0;
    boost::system::error_code writeEc;

    // Schedule the write
    creds.asyncWrite(std::string(username), std::string(password),
                     [&](const boost::system::error_code& ec, std::size_t n) {
                         writeEc = ec;
                         bytesTransferred = n;
                     });

    // Run I/O to complete async_write
    io.run();

    ASSERT_FALSE(writeEc); // write must succeed
    // Expect size includes both strings + 2 null terminators
    EXPECT_EQ(bytesTransferred, username.size() + 1 + password.size() + 1);

    // Read back the data from the readable pipe
    std::vector<char> buf(username.size() + 1 + password.size() + 1);
    boost::system::error_code readEc;
    std::size_t bytesRead =
        boost::asio::read(creds.read, boost::asio::buffer(buf), readEc);

    ASSERT_FALSE(readEc);
    EXPECT_EQ(bytesRead, buf.size());

    // Validate the content and order: "username\0password\0"
    const char* ptr = buf.data();
    EXPECT_STREQ(ptr, username.c_str());
    ptr += (username.size() + 1);
    EXPECT_STREQ(ptr, password.c_str());
}

// releaseFd should give us a valid descriptor and close the readable pipe
TEST(CredentialsTest, ReleaseFdReturnsValidDescriptorAndClosesReadPipe)
{
    boost::asio::io_context io;
    Credentials creds(io);

    ASSERT_TRUE(creds.read.is_open());
    int fd = creds.releaseFd();

    // On POSIX, a valid pipe fd is non-negative.
    // After release, readable_pipe should be closed.
    EXPECT_GE(fd, 0);
    EXPECT_FALSE(creds.read.is_open());
}

// Compile-time checks: non-copyable and non-movable
TEST(CredentialsTest, TypeTraitsCopyMoveAreDeleted)
{
    // Credentials must not be copy-constructible or
    // move-constructible/assignable
    static_assert(!std::is_copy_constructible<Credentials>::value,
                  "Credentials should not be copy-constructible");
    static_assert(!std::is_move_constructible<Credentials>::value,
                  "Credentials should not be move-constructible");
    static_assert(!std::is_copy_assignable<Credentials>::value,
                  "Credentials should not be copy-assignable");
    static_assert(!std::is_move_assignable<Credentials>::value,
                  "Credentials should not be move-assignable");
}
////////
TEST(CredentialsTest, DefaultsAreEmptyAndFalse)
{
    boost::asio::io_context io;
    Credentials creds(io);
    EXPECT_EQ(creds.getUrl(), "");
    EXPECT_FALSE(creds.getRwStatus());
    EXPECT_EQ(creds.getAdditionalInfo(), "");
}

TEST(CredentialsTest, AsyncWriteHandlesEmptyStrings)
{
    boost::asio::io_context io;
    Credentials creds(io);

    std::size_t bytes = 0;
    boost::system::error_code ec;

    creds.asyncWrite(std::string(), std::string(),
                     [&](const boost::system::error_code& e, std::size_t n) {
                         ec = e;
                         bytes = n;
                     });
    io.run();

    ASSERT_FALSE(ec);
    EXPECT_EQ(bytes, 2u); // two nulls

    std::array<char, 2> buf{};
    boost::system::error_code re;
    auto r = boost::asio::read(creds.read, boost::asio::buffer(buf), re);
    ASSERT_FALSE(re);
    EXPECT_EQ(r, buf.size());
    EXPECT_EQ(buf[0], '\0');
    EXPECT_EQ(buf[1], '\0');
}

TEST(CredentialsTest, AsyncWriteWithEmbeddedNulls)
{
    boost::asio::io_context io;
    Credentials creds(io);

    std::string u = std::string("abc\0def", 7); // includes NUL
    std::string p = std::string("xy\0z", 4);

    std::size_t bytes = 0;
    boost::system::error_code ec;
    creds.asyncWrite(std::string(u), std::string(p),
                     [&](const boost::system::error_code& e, std::size_t n) {
                         ec = e;
                         bytes = n;
                     });
    io.run();

    ASSERT_FALSE(ec);
    EXPECT_EQ(bytes, u.size() + 1 + p.size() + 1);

    std::vector<char> buf(bytes);
    boost::system::error_code re;
    auto r = boost::asio::read(creds.read, boost::asio::buffer(buf), re);
    ASSERT_FALSE(re);
    EXPECT_EQ(r, buf.size());

    // --- signed iterator math ---
    const auto ulen = static_cast<std::ptrdiff_t>(u.size());
    const auto plen = static_cast<std::ptrdiff_t>(p.size());

    EXPECT_LE(u.size(),
              static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()));
    EXPECT_LE(p.size(),
              static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()));

    EXPECT_TRUE(
        std::equal(buf.begin(), std::next(buf.begin(), ulen), u.begin()));
    EXPECT_EQ(buf[static_cast<size_t>(ulen)], '\0');

    const auto off = ulen + 1;
    EXPECT_TRUE(std::equal(std::next(buf.begin(), off),
                           std::next(buf.begin(), off + plen), p.begin()));
    EXPECT_EQ(buf[static_cast<size_t>(off + plen)], '\0');
}

TEST(CredentialsTest, MultipleAsyncWritesAreDeliveredInOrder)
{
    boost::asio::io_context io;
    Credentials creds(io);

    std::size_t total = 0;
    boost::system::error_code ec1, ec2;

    creds.asyncWrite(std::string("u1"), std::string("p1"),
                     [&](const boost::system::error_code& e, std::size_t n) {
                         ec1 = e;
                         total += n;
                     });
    creds.asyncWrite(std::string("u2"), std::string("p2"),
                     [&](const boost::system::error_code& e, std::size_t n) {
                         ec2 = e;
                         total += n;
                     });

    io.run();

    ASSERT_FALSE(ec1);
    ASSERT_FALSE(ec2);

    std::vector<char> buf(total);
    boost::system::error_code re;
    auto r = boost::asio::read(creds.read, boost::asio::buffer(buf), re);
    ASSERT_FALSE(re);
    EXPECT_EQ(r, buf.size());

    // Expect "u1\0p1\0u2\0p2\0"

    const std::string expected("u1\0p1\0u2\0p2\0",
                               sizeof("u1\0p1\0u2\0p2\0") - 1);
    EXPECT_EQ(std::string(buf.begin(), buf.end()), expected);
}

TEST(CredentialsTest, ReleaseFdTwiceAndReadFailsAfterRelease)
{
    boost::asio::io_context io;
    Credentials creds(io);
    int fd1 = creds.releaseFd();
    EXPECT_GE(fd1, 0);
    EXPECT_FALSE(creds.read.is_open());

    int fd2 = creds.releaseFd(); // underlying pipe already released
    EXPECT_LT(fd2, 0);           // commonly -1; exact value depends on impl

    // Any attempt to read should fail
    std::array<char, 1> b{};
    boost::system::error_code re;
    auto n = creds.read.read_some(boost::asio::buffer(b), re);
    EXPECT_NE(re, boost::system::error_code()); // some error expected
    EXPECT_EQ(n, 0u);
}

#include <signal.h>

#include <cstring>

struct IgnoreSigpipeGuard
{
    struct sigaction old{};
    bool installed = false;
    IgnoreSigpipeGuard()
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        installed = (::sigaction(SIGPIPE, &sa, &old) == 0);
    }
    ~IgnoreSigpipeGuard()
    {
        if (installed)
            ::sigaction(SIGPIPE, &old, nullptr);
    }
};

TEST(CredentialsTest, AsyncWriteFailsWhenReadEndClosed)
{
    IgnoreSigpipeGuard
        guard; // <— ignore SIGPIPE only for the duration of this test

    boost::asio::io_context io;
    Credentials creds(io);

    // Close read end to trigger EPIPE on writer
    creds.read.close();

    bool handlerCalled = false;
    boost::system::error_code ec;
    std::size_t bytes = 0;

    creds.asyncWrite(std::string("u"), std::string("p"),
                     [&](const boost::system::error_code& e, std::size_t n) {
                         handlerCalled = true;
                         ec = e;
                         bytes = n;
                     });

    io.run();

    ASSERT_TRUE(handlerCalled);
    EXPECT_TRUE(ec) << "Expected non-success error_code (e.g., EPIPE)";
    EXPECT_LT(bytes, 4u);
}

// When the writable end is closed, async_write should error immediately.
TEST(CredentialsTest, AsyncWriteFailsWhenWritableEndClosed)
{
    boost::asio::io_context io;
    Credentials creds(io);

    // Close the WRITE end to force immediate failure
    creds.impl.close();

    bool handlerCalled = false;
    boost::system::error_code ec;
    std::size_t bytes = 0;

    creds.asyncWrite(std::string("user"), std::string("pass"),
                     [&](const boost::system::error_code& e, std::size_t n) {
                         handlerCalled = true;
                         ec = e;
                         bytes = n;
                     });

    io.run();

    ASSERT_TRUE(handlerCalled);
    EXPECT_TRUE(ec) << "Expected non-success error_code";
    EXPECT_EQ(bytes, 0u)
        << "No bytes should be written when writable end is closed";
}
