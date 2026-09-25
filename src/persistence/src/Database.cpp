#include <studyapp/persistence/Database.hpp>

#include "SqliteError.hpp"
#include "Utf8Path.hpp"

#include <sqlite3.h>

#include <cassert>
#include <cstring>
#include <limits>
#include <utility>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;
using detail::sqliteError;

// ---------------------------------------------------------------------------- Statement

Statement::~Statement() {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

Statement::Statement(Statement&& other) noexcept
    : statement_(std::exchange(other.statement_, nullptr)),
      connection_(std::exchange(other.connection_, nullptr)),
      bindError_(std::move(other.bindError_)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
        statement_ = std::exchange(other.statement_, nullptr);
        connection_ = std::exchange(other.connection_, nullptr);
        bindError_ = std::move(other.bindError_);
    }
    return *this;
}

void Statement::recordBindError(int index, int code) {
    if (code != SQLITE_OK && !bindError_) {
        bindError_ = sqliteError(connection_, code,
                                 "binding parameter " + std::to_string(index) + " of '" +
                                     std::string(sql()) + "'");
    }
}

Statement& Statement::bindNull(int index) {
    recordBindError(index, sqlite3_bind_null(statement_, index));
    return *this;
}

Statement& Statement::bindInt(int index, std::int64_t value) {
    recordBindError(index, sqlite3_bind_int64(statement_, index, value));
    return *this;
}

Statement& Statement::bindReal(int index, double value) {
    recordBindError(index, sqlite3_bind_double(statement_, index, value));
    return *this;
}

namespace {

/// Copy of `size` bytes owned by SQLite (released with sqlite3_free, also when binding
/// fails). Equivalent to SQLITE_TRANSIENT, whose definition is an old-style cast that
/// -Wold-style-cast rejects. Never null, so an empty value still binds as TEXT/BLOB.
void* sqliteCopy(const void* data, std::size_t size) noexcept {
    void* copy = sqlite3_malloc64(static_cast<sqlite3_uint64>(size == 0 ? 1 : size));
    if (copy != nullptr && size > 0) {
        std::memcpy(copy, data, size);
    }
    return copy;
}

} // namespace

Statement& Statement::bindText(int index, std::string_view value) {
    void* copy = sqliteCopy(value.data(), value.size());
    if (copy == nullptr) {
        recordBindError(index, SQLITE_NOMEM);
        return *this;
    }
    recordBindError(index, sqlite3_bind_text64(statement_, index, static_cast<const char*>(copy),
                                               static_cast<sqlite3_uint64>(value.size()),
                                               sqlite3_free, SQLITE_UTF8));
    return *this;
}

Statement& Statement::bindBlob(int index, std::span<const std::uint8_t> value) {
    void* copy = sqliteCopy(value.data(), value.size());
    if (copy == nullptr) {
        recordBindError(index, SQLITE_NOMEM);
        return *this;
    }
    recordBindError(index,
                    sqlite3_bind_blob64(statement_, index, copy,
                                        static_cast<sqlite3_uint64>(value.size()), sqlite3_free));
    return *this;
}

Statement& Statement::bindUuid(int index, const core::Uuid& value) {
    return bindBlob(index, value.bytes());
}

Result<bool> Statement::step() {
    if (statement_ == nullptr) {
        return makeError(ErrorCode::Internal, "step on an empty statement");
    }
    if (bindError_) {
        auto error = std::move(*bindError_);
        bindError_.reset();
        return tl::unexpected<core::Error>(std::move(error));
    }
    const int code = sqlite3_step(statement_);
    if (code == SQLITE_ROW) {
        return true;
    }
    if (code == SQLITE_DONE) {
        return false;
    }
    auto error = sqliteError(connection_, code, "executing '" + std::string(sql()) + "'");
    sqlite3_reset(statement_);
    return tl::unexpected<core::Error>(std::move(error));
}

Result<void> Statement::run() {
    auto stepped = step();
    reset();
    if (!stepped) {
        return tl::unexpected<core::Error>(std::move(stepped.error()));
    }
    return {};
}

void Statement::reset() noexcept {
    if (statement_ != nullptr) {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }
    bindError_.reset();
}

int Statement::columnCount() const noexcept {
    return sqlite3_column_count(statement_);
}

ColumnType Statement::columnType(int column) const noexcept {
    switch (sqlite3_column_type(statement_, column)) {
    case SQLITE_INTEGER:
        return ColumnType::Integer;
    case SQLITE_FLOAT:
        return ColumnType::Real;
    case SQLITE_TEXT:
        return ColumnType::Text;
    case SQLITE_BLOB:
        return ColumnType::Blob;
    default:
        return ColumnType::Null;
    }
}

std::int64_t Statement::columnInt(int column) const noexcept {
    return sqlite3_column_int64(statement_, column);
}

double Statement::columnReal(int column) const noexcept {
    return sqlite3_column_double(statement_, column);
}

std::string_view Statement::columnText(int column) const noexcept {
    const auto* text = sqlite3_column_text(statement_, column);
    const int size = sqlite3_column_bytes(statement_, column);
    if (text == nullptr || size <= 0) {
        return {};
    }
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(size)};
}

std::span<const std::uint8_t> Statement::columnBlob(int column) const noexcept {
    const void* data = sqlite3_column_blob(statement_, column);
    const int size = sqlite3_column_bytes(statement_, column);
    if (data == nullptr || size <= 0) {
        return {};
    }
    return {static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size)};
}

std::string_view Statement::columnName(int column) const noexcept {
    const char* name = sqlite3_column_name(statement_, column);
    return name != nullptr ? std::string_view(name) : std::string_view();
}

std::string_view Statement::sql() const noexcept {
    const char* text = statement_ != nullptr ? sqlite3_sql(statement_) : nullptr;
    return text != nullptr ? std::string_view(text) : std::string_view();
}

// ---------------------------------------------------------------------------- Database

Result<Database> Database::open(const std::filesystem::path& file, OpenMode mode) {
    int flags = SQLITE_OPEN_EXRESCODE;
    switch (mode) {
    case OpenMode::ReadOnly:
        flags |= SQLITE_OPEN_READONLY;
        break;
    case OpenMode::ReadWrite:
        flags |= SQLITE_OPEN_READWRITE;
        break;
    case OpenMode::Create:
        flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        break;
    }
    sqlite3* connection = nullptr;
    const std::string name = detail::utf8(file);
    const int code = sqlite3_open_v2(name.c_str(), &connection, flags, nullptr);
    if (code != SQLITE_OK) {
        auto error = sqliteError(connection, code, "opening database '" + name + "'");
        sqlite3_close(connection); // safe with nullptr
        return tl::unexpected<core::Error>(std::move(error));
    }
    Database database(connection, file, mode == OpenMode::ReadOnly);
    if (auto configured = database.configureConnection(); !configured) {
        return tl::unexpected<core::Error>(std::move(configured.error()));
    }
    return database;
}

Result<Database> Database::openInMemory() {
    sqlite3* connection = nullptr;
    const int code = sqlite3_open_v2(
        ":memory:", &connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE,
        nullptr);
    if (code != SQLITE_OK) {
        auto error = sqliteError(connection, code, "opening in-memory database");
        sqlite3_close(connection);
        return tl::unexpected<core::Error>(std::move(error));
    }
    Database database(connection, {}, false);
    if (auto configured = database.configureConnection(); !configured) {
        return tl::unexpected<core::Error>(std::move(configured.error()));
    }
    return database;
}

Result<void> Database::configureConnection() {
    // docs/DATABASE_SCHEMA.md §2: applied on every connection.
    return execute("PRAGMA foreign_keys = ON;"
                   "PRAGMA synchronous = NORMAL;"
                   "PRAGMA busy_timeout = 5000;"
                   "PRAGMA temp_store = MEMORY;"
                   "PRAGMA cache_size = -32768;");
}

Database::~Database() {
    if (connection_ != nullptr) {
        cache_.clear(); // finalise cached statements first
        const int code = sqlite3_close(connection_);
        // Statements owned by callers must not outlive their database.
        assert(code == SQLITE_OK);
        if (code != SQLITE_OK) {
            sqlite3_close_v2(connection_); // defer the close until they are finalised
        }
    }
}

Database::Database(Database&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)), file_(std::move(other.file_)),
      readOnly_(other.readOnly_), cache_(std::move(other.cache_)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        Database closing(std::move(*this));
        connection_ = std::exchange(other.connection_, nullptr);
        file_ = std::move(other.file_);
        readOnly_ = other.readOnly_;
        cache_ = std::move(other.cache_);
    }
    return *this;
}

Result<void> Database::execute(std::string_view sql) {
    if (connection_ == nullptr) {
        return makeError(ErrorCode::Internal, "database is not open");
    }
    const std::string text(sql);
    char* message = nullptr;
    const int code = sqlite3_exec(connection_, text.c_str(), nullptr, nullptr, &message);
    if (code != SQLITE_OK) {
        std::string reason = message != nullptr ? message : sqlite3_errstr(code);
        sqlite3_free(message);
        const std::string_view shown = sql.substr(0, 120);
        return makeError(detail::errorCodeFor(code), "executing '" + std::string(shown) +
                                                         (sql.size() > shown.size() ? "..." : "") +
                                                         "': " + reason + " (SQLite code " +
                                                         std::to_string(code) + ")");
    }
    return {};
}

Result<Statement> Database::prepare(std::string_view sql) {
    if (connection_ == nullptr) {
        return makeError(ErrorCode::Internal, "database is not open");
    }
    if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return makeError(ErrorCode::InvalidArgument, "SQL text is too long");
    }
    sqlite3_stmt* statement = nullptr;
    const int code = sqlite3_prepare_v3(connection_, sql.data(), static_cast<int>(sql.size()),
                                        SQLITE_PREPARE_PERSISTENT, &statement, nullptr);
    if (code != SQLITE_OK) {
        return tl::unexpected<core::Error>(
            sqliteError(connection_, code, "preparing '" + std::string(sql) + "'"));
    }
    if (statement == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "SQL contains no statement");
    }
    return Statement(statement, connection_);
}

Result<CachedStatement> Database::cached(std::string_view sql) {
    const std::string key(sql);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        auto prepared = prepare(sql);
        if (!prepared) {
            return tl::unexpected<core::Error>(std::move(prepared.error()));
        }
        it = cache_.emplace(key, std::make_unique<Statement>(std::move(*prepared))).first;
    }
    it->second->reset();
    return CachedStatement(*it->second);
}

Result<std::int64_t> Database::queryInt(std::string_view sql) {
    auto statement = prepare(sql);
    if (!statement) {
        return tl::unexpected<core::Error>(std::move(statement.error()));
    }
    auto row = statement->step();
    if (!row) {
        return tl::unexpected<core::Error>(std::move(row.error()));
    }
    if (!*row || statement->columnType(0) != ColumnType::Integer) {
        return makeError(ErrorCode::Internal, "'" + std::string(sql) + "' returned no integer");
    }
    return statement->columnInt(0);
}

Result<std::string> Database::queryText(std::string_view sql) {
    auto statement = prepare(sql);
    if (!statement) {
        return tl::unexpected<core::Error>(std::move(statement.error()));
    }
    auto row = statement->step();
    if (!row) {
        return tl::unexpected<core::Error>(std::move(row.error()));
    }
    if (!*row || statement->columnIsNull(0)) {
        return makeError(ErrorCode::Internal, "'" + std::string(sql) + "' returned no value");
    }
    return std::string(statement->columnText(0));
}

std::int64_t Database::changes() const noexcept {
    return connection_ != nullptr ? sqlite3_changes64(connection_) : 0;
}

bool Database::inTransaction() const noexcept {
    return connection_ != nullptr && sqlite3_get_autocommit(connection_) == 0;
}

Result<void> Database::close() {
    if (connection_ == nullptr) {
        return {};
    }
    cache_.clear();
    const int code = sqlite3_close(connection_);
    if (code != SQLITE_OK) {
        return tl::unexpected<core::Error>(
            sqliteError(connection_, code, "closing database (unfinalised statements?)"));
    }
    connection_ = nullptr;
    return {};
}

} // namespace studyapp::persistence
