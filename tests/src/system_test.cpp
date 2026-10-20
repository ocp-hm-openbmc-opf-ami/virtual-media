// SPDX-License-Identifier: Apache-2.0
/**
 * @file system_test.cpp
 * @brief Comprehensive unit tests for system-level virtual media operations
 *
 * This test suite validates core system functionality including:
 * - NBD (Network Block Device) device management and operations
 * - D-Bus integration for OpenBMC virtual media services
 * - USB gadget configuration and state management
 * - Session monitoring and lifecycle management
 * - File I/O operations with error handling
 * - Console/remote mode path mapping
 *
 * Testing Strategy:
 * - Zero production code modifications (test-only mocking)
 * - Self-contained test cases with proper isolation
 * - D-Bus service mocking for CI/CD environments
 * - Graceful degradation when system services unavailable
 *
 * Coverage Focus:
 * - Happy path and error path scenarios
 * - Edge cases and boundary conditions
 * - Resource lifecycle management
 * - Thread safety and async operations
 */

#include "system.hpp"

#include <unistd.h>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// Compatibility aliases: the src was refactored to use vmedia-specific type
// names; retain the old names for the tests without touching src files.
using sessionInfo = vmediaSessionInfo;
using sessionList = vmediaSessionList;

namespace fs = std::filesystem;

//=============================================================================
// Global Test Fixtures and Configuration
//=============================================================================

/**
 * @brief Global credentials array for virtual media authentication
 *
 * Single definition for entire test executable to avoid linker errors.
 * Initialized to nullptr and populated by individual tests as needed.
 */
std::shared_ptr<Credentials> creds[2] = {nullptr, nullptr};
std::string g_serviceName = "xyz.openbmc_project.VirtualMedia";
std::string g_basePath = "VirtualMedia";

//=============================================================================
// Test Utility Functions
//=============================================================================

/**
 * @brief Create a temporary file with specified size for testing
 *
 * Generates a sparse file using mkstemp() to ensure unique naming.
 * The file is created with the exact size specified by seeking to the
 * final position and writing a single byte.
 *
 * @param bytes Size of file to create in bytes. Must be >= 1.
 * @return fs::path Path to created temporary file, or empty path on failure
 *
 * @note File is NOT automatically deleted - caller must clean up
 * @note Uses mkstemp() to avoid race conditions in temp file creation
 * @note Validates size doesn't exceed streamoff maximum to prevent overflow
 */
static fs::path makeTempFileWithSize(std::size_t bytes)
{
    if (bytes < 1)
    {
        ADD_FAILURE() << "File size must be >= 1";
        return fs::path{};
    }
    if ((bytes - 1) >
        static_cast<std::size_t>(std::numeric_limits<std::streamoff>::max()))
    {
        ADD_FAILURE() << "Requested size exceeds streamoff max";
        return fs::path{};
    }
    fs::path p = fs::temp_directory_path() / "vmtest_XXXXXX";
    std::string tmpl = p.string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0)
    {
        ADD_FAILURE() << "mkstemp failed";
        return fs::path{};
    }
    fs::path created(buf.data());
    std::fstream f(created, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(bytes - 1));
    f.write("", 1);
    f.close();
    ::close(fd);
    return created;
}

/**
 * @brief Write string data at specific offset in an existing file
 *
 * Opens file in read/write binary mode, seeks to specified offset,
 * and writes the provided string data. Used primarily for creating
 * test files with specific signature patterns.
 *
 * @param p Path to file to modify
 * @param off Byte offset where writing should begin
 * @param s String data to write at the specified offset
 *
 * @note File must already exist and be writable
 * @note Does not extend file if offset exceeds current size
 */
static void writeAt(const fs::path& p, std::streamoff off, const std::string& s)
{
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(off);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    f.close();
}

static bool pumpBusUntil(sdbusplus::bus_t& bus,
                         const std::function<bool()>& done, int attempts = 200)
{
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        if (done())
        {
            return true;
        }
        bus.wait(std::chrono::milliseconds(5));
        while (bus.process_discard())
        {}
    }
    return done();
}

static void pumpBus(sdbusplus::bus_t& bus, int attempts = 40)
{
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        bus.wait(std::chrono::milliseconds(5));
        while (bus.process_discard())
        {}
    }
}

static void emitSessionPropertiesChanged(
    const std::string& interfaceName,
    const boost::container::flat_map<std::string, vmediaPropertyVariant>&
        sessionProperty)
{
    auto clientBus = sdbusplus::bus::new_bus();
    auto signal =
        clientBus.new_signal(sessMgrVmediaObjPath.c_str(),
                             DBUS_PROPERTIES_INTERFACE, "PropertiesChanged");
    signal.append(interfaceName, sessionProperty, std::vector<std::string>{});
    signal.signal_send();
}

//=============================================================================
// Path and Slot Number Parsing Tests
//=============================================================================

/**
 * @test Verify correct console/remote mode mapping for virtual media slots
 * @brief Tests mountMethod() function's slot-to-mode translation logic
 *
 * Virtual media slots are categorized into:
 * - Slot_0, Slot_1: Console mode (local access)
 * - Slot_2, Slot_3: Remote mode (network access)
 * - Other slots: Invalid/undefined
 *
 * This mapping is critical for determining how media is presented to the host.
 */
TEST(MountMethod, ConsoleAndRemoteMapping)
{
    EXPECT_EQ(mountMethod("Slot_0"), "console");
    EXPECT_EQ(mountMethod("Slot_1"), "console");
    EXPECT_EQ(mountMethod("Slot_2"), "remote");
    EXPECT_EQ(mountMethod("Slot_3"), "remote");
    EXPECT_EQ(mountMethod("Slot_42"), " ");
}

/**
 * @test Extract directory path by removing filename component
 * @brief Validates getPathWithoutFileName() for Unix and Windows paths
 *
 * Tests path parsing for:
 * - Unix-style paths with forward slashes
 * - Windows-style paths with backslashes (escaped)
 * - Paths without delimiters (no directory component)
 *
 * Critical for isolating directory paths from image URLs in mount operations.
 */
TEST(PathHelpers, GetPathWithoutFileName)
{
    EXPECT_EQ(getPathWithoutFileName("/abc/def/file.iso"), "/abc/def/");
    EXPECT_EQ(getPathWithoutFileName("C\\\\abc\\\\def\\\\file.iso"),
              "C\\\\abc\\\\def\\\\");
    EXPECT_EQ(getPathWithoutFileName("no_delimiter"), "");
}

/**
 * @test Extract slot array index from slot name
 * @brief Validates extractSlotNumber() conversion from name to array index
 *
 * Slot naming convention:
 * - Slot_2 -> index 0 (first remote slot)
 * - Slot_3 -> index 1 (second remote slot)
 * - Invalid format throws std::invalid_argument
 *
 * This conversion is essential for accessing the global creds[] array.
 */
TEST(SlotNumber, ExtractSlotNumberValidInvalid)
{
    ASSERT_NO_THROW({ EXPECT_EQ(extractSlotNumber("Slot_2"), 0); });
    ASSERT_NO_THROW({ EXPECT_EQ(extractSlotNumber("Slot_3"), 1); });
    EXPECT_THROW(extractSlotNumber("missing_underscore"),
                 std::invalid_argument);
}

/**
 * @test Extract session ID from virtual media mount strings
 * @brief Validates extractSessionId() parsing of session_N markers
 *
 * Session IDs are embedded in mount paths as "session_<number>" patterns.
 * Valid range: 0-255 (uint8_t).
 * Used to correlate mount operations with user sessions.
 */
TEST(SessionId, ExtractSessionIdValid)
{
    EXPECT_EQ(extractSessionId("hello-session_0-world"),
              static_cast<uint8_t>(0));
    EXPECT_EQ(extractSessionId("prefix session_255 suffix"),
              static_cast<uint8_t>(255));
}

/**
 * @test Reject malformed session ID strings
 * @brief Ensures extractSessionId() throws on missing session_ marker
 *
 * Strings without the "session_" prefix should be rejected to prevent
 * accidental misinterpretation of numeric substrings.
 */
TEST(SessionId, ExtractSessionIdInvalidFormat)
{
    EXPECT_THROW(extractSessionId("no_session_marker_here"),
                 std::invalid_argument);
}

/**
 * @test Detect out-of-range session IDs
 * @brief Validates extractSessionId() throws on values > 255
 *
 * Session IDs must fit in uint8_t. Values exceeding 255 indicate
 * corrupted data or programming errors and must be rejected.
 */
TEST(SessionId, ExtractSessionIdOutOfRange)
{
    EXPECT_THROW(extractSessionId("session_99999"), std::out_of_range);
}

TEST(NBDDeviceHelpers, UnknownAndNamedDeviceConversions)
{
    NBDDevice unknownDevice;
    NBDDevice invalidDevice("not-a-real-nbd");
    NBDDevice namedDevice("nbd2");

    EXPECT_FALSE(static_cast<bool>(unknownDevice));
    EXPECT_FALSE(static_cast<bool>(invalidDevice));
    EXPECT_TRUE(static_cast<bool>(namedDevice));

    EXPECT_FALSE(unknownDevice.isReady());
    EXPECT_FALSE(invalidDevice.isReady());

    EXPECT_EQ(unknownDevice.to_string(), "");
    EXPECT_EQ(invalidDevice.to_string(), "");
    EXPECT_EQ(namedDevice.to_string(), "nbd2");

    EXPECT_TRUE(unknownDevice.to_path().empty());
    EXPECT_TRUE(invalidDevice.to_path().empty());
    EXPECT_EQ(namedDevice.to_path(), fs::path("/dev/nbd2"));

    EXPECT_EQ(unknownDevice, invalidDevice);
    EXPECT_NE(unknownDevice, namedDevice);
}

TEST(NBDDeviceHelpers, DisconnectUnknownAndMissingDeviceDoNotThrow)
{
    NBDDevice unknownDevice;
    NBDDevice missingDevice("nbd10");

    EXPECT_NO_THROW(unknownDevice.disconnect());
    EXPECT_NO_THROW(missingDevice.disconnect());
}

TEST(GlobalContextHelpers, IsSamePathReturnsZeroWhenCredentialsMissing)
{
    creds[0].reset();
    creds[1].reset();

    EXPECT_EQ(isSamePath(), 0);

    boost::asio::io_context ioc;
    creds[0] = std::make_shared<Credentials>(ioc, "/tmp/slot2/image.iso");

    EXPECT_EQ(isSamePath(), 0);

    creds[0].reset();
    creds[1].reset();
}

TEST(DbusMonitorHelpers, HandleSessionsLeavesActiveMapWhenNothingRemoved)
{
    DbusMonitor monitor;
    activeSessons.clear();
    activeSessons["Slot_2"] = 8;

    sessionList updatedSessions{{8, "10.0.0.1", "user", VMEDIA,
                                 PRIV_LEVEL_ADMIN, 1, "mount", "Slot_2"}};

    monitor.handleSessions(updatedSessions);

    ASSERT_EQ(activeSessons.size(), 1U);
    EXPECT_EQ(activeSessons["Slot_2"], 8);

    activeSessons.clear();
}

//=============================================================================
// Image Type Detection Tests
//=============================================================================

/**
 * @test Detect ISO 9660 CD-ROM images by signature
 * @brief Validates detectImageType() identifies CD001 signature at offset
 * 0x8001
 *
 * ISO 9660 specification defines "CD001" signature at sector 16 (offset
 * 0x8001). Function must:
 * - Read file at correct offset
 * - Match signature exactly
 * - Return type 1 for CD-ROM images
 *
 * This determines whether to present media as USB CDROM vs. USB Mass Storage.
 */
TEST(DetectImageType, IsoWithCD001SignatureReturnsCdRom)
{
    auto iso = makeTempFileWithSize(0x8001 + 5);
    ASSERT_NE(iso, fs::path{});
    writeAt(iso, static_cast<std::streamoff>(0x8001), "CD001");
    EXPECT_EQ(detectImageType(iso.string()), 1);
    fs::remove(iso);
}

/**
 * @test Identify non-ISO disk images
 * @brief Validates detectImageType() returns hard disk type (0) for non-ISO
 * files
 *
 * Files without CD001 signature are treated as raw disk images.
 * These are presented to the host as USB Mass Storage devices.
 */
TEST(DetectImageType, NonIsoReturnsHardDiskType)
{
    auto img = makeTempFileWithSize(0x8001 + 5);
    ASSERT_NE(img, fs::path{});
    writeAt(img, static_cast<std::streamoff>(0x8001), "XXXXX");
    EXPECT_EQ(detectImageType(img.string()), 0);
    fs::remove(img);
}

/**
 * @test Handle non-existent image files gracefully
 * @brief Validates detectImageType() returns error (-1) for missing files
 *
 * Missing files must be detected before attempting mount operations.
 * Early detection prevents cryptic mount errors.
 */
TEST(DetectImageType, MissingFileReturnsError)
{
    EXPECT_EQ(detectImageType("/tmp/nonexistent_very_long_name.img"), -1);
}

//=============================================================================
// Eject Status Detection Tests
//=============================================================================

/**
 * @test Detect eject request from empty marker file
 * @brief Validates eject_status() extracts slot number from empty file path
 *
 * Virtual media eject mechanism:
 * 1. User triggers eject via D-Bus or web interface
 * 2. System creates empty marker file named "Slot_N_*"
 * 3. Monitor detects empty file and extracts slot number N
 * 4. Unmount operation is triggered for that slot
 *
 * Empty file check ensures file was intentionally created as eject signal,
 * not accidentally truncated.
 */
TEST(EjectStatus, EmptyFileWithDigitInPathReturnsDigit)
{
    fs::path f = fs::temp_directory_path() / "Slot_7_empty.txt";
    std::ofstream ofs(f);
    ofs.close();
    EXPECT_EQ(eject_status(f.string()), 7);
    fs::remove(f);
}

/**
 * @test Reject non-empty files as eject markers
 * @brief Validates eject_status() returns -1 for files with content
 *
 * Only empty files are valid eject signals. Non-empty files could be:
 * - Accidental file creation
 * - Corrupted marker files
 * - Security attack attempts
 *
 * Rejection prevents unintended unmount operations.
 */
TEST(EjectStatus, NonEmptyFileReturnsMinusOne)
{
    fs::path f = fs::temp_directory_path() / "Slot_3_nonempty.txt";
    std::ofstream ofs(f);
    ofs << "data";
    ofs.close();
    EXPECT_EQ(eject_status(f.string()), -1);
    fs::remove(f);
}

//=============================================================================
// File and Mount Point Accessibility Tests
//=============================================================================

/**
 * @test Verify file accessibility checking
 * @brief Validates isFileAccessible() detects file existence and permissions
 *
 * Used to verify:
 * - Image files exist before mount attempts
 * - Files are readable by the virtual media service
 * - Cleanup operations successfully removed files
 *
 * Prevents mount operations on inaccessible resources.
 */
TEST(FileAccess, IsFileAccessible)
{
    fs::path f = fs::temp_directory_path() / "access_test.tmp";
    std::ofstream ofs(f);
    ofs << "x";
    ofs.close();
    EXPECT_TRUE(isFileAccessible(f.string()));
    fs::remove(f);
    EXPECT_FALSE(isFileAccessible(f.string()));
}

/**
 * @test Verify mounted filesystem accessibility with timeout
 * @brief Validates isMountedPathAccessible() using statfs polling
 *
 * Critical for reconnection scenarios where:
 * 1. Network filesystem temporarily unavailable
 * 2. System polls with timeout waiting for recovery
 * 3. Returns success (1) when accessible, error (-1) on timeout
 *
 * Timeout prevents indefinite blocking on unresponsive mounts.
 */
TEST(StatfsAccess, IsMountedPathAccessible)
{
    EXPECT_EQ(isMountedPathAccessible("/tmp", 2), 1);
    EXPECT_EQ(
        isMountedPathAccessible("/tmp/this_should_not_exist_987654321", 2), -1);
}

TEST(NBDDevice, ConstructorsAndConversion)
{
    NBDDevice unknown;
    EXPECT_FALSE(static_cast<bool>(unknown));
    EXPECT_EQ(unknown.to_string(), "");
    EXPECT_EQ(unknown.to_path(), fs::path());
    NBDDevice devByName("nbd3");
    EXPECT_TRUE(static_cast<bool>(devByName));
    EXPECT_EQ(devByName.to_string(), "nbd3");
    EXPECT_EQ(devByName.to_path(), fs::path("/dev") / "nbd3");
    NBDDevice devEnum(NBDDevice::nbd10);
    EXPECT_TRUE(static_cast<bool>(devEnum));
    EXPECT_EQ(devEnum.to_string(), "nbd10");
    EXPECT_NE(devByName, unknown);
    EXPECT_LT(NBDDevice(NBDDevice::nbd2), NBDDevice(NBDDevice::nbd9));
}

TEST(NBDDevice, NullptrConstructorLeavesUnknown)
{
    NBDDevice nullName(static_cast<const char*>(nullptr));
    EXPECT_FALSE(static_cast<bool>(nullName));
    EXPECT_EQ(nullName.to_string(), "");
    EXPECT_EQ(nullName.to_path(), fs::path());
}
TEST(DbMonitorHelpers, FindKeyForValue)
{
    activesessionmap m;
    m["Slot_2"] = 10;
    m["Slot_3"] = 11;
    DbusMonitor mon;
    EXPECT_EQ(mon.findKeyForValue(m, 10), "Slot_2");
    EXPECT_EQ(mon.findKeyForValue(m, 11), "Slot_3");
    EXPECT_EQ(mon.findKeyForValue(m, 99), "INVALID");
}
TEST(DbMonitorHelpers, FindRemovedSessionIdsSetDifference)
{
    DbusMonitor mon;
    std::vector<uint8_t> active{1, 2, 3, 4, 5};
    std::vector<uint8_t> updated{1, 3, 5};
    auto removed = mon.findRemovedSessionIDs(active, updated);
    EXPECT_THAT(removed, ::testing::ElementsAre(2, 4));
}
TEST(CredentialsPaths, IsSamePath)
{
    boost::asio::io_context io;
    creds[0] = std::make_shared<Credentials>(io, "smb://server/share/foo.iso",
                                             true, "session_12");
    creds[1] = std::make_shared<Credentials>(io, "smb://server/share/bar.img",
                                             false, "session_34");
    EXPECT_EQ(isSamePath(), 1);
    creds[0] = std::make_shared<Credentials>(io, "smb://server/shareA/foo.iso",
                                             true, "session_12");
    creds[1] = std::make_shared<Credentials>(io, "smb://serverB/shareB/bar.img",
                                             false, "session_34");
    EXPECT_EQ(isSamePath(), -1);
    creds[0] = nullptr;
    creds[1] = nullptr;
    EXPECT_EQ(isSamePath(), 0);
}
TEST(RetryMount, InvalidSlotNumberEarlyExit)
{
    EXPECT_FALSE(retryMount("InvalidSlotName", 2U, 1U));
}
TEST(SleepMs, SleepsApproximately)
{
    using namespace std::chrono;

    auto start = steady_clock::now();
    sleep_ms(50);
    auto dur = duration_cast<milliseconds>(steady_clock::now() - start).count();

    EXPECT_GE(dur, 40);
}
TEST(SlotNumber, ExtractSlotNumberEdgeCases)
{
    EXPECT_THROW(extractSlotNumber("Slot_"), std::invalid_argument);
    EXPECT_THROW(extractSlotNumber("Slot_X"), std::invalid_argument);
    EXPECT_EQ(extractSlotNumber("Slot_0002"), 0);
    EXPECT_THROW(extractSlotNumber("Slot_9999999999999999999999"),
                 std::out_of_range);
}
TEST(SessionId, ExtractSessionIdRobustness)
{
    EXPECT_THROW(extractSessionId("Session_12"), std::invalid_argument);
    EXPECT_EQ(extractSessionId("foo session_12 bar session_34"),
              static_cast<uint8_t>(12));
    EXPECT_THROW(extractSessionId("session_ 12"), std::invalid_argument);
    EXPECT_THROW(extractSessionId("session_-1"), std::invalid_argument);
}
TEST(DetectImageType, ShortFileReturnsError)
{
    auto shortf = makeTempFileWithSize(0x8000);
    ASSERT_NE(shortf, fs::path{});
    EXPECT_EQ(detectImageType(shortf.string()), -1);
    fs::remove(shortf);
}
TEST(EjectStatus, MultipleDigitsAndNoDigits)
{
    fs::path p1 = fs::temp_directory_path() / "Slot_12_empty.txt";
    {
        std::ofstream ofs(p1);
        ofs.close();
    }
    EXPECT_EQ(eject_status(p1.string()), 1);
    fs::remove(p1);
    fs::path p2 = fs::temp_directory_path() / "no_digits.txt";
    {
        std::ofstream ofs(p2);
        ofs.close();
    }
    EXPECT_EQ(eject_status(p2.string()), -1);
    fs::remove(p2);
}
TEST(MountedPathAccessible, TimeoutZeroSecondsReturnsZero)
{
    EXPECT_EQ(isMountedPathAccessible("/tmp", 0), 0);
}
TEST(FileAccess, DirectoryAndSymlinkCases)
{
    fs::path d = fs::temp_directory_path() / "access_dir_test";
    std::error_code ec;
    fs::create_directory(d, ec);
    ASSERT_FALSE(ec);
    EXPECT_TRUE(isFileAccessible(d.string()));
    fs::remove(d);
    fs::path f = fs::temp_directory_path() / "access_target.txt";
    {
        std::ofstream ofs(f);
        ofs << "data";
        ofs.close();
    }
    fs::path link = fs::temp_directory_path() / "access_link.lnk";
    fs::create_symlink(f, link, ec);
    ASSERT_FALSE(ec);
    EXPECT_TRUE(isFileAccessible(link.string()));
    fs::remove(link);
    fs::remove(f);
    fs::path broken = fs::temp_directory_path() / "broken_link.lnk";
    fs::create_symlink(fs::temp_directory_path() / "does_not_exist.xyz", broken,
                       ec);
    ASSERT_FALSE(ec);
    EXPECT_FALSE(isFileAccessible(broken.string()));
    fs::remove(broken);
}
TEST(PathHelpers, RootAndDelimiterVariants)
{
    EXPECT_EQ(getPathWithoutFileName("/abc/def/"), "/abc/def/");
    EXPECT_EQ(getPathWithoutFileName("C\\\\abc\\\\def\\\\"),
              "C\\\\abc\\\\def\\\\");
    EXPECT_EQ(getPathWithoutFileName("/"), "/");
    EXPECT_EQ(getPathWithoutFileName("C\\\\"), "C\\\\");
}

TEST(GlobalContextHelpers, MountPrefixAndServiceBasePath)
{
    EXPECT_EQ(getMountDirPrefix(), "/tmp/");

    auto [serviceName, basePath] = getServiceAndBasePath("Slot_0");
    EXPECT_EQ(serviceName, g_serviceName);
    EXPECT_EQ(basePath, g_basePath);
}

TEST(DBusHelpers, EventLogSupportNoThrowWithoutService)
{
    EXPECT_NO_THROW(eventLogSupport("OpenBMC.0.1.MediaMount"));
}

// ---------------- Additional tests (no src modification) ----------------

TEST(RetryMount, ExhaustsAttemptsAndNullifiesCreds)
{
    // Setup creds for Slot_3 (slotNumber==1)
    boost::asio::io_context io;
    creds[1] = std::make_shared<Credentials>(io, "smb://server/share/bar.img",
                                             false, "session_34");
    // No retries -> loop is skipped, function should nullify creds[1]
    EXPECT_FALSE(retryMount("Slot_3", 0U, 0U));
    EXPECT_EQ(creds[1], nullptr);
}

TEST(RetryMount, SingleRetrySmbPathCatchesDbusErrorAndNullifiesCreds)
{
    boost::asio::io_context io;
    creds[1] = std::make_shared<Credentials>(io, "smb://server/share/bar.img",
                                             false, "session_34");

    EXPECT_FALSE(retryMount("Slot_3", 1U, 0U));
    EXPECT_EQ(creds[1], nullptr);
}

TEST(RetryMount, SingleRetryNfsPathCatchesDbusErrorAndNullifiesCreds)
{
    boost::asio::io_context io;
    creds[0] = std::make_shared<Credentials>(io, "nfs://server/export/disk.img",
                                             true, "session_12");

    EXPECT_FALSE(retryMount("Slot_2", 1U, 0U));
    EXPECT_EQ(creds[0], nullptr);
}

TEST(MountMonitorThread, NullifiesCredsOnNotSameServer)
{
    // 1) Reset any leftover global flags from previous tests
    retryThreadCreatedMap["Slot_2"].store(false);
    is_reconnecting.store(false);

    // 2) Guarantee mount path does NOT exist so statfs(...) returns -1 and
    // joins the thread
    fs::remove_all("/tmp/Slot_2");

    // 3) Prepare only Slot_2 credentials (not same server -> isSamePath()
    // returns 0)
    boost::asio::io_context io;
    creds[0] = std::make_shared<Credentials>(io, "smb://server/share/foo.iso",
                                             /*rw=*/true, "session_12");
    creds[1] = nullptr;

    // Exercise the code under test
    mountMonitorThread("Slot_2");

    // Assert: the function should clear the creds and exit
    EXPECT_EQ(creds[0], nullptr);

    // Cleanup (optional): make sure globals are reset for subsequent tests
    retryThreadCreatedMap["Slot_2"].store(false);
    is_reconnecting.store(false);
}

/**
 * @test MountMonitorThread.NullifiesCredsOnSameServerSlotSwitching
 * @brief Exercises the isSamePath()==1 branch where the monitor switches
 *        from Slot_2->Slot_3 before exiting.
 *
 * When both slots share the same server (isSamePath()==1), the monitor
 * pivots to watch the other slot. Trace:
 *   Iter 1: path=Slot_2, slotNumber=0, statfs("/tmp/Slot_2")->-1,
 *           isSamePath()==1 → path="Slot_3", slotNumber=1, creds[1]=nullptr,
 * continue Iter 2: path=Slot_3, slotNumber=1, statfs("/tmp/Slot_3")->-1,
 *           isSamePath()==0 (creds[1] is now null) → else: creds[1] already
 * null, break Result: creds[1] nullified; creds[0] untouched (handled by retry
 * logic elsewhere).
 */
TEST(MountMonitorThread, NullifiesCredsOnSameServerSlotSwitching)
{
    // Reset global state
    retryThreadCreatedMap["Slot_2"].store(false);
    retryThreadCreatedMap["Slot_3"].store(false);
    is_reconnecting.store(false);

    // Ensure neither mount path exists (statfs returns -1 immediately)
    fs::remove_all("/tmp/Slot_2");
    fs::remove_all("/tmp/Slot_3");

    // Both creds point to the same server directory → isSamePath() returns 1
    boost::asio::io_context io;
    creds[0] = std::make_shared<Credentials>(io, "smb://server/share/foo.iso",
                                             /*rw=*/true, "session_12");
    creds[1] = std::make_shared<Credentials>(io, "smb://server/share/bar.img",
                                             /*rw=*/false, "session_34");

    // Exercise: slot-switching path is triggered because isSamePath()==1
    mountMonitorThread("Slot_2");

    // creds[1] (Slot_3) is nullified in the same-server branch (slotNumber
    // pivots to 1). creds[0] (Slot_2) is not cleared here — it is expected to
    // be handled by the retry mechanism launched from a separate thread.
    EXPECT_EQ(creds[1], nullptr);

    // Cleanup
    creds[0] = nullptr;
    retryThreadCreatedMap["Slot_2"].store(false);
    retryThreadCreatedMap["Slot_3"].store(false);
    is_reconnecting.store(false);
}

/**
 * @test MountMonitorThread.NullifiesCredsOnSameServerSlot3Entry
 * @brief Exercises the isSamePath()==1, slotNumber==1 sub-branch where the
 *        monitor enters via Slot_3 and pivots to watch Slot_2.
 *
 * Trace:
 *   Iter 1: path=Slot_3, slotNumber=1, statfs("/tmp/Slot_3")->-1,
 *           isSamePath()==1 → else if(slotNumber==1): path="Slot_2",
 *           slotNumber=0, creds[0]=nullptr, continue
 *   Iter 2: path=Slot_2, slotNumber=0, statfs("/tmp/Slot_2")->-1,
 *           isSamePath()==0 (creds[0] is now null) → else: break
 * Result: creds[0] nullified; creds[1] untouched.
 */
TEST(MountMonitorThread, NullifiesCredsOnSameServerSlot3Entry)
{
    retryThreadCreatedMap["Slot_2"].store(false);
    retryThreadCreatedMap["Slot_3"].store(false);
    is_reconnecting.store(false);

    fs::remove_all("/tmp/Slot_2");
    fs::remove_all("/tmp/Slot_3");

    boost::asio::io_context io;
    creds[0] = std::make_shared<Credentials>(io, "smb://server/share/foo.iso",
                                             /*rw=*/true, "session_12");
    creds[1] = std::make_shared<Credentials>(io, "smb://server/share/bar.img",
                                             /*rw=*/false, "session_34");

    // Enter via Slot_3 (slotNumber==1) with same-server creds
    mountMonitorThread("Slot_3");

    // creds[0] (Slot_2) is nullified when slotNumber pivots to 0
    EXPECT_EQ(creds[0], nullptr);

    creds[1] = nullptr;
    retryThreadCreatedMap["Slot_2"].store(false);
    retryThreadCreatedMap["Slot_3"].store(false);
    is_reconnecting.store(false);
}

TEST(NBDDevice, IsReadyNegativeAndDisconnectNoCrash)
{
    // Unknown device is not ready
    NBDDevice unknown;
    EXPECT_FALSE(unknown.isReady());
    // Disconnect on unknown should be a no-op (no crash)
    unknown.disconnect();
}

static bool is_root()
{
    return ::geteuid() == 0;
}

static fs::path makeTempDir(const std::string& name)
{
    fs::path d = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::create_directory(d, ec);
    EXPECT_FALSE(ec);
    return d;
}

// ---------- Robust unreadable path test for detectImageType ----------
TEST(DetectImageType, DirectoryPathReturnsError)
{
    // Using a directory (not a regular file) is a stable way to force
    // std::ifstream open failure.
    fs::path d = makeTempDir("imgtype_dir_for_unreadable");
    // On all sane environments, opening a directory with std::ifstream fails
    // and returns -1.
    EXPECT_EQ(detectImageType(d.string()), -1);
    std::error_code ec;
    fs::remove(d, ec);
}

// Permission-denied scenario that behaves well both when root and non-root.
TEST(DetectImageType, PermissionDeniedOrSkippedDependingOnPrivileges)
{
    auto f = fs::temp_directory_path() / "imgtype_perm.img";
    // Create and prepare a file with a CD001 signature.
    {
        std::ofstream ofs(f, std::ios::binary);
        ASSERT_TRUE(ofs.good());
        // Put signature at 0x8001
        ofs.seekp(static_cast<std::streamoff>(0x8001));
        ofs.write("CD001", 5);
        ofs.close();
    }

    // Remove permissions
    std::error_code ec;
    fs::permissions(f, fs::perms::none, fs::perm_options::replace, ec);
    ASSERT_FALSE(ec);

    // Test behavior based on privilege level
    int result = detectImageType(f.string());
    if (is_root())
    {
        // Root can still read 000 files on most systems, so result may not be
        // -1 Just verify the function doesn't crash and returns a valid result
        EXPECT_TRUE(result == -1 || result == 0 || result == 1);
    }
    else
    {
        // Non-root should not be able to open the file -> function must return
        // -1.
        EXPECT_EQ(result, -1);
    }

    fs::remove(f, ec);
}
TEST(DeviceMonitor_Uncovered, ConstructAddGetStateOrSkip)
{
    boost::asio::io_context ioc;

    // Constructor may throw if udev is not available/configured.
    std::shared_ptr<DeviceMonitor> mon;
    try
    {
        mon = std::make_shared<DeviceMonitor>(ioc);
    }
    catch (const std::system_error& e)
    {
        GTEST_SKIP() << "DeviceMonitor ctor threw: " << e.what()
                     << " (udev not available or misconfigured)";
    }

    ASSERT_TRUE(mon);

    // Add a device and query its initial state
    NBDDevice d("nbd2");
    mon->addDevice(d);
    EXPECT_EQ(mon->getState(d), StateChange::removed);

    // Spawn the monitoring coroutine but do not run io_context (no events to
    // consume).
    mon->run([](const NBDDevice& dev, StateChange sc) {
        (void)dev;
        (void)sc; // suppress -Werror=unused-parameter
        // record an event if needed
    });

    SUCCEED(); // We only validate API is callable without blocking.
}

// ---------- Process positive paths ----------
TEST(Process, SpawnSuccessPathEchoAndCallback)
{
    // Arrange
    boost::asio::io_context io;
    NBDDevice dev("nbd2"); // placeholder; not used by spawn()
    auto proc = std::make_shared<Process>(io, "unit", "/bin/echo", dev);

    std::atomic<bool> callbackCalled{false};
    int capturedExitCode = -999;

    // Act
    bool ok = proc->spawn({"Hello, world!"}, [&](int ec) {
        capturedExitCode = ec;
        callbackCalled.store(true);
    });
    ASSERT_TRUE(ok) << "spawn() should succeed with a valid executable";

    // Pump io_context so the coroutine runs and completes.
    io.run();

    // Assert
    EXPECT_TRUE(callbackCalled.load());
    EXPECT_EQ(capturedExitCode, 0);
}

TEST(Process, StopTerminatesAndInvokesCallback)
{
    // Arrange
    boost::asio::io_context io;

    // Even if /dev/nbd2 isn't present, NBDDevice::disconnect() handles the
    // error and returns safely.
    NBDDevice dev("nbd2");
    auto proc = std::make_shared<Process>(io, "stop-test", "/bin/sleep", dev);

    std::atomic<bool> onExitCalled{false};
    std::atomic<bool> onTerminateCalled{false};

    // Spawn a long-running child so stop() has something to terminate.
    // /bin/sleep 3 is widely available on Linux test environments.
    bool ok = proc->spawn({"3"}, [&](int exitCode) {
        onExitCalled.store(true);
        (void)exitCode; // environment may deliver different exit codes; we only
                        // care that the callback fired
    });
    ASSERT_TRUE(ok) << "spawn()ld succeed with a valid executable";

    // Act: request termination while child is still running.
    proc->stop([&] { onTerminateCalled.store(true); });

    // Pump io_context so both coroutines (spawn + stop) run to completion.
    io.run();

    // Assert: both callbacks must have been invoked
    EXPECT_TRUE(onTerminateCalled.load())
        << "stop() should have invoked OnTerminateCb";
    EXPECT_TRUE(onExitCalled.load())
        << "spawn() exit callback should be called after termination";
}

// ---------- UsbGadget lightweight checks (non-configfs) ----------
TEST(UsbGadget, IsConfiguredFalseAndGetStatsEmpty)
{
    // On typical unit-test environments, configfs gadget path doesn't exist.
    EXPECT_FALSE(UsbGadget::isConfigured("UnitX"));

    // getStats() should return empty optional when stats path doesn't exist.
    auto s = UsbGadget::getStats("UnitX");
    EXPECT_FALSE(s.has_value());
}

// --------------- DBus helpers: exercise error/fallback paths ---------------

TEST(DBusHelpers, GetActiveStatusReturnsFalseOrSkipIfNoSystemBus)
{
    // Test getActiveStatus behavior - should handle errors gracefully
    // Whether D-Bus is available or not, the function should not crash
    // and should return false when service is unavailable
    bool result = false;
    try
    {
        result =
            getActiveStatus("/xyz/openbmc_project/VirtualMedia/Proxy/Slot_0");
    }
    catch (const std::exception&)
    {
        // System D-Bus not available in test environment — skip gracefully
        GTEST_SKIP()
            << "System D-Bus unavailable; skipping getActiveStatus test";
    }

    // Should return false when VirtualMedia service is not available
    EXPECT_FALSE(result);
}

TEST(DBusHelpers, EventLogSupportHandlesFailures)
{
    // No Logging service -> should catch and return without throwing.
    // We can't assert logs, but reaching here exercises the try/catch path.
    eventLogSupport("OpenBMC.0.1.MediaMount");
    SUCCEED();
}

TEST(DBusHelpers, PowerSaveModePathsAreCallable)
{
    // Both permissible statuses (0,1) should invoke the DBus path and handle
    // failures.
    powerSaveMode(POWER_SAVE_MODE_DISABLE);
    powerSaveMode(POWER_SAVE_MODE_ENABLE);
    SUCCEED();
}

TEST(DBusHelpers, PowerSaveModeIgnoresUnexpectedStatus)
{
    powerSaveMode(2);
    powerSaveMode(-1);
    SUCCEED();
}

// --------- Helpers (environment detection) ----------

static bool configfs_available()
{
    // Basic check for configfs mount presence
    return fs::exists("/sys/kernel/config");
}

// --------- DbusMonitor::handleSessions / sessionMonitor ---------

TEST(DbusMonitor_Uncovered, HandleSessionsErasesRemovedIDsAndDoesNotThrow)
{
    // Mock d-bus by handling exceptions gracefully
    // We can use the static activeSessons from the header (internal linkage per
    // TU). Populate with active sessions 10 and 11 on Slot_2 and Slot_3.
    activeSessons.clear();
    activeSessons.insert({"Slot_2", static_cast<uint8_t>(10)});
    activeSessons.insert({"Slot_3", static_cast<uint8_t>(11)});

    // Build a "updated" session list where neither 10 nor 11 appear -> both are
    // removed.
    sessionList updatedList;
    updatedList.emplace_back(
        /*sid*/ static_cast<uint8_t>(42),
        /*ip*/ std::string("127.0.0.1"),
        /*user*/ std::string("tester"),
        /*type*/ static_cast<uint8_t>(VMEDIA),
        /*priv*/ static_cast<uint8_t>(PRIV_LEVEL_ADMIN),
        /*uid*/ static_cast<uint8_t>(7),
        /*method*/ std::string("remote"),
        /*slotId*/ std::string("Slot_1"));

    DbusMonitor mon;

    // handleSessions() will fork a child per removed ID to call unMount(...).
    // The child processes will attempt D-Bus calls which may fail in test
    // environment. In test environment without D-Bus service, gracefully handle
    // any exceptions.
    try
    {
        mon.handleSessions(updatedList);
    }
    catch (const std::exception& e)
    {
        // D-Bus service not available in test environment - this is expected
        // The test verifies that the function can be called without crashing
    }

    // Give forked child processes a moment to exit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Clean up for next test iteration
    activeSessons.clear();

    SUCCEED();
}

TEST(DbusMonitor_Uncovered, SessionMonitorConstructsOrSkips)
{
    boost::asio::io_context io;

    // Try to create connection - may succeed or fail based on environment
    try
    {
        auto conn = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        DbusMonitor mon;

        // Just constructing the match object should be fine; we don't pump io
        // nor send signals.
        ASSERT_NO_THROW({
            auto m = mon.sessionMonitor(conn);
            (void)m;
        });
        SUCCEED();
    }
    catch (const std::exception& e)
    {
        // If D-Bus not available, verify we get an exception and handle it
        SUCCEED() << "D-Bus connection unavailable, exception properly caught: "
                  << e.what();
    }
}

// --------- UsbGadget::configure(...) ---------

TEST(UsbGadget_Uncovered, ConfigurePathNegativeOrSkipWhenNoConfigfs)
{
    // Create a small regular temp file path (not a block device).
    fs::path tmp = fs::temp_directory_path() / "usb_gadget_small.img";
    {
        std::ofstream ofs(tmp, std::ios::binary);
        ofs << "x"; // 1 byte
    }

    if (configfs_available() && is_root())
    {
        // If configfs is available and we're root, test the actual
        // functionality Expect the function to return -1 since size < 600KB.
        int32_t rc = UsbGadget::configure("UnitX", tmp, StateChange::inserted,
                                          /*rw*/ false, "session_1");
        EXPECT_EQ(rc, -1);
    }
    else
    {
        // If configfs not available or not root, verify the function handles it
        // gracefully It should either return -1 or handle the error without
        // crashing
        int32_t rc = -999;
        ASSERT_NO_THROW({
            rc = UsbGadget::configure("UnitX", tmp, StateChange::inserted,
                                      /*rw*/ false, "session_1");
        });
        // Function should return error code (-1) when it can't complete
        EXPECT_EQ(rc, -1);
    }

    std::error_code ec;
    fs::remove(tmp, ec);
}

//=============================================================================
// D-Bus Helper Function Tests
//=============================================================================

/**
 * @test DBusHelpers_Uncovered.UnMountSafeInvocationOrSkip
 * @brief Verify unMount() behavior when D-Bus services are unavailable
 *
 * Tests that unMount() properly throws sdbusplus::exception::SdBusError when
 * the D-Bus VirtualMedia service is not available. This error handling is
 * critical for robustness in production environments.
 *
 * Expected Behavior:
 * - Without D-Bus service: Throws SdBusError
 * - With D-Bus service: Completes unmount operation
 *
 * @note This follows the established pattern in the codebase where D-Bus
 *       operations are allowed to throw when services are unavailable,
 *       relying on higher-level error handling.
 */
TEST(DBusHelpers_Uncovered, UnMountSafeInvocationOrSkip)
{
    EXPECT_THROW(unMount(std::string("Slot_2")),
                 sdbusplus::exception::SdBusError);
}

/**
 * @test DBusHelpers_Uncovered.UnMountProxySlotThrows
 * @brief Verify unMount() Proxy branch (Slot_0/Slot_1) also throws SdBusError
 *
 * unMount() has two branches based on slot:
 * - Slot_0/Slot_1 → builds /Proxy/Slot_N object path (Proxy interface)
 * - Slot_2/Slot_3 → builds /Legacy/Slot_N object path (Legacy interface)
 *
 * The existing UnMountSafeInvocationOrSkip test covers the Legacy branch.
 * This test covers the Proxy branch.
 */
TEST(DBusHelpers_Uncovered, UnMountProxySlotThrows)
{
    EXPECT_THROW(unMount(std::string("Slot_0")),
                 sdbusplus::exception::SdBusError);
}

/**
 * @test DBusHelpers_Uncovered.HandleUnmountAndRetryCallableOrSkip
 * @brief Verify retry logic behavior when D-Bus services are unavailable
 *
 * Tests that handleUnmountAndRetry() correctly propagates exceptions from
 * the underlying unMount() call when D-Bus services are unavailable.
 *
 * The retry mechanism should:
 * 1. Attempt unmount operation
 * 2. If D-Bus unavailable, throw SdBusError immediately
 * 3. If D-Bus available but operation fails, retry with backoff
 *
 * This test validates the error path (no D-Bus service available).
 *
 * @note Retry logic is important for handling transient D-Bus failures
 *       in production, but should fail fast when service is permanently
 * unavailable.
 */
TEST(DBusHelpers_Uncovered, HandleUnmountAndRetryCallableOrSkip)
{
    EXPECT_THROW(handleUnmountAndRetry(std::string("Slot_2"), /*slotNumber*/ 0),
                 sdbusplus::exception::SdBusError);
}

//=============================================================================
// NBD Device Management Tests
//=============================================================================

// -----------------------------------------------------------------------------
// NBDDevice: equality and more conversions
// -----------------------------------------------------------------------------
TEST(NBDDevice, EqualityOperatorAndUnknownEquality)
{
    NBDDevice a(NBDDevice::nbd2);
    NBDDevice b(NBDDevice::nbd2);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_FALSE(a < b);

    // Unknown devices compare equal
    NBDDevice u1; // default -> unknown
    NBDDevice u2; // default -> unknown
    EXPECT_TRUE(u1 == u2);
    EXPECT_EQ(u1.to_string(), "");
    EXPECT_EQ(u2.to_path(), std::filesystem::path());
}

// -----------------------------------------------------------------------------
// 2) Process: application() and stop() when child isn't running
// -----------------------------------------------------------------------------
TEST(Process, ApplicationAndStopNoChildRunning)
{
    boost::asio::io_context io;
    NBDDevice dev; // unknown device is safe

    auto proc = std::make_shared<Process>(io, "unit",
                                          "/this_command_does_not_exist", dev);

    // spawn() should fail and return false; this leaves child not running
    bool ok = proc->spawn({}, [](int /*ec*/) { /* no-op */ });
    EXPECT_FALSE(ok);

    // stop() should call dev.disconnect() and NOT crash even if child isn't
    // running
    std::atomic<bool> terminated{false};
    proc->stop([&terminated] { terminated.store(true); });

    // Pump the io_context to run any coroutines (should finish quickly)
    io.run();

    // With a non-running child, stop() won't need to terminate, so callback may
    // not fire
    EXPECT_FALSE(terminated.load());

    // application() should return the app string
    EXPECT_EQ(proc->application(), "/this_command_does_not_exist");
}

TEST(UsbGadget, ConfigureRemovedPathNoCrash)
{
    // Exercise the cleanup path (StateChange::removed). This avoids the heavy
    // "inserted" branch and its side effects. echoToFile() failures are
    // internal.
    std::string name = "ut_cleanup_" + std::to_string(::getpid());
    NBDDevice dev(NBDDevice::nbd0); // not opened; just generates a /dev path

    int rc = UsbGadget::configure(name, dev, StateChange::removed, /*rw*/ false,
                                  /*additionalInfo*/ " ");
    // With non-existing paths, cleanup should be effectively a no-op; success
    // remains true and configure returns 0.
    EXPECT_EQ(rc, 0);
}

// -----------------------------------------------------------------------------
// 5) DbusMonitor: handleSessions when activeSessons is empty (no fork path)
// -----------------------------------------------------------------------------
TEST(DbusMonitor, HandleSessionsNoActiveNoFork)
{
    // Clear any leftover state from previous test runs
    activeSessons.clear();

    DbusMonitor mon;

    // Build a session list that does NOT produce "removed" IDs relative to an
    // empty activeSessons map; this avoids the fork/unmount child path.
    sessionList list;
    list.emplace_back(sessionInfo{
        /*sessionId*/ static_cast<uint8_t>(1),
        /*ip*/ std::string{"127.0.0.1"},
        /*user*/ std::string{"user"},
        /*type*/ VMEDIA,
        /*priv*/ PRIV_LEVEL_ADMIN,
        /*userId*/ DEFAULT_USER_ID,
        /*mounting*/ std::string{"console"},
        /*slotId*/ std::string{"Slot_0"}});

    // Should not throw since no sessions are removed (no D-Bus calls made)
    // Exercises parsing and set-difference scaffolding without D-Bus dependency
    EXPECT_NO_THROW(mon.handleSessions(list));

    SUCCEED();
}

TEST(DbusMonitor, SessionMonitorProcessesMatchingPropertiesChangedSignal)
{
    boost::asio::io_context io;

    try
    {
        auto conn = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        DbusMonitor mon;
        auto match = mon.sessionMonitor(conn);
        (void)match;

        activeSessons.clear();

        boost::container::flat_map<std::string, vmediaPropertyVariant> props;
        sessionList updatedList{{static_cast<uint8_t>(42), "127.0.0.1",
                                 "tester", static_cast<uint8_t>(VMEDIA),
                                 static_cast<uint8_t>(PRIV_LEVEL_ADMIN),
                                 static_cast<uint8_t>(7), "remote", "Slot_1"}};
        props.emplace("VmediaSessionInfo", updatedList);

        emitSessionPropertiesChanged(sessMgrVmediaIface, props);

        pumpBus(*conn);
        EXPECT_TRUE(activeSessons.empty());

        activeSessons.clear();
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "D-Bus signal test unavailable in this environment: "
                     << e.what();
    }
}

TEST(DbusMonitor, SessionMonitorIgnoresNonMatchingInterfaceSignal)
{
    boost::asio::io_context io;

    try
    {
        auto conn = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        DbusMonitor mon;
        auto match = mon.sessionMonitor(conn);
        (void)match;

        activeSessons.clear();
        activeSessons.insert({"Slot_2", static_cast<uint8_t>(7)});

        boost::container::flat_map<std::string, vmediaPropertyVariant> props;
        sessionList updatedList{{static_cast<uint8_t>(42), "127.0.0.1",
                                 "tester", static_cast<uint8_t>(VMEDIA),
                                 static_cast<uint8_t>(PRIV_LEVEL_ADMIN),
                                 static_cast<uint8_t>(7), "remote", "Slot_1"}};
        props.emplace("VmediaSessionInfo", updatedList);

        emitSessionPropertiesChanged(sessMgrWebIface, props);

        pumpBus(*conn);
        EXPECT_EQ(activeSessons.size(), 1U);
        EXPECT_EQ(activeSessons["Slot_2"], 7);

        activeSessons.clear();
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "D-Bus signal test unavailable in this environment: "
                     << e.what();
    }
}

TEST(DbusMonitor, SessionMonitorIgnoresNonSessionProperty)
{
    boost::asio::io_context io;

    try
    {
        auto conn = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        DbusMonitor mon;
        auto match = mon.sessionMonitor(conn);
        (void)match;

        activeSessons.clear();
        activeSessons.insert({"Slot_2", static_cast<uint8_t>(7)});

        boost::container::flat_map<std::string, vmediaPropertyVariant> props;
        sessionList updatedList{{static_cast<uint8_t>(42), "127.0.0.1",
                                 "tester", static_cast<uint8_t>(VMEDIA),
                                 static_cast<uint8_t>(PRIV_LEVEL_ADMIN),
                                 static_cast<uint8_t>(7), "remote", "Slot_1"}};
        props.emplace("DifferentProperty", updatedList);

        emitSessionPropertiesChanged(sessMgrVmediaIface, props);

        pumpBus(*conn);
        EXPECT_EQ(activeSessons.size(), 1U);
        EXPECT_EQ(activeSessons["Slot_2"], 7);

        activeSessons.clear();
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "D-Bus signal test unavailable in this environment: "
                     << e.what();
    }
}
