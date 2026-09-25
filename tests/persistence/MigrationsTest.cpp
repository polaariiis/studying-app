#include <studyapp/persistence/Migrations.hpp>

#include <studyapp/persistence/Database.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace studyapp::persistence {
namespace {

core::Timestamp at(std::int64_t millis) {
    return core::Timestamp(std::chrono::milliseconds(millis));
}

std::set<std::string> tableNames(Database& database) {
    std::set<std::string> names;
    auto statement = database.prepare(
        "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
        "AND name NOT LIKE 'search_index_%'");
    EXPECT_TRUE(statement.has_value());
    while (statement && statement->step().value_or(false)) {
        names.emplace(statement->columnText(0));
    }
    return names;
}

struct MigrationsTest : ::testing::Test {
    testing::TempDirectory dir;

    Database create(const std::string& name = "workspace.db") {
        auto database = Database::open(dir / name, OpenMode::Create);
        EXPECT_TRUE(database.has_value()) << (database ? "" : database.error().message);
        return std::move(*database);
    }
};

TEST_F(MigrationsTest, BuiltinMigrationsAreNumberedFromOne) {
    const auto migrations = builtinMigrations();
    ASSERT_FALSE(migrations.empty());
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        EXPECT_EQ(migrations[i].version, static_cast<int>(i) + 1) << migrations[i].name;
        EXPECT_FALSE(migrations[i].sql.empty());
    }
    EXPECT_EQ(migrations.front().name, "0001_initial.sql");
    EXPECT_EQ(currentSchemaVersion(), migrations.back().version);
}

TEST_F(MigrationsTest, InitialisesNewDatabaseWithDocumentedSchemaAndSettings) {
    Database database = create();
    auto result = migrate(database, builtinMigrations());
    ASSERT_OK(result);
    EXPECT_EQ(result->fromVersion, 0);
    EXPECT_EQ(result->toVersion, currentSchemaVersion());
    EXPECT_FALSE(result->backup.has_value()); // nothing to back up in a new database

    EXPECT_EQ(schemaVersion(database).value_or(-1), 1);
    EXPECT_EQ(database.queryInt("PRAGMA application_id").value_or(0), kApplicationId);
    EXPECT_EQ(database.queryInt("PRAGMA page_size").value_or(0), 4096);
    EXPECT_EQ(database.queryText("PRAGMA journal_mode").value_or(""), "wal");

    // Every table of docs/DATABASE_SCHEMA.md §5.
    const std::set<std::string> expected{
        "workspace_meta", "setting",   "asset",      "course",      "notebook", "section",
        "page",           "layer",     "element",    "stroke",      "text_box", "shape",
        "image",          "connector", "tag",        "page_tag",    "project",  "task",
        "task_tag",       "task_page", "search_doc", "search_index"};
    EXPECT_EQ(tableNames(database), expected);

    EXPECT_EQ(database.queryText("PRAGMA integrity_check").value_or(""), "ok");
    auto violations = database.prepare("PRAGMA foreign_key_check");
    ASSERT_OK(violations);
    EXPECT_FALSE(violations->step().value_or(true));
}

TEST_F(MigrationsTest, WalModePersistsAcrossConnections) {
    {
        Database database = create();
        ASSERT_OK(migrate(database, builtinMigrations()));
    }
    auto reopened = Database::open(dir / "workspace.db", OpenMode::ReadWrite);
    ASSERT_OK(reopened);
    EXPECT_EQ(reopened->queryText("PRAGMA journal_mode").value_or(""), "wal");
}

TEST_F(MigrationsTest, MigratingAnUpToDateDatabaseIsANoOp) {
    Database database = create();
    ASSERT_OK(migrate(database, builtinMigrations()));
    auto again = migrate(database, builtinMigrations());
    ASSERT_OK(again);
    EXPECT_EQ(again->fromVersion, again->toVersion);
    EXPECT_FALSE(again->backup.has_value());
}

TEST_F(MigrationsTest, UpgradesWithBackupAndKeepsData) {
    // Harness for future migrations: a synthetic v2 on top of the real v1.
    std::vector<Migration> migrations(builtinMigrations().begin(), builtinMigrations().end());
    {
        Database database = create();
        ASSERT_OK(migrate(database, migrations));
        ASSERT_OK(
            database.execute("INSERT INTO workspace_meta (key, value) VALUES ('name', 'Keep me')"));
    }
    migrations.push_back(Migration{.version = 2,
                                   .name = "0002_test.sql",
                                   .sql = "ALTER TABLE notebook ADD COLUMN test_column TEXT;"});

    auto database = Database::open(dir / "workspace.db", OpenMode::ReadWrite);
    ASSERT_OK(database);
    const auto backups = dir / "backups";
    std::filesystem::create_directories(backups);
    auto result =
        migrate(*database, migrations, MigrationOptions{.backupDirectory = backups, .now = at(0)});
    ASSERT_OK(result);
    EXPECT_EQ(result->fromVersion, 1);
    EXPECT_EQ(result->toVersion, 2);
    EXPECT_EQ(schemaVersion(*database).value_or(-1), 2);
    EXPECT_EQ(
        database->queryText("SELECT value FROM workspace_meta WHERE key = 'name'").value_or(""),
        "Keep me");
    ASSERT_OK(database->execute("SELECT test_column FROM notebook"));

    // The backup is a complete v1 database named per docs/DATABASE_SCHEMA.md §8.
    ASSERT_TRUE(result->backup.has_value());
    EXPECT_EQ(result->backup->filename(), "pre-migration-v1-19700101T000000000Z.db");
    auto backup = Database::open(*result->backup, OpenMode::ReadOnly);
    ASSERT_OK(backup);
    EXPECT_EQ(schemaVersion(*backup).value_or(-1), 1);
    EXPECT_EQ(backup->queryText("SELECT value FROM workspace_meta WHERE key = 'name'").value_or(""),
              "Keep me");
}

TEST_F(MigrationsTest, UpgradeRequiresABackupDirectory) {
    std::vector<Migration> migrations(builtinMigrations().begin(), builtinMigrations().end());
    Database database = create();
    ASSERT_OK(migrate(database, migrations));
    migrations.push_back(Migration{.version = 2, .name = "0002_test.sql", .sql = "SELECT 1;"});
    EXPECT_FALSE(migrate(database, migrations).has_value());
    EXPECT_EQ(schemaVersion(database).value_or(-1), 1);
}

TEST_F(MigrationsTest, FailedMigrationRollsBackCompletely) {
    Database database = create();
    const std::vector<Migration> migrations{
        Migration{.version = 1,
                  .name = "0001_broken.sql",
                  .sql = "CREATE TABLE a (x INTEGER); CREATE TABLE a (y INTEGER);"}};
    const auto result = migrate(database, migrations);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("0001_broken.sql"), std::string::npos);
    EXPECT_EQ(schemaVersion(database).value_or(-1), 0);
    EXPECT_TRUE(tableNames(database).empty());
}

TEST_F(MigrationsTest, RefusesNewerSchemaForWritingButAllowsReadOnly) {
    {
        Database database = create();
        ASSERT_OK(migrate(database, builtinMigrations()));
        ASSERT_OK(database.execute("PRAGMA user_version = 99"));
    }
    auto writable = Database::open(dir / "workspace.db", OpenMode::ReadWrite);
    ASSERT_OK(writable);
    const auto refused = migrate(*writable, builtinMigrations());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, core::ErrorCode::Unsupported);
    EXPECT_NE(refused.error().message.find("newer version"), std::string::npos);

    auto readOnly = Database::open(dir / "workspace.db", OpenMode::ReadOnly);
    ASSERT_OK(readOnly);
    auto allowed = migrate(*readOnly, builtinMigrations());
    ASSERT_OK(allowed);
    EXPECT_EQ(allowed->toVersion, 99);
}

TEST_F(MigrationsTest, ReadOnlyConnectionIsNeverUpgraded) {
    {
        Database database = create();
    } // empty file, version 0
    auto readOnly = Database::open(dir / "workspace.db", OpenMode::ReadOnly);
    ASSERT_OK(readOnly);
    const auto refused = migrate(*readOnly, builtinMigrations());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, core::ErrorCode::Unsupported);
}

TEST_F(MigrationsTest, RefusesForeignDatabases) {
    Database other = create("other.db");
    ASSERT_OK(other.execute("CREATE TABLE unrelated (x INTEGER)"));
    const auto unversioned = migrate(other, builtinMigrations());
    ASSERT_FALSE(unversioned.has_value());
    EXPECT_EQ(unversioned.error().code, core::ErrorCode::Unsupported);

    Database foreign = create("foreign.db");
    ASSERT_OK(foreign.execute("PRAGMA application_id = 1234; PRAGMA user_version = 1"));
    const auto wrongId = migrate(foreign, builtinMigrations());
    ASSERT_FALSE(wrongId.has_value());
    EXPECT_EQ(wrongId.error().code, core::ErrorCode::Unsupported);
}

TEST_F(MigrationsTest, RejectsMigrationsOutOfSequence) {
    Database database = create();
    const std::vector<Migration> gap{
        Migration{.version = 1, .name = "0001.sql", .sql = "SELECT 1;"},
        Migration{.version = 3, .name = "0003.sql", .sql = "SELECT 1;"}};
    EXPECT_FALSE(migrate(database, gap).has_value());
    EXPECT_EQ(schemaVersion(database).value_or(-1), 0);
}

TEST(FileTimestampTest, FormatsUtcWithoutChronoFormatting) {
    EXPECT_EQ(fileTimestamp(at(0)), "19700101T000000000Z");
    EXPECT_EQ(fileTimestamp(at(1'790'000'000'123)), "20260921T141320123Z");
    EXPECT_EQ(fileTimestamp(at(951'782'400'000)), "20000229T000000000Z"); // leap day
    EXPECT_EQ(fileTimestamp(at(-1)), "19691231T235959999Z");
}

} // namespace
} // namespace studyapp::persistence
