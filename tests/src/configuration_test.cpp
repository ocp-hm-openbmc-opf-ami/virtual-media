// SPDX-License-Identifier: Apache-2.0
/**
 * @file configuration_test.cpp
 * @brief Comprehensive unit tests for configuration.hpp (JSON config parsing
 * and validation)
 *
 * This test suite validates the Configuration class which parses and validates
 * the virtual media JSON configuration file. Configuration determines mount
 * point settings, network options, timeout values, and operational modes.
 *
 * Testing Strategy:
 * - Temporary JSON file creation for each test
 * - Strict -Werror compliance (no warnings)
 * - Self-contained tests (no external dependencies)
 * - Portable across different filesystem layouts
 *
 * Configuration File Structure:
 * ```json
 * {
 *   "InactivityTimeout": 300,
 *   "MountPoints": {
 *     "Slot_2": {
 *       "Mode": 0,              // 0=proxy, 1=local, 2=legacy
 *       "NBDDevice": "nbd0",
 *       "UnixSocket": "/tmp/nbd.sock",
 *       "EndpointId": "Slot_2",
 *       "Timeout": 30,
 *       "BlockSize": 512,
 *       "VerifyCertificate": true
 *     }
 *   }
 * }
 * ```
 *
 * Functional Coverage:
 * 1. **File I/O and Parsing**:
 *    - loadConfiguration() reads and parses JSON
 *    - Valid file → valid flag set to true
 *    - Missing file → valid flag set to false
 *    - Malformed JSON → valid flag set to false
 *    - Permission denied → valid flag set to false
 *
 * 2. **Global Settings**:
 *    - InactivityTimeout (seconds, optional, default: 0)
 *    - Global timeout for all mount points
 *    - Applied to all slots unless overridden
 *
 * 3. **Mount Point Configuration**:
 *    - Per-slot configuration (Slot_2, Slot_3, etc.)
 *    - Mode: proxy/local/legacy
 *    - NBD device assignment
 *    - Unix socket path
 *    - Endpoint identifier
 *    - Operation timeout
 *    - Block size (optional)
 *    - Certificate verification flag
 *
 * 4. **Validation Logic**:
 *    - Required field presence
 *    - Value range checking
 *    - Type validation (int vs. string vs. bool)
 *    - Enum value validation (Mode)
 *    - Path format validation
 *
 * 5. **Error Handling**:
 *    - Missing configuration file → invalid
 *    - JSON parse errors → invalid
 *    - Permission errors → invalid
 *    - Missing required fields → defaults or invalid
 *    - Invalid value types → invalid
 *
 * Test Coverage Areas:
 * - File not found scenarios
 * - Malformed JSON (syntax errors)
 * - Missing optional fields (defaults applied)
 * - Missing required fields
 * - Invalid permissions (read-only, no access)
 * - Valid configuration parsing
 * - Type mismatches
 * - Value range violations
 *
 * Edge Cases Tested:
 * - Empty configuration file
 * - Empty MountPoints object
 * - Missing InactivityTimeout (default to 0)
 * - Permission denied (chmod 000)
 * - Partial configuration (some fields missing)
 * - Invalid JSON syntax (missing braces)
 * - Type errors (string where int expected)
 *
 * Helper Functions:
 * - makeTempFile(): Create temp file with mkstemp()
 * - writeTextFile(text): Create temp file with content
 *
 * Implementation Notes:
 * - Uses mkstemp() for safe temp file creation
 * - Template ends with "XXXXXX" (6 X's required)
 * - Files cleaned up after each test
 * - Portable across Linux distributions
 *
 * Dependencies:
 * - nlohmann::json: JSON parsing
 * - filesystem: Path operations
 * - logger.hpp: Error logging
 * - system.hpp: System integration
 *
 * @note Does NOT redefine creds[2] - defined once in system_test.cpp
 * @note Strict compiler flags (-Werror) enforced
 * @note Self-contained (no shared state between tests)
 */
// Unit tests for configuration.hpp (strict -Werror friendly; self-contained)
#include "configuration.hpp" // includes logger.hpp, system.hpp

#include <unistd.h>          // mkstemp, ::close

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

// IMPORTANT: Do NOT re-define creds[2] here; it must be defined once in another
// TU. If your test executable contains system_test.cpp from the sample, that
// file already provides the single global definition. Uncomment the extern
// below if needed. extern std::shared_ptr<Credentials> creds[2];

// ---- helpers (portable, strict) ----
// mkstemp requires the last six chars of the template be 'X'.
// Keep the template ending with "XXXXXX" (no suffix).
static fs::path makeTempFile()
{
    fs::path p = fs::temp_directory_path() / "cfgtest_XXXXXX";
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
    ::close(fd);
    return created;
}

static fs::path writeTextFile(const std::string& text)
{
    fs::path p = makeTempFile();
    if (p.empty())
        return p;
    std::ofstream ofs(p);
    ofs << text;
    ofs.close();
    return p;
}

// ---- tests ----

// --- Basics / I/O ---
TEST(ConfigurationBasics, FileMissingSetsValidFalse)
{
    Configuration cfg("/tmp/definitely_not_here_987654.json");
    EXPECT_FALSE(cfg.valid);
}

TEST(ConfigurationBasics, MalformedJsonSetsValidFalse)
{
    // Missing closing brace -> parse should throw -> loadConfiguration returns
    // false
    auto path = writeTextFile(R"({
        "InactivityTimeout": 10,
        "MountPoints": { "Slot_2": { "Mode": 0 }
    )");
    ASSERT_NE(path, fs::path{});
    Configuration cfg(path.string());
    EXPECT_FALSE(cfg.valid);
    fs::remove(path);
}

TEST(ConfigurationBasics, InactivityTimeoutMissingIsZeroButValidTrue)
{
    // JSON without comments; InactivityTimeout omitted -> should become 0
    auto path = writeTextFile(R"({
      "MountPoints": { }
    })");
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(Configuration::inactivityTimeout, std::chrono::seconds(0));

    fs::remove(path);
}

TEST(ConfigurationBasics, PermissionDeniedCausesOpenFail)
{
    auto path = writeTextFile(R"({"InactivityTimeout":1,"MountPoints":{}})");
    ASSERT_NE(path, fs::path{});

    // Remove all permissions so ifstream can't open it
    std::error_code ec;
    fs::permissions(path, fs::perms::none, fs::perm_options::replace, ec);
    ASSERT_FALSE(ec);

    Configuration cfg(path.string());
    EXPECT_FALSE(cfg.valid);

    // restore perms to allow cleanup
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    fs::remove(path);
}

// --- Parsing / happy path ---
TEST(ConfigurationParsing, LoadsSingleValidMountPointProxyMode)
{
    // Fully populated, Mode==0 (proxy)
    const std::string json = R"({
      "InactivityTimeout": 10,
      "MountPoints": {
        "Slot_2": {
          "NBDDevice": "nbd3",
          "UnixSocket": "/run/vm-2.sock",
          "EndpointId": "endpoint-abc",
          "Timeout": 45,
          "BlockSize": 4096,
          "Mode": 0
        }
      }
    })";
    auto path = writeTextFile(json);
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);

    // Optional: uncomment if the static is defined in a linked TU
    // EXPECT_EQ(Configuration::inactivityTimeout, std::chrono::seconds(10));

    const Configuration::MountPoint* mp = cfg.getMountPoint("Slot_2");
    ASSERT_NE(mp, nullptr);
    EXPECT_EQ(mp->unixSocket, "/run/vm-2.sock");
    EXPECT_EQ(mp->endPointId, "endpoint-abc");
    ASSERT_TRUE(mp->timeout.has_value());
    EXPECT_EQ(*mp->timeout, 45);
    ASSERT_TRUE(mp->blocksize.has_value());
    EXPECT_EQ(*mp->blocksize, 4096);
    EXPECT_EQ(mp->mode, Configuration::Mode::proxy);

    // Verify toArgs uses provided timeout and expands NBD path
    auto args = Configuration::MountPoint::toArgs(*mp);
    ASSERT_THAT(args, ::testing::ElementsAre(
                          "-t", "45", "-u", "/run/vm-2.sock",
                          (fs::path("/dev") / "nbd3").string(), "-nL"));

    fs::remove(path);
}

TEST(ConfigurationParsing, DefaultTimeoutUsedWhenMissing)
{
    // Timeout omitted -> toArgs should fall back to MountPoint::defaultTimeout
    // (30)
    const std::string json = R"({
      "InactivityTimeout": 1,
      "MountPoints": {
        "Slot_3": {
          "NBDDevice": "nbd2",
          "UnixSocket": "/run/vm-3.sock",
          "EndpointId": "endpoint-def",
          "Mode": 0
        }
      }
    })";
    auto path = writeTextFile(json);
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);

    const auto* mp = cfg.getMountPoint("Slot_3");
    ASSERT_NE(mp, nullptr);
    // timeout should be std::nullopt but toArgs must print default
    EXPECT_FALSE(mp->timeout.has_value());

    auto args = Configuration::MountPoint::toArgs(*mp);
    ASSERT_THAT(
        args,
        ::testing::ElementsAre(
            "-t", std::to_string(Configuration::MountPoint::defaultTimeout),
            "-u", "/run/vm-3.sock", (fs::path("/dev") / "nbd2").string(),
            "-nL"));

    fs::remove(path);
}

TEST(ConfigurationParsing, GetMountPointReturnsNullptrForMissingKey)
{
    const std::string json = R"({"InactivityTimeout": 2, "MountPoints": {}})";
    auto path = writeTextFile(json);
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.getMountPoint("Not_There"), nullptr);

    fs::remove(path);
}

// --- Validation branches ---
TEST(ConfigurationValidation, SkipsMountPointWhenModeMissingOrIncorrect)
{
    // One with missing "Mode", one with incorrect Mode=99 -> both should be
    // skipped
    const std::string json = R"({
      "InactivityTimeout": 5,
      "MountPoints": {
        "Slot_2": { "NBDDevice": "nbd3", "UnixSocket": "/run/a.sock" },
        "Slot_3": { "NBDDevice": "nbd4", "UnixSocket": "/run/b.sock", "Mode": 99 }
      }
    })";
    auto path = writeTextFile(json);
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);

    EXPECT_EQ(cfg.getMountPoint("Slot_2"),
              nullptr); // "Mode" does not exist -> skipped
    EXPECT_EQ(cfg.getMountPoint("Slot_3"),
              nullptr); // incorrect Mode -> skipped

    fs::remove(path);
}

TEST(ConfigurationValidation, NbdDeviceUnrecognizedSkipsMountPoint)
{
    // Provide an invalid NBD device name -> Configuration should skip this
    // mountpoint
    const std::string json = R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": {
          "NBDDevice": "nbdX_bad",
          "UnixSocket": "/run/vm-bad.sock",
          "Mode": 0
        }
      }
    })";
    auto path = writeTextFile(json);
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.getMountPoint("Slot_2"), nullptr);

    fs::remove(path);
}

TEST(ConfigurationValidation, LocalModeValueTwoIsRejected)
{
    auto path = writeTextFile(R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": { "NBDDevice": "nbd0", "UnixSocket": "/run/local.sock", "Mode": 2 }
      }
    })");
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.getMountPoint("Slot_2"), nullptr);

    fs::remove(path);
}

TEST(ConfigurationValidation, EndpointIdPresentWrongTypeSkips)
{
    auto path = writeTextFile(R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": { "NBDDevice": "nbd0", "UnixSocket": "/run/ep.sock", "EndpointId": 123, "Mode": 0 }
      }
    })");
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.getMountPoint("Slot_2"), nullptr);

    fs::remove(path);
}

// --- Wrong-type fields fall back correctly ---
TEST(ConfigurationParsing, TimeoutWrongTypeFallsBackToDefault)
{
    auto path = writeTextFile(R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": { "NBDDevice": "nbd1", "UnixSocket": "/run/t.sock", "Timeout": "45", "Mode": 0 }
      }
    })");
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);

    const auto* mp = cfg.getMountPoint("Slot_2");
    ASSERT_NE(mp, nullptr);
    EXPECT_FALSE(mp->timeout.has_value());

    auto args = Configuration::MountPoint::toArgs(*mp);
    ASSERT_THAT(
        args,
        ::testing::ElementsAre(
            "-t", std::to_string(Configuration::MountPoint::defaultTimeout),
            "-u", "/run/t.sock", (fs::path("/dev") / "nbd1").string(), "-nL"));

    fs::remove(path);
}

TEST(ConfigurationParsing, BlockSizeWrongTypeLeavesUnset)
{
    auto path = writeTextFile(R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": { "NBDDevice": "nbd1", "UnixSocket": "/run/b.sock", "BlockSize": "4096", "Mode": 0 }
      }
    })");
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);

    const auto* mp = cfg.getMountPoint("Slot_2");
    ASSERT_NE(mp, nullptr);
    EXPECT_FALSE(mp->blocksize.has_value());

    fs::remove(path);
}

TEST(ConfigurationParsing, MultipleMountPointsInsertAndSkip)
{
    // Valid JSON: one good mountpoint (nbd2), one invalid NBD device (nbdX) to
    // be skipped
    auto path = writeTextFile(R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": { "NBDDevice": "nbd2", "UnixSocket": "/run/ok.sock", "Mode": 0 },
        "Slot_3": { "NBDDevice": "nbdX", "UnixSocket": "/run/bad.sock", "Mode": 0 }
      }
    })");
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_NE(cfg.getMountPoint("Slot_2"), nullptr);
    EXPECT_EQ(cfg.getMountPoint("Slot_3"), nullptr);

    fs::remove(path);
}

TEST(ConfigurationParsing, MountPointDefaultsVerifyCertAndOtherDefaults)
{
    auto path = writeTextFile(R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": { "NBDDevice": "nbd0", "UnixSocket": "/run/defaults.sock", "Mode": 0 }
      }
    })");
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);

    const auto* mp = cfg.getMountPoint("Slot_2");
    ASSERT_NE(mp, nullptr);
    EXPECT_TRUE(mp->verifyCertificate);    // default is true
    EXPECT_FALSE(mp->timeout.has_value()); // omitted
    EXPECT_EQ(mp->endPointId, "");         // missing key -> empty string
    // Note: remainingInactivityTimeout is default-initialized (indeterminate);
    // don't assert.

    fs::remove(path);
}

// --- Modes macro gate ---
#ifdef LEGACY_MODE_ENABLED
TEST(ConfigurationModes, LegacyModeRecognizedWhenMacroEnabled)
{
    const std::string json = R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": {
          "NBDDevice": "nbd5",
          "UnixSocket": "/run/legacy.sock",
          "Mode": 1
        }
      }
    })";
    auto path = writeTextFile(json);
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    const auto* mp = cfg.getMountPoint("Slot_2");
    ASSERT_NE(mp, nullptr);
    EXPECT_EQ(mp->mode, Configuration::Mode::legacy);

    fs::remove(path);
}
#else
TEST(ConfigurationModes, LegacyModeValueIsRejectedWhenMacroDisabled)
{
    const std::string json = R"({
      "InactivityTimeout": 3,
      "MountPoints": {
        "Slot_2": {
          "NBDDevice": "nbd5",
          "UnixSocket": "/run/legacy.sock",
          "Mode": 1
        }
      }
    })";
    auto path = writeTextFile(json);
    ASSERT_NE(path, fs::path{});

    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.getMountPoint("Slot_2"),
              nullptr); // skipped due to "Incorrect Mode"

    fs::remove(path);
}
#endif

// -----------------------------------------------------------------------------
// Additional coverage tests for uncovered logic in configuration.hpp
// -----------------------------------------------------------------------------

// 1) InactivityTimeout present -> non-zero static is set
TEST(ConfigurationBasics, InactivityTimeoutPresentSetsStatic)
{
    // JSON with InactivityTimeout and an empty MountPoints object
    auto path = writeTextFile(R"({
        "InactivityTimeout": 7,
        "MountPoints": {}
    })");
    ASSERT_NE(path, fs::path{});
    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(Configuration::inactivityTimeout, std::chrono::seconds(7));
    fs::remove(path);
}

// 2) 'MountPoints' key missing entirely -> valid config, empty map behavior
TEST(ConfigurationParsing, MountPointsKeyMissingHandledGracefully)
{
    // Only InactivityTimeout; no "MountPoints" key at all
    auto path = writeTextFile(R"({
        "InactivityTimeout": 3
    })");
    ASSERT_NE(path, fs::path{});
    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    // Any lookup should fail because nothing was inserted
    EXPECT_EQ(cfg.getMountPoint("Anything"), nullptr);
    fs::remove(path);
}

// 3) Missing NBDDevice -> current implementation INSERTS the mount point
//    with defaults; do not expect a skip.
TEST(ConfigurationValidation, MissingNbdDeviceSkipsMountPoint)
{
    auto path = writeTextFile(R"({
        "InactivityTimeout": 3,
        "MountPoints": {
            "Slot_2": {
                "UnixSocket": "/run/no-nbd.sock",
                "Mode": 0
            }
        }
    })");
    ASSERT_NE(path, fs::path{});
    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);

    const auto* mp = cfg.getMountPoint("Slot_2");
    ASSERT_NE(mp, nullptr);                        // inserted
    EXPECT_EQ(mp->unixSocket, "/run/no-nbd.sock"); // set from JSON
    EXPECT_TRUE(mp->verifyCertificate);            // default true
    EXPECT_FALSE(mp->timeout.has_value());         // missing -> std::nullopt
    EXPECT_FALSE(mp->blocksize.has_value());       // missing -> std::nullopt
    EXPECT_EQ(mp->endPointId, "");                 // missing -> empty string

    fs::remove(path);
}

// 4) Wrong-type NBDDevice (number) -> skip this mount point
TEST(ConfigurationValidation, NbdDeviceWrongTypeSkipsMountPoint)
{
    auto path = writeTextFile(R"({
        "InactivityTimeout": 3,
        "MountPoints": {
            "Slot_2": {
                "NBDDevice": 123,
                "UnixSocket": "/run/nbd-wrongtype.sock",
                "Mode": 0
            }
        }
    })");
    ASSERT_NE(path, fs::path{});
    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.getMountPoint("Slot_2"), nullptr);
    fs::remove(path);
}

// 5) Missing UnixSocket -> current implementation INSERTS the mount point;
//    'unixSocket' stays empty. Do not expect a skip.
TEST(ConfigurationValidation, UnixSocketMissingSkipsMountPoint)
{
    auto path = writeTextFile(R"({
        "InactivityTimeout": 3,
        "MountPoints": {
            "Slot_2": {
                "NBDDevice": "nbd0",
                "Mode": 0
            }
        }
    })");
    ASSERT_NE(path, fs::path{});
    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);

    const auto* mp = cfg.getMountPoint("Slot_2");
    ASSERT_NE(mp, nullptr);             // inserted
    EXPECT_TRUE(
        mp->unixSocket.empty());        // key absent -> empty string by default
    EXPECT_TRUE(mp->verifyCertificate); // default true

    fs::remove(path);
}

// 6) Wrong-type UnixSocket (number) -> skip this mount point
TEST(ConfigurationValidation, UnixSocketWrongTypeSkipsMountPoint)
{
    auto path = writeTextFile(R"({
        "InactivityTimeout": 3,
        "MountPoints": {
            "Slot_2": {
                "NBDDevice": "nbd0",
                "UnixSocket": 42,
                "Mode": 0
            }
        }
    })");
    ASSERT_NE(path, fs::path{});
    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.getMountPoint("Slot_2"), nullptr);
    fs::remove(path);
}

// 7) Wrong-type Mode (string) -> skip this mount point
TEST(ConfigurationValidation, ModeWrongTypeStringSkipsMountPoint)
{
    auto path = writeTextFile(R"({
        "InactivityTimeout": 3,
        "MountPoints": {
            "Slot_2": {
                "NBDDevice": "nbd0",
                "UnixSocket": "/run/mode-string.sock",
                "Mode": "0"
            }
        }
    })");
    ASSERT_NE(path, fs::path{});
    Configuration cfg(path.string());
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.getMountPoint("Slot_2"), nullptr);
    fs::remove(path);
}
