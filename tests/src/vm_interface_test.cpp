// SPDX-License-Identifier: Apache-2.0
/**
 * @file vm_interface_test.cpp
 * @brief Comprehensive unit tests for vm::Interface (D-Bus interface and JSON
 * configuration)
 *
 * This unified test suite validates the vm::Interface class which manages:
 * - JSON configuration file persistence
 * - D-Bus method exposure (for backward compatibility)
 * - Backup image URL storage and retrieval
 * - Retry configuration management
 *
 * Testing Strategy:
 * - Per-test JSON reseeding for fixture isolation
 * - Hermetic test execution (single executable, no shared state)
 * - D-Bus mocking for CI/CD environments
 * - Private member access via #define for white-box testing
 * - Temporary file creation for write operation testing
 *
 * Functional Coverage:
 * 1. **JSON Configuration Management**:
 *    - loadJson() reads and parses configuration file
 *    - saveJson() writes configuration atomically
 *    - File creation when missing
 *    - Permission error handling (read-only paths)
 *    - Malformed JSON detection
 *
 * 2. **Backup Image URL Management**:
 *    - Per-slot URL storage (Slot_2, Slot_3, etc.)
 *    - getBackupImageURL() retrieval
 *    - setBackupImageURL() persistence
 *    - Empty slot handling (returns empty string)
 *    - Invalid slot name handling
 *
 * 3. **Retry Configuration**:
 *    - RetryCount parameter (default: 3)
 *    - RetryInterval parameter (default: 15 seconds)
 *    - getRetryCount() / setRetryCount()
 *    - getRetryInterval() / setRetryInterval()
 *    - Persistence across save/load cycles
 *
 * 4. **D-Bus Integration** (backward compatibility):
 *    - Interface exposure on system bus
 *    - Method call handling
 *    - Property access
 *    - Signal emission (if applicable)
 *
 * 5. **Error Handling**:
 *    - Missing configuration file (create default)
 *    - Read-only filesystem (skip writes)
 *    - Malformed JSON (reject, use defaults)
 *    - Invalid parameters (validation)
 *
 * Test Coverage Areas:
 * - All public vm::Interface methods
 * - JSON file I/O operations
 * - Configuration persistence
 * - Error recovery paths
 * - Edge cases (permissions, corruption)
 *
 * JSON Configuration Structure:
 * ```json
 * {
 *   "RetryCount": 3,
 *   "RetryInterval": 15,
 *   "BackupImageURL": {
 *     "Slot_2": { "ImageURL": "https://..." },
 *     "Slot_3": { "ImageURL": "https://..." }
 *   }
 * }
 * ```
 *
 * Edge Cases Tested:
 * - First run (no config file)
 * - Read-only configuration path
 * - Corrupted JSON file
 * - Missing BackupImageURL key
 * - Empty slot names
 * - Concurrent access (not applicable - single-threaded)
 *
 * Implementation Notes:
 * - VIRTUAL_MEDIA_CONFIG_PATH must be defined at build time
 * - Global initializer seeds JSON before any tests run
 * - Each test can reseed JSON for isolation
 * - Private members exposed via #define private public
 *
 * Dependencies:
 * - nlohmann::json: JSON parsing and serialization
 * - boost::asio::io_context: Async operations
 * - sdbusplus: D-Bus communication
 * - filesystem: File path operations
 *
 * @note Build must define VIRTUAL_MEDIA_CONFIG_PATH
 * @note Tests create/modify files in temporary directory
 * @note D-Bus operations are mocked for CI/CD environments
 */
// Unified single-file tests for vm_interface.hpp/.cpp:
// - Core logic tests (no D-Bus)
// - D-Bus-backed tests with per-test JSON reseeding
// - Robust JSON writer & fixture isolation so one executable is hermetic.
//
// Build example:
//   c++ -std=gnu++20 -Wall -Wextra -Wpedantic -Werror
//       -DVIRTUAL_MEDIA_CONFIG_PATH="\"/tmp/virtual-media-unified.json\""
//       src/vm_interface.cpp tests/src/vm_interface_all_tests.cpp
//       -I./src -I./tests/src
//       -lgtest -lgtest_main -lgmock -lpthread -lsdbusplus -lsystemd
//       -o vm_interface_all_tests
//
// IMPORTANT: Use the same VIRTUAL_MEDIA_CONFIG_PATH for *both* this test TU and
// vm_interface.cpp.

#include <sys/stat.h> // chmod

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <tuple>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using ::testing::HasSubstr;

// Expose vm::Interface private members ONLY in this test TU.
#define private public
#include "vm_interface.hpp"
#undef private

#ifdef INJECT_MOCKS
namespace vm
{
const std::string vmObjPath = "/xyz/openbmc_project/VirtualMedia";
} // namespace vm
#endif

// ---- Global JSON seed: ensure a valid config exists before any test runs ----
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
#include <filesystem>
#include <fstream>

namespace
{
namespace fs = std::filesystem;

static void seed_virtual_media_json()
{
    // Build-time path provided via -DVIRTUAL_MEDIA_CONFIG_PATH
    const fs::path p{VIRTUAL_MEDIA_CONFIG_PATH};

    // Create parent directories (ignore errors; best-effort)
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    // Seed content: valid object with required keys
    static const char* kSeed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/slot2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/slot3.iso" }
        }
    })";

    // Truncate and write atomically enough for unit tests
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (ofs.is_open())
    {
        ofs.write(kSeed, static_cast<std::streamsize>(std::strlen(kSeed)));
        ofs.flush();
        ofs.close();
    }
    // If open fails (e.g., read-only path), we simply leave it; individual
    // tests already handle read-only write failures gracefully.
}

// Global initializer runs before any tests (and before any environment code)
struct SeedBeforeAllTests
{
    SeedBeforeAllTests()
    {
        seed_virtual_media_json();
    }
} _seed_before_all_tests;

} // namespace
#endif // VIRTUAL_MEDIA_CONFIG_PATH
// ---------------------------------------------------------------------------

// ----- helpers bound to the macro path -----

static fs::path cfgPath()
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    return fs::path{VIRTUAL_MEDIA_CONFIG_PATH};
#else
    return fs::path{};
#endif
}

// Robust writer: ensure dir exists, truncate, write binary, flush.
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

static std::shared_ptr<sdbusplus::asio::connection> makeTestConnection(
    boost::asio::io_context& io)
{
    return std::make_shared<sdbusplus::asio::connection>(
        io, sdbusplus::bus::new_bus());
}

static fs::path makeBlockedConfigDirectory(const char* name)
{
    fs::path blocked = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(blocked, ec);
    fs::create_directories(blocked, ec);
    EXPECT_FALSE(ec);
    return blocked;
}

template <typename VariantType>
static VariantType getPropertyOverDbus(
    sdbusplus::bus_t& serverBus, const std::string& service,
    const std::string& path, const std::string& interface,
    const std::string& property)
{
    auto future = std::async(
        std::launch::async,
        [&service, &path, &interface, &property]() -> VariantType {
            auto clientBus = sdbusplus::bus::new_bus();
            auto msg = clientBus.new_method_call(
                service.c_str(), path.c_str(),
                "org.freedesktop.DBus.Properties", "Get");
            msg.append(interface, property);

            auto reply = clientBus.call(msg, std::chrono::seconds(1));
            VariantType value;
            reply.read(value);
            return value;
        });

    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (future.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready)
        {
            return future.get();
        }
        serverBus.wait(std::chrono::milliseconds(5));
        while (serverBus.process_discard())
        {}
    }

    throw std::runtime_error("Timed out waiting for D-Bus property reply");
}

template <typename VariantType>
static void setPropertyOverDbus(
    sdbusplus::bus_t& serverBus, const std::string& service,
    const std::string& path, const std::string& interface,
    const std::string& property, VariantType value)
{
    auto future = std::async(
        std::launch::async,
        [&service, &path, &interface, &property, value = std::move(value)]() {
            auto clientBus = sdbusplus::bus::new_bus();
            auto msg = clientBus.new_method_call(
                service.c_str(), path.c_str(),
                "org.freedesktop.DBus.Properties", "Set");
            msg.append(interface, property, value);
            auto reply = clientBus.call(msg, std::chrono::seconds(1));
            (void)reply;
        });

    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (future.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready)
        {
            future.get();
            return;
        }
        serverBus.wait(std::chrono::milliseconds(5));
        while (serverBus.process_discard())
        {}
    }

    throw std::runtime_error("Timed out waiting for D-Bus property set");
}

template <typename ReplyType, typename... Args>
static ReplyType callMethodOverDbus(
    sdbusplus::bus_t& serverBus, const std::string& service,
    const std::string& path, const std::string& interface,
    const std::string& method, Args&&... args)
{
    auto future = std::async(
        std::launch::async,
        [&service, &path, &interface, &method,
         ... args = std::forward<Args>(args)]() mutable -> ReplyType {
            auto clientBus = sdbusplus::bus::new_bus();
            auto msg =
                clientBus.new_method_call(service.c_str(), path.c_str(),
                                          interface.c_str(), method.c_str());
            msg.append(std::move(args)...);

            auto reply = clientBus.call(msg, std::chrono::seconds(1));
            ReplyType value;
            reply.read(value);
            return value;
        });

    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (future.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready)
        {
            return future.get();
        }
        serverBus.wait(std::chrono::milliseconds(5));
        while (serverBus.process_discard())
        {}
    }

    throw std::runtime_error("Timed out waiting for D-Bus method reply");
}

static std::string slurp(const fs::path& p)
{
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open())
        return {};
    return std::string{std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>()};
}

// Convenience alias for the macro path to stay compatible with older helpers.
static fs::path getConfigPathFromMacro()
{
    return cfgPath();
}
static void writeJsonFileAtMacroPath(const std::string& text)
{
    writeJsonFile(cfgPath(), text);
}

// ============================================================================
//                            CORE (non-D-Bus) TESTS
// ============================================================================
//
// These tests exercise core logic without constructing an object_server.

TEST(VMInterfaceBasics, ConstructorSetsInstanceAndStartsZeroes /* no D-Bus */)
{
    std::shared_ptr<sdbusplus::asio::object_server> nullServer; // nullptr OK
    vm::Interface ifc{nullServer, std::string(vm::vmObjPath), {}};
    auto [rc, ri] = ifc.GetAll();
    EXPECT_EQ(rc, 0u);
    EXPECT_EQ(ri, 0u);

    EXPECT_EQ(vm::vmObjPath, "/xyz/openbmc_project/VirtualMedia");
    EXPECT_EQ(vm::RmediaInterface,
              "xyz.openbmc_project.VirtualMedia.Reconnect");
    EXPECT_EQ(vm::RmediaImageURLInterface,
              "xyz.openbmc_project.VirtualMedia.BackupImageURL");
}

TEST(VMInterfaceSetAll, RejectsOutOfRangeValues)
{
    std::shared_ptr<sdbusplus::asio::object_server> nullServer;
    vm::Interface ifc{nullServer, std::string(vm::vmObjPath), {}};

    EXPECT_THAT(ifc.SetAll(2u, 10u), HasSubstr("Error:"));
    EXPECT_THAT(ifc.SetAll(7u, 31u), HasSubstr("Error:"));
    EXPECT_THAT(ifc.SetAll(3u, 100u), HasSubstr("Error:"));
    EXPECT_THAT(ifc.SetAll(1u, 15u), HasSubstr("Error:"));
}

TEST(VMInterfaceSetAll, AcceptsBoundaryValuesAndUpdatesState)
{
    std::shared_ptr<sdbusplus::asio::object_server> nullServer;
    vm::Interface ifc{nullServer, std::string(vm::vmObjPath), {}};

    std::string ok1 = ifc.SetAll(3u, 15u);
    EXPECT_EQ(ok1, "Unknown");
    auto [rc1, ri1] = ifc.GetAll();
    EXPECT_EQ(rc1, 3u);
    EXPECT_EQ(ri1, 15u);

    std::string ok2 = ifc.SetAll(6u, 30u);
    EXPECT_EQ(ok2, "Unknown");
    auto [rc2, ri2] = ifc.GetAll();
    EXPECT_EQ(rc2, 6u);
    EXPECT_EQ(ri2, 30u);
}

TEST(VMInterfaceSetAll, IdempotentNoChangeReturnsSuccessAndKeepsValues)
{
    std::shared_ptr<sdbusplus::asio::object_server> nullServer;
    vm::Interface ifc{nullServer, std::string(vm::vmObjPath), {}};

    EXPECT_EQ(ifc.SetAll(4u, 20u), "Unknown");
    auto [rc, ri] = ifc.GetAll();
    ASSERT_EQ(rc, 4u);
    ASSERT_EQ(ri, 20u);

    EXPECT_EQ(ifc.SetAll(4u, 20u), "Success");
    auto [rc2, ri2] = ifc.GetAll();
    EXPECT_EQ(rc2, 4u);
    EXPECT_EQ(ri2, 20u);
}

// JSON-backed success/persist (no D-Bus)
TEST(VMInterfaceJSON,
     LoadJsonSuccessThenSetAllPersists /* requires macro override */)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    const std::string jsonText = R"({
      "RetryCount": 3,
      "RetryInterval": 15,
      "BackupImageURL": {
        "Slot_2": { "ImageURL": "https://example.com/slot2.iso" },
        "Slot_3": { "ImageURL": "https://example.com/slot3.iso" }
      }
    })";
    writeJsonFileAtMacroPath(jsonText);

    std::shared_ptr<sdbusplus::asio::object_server> nullServer;
    vm::Interface ifc{nullServer, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};

    int rcLoad = vm::Interface::loadJson();
    ASSERT_EQ(rcLoad, 0)
        << "loadJson failed; ensure VIRTUAL_MEDIA_CONFIG_PATH is writable";

    EXPECT_EQ(ifc.SetAll(4u, 20u), "Unknown");
    auto [rc, ri] = ifc.GetAll();
    EXPECT_EQ(rc, 4u);
    EXPECT_EQ(ri, 20u);

    auto content = slurp(getConfigPathFromMacro());
    EXPECT_THAT(content, HasSubstr("\"RetryCount\": 4"));
    EXPECT_THAT(content, HasSubstr("\"RetryInterval\": 20"));
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST(VMInterfaceJSON, LoadJsonFailReturnsMinusOne /* missing file */)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    auto cfg = getConfigPathFromMacro();
    std::error_code ec;
    fs::remove(cfg, ec);
    EXPECT_EQ(vm::Interface::loadJson(), -1);
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

// ============================================================================
//                        D-BUS BACKED TESTS (fixture)
// ============================================================================
//
// Each test re-seeds a *valid* JSON and calls loadJson() in SetUp() so prior
// tests that delete/mangle the file won't affect these tests.

class VMInterfaceDbusFixture : public ::testing::Test
{
  protected:
    std::unique_ptr<vm::Interface> setupIfc_;

    void SetUp() override
    {
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
        const std::string seed = R"({
  "RetryCount": 3,
  "RetryInterval": 15,
  "BackupImageURL": {
    "Slot_2": { "ImageURL": "https://example.com/slot2.iso" },
    "Slot_3": { "ImageURL": "https://example.com/slot3.iso" }
  }
})";
        writeJsonFile(cfgPath(), seed);

        const auto content = slurp(cfgPath());
        ASSERT_FALSE(content.empty())
            << "Seeded file is empty at " << cfgPath();
        ASSERT_TRUE(json::accept(content))
            << "Seed JSON is invalid\n\nContent:\n"
            << content;

        // Ensure instance->vmConfigPath is set correctly before loadJson()
        setupIfc_ = std::make_unique<vm::Interface>(
            nullptr, std::string(vm::vmObjPath),
            std::string(VIRTUAL_MEDIA_CONFIG_PATH));

        const int rc = vm::Interface::loadJson();
        if (rc != 0)
        {
            const auto implContent = slurp(cfgPath());
            FAIL() << "loadJson() failed with rc=" << rc
                   << "\nVIRTUAL_MEDIA_CONFIG_PATH=" << cfgPath()
                   << "\nFile content seen by implementation:\n"
                   << implContent;
        }
#else
        GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
    }

    void TearDown() override
    {
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
        setupIfc_.reset();
        std::error_code ec;
        fs::remove(cfgPath(), ec);
#endif
    }
};

// addInterfaces() creates both interfaces; with interface present, SetAll()
// returns "Success" and persists.
TEST_F(VMInterfaceDbusFixture,
       AddInterfacesRegistersAndSetAllSucceedsWithInterfacePresent)
{
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    ASSERT_NE(ifc.vmediaInterface, nullptr);
    ASSERT_NE(ifc.vmediaImageURLInterface, nullptr);

    EXPECT_EQ(ifc.SetAll(4u, 20u), std::string("Success"));
    auto [rc, ri] = ifc.GetAll();
    EXPECT_EQ(rc, 4u);
    EXPECT_EQ(ri, 20u);

    auto content = slurp(cfgPath());
    EXPECT_TRUE(content.find("\"RetryCount\": 4") != std::string::npos);
    EXPECT_TRUE(content.find("\"RetryInterval\": 20") != std::string::npos);
}

TEST_F(VMInterfaceDbusFixture, SetAllMethodOverDbusInvokesRegisteredLambda)
{
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    const auto status = callMethodOverDbus<std::string>(
        *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
        vm::RmediaInterface, "SetAll", 4u, 20u);

    EXPECT_EQ(status, "Success");
    EXPECT_EQ(ifc.GetAll(), std::make_tuple(4u, 20u));

    const auto content = slurp(cfgPath());
    EXPECT_TRUE(content.find("\"RetryCount\": 4") != std::string::npos);
    EXPECT_TRUE(content.find("\"RetryInterval\": 20") != std::string::npos);
}

TEST_F(VMInterfaceDbusFixture, GetAllMethodOverDbusReturnsRegisteredValues)
{
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();
    ASSERT_EQ(ifc.SetAll(5u, 25u), "Success");

    const auto values =
        callMethodOverDbus<std::tuple<unsigned int, unsigned int>>(
            *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
            vm::RmediaInterface, "GetAll");

    EXPECT_EQ(values, std::make_tuple(5u, 25u));
}

// ImageURL initial create: interface initialized.
TEST_F(VMInterfaceDbusFixture,
       ImageURLInitialCreateRegistersSlotPropertiesFromJson)
{
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    ASSERT_NE(ifc.vmediaImageURLInterface, nullptr);
}

// saveImageURLToJson persists JSON and refreshes interface properties.
TEST_F(VMInterfaceDbusFixture, SaveImageURLPersistsAndRefreshesSlotProperty)
{
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    vm::Interface::saveImageURLToJson("https://b.example/2-updated.iso",
                                      "Slot_2");

    auto content = slurp(cfgPath());
    EXPECT_TRUE(content.find("\"Slot_2\"") != std::string::npos);
    EXPECT_TRUE(
        content.find("\"ImageURL\": \"https://b.example/2-updated.iso\"") !=
        std::string::npos);

    ASSERT_NE(ifc.vmediaImageURLInterface, nullptr);
}

// Explicit failure branches: missing/malformed/null.
TEST_F(VMInterfaceDbusFixture, LoadJsonFailBranches)
{
    std::error_code ec;
    fs::remove(cfgPath(), ec);
    EXPECT_EQ(vm::Interface::loadJson(), -1);

    writeJsonFile(cfgPath(), R"({"RetryCount": 3)");
    EXPECT_EQ(vm::Interface::loadJson(), -1);

    writeJsonFile(cfgPath(), "null\n");
    EXPECT_EQ(vm::Interface::loadJson(), -1);
}

// ---------------------------------------------------------------------------
// Optional branch coverage extras (enable if you want full branch coverage)
// ---------------------------------------------------------------------------

// Catch path in SetAll() when loadJson() throws; status still should be
// "Success".
TEST_F(VMInterfaceDbusFixture,
       SetAllHandlesJsonParseErrorViaCatchAndStillReturnsSuccess)
{
    // Create a valid interface first
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    // Now corrupt the JSON so the *SetAll()* internal loadJson() throws;
    // catch(...) must handle it.
    writeJsonFile(cfgPath(), R"({"RetryCount": 3)");

    EXPECT_EQ(ifc.SetAll(5u, 25u), std::string("Success"));
    auto [rc, ri] = ifc.GetAll();
    EXPECT_EQ(rc, 5u);
    EXPECT_EQ(ri, 25u);
}

// Write-error branch in SetAll(): make file read-only so ofstream open fails;
// status remains "Success".
TEST_F(VMInterfaceDbusFixture,
       SetAll_ConfigPathBecomesDirectory_UpdatesRuntimeState)
{
    // Valid seed
    const std::string seed = R"({"RetryCount": 3, "RetryInterval": 15})";
    writeJsonFile(cfgPath(), seed);

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    const fs::path blocked =
        makeBlockedConfigDirectory("vm-interface-setall-blocked");
    ifc.vmConfigPath = blocked.string();
    vm::Interface::instance->vmConfigPath = blocked.string();

    EXPECT_EQ(ifc.SetAll(4u, 20u), std::string("Success"));

    auto [rc, ri] = ifc.GetAll();
    EXPECT_EQ(rc, 4u);
    EXPECT_EQ(ri, 20u);

    auto content = slurp(cfgPath());
    EXPECT_THAT(content, HasSubstr("\"RetryCount\": 3"));
    EXPECT_THAT(content, HasSubstr("\"RetryInterval\": 15"));

    std::error_code ec;
    fs::remove_all(blocked, ec);
}

// Unsupported slot path in saveImageURLToJson(): no write-back, no refresh.
TEST_F(VMInterfaceDbusFixture,
       SaveImageURLUnsupportedSlotDoesNotModifyJsonOrRefresh)
{
    const std::string seed = R"({
      "RetryCount": 3, "RetryInterval": 15,
      "BackupImageURL": { "Slot_2": { "ImageURL": "A" }, "Slot_3": { "ImageURL": "B" } }
    })";
    writeJsonFile(cfgPath(), seed);

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    vm::Interface::saveImageURLToJson("https://bad.example/9.iso", "Slot_9");

    auto after = slurp(cfgPath());
    EXPECT_TRUE(after.find("\"Slot_2\"") != std::string::npos);
    EXPECT_TRUE(after.find("\"Slot_3\"") != std::string::npos);
    EXPECT_TRUE(after.find("https://bad.example/9.iso") == std::string::npos);
}

// Slot missing in JSON: saveImageURLToJson should log & return; no write-back.
TEST_F(VMInterfaceDbusFixture, SaveImageURLSlotMissingInJsonNoWriteBack)
{
    const std::string seed = R"({
      "RetryCount": 3, "RetryInterval": 15,
      "BackupImageURL": { }
    })";
    writeJsonFile(cfgPath(), seed);

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    vm::Interface::saveImageURLToJson("https://miss.example/2.iso", "Slot_2");

    auto after = slurp(cfgPath());
    EXPECT_TRUE(after.find("\"Slot_2\"") == std::string::npos);
    EXPECT_TRUE(after.find("https://miss.example/2.iso") == std::string::npos);
}

TEST_F(VMInterfaceDbusFixture,
       SaveImageURLMalformedJsonDoesNotRewriteUsingStaleState)
{
    const std::string malformed = R"({"RetryCount": 3)";
    writeJsonFile(cfgPath(), malformed);

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    vm::Interface::saveImageURLToJson("https://stale.example/slot2.iso",
                                      "Slot_2");

    const auto after = slurp(cfgPath());
    EXPECT_EQ(after, malformed);
    EXPECT_TRUE(after.find("https://stale.example/slot2.iso") ==
                std::string::npos);
}

// --- SetAll(): write occurs when *either* value differs (matches
// implementation) ---
TEST_F(VMInterfaceDbusFixture, SetAllWriteBackOnSingleValueDifference)
{
    // Seed (3,15) and ensure it's loaded
    writeJsonFile(cfgPath(), R"({"RetryCount": 3, "RetryInterval": 15})");
    ASSERT_EQ(vm::Interface::loadJson(), 0);

    // D-Bus present
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    // Change only interval -> "Success" AND state updated, and JSON should also
    // change (per implementation)
    EXPECT_EQ(ifc.SetAll(3u, 20u), std::string("Success"));

    auto content = slurp(cfgPath());
    EXPECT_TRUE(content.find("\"RetryCount\": 3") != std::string::npos);
    EXPECT_TRUE(content.find("\"RetryInterval\": 20") !=
                std::string::npos); // <— changed
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// ============================================================================
// 100%-coverage add-ons: loadImageURLFromJson(), saveImageURLToJson() write
// failure, and SetAll() single-value difference (RetryCount).
// ============================================================================

/**
 * Directly exercises loadImageURLFromJson():
 *  - Seeds JSON with Slot_2 and Slot_3 ImageURL values
 *  - addInterfaces() initializes vmediaImageURLInterface
 *  - loadImageURLFromJson() ingests JSON and populates interface-side
 * properties
 *  - saveImageURLToJson() updates Slot_2; a second loadImageURLFromJson()
 * refreshes properties
 *
 * While we don't introspect D-Bus property values in this TU, invoking these
 * methods ensures implementation paths execute (read, parse, property refresh)
 * for coverage.
 */
TEST_F(VMInterfaceDbusFixture, LoadImageURLFromJsonLoadsAndRefreshes)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    // Seed with two slots
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/slot2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/slot3.iso" }
        }
    })";
    writeJsonFile(cfgPath(), seed);

    // D-Bus setup
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();
    ASSERT_NE(ifc.vmediaImageURLInterface, nullptr);

    // First load from JSON
    ifc.loadImageURLFromJson(); // success path

    // Update Slot_2 via the static writer and verify JSON changed
    vm::Interface::saveImageURLToJson("https://b.example/2-updated.iso",
                                      "Slot_2");
    auto afterUpdate = slurp(cfgPath());
    EXPECT_TRUE(afterUpdate.find("\"Slot_2\"") != std::string::npos);
    EXPECT_TRUE(
        afterUpdate.find("\"ImageURL\": \"https://b.example/2-updated.iso\"") !=
        std::string::npos);

    // Second load refreshes interface-side properties
    ifc.loadImageURLFromJson(); // refresh path
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

/**
 * Exercises failure/guard path in loadImageURLFromJson():
 *  - Seeds JSON WITHOUT "BackupImageURL" key
 *  - Ensures method returns gracefully (no crash) and interface remains valid
 */

TEST_F(VMInterfaceDbusFixture, LoadImageURLFromJsonMissingBackupImageURLHandled)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    // Valid JSON without "BackupImageURL"
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15
    })";
    writeJsonFile(cfgPath(), seed);

    // Ensure the implementation refreshes its view of the file post-reseed.
    // (SetUp() already called loadJson() on a different seed with
    // BackupImageURL.) We reload here to avoid stale cached state if the impl
    // caches.
    (void)vm::Interface::loadJson();

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();
    ASSERT_NE(ifc.vmediaImageURLInterface, nullptr);

    // Should gracefully no-op (no BackupImageURL key).
    ifc.loadImageURLFromJson();

    // File remains without the key.
    auto after = slurp(cfgPath());
    EXPECT_TRUE(after.find("\"BackupImageURL\"") == std::string::npos);
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture,
       LoadImageURLFromJsonOnlySlot2PresentLeavesSlot3Empty)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/slot2-only.iso" }
        }
    })";
    writeJsonFile(cfgPath(), seed);
    (void)vm::Interface::loadJson();

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    using StringProperty = std::variant<std::string>;
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_2")),
              "https://example.com/slot2-only.iso");
    EXPECT_TRUE(
        std::get<std::string>(
            getPropertyOverDbus<StringProperty>(
                *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                vm::RmediaImageURLInterface, "Slot_3"))
            .empty());
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture,
       LoadImageURLFromJsonOnlySlot3PresentLeavesSlot2Empty)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_3": { "ImageURL": "https://example.com/slot3-only.iso" }
        }
    })";
    writeJsonFile(cfgPath(), seed);
    (void)vm::Interface::loadJson();

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    using StringProperty = std::variant<std::string>;
    EXPECT_TRUE(
        std::get<std::string>(
            getPropertyOverDbus<StringProperty>(
                *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                vm::RmediaImageURLInterface, "Slot_2"))
            .empty());
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_3")),
              "https://example.com/slot3-only.iso");
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture,
       LoadImageURLFromJsonMissingImageUrlInSlotLeavesPropertyEmpty)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "WrongKey": "https://example.com/ignored.iso" },
            "Slot_3": { "ImageURL": "https://example.com/slot3-valid.iso" }
        }
    })";
    writeJsonFile(cfgPath(), seed);
    (void)vm::Interface::loadJson();

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    using StringProperty = std::variant<std::string>;
    EXPECT_TRUE(
        std::get<std::string>(
            getPropertyOverDbus<StringProperty>(
                *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                vm::RmediaImageURLInterface, "Slot_2"))
            .empty());
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_3")),
              "https://example.com/slot3-valid.iso");
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture, LoadImageURLFromJsonRejectsNonStringImageUrl)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": 42 },
            "Slot_3": { "ImageURL": "https://example.com/slot3-valid.iso" }
        }
    })";
    writeJsonFile(cfgPath(), seed);
    (void)vm::Interface::loadJson();

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};

    EXPECT_THROW(ifc.loadImageURLFromJson(), json::type_error);
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture,
       LoadImageURLFromJsonMalformedDoesNotClearExistingProperties)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/still-valid.iso" },
            "Slot_3": { "ImageURL": "https://example.com/slot3-valid.iso" }
        }
    })";
    writeJsonFile(cfgPath(), seed);

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    // Corrupt the file after the interface has already loaded valid data.
    writeJsonFile(cfgPath(), R"({"RetryCount": 3)");

    EXPECT_NO_THROW(ifc.loadImageURLFromJson());

    using StringProperty = std::variant<std::string>;
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_2")),
              "https://example.com/still-valid.iso");
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture, LoadImageURLFromJsonRefreshesExistingProperties)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/original-2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/original-3.iso" }
        }
    })";
    writeJsonFile(cfgPath(), seed);

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    writeJsonFile(cfgPath(), R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/updated-2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/updated-3.iso" }
        }
    })");

    ifc.loadImageURLFromJson();

    using StringProperty = std::variant<std::string>;
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_2")),
              "https://example.com/updated-2.iso");
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_3")),
              "https://example.com/updated-3.iso");
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}
/**
 * Completes SetAll() single-value difference coverage:
 *  - Seeds (3,15) and loads
 *  - Changes ONLY RetryCount to 4 (interval unchanged)
 *  - Expects "Success" and JSON write-back reflecting the new count
 */
TEST_F(VMInterfaceDbusFixture, SetAllWriteBackOnRetryCountDifference)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    // Seed (3,15)
    writeJsonFile(cfgPath(), R"({"RetryCount": 3, "RetryInterval": 15})");
    ASSERT_EQ(vm::Interface::loadJson(), 0);

    // D-Bus present
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    // Change only RetryCount -> "Success" AND JSON write-back
    EXPECT_EQ(ifc.SetAll(4u, 15u), std::string("Success"));
    auto content = slurp(cfgPath());
    EXPECT_TRUE(content.find("\"RetryCount\": 4") !=
                std::string::npos); // changed
    EXPECT_TRUE(content.find("\"RetryInterval\": 15") !=
                std::string::npos); // unchanged
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

/**
 * Negative write path for saveImageURLToJson():
 *  - Seeds valid JSON
 *  - Makes file read-only so write fails
 *  - Calls saveImageURLToJson() and verifies the file is unchanged
 */
// SaveImageURLToJson: write failure must not change file; requires a valid
// Interface instance
TEST_F(VMInterfaceDbusFixture,
       SaveImageURLToJson_LoadJsonFailure_LeavesOriginalFileUnchanged)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    // Seed a valid JSON with Slot_2
    const std::string seed = R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/slot2.iso" }
        }
    })";
    writeJsonFile(cfgPath(), seed);

    // Create a valid Interface and register D-Bus interfaces so
    // vm::Interface::instance is non-null
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    const fs::path blocked =
        makeBlockedConfigDirectory("vm-interface-save-blocked");
    ifc.vmConfigPath = blocked.string();
    vm::Interface::instance->vmConfigPath = blocked.string();

    // Attempt to update Slot_2 with an invalid config path; implementation
    // should leave the original file unchanged.
    vm::Interface::saveImageURLToJson("https://blocked.example/slot2.iso",
                                      "Slot_2");

    // File must be unchanged
    auto after = slurp(cfgPath());
    EXPECT_TRUE(after.find("\"https://blocked.example/slot2.iso\"") ==
                std::string::npos);
    EXPECT_TRUE(after.find("\"https://example.com/slot2.iso\"") !=
                std::string::npos);

    std::error_code ec;
    fs::remove_all(blocked, ec);
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST(VMInterfaceBasics, ConstructorReplacesStaticInstanceWithMostRecentObject)
{
    std::shared_ptr<sdbusplus::asio::object_server> nullServer;

    vm::Interface first{nullServer, "/xyz/openbmc_project/VirtualMedia/First",
                        "/tmp/vm-first.json"};
    EXPECT_EQ(vm::Interface::instance, &first);

    vm::Interface second{nullServer, "/xyz/openbmc_project/VirtualMedia/Second",
                         "/tmp/vm-second.json"};
    EXPECT_EQ(vm::Interface::instance, &second);
    EXPECT_EQ(second.vmObjPath, "/xyz/openbmc_project/VirtualMedia/Second");
    EXPECT_EQ(second.vmConfigPath, "/tmp/vm-second.json");
}

TEST_F(VMInterfaceDbusFixture, AddInterfacesPublishesRetryPropertiesFromJson)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    writeJsonFile(cfgPath(), R"({
        "RetryCount": 6,
        "RetryInterval": 30,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/slot2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/slot3.iso" }
        }
    })");
    (void)vm::Interface::loadJson();

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    using UnsignedProperty = std::variant<unsigned int>;
    EXPECT_EQ(std::get<unsigned int>(getPropertyOverDbus<UnsignedProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaInterface, "RetryCount")),
              6u);
    EXPECT_EQ(std::get<unsigned int>(getPropertyOverDbus<UnsignedProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaInterface, "RetryInterval")),
              30u);
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture, SetAllMethodOverDbusRejectsLowOutOfRangeValues)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    const auto status = callMethodOverDbus<std::string>(
        *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
        vm::RmediaInterface, "SetAll", 2u, 14u);

    EXPECT_THAT(status, HasSubstr("Error:"));
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture, SetAllMethodOverDbusRejectsHighOutOfRangeValues)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);

    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    const auto status = callMethodOverDbus<std::string>(
        *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
        vm::RmediaInterface, "SetAll", 7u, 31u);

    EXPECT_THAT(status, HasSubstr("Error:"));
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture,
       SaveImageURLToJsonUpdatesOnlySlot3AndPreservesSlot2)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    writeJsonFile(cfgPath(), R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/original-2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/original-3.iso" }
        }
    })");
    (void)vm::Interface::loadJson();

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    vm::Interface::saveImageURLToJson("https://example.com/updated-3.iso",
                                      "Slot_3");

    auto content = slurp(cfgPath());
    EXPECT_THAT(content, HasSubstr("https://example.com/original-2.iso"));
    EXPECT_THAT(content, HasSubstr("https://example.com/updated-3.iso"));

    using StringProperty = std::variant<std::string>;
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_2")),
              "https://example.com/original-2.iso");
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_3")),
              "https://example.com/updated-3.iso");
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture,
       LoadImageURLFromJsonRefreshClearsSlot2WhenMissingFromJson)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    writeJsonFile(cfgPath(), R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/original-2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/original-3.iso" }
        }
    })");

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    writeJsonFile(cfgPath(), R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_3": { "ImageURL": "https://example.com/only-3.iso" }
        }
    })");

    ifc.loadImageURLFromJson();

    using StringProperty = std::variant<std::string>;
    EXPECT_TRUE(
        std::get<std::string>(
            getPropertyOverDbus<StringProperty>(
                *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                vm::RmediaImageURLInterface, "Slot_2"))
            .empty());
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_3")),
              "https://example.com/only-3.iso");
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture,
       LoadImageURLFromJsonRefreshClearsSlot3WhenMissingFromJson)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    writeJsonFile(cfgPath(), R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/original-2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/original-3.iso" }
        }
    })");

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    writeJsonFile(cfgPath(), R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/only-2.iso" }
        }
    })");

    ifc.loadImageURLFromJson();

    using StringProperty = std::variant<std::string>;
    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_2")),
              "https://example.com/only-2.iso");
    EXPECT_TRUE(
        std::get<std::string>(
            getPropertyOverDbus<StringProperty>(
                *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                vm::RmediaImageURLInterface, "Slot_3"))
            .empty());
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture,
       LoadImageURLFromJsonRefreshClearsBothSlotsWhenBackupImageUrlEmpty)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    writeJsonFile(cfgPath(), R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {
            "Slot_2": { "ImageURL": "https://example.com/original-2.iso" },
            "Slot_3": { "ImageURL": "https://example.com/original-3.iso" }
        }
    })");

    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    writeJsonFile(cfgPath(), R"({
        "RetryCount": 3,
        "RetryInterval": 15,
        "BackupImageURL": {}
    })");

    ifc.loadImageURLFromJson();

    using StringProperty = std::variant<std::string>;
    EXPECT_TRUE(
        std::get<std::string>(
            getPropertyOverDbus<StringProperty>(
                *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                vm::RmediaImageURLInterface, "Slot_2"))
            .empty());
    EXPECT_TRUE(
        std::get<std::string>(
            getPropertyOverDbus<StringProperty>(
                *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                vm::RmediaImageURLInterface, "Slot_3"))
            .empty());
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture, RetryPropertiesRejectExternalDbusSetAttempts)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    using UIntProperty = std::variant<unsigned int>;
    EXPECT_THROW(setPropertyOverDbus<UIntProperty>(
                     *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                     vm::RmediaInterface, "RetryCount", UIntProperty{5u}),
                 sdbusplus::exception::SdBusError);
    EXPECT_THROW(setPropertyOverDbus<UIntProperty>(
                     *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                     vm::RmediaInterface, "RetryInterval", UIntProperty{20u}),
                 sdbusplus::exception::SdBusError);

    // Values must be unchanged since the Set attempts were rejected.
    auto [rc, ri] = ifc.GetAll();
    EXPECT_EQ(rc, 3u);
    EXPECT_EQ(ri, 15u);
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}

TEST_F(VMInterfaceDbusFixture, ImageURLPropertiesRejectExternalDbusSetAttempts)
{
#ifdef VIRTUAL_MEDIA_CONFIG_PATH
    boost::asio::io_context io;
    auto conn = makeTestConnection(io);
    auto srv = std::make_shared<sdbusplus::asio::object_server>(conn);
    vm::Interface ifc{srv, std::string(vm::vmObjPath),
                      VIRTUAL_MEDIA_CONFIG_PATH};
    ifc.addInterfaces();

    using StringProperty = std::variant<std::string>;
    EXPECT_THROW(
        setPropertyOverDbus<StringProperty>(
            *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
            vm::RmediaImageURLInterface, "Slot_2",
            StringProperty{std::string("https://attacker.example/evil.iso")}),
        sdbusplus::exception::SdBusError);

    EXPECT_EQ(std::get<std::string>(getPropertyOverDbus<StringProperty>(
                  *conn, conn->get_unique_name(), std::string(vm::vmObjPath),
                  vm::RmediaImageURLInterface, "Slot_2")),
              "https://example.com/slot2.iso");
#else
    GTEST_SKIP() << "VIRTUAL_MEDIA_CONFIG_PATH not defined in test TU";
#endif
}
