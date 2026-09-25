#include <studyapp/core/Log.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace studyapp::core {
namespace {

struct Captured {
    LogLevel level;
    std::string category;
    std::string message;
};

class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        setLogSink([this](LogLevel level, std::string_view category, std::string_view message) {
            captured_.push_back({level, std::string(category), std::string(message)});
        });
    }
    void TearDown() override {
        setLogSink({});
        setMinimumLogLevel(LogLevel::Debug);
    }

    std::vector<Captured> captured_;
};

TEST_F(LogTest, ForwardsToInstalledSink) {
    logInfo("test", "hello");
    logError("db", "failed");
    ASSERT_EQ(captured_.size(), 2U);
    EXPECT_EQ(captured_[0].level, LogLevel::Info);
    EXPECT_EQ(captured_[0].category, "test");
    EXPECT_EQ(captured_[0].message, "hello");
    EXPECT_EQ(captured_[1].level, LogLevel::Error);
}

TEST_F(LogTest, FiltersBelowMinimumLevel) {
    setMinimumLogLevel(LogLevel::Warning);
    logDebug("test", "dropped");
    logInfo("test", "dropped");
    logWarning("test", "kept");
    ASSERT_EQ(captured_.size(), 1U);
    EXPECT_EQ(captured_[0].message, "kept");
    EXPECT_EQ(minimumLogLevel(), LogLevel::Warning);
}

TEST_F(LogTest, SinkMayLogWithoutDeadlock) {
    int depth = 0;
    setLogSink([&](LogLevel, std::string_view, std::string_view message) {
        if (depth++ == 0) {
            log(LogLevel::Info, "nested", message);
        }
    });
    logInfo("test", "outer");
    EXPECT_EQ(depth, 2);
}

TEST(LogLevelTest, HasReadableNames) {
    EXPECT_EQ(toString(LogLevel::Debug), "debug");
    EXPECT_EQ(toString(LogLevel::Warning), "warning");
}

} // namespace
} // namespace studyapp::core
