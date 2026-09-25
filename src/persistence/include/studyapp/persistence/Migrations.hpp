#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/persistence/Database.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace studyapp::persistence {

// Schema versioning (docs/DATABASE_SCHEMA.md §8).
//
// `PRAGMA user_version` holds the schema version. Migration N upgrades version N-1 to N;
// each runs in its own transaction together with the user_version update. Before an
// existing database (version > 0) is upgraded, a consistent copy is written to the backup
// directory with `VACUUM INTO`. A database whose version is newer than the newest known
// migration is refused for writing.

/// 'STUD': identifies StudyBoard databases (`PRAGMA application_id`).
inline constexpr std::int32_t kApplicationId = 0x53545544;

struct Migration {
    int version = 0;
    std::string_view name; ///< file name, e.g. "0001_initial.sql"
    std::string_view sql;
};

/// The migrations built into this binary (src/persistence/migrations/), in version order.
[[nodiscard]] std::span<const Migration> builtinMigrations();

/// Schema version this binary writes (the newest built-in migration).
[[nodiscard]] int currentSchemaVersion();

class Transaction;

struct MigrationOptions {
    using Initializer = std::function<core::Result<void>(Transaction&)>;

    /// Where to write the pre-migration backup of an existing database. Required when an
    /// existing database needs upgrading (never needed for a new one).
    std::optional<std::filesystem::path> backupDirectory;
    /// Used in the backup file name.
    core::Timestamp now{};
    /// New databases only: writes initial data (e.g. workspace metadata) inside the
    /// transaction of the last migration, so a database never exists with a schema but
    /// without its initial data. A failure rolls that migration back.
    Initializer initializeNew{};
};

struct MigrationResult {
    int fromVersion = 0;
    int toVersion = 0;
    std::optional<std::filesystem::path> backup; ///< written before upgrading
};

/// Brings `database` to the newest version in `migrations` (which must be numbered
/// 1..N without gaps).
///
///   * new, empty database (version 0, no schema objects, application_id 0 or ours — the
///     latter after an interrupted creation): sets page_size / WAL, then runs all
///     migrations, the first one together with application_id and the last one together
///     with `options.initializeNew` (no backup; nothing to lose);
///   * older version: writes a backup, then runs the pending migrations;
///   * same version: nothing to do;
///   * newer version: fails with Unsupported for a read-write connection (a newer app
///     created it); a read-only connection is allowed to proceed;
///   * a database that is not a StudyBoard workspace (foreign application_id, or tables
///     without a version) fails with Unsupported.
///
/// A read-only connection is never modified: it fails with Unsupported if it would need
/// an upgrade.
[[nodiscard]] core::Result<MigrationResult> migrate(Database& database,
                                                    std::span<const Migration> migrations,
                                                    const MigrationOptions& options = {});

/// `PRAGMA user_version`.
[[nodiscard]] core::Result<int> schemaVersion(Database& database);

/// Consistent snapshot of the whole database (`VACUUM INTO`); `target` must not exist.
[[nodiscard]] core::Result<void> backupDatabase(Database& database,
                                                const std::filesystem::path& target);

/// `YYYYMMDDTHHMMSSmmmZ` (UTC), for file names. Formatted with integer arithmetic only
/// (no std::chrono formatting; docs/ARCHITECTURE.md §17).
[[nodiscard]] std::string fileTimestamp(core::Timestamp time);

} // namespace studyapp::persistence
