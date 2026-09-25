#include <studyapp/application/ComponentVersions.hpp>

#include <studyapp/core/BuildInfo.hpp>

#include <gtest/gtest.h>

#include <algorithm>

namespace studyapp::application {
namespace {

TEST(ComponentVersionsTest, ListsProductFirst) {
    const auto components = componentVersions();
    ASSERT_FALSE(components.empty());
    EXPECT_EQ(components.front().name, core::build::kProductName);
    EXPECT_EQ(components.front().version, core::build::kVersion);
}

TEST(ComponentVersionsTest, IncludesSqlite) {
    const auto components = componentVersions();
    const auto sqlite = std::find_if(components.begin(), components.end(),
                                     [](const ComponentVersion& c) { return c.name == "SQLite"; });
    ASSERT_NE(sqlite, components.end());
    EXPECT_FALSE(sqlite->version.empty());
}

} // namespace
} // namespace studyapp::application
