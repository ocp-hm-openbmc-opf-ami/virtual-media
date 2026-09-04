// SPDX-License-Identifier: Apache-2.0
/**
 * @file logger_test.cpp
 * @brief Unit tests for Logger (compile-time level filtering, output
 * formatting, baseName(), operator<<(vector), and LogMsg macro).
 *
 * Assumes LOG_LEVEL == Logger::Info at build time.
 * Output is verified by redirecting std::cout to a std::ostringstream.
 */
// tests/src/logger_test.cpp
#include "logger.hpp" // adjust if your path differs

#include <regex>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>

using namespace Logger;

TEST(BaseNameTest, HandlesUnixStylePath)
{
    constexpr const char* p = baseName("dir/sub/file.hpp");
    EXPECT_STREQ(p, "file.hpp");
}

TEST(BaseNameTest, HandlesWindowsStylePath)
{
    constexpr const char* p = baseName("C:\\projects\\app\\main.cpp");
    EXPECT_STREQ(p, "main.cpp");
}

TEST(VectorOstreamTest, PrintsElementsSeparatedBySpacesWithTrailingSpace)
{
    std::vector<int> v{1, 2, 3};
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ("1 2 3 ", oss.str());
}

TEST(LogImplTest, RespectsLogLevelGating)
{
    // LOG_LEVEL == Info (4): Debug (5) suppressed; Info (4) emitted.
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());

    Logger::log<Debug>("f.cpp", 10, "fn", "debug msg");
    std::cout.rdbuf(old);
    EXPECT_TRUE(oss.str().empty())
        << "Debug should be suppressed at LOG_LEVEL Info";

    oss.str("");
    oss.clear();
    old = std::cout.rdbuf(oss.rdbuf());
    Logger::log<Info>("f.cpp", 10, "fn", "info msg");
    std::cout.rdbuf(old);
    EXPECT_FALSE(oss.str().empty())
        << "Info should be emitted at LOG_LEVEL Info";
    EXPECT_NE(oss.str().find("[Info    ]"), std::string::npos)
        << "Info level name must appear in output";
}

TEST(LogImplTest, FormatsOutputCorrectly)
{
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());

    Logger::log<Warning>("file.cpp", 123, "myFunc", "warn: ", 42);
    std::cout.rdbuf(old);

    const std::string s = oss.str();
    EXPECT_NE(s.find("[Warning ] [file.cpp:123] myFunc(): warn: 42"),
              std::string::npos)
        << s;
    EXPECT_EQ('\n', s.back());
}

static void callMacroWarning()
{
    LogMsg(Logger::Warning, "hello ", 42);
}

static void callMacroDebug()
{
    LogMsg(Logger::Debug, "dbg ", 99);
}

TEST(LogMacroTest, UsesBaseNameFunctionAndFormatsLineNumber)
{
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());

    callMacroWarning();
    std::cout.rdbuf(old);

    const std::string out = oss.str();
    constexpr const char* base = Logger::baseName(__FILE__);

    std::string pat = std::string("\\[Warning \\] \\[") + base +
                      ":\\d+\\] callMacroWarning\\(\\): hello 42\\n";
    std::regex re(pat);
    EXPECT_TRUE(std::regex_search(out, re)) << out;
}

TEST(LogMacroTest, SuppressesDebugAtInfoLevel)
{
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());

    callMacroDebug();
    std::cout.rdbuf(old);

    EXPECT_TRUE(oss.str().empty())
        << "Debug should be suppressed by LOG_LEVEL Info";
}

// ------------------------------
// baseName() — additional edge cases
// ------------------------------

TEST(BaseNameEdgeTest, HandlesSingleComponent)
{
    constexpr const char* p = baseName("file.cpp");
    EXPECT_STREQ(p, "file.cpp");
}

TEST(BaseNameEdgeTest, HandlesMixedSeparators)
{
    constexpr const char* p = baseName("dir\\sub/file.cpp");
    EXPECT_STREQ(p, "file.cpp");
}

// ------------------------------
// operator<<(std::vector<T>) — additional cases
// ------------------------------

TEST(VectorOstreamEdgeTest, EmptyVectorProducesEmptyString)
{
    std::vector<int> v{};
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ("", oss.str());
}

TEST(VectorOstreamEdgeTest, SingleElementHasTrailingSpace)
{
    std::vector<int> v{42};
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ("42 ", oss.str());
}

// ------------------------------
// LogMsg macro — deeper checks
// ------------------------------

// Helper functions to call the macro so __FUNCTION__ is predictable.
static void callMacroInfo()
{
    LogMsg(Logger::Info, "info payload");
}
static void callMacroError()
{
    LogMsg(Logger::Error, "err ", 123);
}
static void callMacroCritical()
{
    LogMsg(Logger::Critical, "crit");
}
static void callMacroStruct()
{
    LogMsg(Logger::Struct, "struct ", 1);
}
static void callMacroVectorPayload(const std::vector<int>& v)
{
    LogMsg(Logger::Warning, "vec ", v);
}

// Extract the line number from a log line.
static int extractLine(const std::string& s, const char* funcName)
{
    std::regex re(
        std::string(R"(\[[^[\]]+\] \[[^:]+:(\d+)\] )") + funcName + R"(\(\):)");
    std::smatch m;
    if (std::regex_search(s, m, re))
        return std::stoi(m[1].str());
    return -1;
}

static int emitWarningOnKnownLine(std::ostringstream& oss)
{
    auto* old = std::cout.rdbuf(oss.rdbuf());
    const int expected = __LINE__ + 1;
    LogMsg(Logger::Warning, "check");
    std::cout.rdbuf(old);
    return expected;
}

TEST(LogMacroMoreTest, EmitsInfoLevelViaMacro)
{
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());
    callMacroInfo();
    std::cout.rdbuf(old);

    const std::string s = oss.str();
    ASSERT_FALSE(s.empty()) << "Info should be emitted at LOG_LEVEL Info";
    EXPECT_NE(s.find("[Info    ]"), std::string::npos) << s;
    EXPECT_NE(s.find("callMacroInfo(): info payload"), std::string::npos) << s;
    EXPECT_EQ('\n', s.back());
}

TEST(LogMacroMoreTest, ExactLineNumberIsPrinted)
{
    std::ostringstream oss;
    const int expected = emitWarningOnKnownLine(oss);
    const std::string s = oss.str();

    const int got = extractLine(s, "emitWarningOnKnownLine");
    ASSERT_NE(got, -1) << s;
    EXPECT_EQ(got, expected) << s;
}

// --- LogMacroMoreTest: padded level names ---
TEST(LogMacroMoreTest, FormatsLevelNamesExactlyForErrorAndCritical)
{
    {
        std::ostringstream oss;
        auto* old = std::cout.rdbuf(oss.rdbuf());
        callMacroError();
        std::cout.rdbuf(old);
        const std::string s = oss.str();

        std::regex re_err(
            R"(\[Error\s+\]\s+\[.+?:\d+\]\s+callMacroError\(\): err 123\n?)");
        EXPECT_TRUE(std::regex_search(s, re_err)) << s;
        ASSERT_FALSE(s.empty());
        EXPECT_EQ('\n', s.back());
    }
    {
        std::ostringstream oss;
        auto* old = std::cout.rdbuf(oss.rdbuf());
        callMacroCritical();
        std::cout.rdbuf(old);
        const std::string s = oss.str();

        std::regex re_crit(
            R"(\[Critical\s*\]\s+\[.+?:\d+\]\s+callMacroCritical\(\): crit\n?)");
        EXPECT_TRUE(std::regex_search(s, re_crit)) << s;
        ASSERT_FALSE(s.empty());
        EXPECT_EQ('\n', s.back());
    }
}

TEST(LogMacroMoreTest, SuppressesStructAtInfoLevel)
{
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());
    callMacroStruct();
    std::cout.rdbuf(old);
    EXPECT_TRUE(oss.str().empty());
}

TEST(LogMacroMoreTest, AcceptsVectorPayloadAndUsesOverloadedStreamOp)
{
    std::vector<int> v{10, 20, 30};
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());
    callMacroVectorPayload(v);
    std::cout.rdbuf(old);

    const std::string s = oss.str();
    // Expect the payload "vec 10 20 30 " (vector prints trailing space)
    // and the function name "callMacroVectorPayload()"
    EXPECT_NE(s.find("callMacroVectorPayload(): vec 10 20 30 "),
              std::string::npos)
        << s;
    ASSERT_FALSE(s.empty());
    EXPECT_EQ('\n', s.back());
}
