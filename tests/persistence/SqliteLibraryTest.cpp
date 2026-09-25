#include <studyapp/persistence/SqliteLibrary.hpp>

#include <gtest/gtest.h>

namespace studyapp::persistence {
namespace {

// Verifies the linked SQLite (vendored or system, see STUDYAPP_USE_SYSTEM_SQLITE) meets the
// requirements documented in docs/DATABASE_SCHEMA.md §2.

TEST(SqliteLibraryTest, ReportsVersion) {
    const SqliteLibraryInfo info = sqliteLibraryInfo();
    EXPECT_FALSE(info.version.empty());
    EXPECT_EQ(info.version.front(), '3');
    EXPECT_GE(info.versionNumber, 3000000);
}

TEST(SqliteLibraryTest, MeetsMinimumVersion) {
    EXPECT_GE(sqliteLibraryInfo().versionNumber, kMinimumSqliteVersionNumber);
}

TEST(SqliteLibraryTest, HasRequiredFeatures) {
    const SqliteLibraryInfo info = sqliteLibraryInfo();
    EXPECT_TRUE(info.hasFts5) << "SQLite must be built with FTS5";
    EXPECT_TRUE(info.threadSafe) << "SQLite must be built thread-safe";
}

} // namespace
} // namespace studyapp::persistence
