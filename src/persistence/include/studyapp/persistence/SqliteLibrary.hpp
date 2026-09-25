#pragma once

#include <string_view>

namespace studyapp::persistence {

/// Facts about the SQLite library linked into the application.
///
/// Phase 1 only establishes the SQLite dependency; the persistence subsystem itself
/// (schema, migrations, stores, workspace locking) is Phase 3. This lets the About dialog
/// and the test suite verify that the linked SQLite meets the documented requirements,
/// which matters when building with `STUDYAPP_USE_SYSTEM_SQLITE=ON`.
struct SqliteLibraryInfo {
    std::string_view version; ///< e.g. "3.50.4"
    int versionNumber = 0;    ///< e.g. 3050004
    bool hasFts5 = false;     ///< full-text search (required from Phase 8)
    bool hasRtree = false;    ///< R*Tree spatial index (optional, future)
    bool threadSafe = false;  ///< built with thread-safety enabled
};

[[nodiscard]] SqliteLibraryInfo sqliteLibraryInfo() noexcept;

/// Oldest SQLite version the schema design relies on (FTS5 contentless-delete tables).
inline constexpr int kMinimumSqliteVersionNumber = 3043000;

} // namespace studyapp::persistence
