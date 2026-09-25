#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Id.hpp>
#include <studyapp/core/Uuid.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// SQLite handles are forward-declared so that <sqlite3.h> stays private to this module.
struct sqlite3;
struct sqlite3_stmt;

namespace studyapp::persistence {

// Thin RAII wrapper over the SQLite C API (docs/ARCHITECTURE.md decision D2).
//
//   Database     one connection; owns a prepared-statement cache
//   Statement    one prepared statement; bind/step/column access
//   Transaction  (Transaction.hpp) BEGIN ... COMMIT, ROLLBACK on destruction
//
// Every SQLite failure is reported as a core::Error that names the operation and carries
// SQLite's message. Nothing here throws.

enum class OpenMode {
    ReadOnly,  ///< SQLITE_OPEN_READONLY; the file must exist
    ReadWrite, ///< the file must exist
    Create,    ///< read-write, creating the file if it does not exist
};

/// Storage class of a column value (sqlite3_column_type).
enum class ColumnType {
    Integer,
    Real,
    Text,
    Blob,
    Null
};

class Database;

/// A prepared statement. Parameter indexes are 1-based (as in SQLite), column indexes
/// 0-based. Bind calls record the first failure, which the next step()/run() reports, so
/// binds can be chained.
class Statement {
public:
    Statement() = default;
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bindNull(int index);
    Statement& bindInt(int index, std::int64_t value);
    Statement& bindReal(int index, double value);
    Statement& bindText(int index, std::string_view value);
    Statement& bindBlob(int index, std::span<const std::uint8_t> value);
    /// UUIDs are stored as 16-byte BLOBs (docs/DATABASE_SCHEMA.md §4).
    Statement& bindUuid(int index, const core::Uuid& value);
    template <class Tag>
    Statement& bindId(int index, const core::Id<Tag>& id) {
        return bindUuid(index, id.value());
    }
    template <class Tag>
    Statement& bindId(int index, const std::optional<core::Id<Tag>>& id) {
        return id ? bindUuid(index, id->value()) : bindNull(index);
    }

    /// Advances the statement: true if a row is available, false when done.
    [[nodiscard]] core::Result<bool> step();
    /// Steps a statement that returns no rows to completion, then resets it.
    [[nodiscard]] core::Result<void> run();
    /// Rewinds the statement and clears its bindings.
    void reset() noexcept;

    [[nodiscard]] int columnCount() const noexcept;
    [[nodiscard]] ColumnType columnType(int column) const noexcept;
    [[nodiscard]] bool columnIsNull(int column) const noexcept {
        return columnType(column) == ColumnType::Null;
    }
    [[nodiscard]] std::int64_t columnInt(int column) const noexcept;
    [[nodiscard]] double columnReal(int column) const noexcept;
    /// UTF-8 text; valid until the next step/reset.
    [[nodiscard]] std::string_view columnText(int column) const noexcept;
    /// Blob bytes; valid until the next step/reset.
    [[nodiscard]] std::span<const std::uint8_t> columnBlob(int column) const noexcept;

    /// Name of a result column (as written in the SELECT, or the column name).
    [[nodiscard]] std::string_view columnName(int column) const noexcept;
    [[nodiscard]] std::string_view sql() const noexcept;

private:
    friend class Database;
    Statement(sqlite3_stmt* statement, sqlite3* connection) noexcept
        : statement_(statement), connection_(connection) {}
    void recordBindError(int index, int code);

    sqlite3_stmt* statement_ = nullptr;
    sqlite3* connection_ = nullptr;
    std::optional<core::Error> bindError_;
};

/// Resets a cached statement when the scope ends, so it never stays active (holding a
/// read snapshot) between uses. Obtained from Database::cached().
class CachedStatement {
public:
    explicit CachedStatement(Statement& statement) noexcept : statement_(&statement) {}
    ~CachedStatement() {
        if (statement_ != nullptr) {
            statement_->reset();
        }
    }
    CachedStatement(const CachedStatement&) = delete;
    CachedStatement& operator=(const CachedStatement&) = delete;
    CachedStatement(CachedStatement&& other) noexcept : statement_(other.statement_) {
        other.statement_ = nullptr;
    }
    CachedStatement& operator=(CachedStatement&&) = delete;

    Statement& operator*() const noexcept { return *statement_; }
    Statement* operator->() const noexcept { return statement_; }

private:
    Statement* statement_;
};

/// One SQLite connection. Move-only; closing finalises all cached statements.
///
/// Every connection is configured as documented in docs/DATABASE_SCHEMA.md §2
/// (foreign_keys, synchronous=NORMAL, busy_timeout, temp_store, cache_size).
class Database {
public:
    [[nodiscard]] static core::Result<Database> open(const std::filesystem::path& file,
                                                     OpenMode mode);
    /// Private in-memory database (tests, scratch work).
    [[nodiscard]] static core::Result<Database> openInMemory();

    Database() = default;
    ~Database();
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return connection_ != nullptr; }
    [[nodiscard]] bool isReadOnly() const noexcept { return readOnly_; }
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

    /// Executes one or more SQL statements that return no rows needed by the caller.
    [[nodiscard]] core::Result<void> execute(std::string_view sql);

    /// Prepares a statement owned by the caller.
    [[nodiscard]] core::Result<Statement> prepare(std::string_view sql);

    /// Prepared once per connection and reused; reset when the returned guard is destroyed.
    [[nodiscard]] core::Result<CachedStatement> cached(std::string_view sql);

    /// Runs a query expected to return exactly one integer (e.g. `PRAGMA user_version`).
    [[nodiscard]] core::Result<std::int64_t> queryInt(std::string_view sql);
    /// Runs a query expected to return exactly one text value.
    [[nodiscard]] core::Result<std::string> queryText(std::string_view sql);

    /// Rows changed by the most recent INSERT/UPDATE/DELETE on this connection.
    [[nodiscard]] std::int64_t changes() const noexcept;
    /// True while a transaction is open (not in autocommit mode).
    [[nodiscard]] bool inTransaction() const noexcept;

    /// Finalises cached statements and closes the connection. Fails (and keeps the
    /// connection open) if statements owned by callers are still alive.
    [[nodiscard]] core::Result<void> close();

private:
    Database(sqlite3* connection, std::filesystem::path file, bool readOnly) noexcept
        : connection_(connection), file_(std::move(file)), readOnly_(readOnly) {}
    [[nodiscard]] core::Result<void> configureConnection();

    sqlite3* connection_ = nullptr;
    std::filesystem::path file_;
    bool readOnly_ = false;
    std::unordered_map<std::string, std::unique_ptr<Statement>> cache_;
};

} // namespace studyapp::persistence
