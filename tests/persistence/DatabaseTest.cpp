#include <studyapp/persistence/Database.hpp>

#include <studyapp/persistence/Transaction.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace studyapp::persistence {
namespace {

Database openMemory() {
    auto database = Database::openInMemory();
    EXPECT_TRUE(database.has_value()) << (database ? "" : database.error().message);
    return std::move(*database);
}

TEST(DatabaseTest, ConfiguresEveryConnectionAsDocumented) {
    testing::TempDirectory dir;
    auto database = Database::open(dir / "test.db", OpenMode::Create);
    ASSERT_OK(database);
    EXPECT_EQ(database->queryInt("PRAGMA foreign_keys").value_or(-1), 1);
    EXPECT_EQ(database->queryInt("PRAGMA synchronous").value_or(-1), 1); // NORMAL
    EXPECT_EQ(database->queryInt("PRAGMA busy_timeout").value_or(-1), 5000);
    EXPECT_EQ(database->queryInt("PRAGMA temp_store").value_or(-1), 2); // MEMORY
    EXPECT_EQ(database->queryInt("PRAGMA cache_size").value_or(0), -32768);
    EXPECT_FALSE(database->isReadOnly());
    EXPECT_OK(database->close());
}

TEST(DatabaseTest, OpenMissingFileFailsUnlessCreating) {
    testing::TempDirectory dir;
    const auto missing = Database::open(dir / "missing.db", OpenMode::ReadWrite);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, core::ErrorCode::IoError);
    EXPECT_FALSE(Database::open(dir / "missing.db", OpenMode::ReadOnly).has_value());
}

TEST(DatabaseTest, ReadOnlyConnectionRejectsWrites) {
    testing::TempDirectory dir;
    {
        auto database = Database::open(dir / "test.db", OpenMode::Create);
        ASSERT_OK(database);
        ASSERT_OK(database->execute("CREATE TABLE t (x INTEGER)"));
    }
    auto readOnly = Database::open(dir / "test.db", OpenMode::ReadOnly);
    ASSERT_OK(readOnly);
    EXPECT_TRUE(readOnly->isReadOnly());
    const auto written = readOnly->execute("INSERT INTO t VALUES (1)");
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code, core::ErrorCode::IoError);
}

TEST(DatabaseTest, BindsAndReadsEveryStorageClass) {
    Database database = openMemory();
    ASSERT_OK(database.execute("CREATE TABLE t (i INTEGER, r REAL, s TEXT, b BLOB, n)"));
    auto insert = database.prepare("INSERT INTO t VALUES (?1, ?2, ?3, ?4, ?5)");
    ASSERT_OK(insert);
    const std::array<std::uint8_t, 4> bytes{0x00, 0xFF, 0x10, 0x80};
    insert->bindInt(1, -9'007'199'254'740'993LL)
        .bindReal(2, 0.1)
        .bindText(3, "caf\xC3\xA9 \xE2\x9C\x93") // UTF-8 survives unchanged
        .bindBlob(4, bytes)
        .bindNull(5);
    ASSERT_OK(insert->run());

    auto select = database.prepare("SELECT i, r, s, b, n FROM t");
    ASSERT_OK(select);
    auto row = select->step();
    ASSERT_OK(row);
    ASSERT_TRUE(*row);
    EXPECT_EQ(select->columnType(0), ColumnType::Integer);
    EXPECT_EQ(select->columnInt(0), -9'007'199'254'740'993LL);
    EXPECT_EQ(select->columnType(1), ColumnType::Real);
    EXPECT_EQ(select->columnReal(1), 0.1);
    EXPECT_EQ(select->columnText(2), "caf\xC3\xA9 \xE2\x9C\x93");
    ASSERT_EQ(select->columnBlob(3).size(), bytes.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), select->columnBlob(3).begin()));
    EXPECT_TRUE(select->columnIsNull(4));
    EXPECT_EQ(select->columnName(2), "s");
    auto done = select->step();
    ASSERT_OK(done);
    EXPECT_FALSE(*done);
}

TEST(DatabaseTest, EmptyBlobIsNotNull) {
    Database database = openMemory();
    auto statement = database.prepare("SELECT typeof(?1)");
    ASSERT_OK(statement);
    statement->bindBlob(1, {});
    ASSERT_OK(statement->step());
    EXPECT_EQ(statement->columnText(0), "blob");
}

TEST(DatabaseTest, UuidsAreSixteenByteBlobs) {
    Database database = openMemory();
    const auto uuid = core::Uuid::parse("01890a5d-ac96-774b-bcce-b302099a8057");
    ASSERT_OK(uuid);
    auto statement = database.prepare("SELECT typeof(?1), length(?1)");
    ASSERT_OK(statement);
    statement->bindUuid(1, *uuid);
    ASSERT_OK(statement->step());
    EXPECT_EQ(statement->columnText(0), "blob");
    EXPECT_EQ(statement->columnInt(1), 16);
}

TEST(DatabaseTest, ReportsSqlErrorsWithContext) {
    Database database = openMemory();
    const auto prepared = database.prepare("SELECT * FROM no_such_table");
    ASSERT_FALSE(prepared.has_value());
    EXPECT_NE(prepared.error().message.find("no_such_table"), std::string::npos);

    ASSERT_OK(database.execute("CREATE TABLE t (x INTEGER PRIMARY KEY)"));
    ASSERT_OK(database.execute("INSERT INTO t VALUES (1)"));
    const auto duplicate = database.execute("INSERT INTO t VALUES (1)");
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().code, core::ErrorCode::Conflict); // constraint violation
}

TEST(DatabaseTest, BindErrorsAreReportedByStep) {
    Database database = openMemory();
    auto statement = database.prepare("SELECT ?1");
    ASSERT_OK(statement);
    statement->bindInt(7, 1); // out of range
    const auto stepped = statement->step();
    ASSERT_FALSE(stepped.has_value());
    EXPECT_NE(stepped.error().message.find("parameter 7"), std::string::npos);
}

TEST(DatabaseTest, CachedStatementsAreReusedAndReset) {
    Database database = openMemory();
    ASSERT_OK(database.execute("CREATE TABLE t (x INTEGER); INSERT INTO t VALUES (1), (2), (3)"));
    for (int i = 0; i < 3; ++i) {
        auto statement = database.cached("SELECT x FROM t ORDER BY x");
        ASSERT_OK(statement);
        // Only the first row is read; the guard resets the statement for the next use.
        ASSERT_OK((*statement)->step());
        EXPECT_EQ((*statement)->columnInt(0), 1);
    }
    // No statement is left active: a write transaction can begin and commit.
    auto transaction = Transaction::begin(database);
    ASSERT_OK(transaction);
    ASSERT_OK(database.execute("INSERT INTO t VALUES (4)"));
    EXPECT_OK(transaction->commit());
}

TEST(DatabaseTest, CloseFailsWhileCallerStatementsAreAlive) {
    Database database = openMemory();
    {
        auto statement = database.prepare("SELECT 1");
        ASSERT_OK(statement);
        EXPECT_FALSE(database.close().has_value());
    }
    EXPECT_OK(database.close());
    EXPECT_FALSE(database.isOpen());
}

// ---------------------------------------------------------------------------- transactions

TEST(TransactionTest, CommitMakesChangesDurable) {
    Database database = openMemory();
    ASSERT_OK(database.execute("CREATE TABLE t (x INTEGER)"));
    {
        auto transaction = Transaction::begin(database);
        ASSERT_OK(transaction);
        EXPECT_TRUE(database.inTransaction());
        ASSERT_OK(database.execute("INSERT INTO t VALUES (1)"));
        ASSERT_OK(transaction->commit());
    }
    EXPECT_FALSE(database.inTransaction());
    EXPECT_EQ(database.queryInt("SELECT count(*) FROM t").value_or(-1), 1);
}

TEST(TransactionTest, DestructionWithoutCommitRollsBack) {
    Database database = openMemory();
    ASSERT_OK(database.execute("CREATE TABLE t (x INTEGER)"));
    {
        auto transaction = Transaction::begin(database, Transaction::Kind::Deferred);
        ASSERT_OK(transaction);
        ASSERT_OK(database.execute("INSERT INTO t VALUES (1)"));
    }
    EXPECT_FALSE(database.inTransaction());
    EXPECT_EQ(database.queryInt("SELECT count(*) FROM t").value_or(-1), 0);
}

TEST(TransactionTest, FailedCommitRollsBack) {
    Database database = openMemory();
    ASSERT_OK(database.execute("CREATE TABLE parent (id INTEGER PRIMARY KEY);"
                               "CREATE TABLE child (p INTEGER REFERENCES parent(id))"));
    auto transaction = Transaction::begin(database);
    ASSERT_OK(transaction);
    ASSERT_OK(database.execute("PRAGMA defer_foreign_keys = ON"));
    ASSERT_OK(database.execute("INSERT INTO child VALUES (42)")); // dangling, checked at commit
    const auto committed = transaction->commit();
    ASSERT_FALSE(committed.has_value());
    EXPECT_EQ(committed.error().code, core::ErrorCode::Conflict);
    EXPECT_FALSE(transaction->isActive());
    EXPECT_FALSE(database.inTransaction());
    EXPECT_EQ(database.queryInt("SELECT count(*) FROM child").value_or(-1), 0);
}

TEST(TransactionTest, DoesNotNest) {
    Database database = openMemory();
    auto outer = Transaction::begin(database);
    ASSERT_OK(outer);
    EXPECT_FALSE(Transaction::begin(database).has_value());
}

} // namespace
} // namespace studyapp::persistence
