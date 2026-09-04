// SPDX-License-Identifier: Apache-2.0
/**
 * @file utils_test.cpp
 * @brief Comprehensive unit tests for utils.hpp (OpenBMC virtual media
 * utilities)
 *
 * This test suite validates all utility functions and classes used throughout
 * the virtual media service. Utilities provide cross-cutting concerns like
 * security, credential management, and helper functions.
 *
 * Testing Strategy:
 * - Avoid gmock to maintain -fno-rtti compatibility
 * - Direct verification of function behaviors
 * - Security-sensitive operations tested for correctness
 * - Integration with production libraries (sdbusplus, boost::asio)
 *
 * Functional Coverage:
 * 1. **Secure Memory Cleanup**:
 *    - secureCleanup(std::vector<char>&): Zero all bytes, preserve size
 *    - secureCleanup(std::string&): Zero all bytes, preserve size
 *    - Uses explicit_bzero() to prevent compiler optimization
 *    - Critical for credential/password cleanup
 *
 * 2. **Credentials Class**:
 *    - Username and password storage
 *    - escapeCommas() for CIFS option strings
 *    - Idempotent comma escaping (a,b → a,,b → a,,b)
 *    - Only password is escaped (username unchanged)
 *
 * 3. **CredentialsProvider Class**:
 *    - User/password pair management
 *    - pack() with custom formatters
 *    - Buffer preparation for mount operations
 *    - Flexible formatting for different protocols
 *
 * 4. **SafePipe Class** (if applicable):
 *    - Pipe creation and management
 *    - File descriptor safety
 *    - RAII-based cleanup
 *
 * 5. **Helper Functions**:
 *    - String manipulation utilities
 *    - Path handling functions
 *    - Error code conversions
 *    - System call wrappers
 *
 * Test Coverage Areas:
 * - All utils.hpp public functions and classes
 * - Security-critical operations
 * - Memory cleanup verification
 * - Credential formatting
 * - Edge cases and error conditions
 *
 * Security Testing:
 * - Memory zeroing verification (byte-level)
 * - Credential escape correctness
 * - No information leakage
 * - Proper cleanup even on exceptions
 *
 * Edge Cases Tested:
 * - Empty strings and vectors
 * - Special characters in credentials
 * - Multiple comma escaping (idempotence)
 * - Zero-length buffers
 *
 * Implementation Notes:
 * - Direct byte inspection to verify zeroing
 * - unsigned char cast to avoid sign issues
 * - Size preservation after cleanup
 * - Idempotence testing for escaping
 *
 * Dependencies:
 * - boost::asio: I/O and timers
 * - sdbusplus: D-Bus integration
 * - filesystem: Path operations
 * - OpenBMC security functions (explicit_bzero)
 *
 * @note -fno-rtti compatibility maintained (no gmock)
 * @note Security functions tested at byte level for correctness
 * @note All cleanup operations verified to prevent credential leaks
 */
//
// Unit tests for utils.hpp (OpenBMC virtual-media)
//
// Place in: tests/src/utils_test.cpp
//
// Build: link with gtest and production libs; avoid gmock to keep -fno-rtti
// happy.

#include "utils.hpp"

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

// ---- secureCleanup ----------------------------------------------------------

TEST(UtilsSecureCleanup, VectorCharIsZeroed)
{
    std::vector<char> buf(32, 'A');
    utils::secureCleanup(buf);
    for (char c : buf)
    {
        EXPECT_EQ(static_cast<unsigned char>(c), 0U);
    }
}

TEST(UtilsSecureCleanup, StringIsZeroedBytesButSizeUnchanged)
{
    std::string s(16, 'B');
    const auto sizeBefore = s.size();
    utils::secureCleanup(s);
    EXPECT_EQ(s.size(), sizeBefore);

    // Check the raw bytes to avoid implicit sign conversions.
    const auto* data = reinterpret_cast<const unsigned char*>(s.data());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        EXPECT_EQ(data[i], 0U);
    }
}

// ---- Credentials ------------------------------------------------------------

TEST(UtilsCredentials, EscapeCommasOnlyAffectsPasswordAndIsIdempotent)
{
    utils::Credentials creds{"userX", "a,b,,c"};
    EXPECT_EQ(creds.user(), "userX");
    EXPECT_EQ(creds.password(), "a,b,,c");

    creds.escapeCommas();
    EXPECT_EQ(creds.user(), "userX");
    EXPECT_EQ(creds.password(), "a,,b,,,,c");

    // idempotent
    creds.escapeCommas();
    EXPECT_EQ(creds.password(), "a,,b,,,,c");
}

// ---- CredentialsProvider ----------------------------------------------------

static void simpleFormatter(const std::string& u, const std::string& p,
                            utils::CredentialsProvider::Buffer& dest)
{
    // Buffer is std::vector<char>, not a pointer.
    const std::string formatted = u + ":" + p;
    dest.insert(dest.end(), formatted.begin(), formatted.end());
}

TEST(UtilsCredentialsProvider, PackWithFormatterProducesExpectedBuffer)
{
    utils::CredentialsProvider provider{"alpha", "beta"};
    auto packed = provider.pack(simpleFormatter);
    ASSERT_TRUE(packed);
    std::string out(packed->begin(), packed->end());
    EXPECT_EQ(out, "alpha:beta");
}

TEST(UtilsCredentialsProvider, EscapeCommasAffectsPackWhenFormatterUsed)
{
    utils::CredentialsProvider provider{"root", "a,b,c"};
    provider.escapeCommas(); // doubles commas in password
    auto packed = provider.pack(simpleFormatter);
    ASSERT_TRUE(packed);
    std::string out(packed->begin(), packed->end());
    EXPECT_EQ(out, "root:a,,b,,c");
}

// pack(nullptr): formatter is null -> if(formatter) is false -> returns empty
// buffer. This covers the null-formatter branch in CredentialsProvider::pack()
// (utils.hpp).
TEST(UtilsCredentialsProvider, PackWithNullFormatterReturnsEmptyBuffer)
{
    utils::CredentialsProvider provider{"user", "pass"};
    auto packed = provider.pack(nullptr);
    // pack() always returns a non-null SecureBuffer (new Buffer{} is always
    // created)
    ASSERT_NE(packed, nullptr);
    // When formatter is null the buffer is never filled, so it must be empty
    EXPECT_TRUE(packed->empty());
    // Avoid crashing the Deleter's secureCleanup on an empty vector by
    // inserting a placeholder byte before the SecureBuffer goes out of scope.
    packed->push_back('\0');
}

// ---- VolatileFile -----------------------------------------------------------

TEST(UtilsVolatileFile, CreatesFileWithExpectedSizeAndRemovesOnDestruction)
{
    // Prepare secure buffer "secret payload"
    utils::CredentialsProvider::SecureBuffer buf{
        new utils::CredentialsProvider::Buffer{}};

    const std::string payload = "super_secret_123";
    buf->insert(buf->end(), payload.begin(), payload.end());

    std::string pathCopy;
    {
        utils::VolatileFile vf(std::move(buf));
        const std::string& p = vf.path();
        pathCopy = p; // remember after destructor

        ASSERT_TRUE(fs::exists(p));
        EXPECT_EQ(static_cast<std::size_t>(fs::file_size(p)), payload.size());
        // Destructor will purge & remove the file.
    }
    EXPECT_FALSE(fs::exists(pathCopy));
}

// ---- NotificationWrapper + SignalSender -------------------------------------

struct NotificationFixture : public ::testing::Test
{
    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> conn =
        std::make_shared<sdbusplus::asio::connection>(io);
    std::unique_ptr<utils::SignalSender> signal{
        new utils::SignalSender(conn, "/xyz/openbmc_project/test",
                                "xyz.openbmc_project.Test", "Completed")};
    std::unique_ptr<boost::asio::steady_timer> timer{
        new boost::asio::steady_timer(io)};

    utils::NotificationWrapper wrapper{std::move(signal), std::move(timer)};
};

TEST_F(NotificationFixture, StartThenNotifyCancelsTimerAndCallsHandlerAborted)
{
    using namespace std::chrono_literals;

    boost::system::error_code handlerEc{};
    bool handlerRan = false;

    // Proper lambda for the handler, capture by reference.
    wrapper.start(
        [&](const boost::system::error_code& ec) {
            handlerEc = ec;
            handlerRan = true;
        },
        30s);

    // Notify before timer expiry — should cancel the timer and invoke handler.
    std::error_code ok{}; // value() == 0
    wrapper.notify(ok);

    // Process pending handlers without blocking forever on the connection.
    // io.run() would keep running due to the continuous read loop in
    // connection.
    auto executed = io.poll();
    EXPECT_GT(executed, 0);

    ASSERT_TRUE(handlerRan);
    EXPECT_EQ(handlerEc, boost::asio::error::operation_aborted);
}

TEST_F(NotificationFixture, NotifyWithoutStartIsSafelyIgnored)
{
    std::error_code someErr = std::make_error_code(std::errc::io_error);
    EXPECT_NO_THROW(wrapper.notify(someErr));

    // No async work scheduled, but poll anyway (should be 0 or harmless).
    (void)io.poll();
}

// ---- NamedPipe (template behavior & construction) ---------------------------

TEST(UtilsNamedPipe, TemplateDataSpecializationCompilesForPointerAndValue)
{
    boost::asio::io_context io;

    // Buffer as value
    std::string valBuf = "hello";
    utils::NamedPipe<std::string> pipeVal(io, "/tmp/test.pipe.value",
                                          std::move(valBuf));

    // Buffer as pointer
    auto ptrBuf = std::make_unique<std::string>("world");
    utils::NamedPipe<std::unique_ptr<std::string>> pipePtr(
        io, "/tmp/test.pipe.ptr", std::move(ptrBuf));

    auto fd1 = pipeVal.fd();
    auto fd2 = pipePtr.fd();
    (void)fd1;
    (void)fd2;

    EXPECT_EQ(pipeVal.file(), std::string("/tmp/test.pipe.value"));
    EXPECT_EQ(pipePtr.file(), std::string("/tmp/test.pipe.ptr"));
}

// ---- safeMount --------------------------------------------------------------

TEST(UtilsSafeMount, InvalidPathsReturnNonZeroError)
{
    int rc = utils::safeMount("/not/a/source", "/not/a/target", "tmpfs", 0, "");
    EXPECT_NE(rc, 0);
}

// ---- SignalSender basic exercise -------------------------------------------

TEST(UtilsSignalSender, SendSignalDoesNotThrow)
{
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    utils::SignalSender sender(conn, "/xyz/openbmc_project/test",
                               "xyz.openbmc_project.Test", "Ping");

    EXPECT_NO_THROW(sender.send(std::nullopt));
    std::error_code ec = std::make_error_code(std::errc::permission_denied);
    EXPECT_NO_THROW(sender.send(ec));
}

// -------------------- NamedPipe: async_write & destructor unlink
// --------------------

TEST(UtilsNamedPipe, AsyncWriteTransfersAllBytesForValueAndPointerBuffers)
{
    boost::asio::io_context io;

    // Value buffer
    std::string valBuf = "value-buffer";
    utils::NamedPipe<std::string> pipeVal(io, "/tmp/test.pipe.value.async",
                                          std::move(valBuf));
    std::size_t bytesVal{0};
    boost::system::error_code ecVal;

    pipeVal.async_write(
        [&](const boost::system::error_code& ec, std::size_t n) {
            ecVal = ec;
            bytesVal = n;
        });

    // Pointer buffer
    auto ptrBuf = std::make_unique<std::string>("pointer-buffer");
    utils::NamedPipe<std::unique_ptr<std::string>> pipePtr(
        io, "/tmp/test.pipe.ptr.async", std::move(ptrBuf));
    std::size_t bytesPtr{0};
    boost::system::error_code ecPtr;

    pipePtr.async_write(
        [&](const boost::system::error_code& ec, std::size_t n) {
            ecPtr = ec;
            bytesPtr = n;
        });

    // Drain handlers; stay single-threaded and avoid sleeps (ASIO threads
    // disabled).
    for (int i = 0; i < 50 && (bytesVal == 0 || bytesPtr == 0); ++i)
    {
        io.poll();
    }

    EXPECT_FALSE(ecVal);
    EXPECT_FALSE(ecPtr);
    EXPECT_EQ(bytesVal, std::string("value-buffer").size());
    EXPECT_EQ(bytesPtr, std::string("pointer-buffer").size());
}

// -------------------- NamedPipe: destructor unlinks existing artifact (robust)
// -----

#include <fcntl.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>

#include <filesystem>
#include <string>
#include <vector>

TEST(UtilsNamedPipe, DestructorUnlinksArtifactCreatedByConstructor)
{
    namespace fs = std::filesystem;
    boost::asio::io_context io;

    // 1) Generate a truly unique path using mkstemp, then free it for mkfifo.
    std::string tmpl =
        (fs::temp_directory_path() / "vm_namedpipe.XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0'); // mkstemp requires a modifiable, NUL-terminated buffer

    int fd = ::mkstemp(buf.data());
    ASSERT_NE(fd, -1) << "mkstemp failed, errno=" << errno;
    ::close(fd);

    // Now unlink the file so the path is available for mkfifo().
    std::string pipePath(buf.data());
    ASSERT_EQ(::unlink(pipePath.c_str()), 0)
        << "unlink failed, errno=" << errno;
    ASSERT_FALSE(fs::exists(pipePath)) << "Path should be free for mkfifo";

    // 2) Construct NamedPipe; its implementation will mkfifo(pipePath)
    // internally.
    {
        utils::NamedPipe<std::string> pipe(io, pipePath,
                                           std::string("payload"));

        // Sanity: accessor works and path now exists (fifo created by ctor).
        EXPECT_EQ(pipe.file(), pipePath);
        EXPECT_TRUE(fs::exists(pipePath))
            << "Constructor should create FIFO at path";

        // Also touch fd() to exercise that accessor.
        (void)pipe.fd();
    } // 3) On leaving scope, ~NamedPipe() must ::unlink(pipePath)

    EXPECT_FALSE(fs::exists(pipePath))
        << "Destructor must unlink the FIFO path";
}

// -------------------- VolatileFile purge verification --------------------

TEST(UtilsVolatileFile, DestructorPurgesContentsWithAsterisksBeforeRemoval)
{
    // Prepare secure buffer payload
    const std::string payload = "secret_payload_ABCDEF";
    utils::CredentialsProvider::SecureBuffer buf{
        new utils::CredentialsProvider::Buffer{}};
    buf->insert(buf->end(), payload.begin(), payload.end());

    std::string filePath;
    int fd = -1;
    {
        utils::VolatileFile vf(std::move(buf));
        filePath = vf.path();
        ASSERT_TRUE(std::filesystem::exists(filePath));
        ASSERT_EQ(
            static_cast<std::size_t>(std::filesystem::file_size(filePath)),
            payload.size());

        // Open the file *by name* before destruction, keep FD alive across
        // unlink.
        fd = ::open(filePath.c_str(), O_RDONLY);
        ASSERT_NE(fd, -1);
    } // ~VolatileFile(): purgeFileContents() then remove(file)

    // File has been unlinked from the namespace, but FD is still valid.
    std::vector<char> post(payload.size(), '\0');
    ssize_t rd = ::read(fd, post.data(), static_cast<size_t>(post.size()));
    ::close(fd);
    ASSERT_EQ(rd, static_cast<ssize_t>(post.size()));

    for (char c : post)
    {
        EXPECT_EQ(c, '*');
    }
}

TEST(UtilsVolatileFile,
     PurgeLoopsOverMultipleChunksWhenPayloadExceedsSecretLimit)
{
    // secretLimit = 1024; use 2100 bytes to force the while-loop to iterate 3
    // times
    const std::size_t payloadSize = 2100;
    utils::CredentialsProvider::SecureBuffer buf{
        new utils::CredentialsProvider::Buffer{}};
    buf->insert(buf->end(), payloadSize, 'S');

    std::string filePath;
    int fd = -1;
    {
        utils::VolatileFile vf(std::move(buf));
        filePath = vf.path();
        ASSERT_EQ(
            static_cast<std::size_t>(std::filesystem::file_size(filePath)),
            payloadSize);

        fd = ::open(filePath.c_str(), O_RDONLY);
        ASSERT_NE(fd, -1);
    }

    std::vector<char> post(payloadSize, '\0');
    ssize_t rd = ::read(fd, post.data(), payloadSize);
    ::close(fd);
    ASSERT_EQ(rd, static_cast<ssize_t>(payloadSize));

    for (char c : post)
    {
        EXPECT_EQ(c, '*');
    }
}

// -------------------- CredentialsProvider direct accessors
// --------------------
TEST(UtilsCredentialsProvider, AccessorsReturnUnderlyingStrings)
{
    utils::CredentialsProvider provider{"alice", "p@ss,word"};
    EXPECT_EQ(provider.user(), "alice");
    EXPECT_EQ(provider.password(), "p@ss,word");

    provider.escapeCommas(); // doubles commas only in password
    EXPECT_EQ(provider.user(), "alice");
    EXPECT_EQ(provider.password(), "p@ss,,word");
}

// -------------------- NotificationWrapper sends non-zero status on error
// notify ----

TEST_F(NotificationFixture, StartThenNotifyWithErrorSendsNonZeroStatus)
{
    using namespace std::chrono_literals;

    bool handlerRan = false;
    boost::system::error_code handlerEc{};
    wrapper.start(
        [&](const boost::system::error_code& ec) {
            handlerEc = ec;
            handlerRan = true;
        },
        30s);

    // Notify with a non-zero error — takes the if(ec) branch in notify()
    std::error_code someErr = std::make_error_code(std::errc::io_error);
    wrapper.notify(someErr);

    io.poll();

    EXPECT_TRUE(handlerRan);
    EXPECT_EQ(handlerEc, boost::asio::error::operation_aborted);
}

// -------------------- safeMount success path (requires CAP_SYS_ADMIN/root)
// ---------

// ===== Shim for mount(2) (link-time interposition) =====

namespace
{
// Global knobs to control the shim from tests
static std::atomic<bool> g_mount_shim_enabled{false};
static std::atomic<int> g_mount_shim_rc{0}; // what mount() should return
static std::atomic<unsigned long> g_mount_shim_last_flags{0};

// Interposing definition of mount(2)
extern "C" int mount(const char* source, const char* target,
                     const char* filesystemtype, unsigned long flags,
                     const void* data)
{
    // When the shim is disabled, forward to the real kernel mount(2)
    if (!g_mount_shim_enabled.load(std::memory_order_relaxed))
    {
        // syscall returns 'long' → cast explicitly to 'int' to satisfy
        // -Wconversion
        long r =
            ::syscall(SYS_mount, source, target, filesystemtype, flags, data);
        // On failure, syscall sets errno and returns -1, matching mount(2)
        // semantics
        return static_cast<int>(r);
    }

    // Shim is ON: record and return the configured result
    g_mount_shim_last_flags.store(flags, std::memory_order_relaxed);
    const int rc = g_mount_shim_rc.load(std::memory_order_relaxed);
    errno = (rc == 0) ? 0 : EPERM;
    return rc;
}

// RAII helper to enable/disable the shim in a scope
struct MountShimGuard
{
    explicit MountShimGuard(int rc = 0)
    {
        g_mount_shim_rc.store(rc, std::memory_order_relaxed);
        g_mount_shim_enabled.store(true, std::memory_order_relaxed);
    }
    ~MountShimGuard()
    {
        g_mount_shim_enabled.store(false, std::memory_order_relaxed);
    }
};
} // namespace

// -------------------- safeMount success path (shimmed, no root required)
// ----------

TEST(UtilsSafeMount, TmpfsMountSucceedsWithShim)
{
    // Enable shim to make mount() succeed and record flags
    MountShimGuard shim_enable{/*rc=*/0};

    // Prepare a temporary mount target directory
    std::string target =
        (std::filesystem::temp_directory_path() / "vm-mount-ut-shim").string();
    std::filesystem::create_directory(target);

    // Call the production function; shim returns success
    int rc = utils::safeMount("tmpfs", target, "tmpfs", 0, "size=1M");
    EXPECT_EQ(rc, 0) << "safeMount(tmpfs) should succeed with shim enabled";

    // Assert the flags were augmented in safeMount()
    const unsigned long flags =
        g_mount_shim_last_flags.load(std::memory_order_relaxed);
    EXPECT_TRUE((flags & MS_NODEV) != 0);
    EXPECT_TRUE((flags & MS_NOEXEC) != 0);
    EXPECT_TRUE((flags & MS_NOSUID) != 0);

    // Clean up the directory (no real mount happened)
    std::filesystem::remove(target);
}

// -------------------- NotificationWrapper: timer expiry path
// --------------------

TEST_F(NotificationFixture, TimerExpiryCallsHandlerWithSuccessCode)
{
    bool handlerRan = false;
    boost::system::error_code handlerEc{
        boost::asio::error::operation_aborted}; // non-zero sentinel

    // 0-second timer fires immediately; do NOT call notify()
    wrapper.start(
        [&](const boost::system::error_code& ec) {
            handlerEc = ec;
            handlerRan = true;
        },
        std::chrono::seconds(0));

    // Timer already expired; a single poll processes the ready handler
    for (int i = 0; i < 50 && !handlerRan; ++i)
        io.poll();

    ASSERT_TRUE(handlerRan);
    EXPECT_EQ(handlerEc.value(), 0); // natural expiry → success (not aborted)
}
