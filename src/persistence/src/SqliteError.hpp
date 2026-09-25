#pragma once

#include <studyapp/core/Error.hpp>

#include <sqlite3.h>

#include <string>
#include <string_view>

namespace studyapp::persistence::detail {

/// Maps a SQLite result code to the project's error model:
///
///   BUSY, LOCKED, CONSTRAINT          -> Conflict
///   CANTOPEN, IOERR, FULL, READONLY,
///   PERM, NOLFS                       -> IoError
///   CORRUPT, NOTADB, FORMAT           -> ParseError
///   everything else                   -> Internal
[[nodiscard]] inline core::ErrorCode errorCodeFor(int sqliteCode) noexcept {
    switch (sqliteCode & 0xFF) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
    case SQLITE_CONSTRAINT:
        return core::ErrorCode::Conflict;
    case SQLITE_CANTOPEN:
    case SQLITE_IOERR:
    case SQLITE_FULL:
    case SQLITE_READONLY:
    case SQLITE_PERM:
    case SQLITE_NOLFS:
        return core::ErrorCode::IoError;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_FORMAT:
        return core::ErrorCode::ParseError;
    default:
        return core::ErrorCode::Internal;
    }
}

/// Error for a failed SQLite call. `connection` may be null (e.g. a failed open).
[[nodiscard]] inline core::Error sqliteError(sqlite3* connection, int sqliteCode,
                                             std::string_view context) {
    std::string message(context);
    message += ": ";
    message += connection != nullptr ? sqlite3_errmsg(connection) : sqlite3_errstr(sqliteCode);
    message += " (SQLite code " + std::to_string(sqliteCode) + ")";
    return core::Error{errorCodeFor(sqliteCode), std::move(message)};
}

} // namespace studyapp::persistence::detail
