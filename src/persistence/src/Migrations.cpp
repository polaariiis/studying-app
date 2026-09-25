#include <studyapp/persistence/Migrations.hpp>

#include "EmbeddedFiles.hpp"
#include "Utf8Path.hpp"

#include <studyapp/persistence/Transaction.hpp>

#include <charconv>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

std::vector<Migration> loadBuiltinMigrations() {
    std::vector<Migration> migrations;
    for (const detail::EmbeddedFile& file : detail::migrationFiles()) {
        int version = 0;
        const auto* begin = file.name.data();
        const auto [end, ec] = std::from_chars(begin, begin + file.name.size(), version);
        // File names are fixed at build time; a malformed one is a programming error that
        // the migration tests catch (version 0 fails the numbering check in migrate()).
        (void)end;
        migrations.push_back(Migration{
            .version = ec == std::errc{} ? version : 0, .name = file.name, .sql = file.content});
    }
    return migrations;
}

Result<void> checkNumbering(std::span<const Migration> migrations) {
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        if (migrations[i].version != static_cast<int>(i) + 1) {
            return makeError(ErrorCode::Internal, "migration '" + std::string(migrations[i].name) +
                                                      "' is out of sequence (expected version " +
                                                      std::to_string(i + 1) + ")");
        }
    }
    return {};
}

Result<void> runMigration(Database& database, const Migration& migration) {
    auto transaction = Transaction::begin(database, Transaction::Kind::Immediate);
    if (!transaction) {
        return tl::unexpected<core::Error>(std::move(transaction.error()));
    }
    if (auto applied = database.execute(migration.sql); !applied) {
        return makeError(applied.error().code, "migration '" + std::string(migration.name) +
                                                   "' failed: " + applied.error().message);
    }
    if (auto versioned =
            database.execute("PRAGMA user_version = " + std::to_string(migration.version));
        !versioned) {
        return versioned;
    }
    return transaction->commit();
}

// Howard Hinnant's civil_from_days: days since 1970-01-01 -> (year, month, day).
struct CivilDate {
    std::int64_t year;
    unsigned month;
    unsigned day;
};

CivilDate civilFromDays(std::int64_t days) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned mp = (5 * dayOfYear + 2) / 153;
    const unsigned day = dayOfYear - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year =
        static_cast<std::int64_t>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0);
    return {year, month, day};
}

} // namespace

std::span<const Migration> builtinMigrations() {
    static const std::vector<Migration> migrations = loadBuiltinMigrations();
    return migrations;
}

int currentSchemaVersion() {
    const auto migrations = builtinMigrations();
    return migrations.empty() ? 0 : migrations.back().version;
}

Result<int> schemaVersion(Database& database) {
    auto version = database.queryInt("PRAGMA user_version");
    if (!version) {
        return tl::unexpected<core::Error>(std::move(version.error()));
    }
    return static_cast<int>(*version);
}

std::string fileTimestamp(core::Timestamp time) {
    const std::int64_t millis = time.time_since_epoch().count();
    constexpr std::int64_t kMillisPerDay = 86'400'000;
    std::int64_t days = millis / kMillisPerDay;
    std::int64_t ofDay = millis % kMillisPerDay;
    if (ofDay < 0) {
        ofDay += kMillisPerDay;
        --days;
    }
    const CivilDate date = civilFromDays(days);
    const std::int64_t seconds = ofDay / 1000;
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%04lld%02u%02uT%02lld%02lld%02lld%03lldZ",
                  static_cast<long long>(date.year), date.month, date.day,
                  static_cast<long long>(seconds / 3600),
                  static_cast<long long>((seconds / 60) % 60), static_cast<long long>(seconds % 60),
                  static_cast<long long>(ofDay % 1000));
    return buffer;
}

Result<void> backupDatabase(Database& database, const std::filesystem::path& target) {
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        return makeError(ErrorCode::AlreadyExists,
                         "backup target '" + detail::utf8(target) + "' already exists");
    }
    auto statement = database.prepare("VACUUM INTO ?1");
    if (!statement) {
        return tl::unexpected<core::Error>(std::move(statement.error()));
    }
    statement->bindText(1, detail::utf8(target));
    if (auto done = statement->run(); !done) {
        return makeError(done.error().code, "backup to '" + detail::utf8(target) +
                                                "' failed: " + done.error().message);
    }
    return {};
}

Result<MigrationResult> migrate(Database& database, std::span<const Migration> migrations,
                                const MigrationOptions& options) {
    if (auto numbering = checkNumbering(migrations); !numbering) {
        return tl::unexpected<core::Error>(std::move(numbering.error()));
    }
    const int latest = migrations.empty() ? 0 : migrations.back().version;

    auto version = schemaVersion(database);
    if (!version) {
        return tl::unexpected<core::Error>(std::move(version.error()));
    }
    auto applicationId = database.queryInt("PRAGMA application_id");
    if (!applicationId) {
        return tl::unexpected<core::Error>(std::move(applicationId.error()));
    }
    const bool ours = *applicationId == kApplicationId;

    MigrationResult result{.fromVersion = *version, .toVersion = *version, .backup = {}};

    if (*version == 0) {
        auto objects = database.queryInt("SELECT count(*) FROM sqlite_schema");
        if (!objects) {
            return tl::unexpected<core::Error>(std::move(objects.error()));
        }
        if (*applicationId != 0 || *objects != 0) {
            return makeError(ErrorCode::Unsupported,
                             "the database is not an empty or StudyBoard workspace database");
        }
        if (database.isReadOnly()) {
            return makeError(ErrorCode::Unsupported,
                             "the workspace database is not initialised (opened read-only)");
        }
        // Database-level settings (docs/DATABASE_SCHEMA.md §2); not allowed in a transaction.
        if (auto configured =
                database.execute("PRAGMA application_id = " + std::to_string(kApplicationId) +
                                 ";PRAGMA page_size = 4096;PRAGMA journal_mode = WAL;");
            !configured) {
            return tl::unexpected<core::Error>(std::move(configured.error()));
        }
    } else {
        if (!ours) {
            return makeError(ErrorCode::Unsupported, "the database is not a StudyBoard workspace");
        }
        if (*version > latest) {
            if (database.isReadOnly()) {
                return result;
            }
            return makeError(
                ErrorCode::Unsupported,
                "the workspace was created by a newer version of StudyBoard (schema v" +
                    std::to_string(*version) + ", this version supports v" +
                    std::to_string(latest) + "); it can only be opened read-only");
        }
        if (*version == latest) {
            return result;
        }
        if (database.isReadOnly()) {
            return makeError(ErrorCode::Unsupported,
                             "the workspace needs a schema upgrade (v" + std::to_string(*version) +
                                 " to v" + std::to_string(latest) +
                                 ") and cannot be upgraded while opened read-only");
        }
        if (!options.backupDirectory) {
            return makeError(ErrorCode::InvalidArgument,
                             "a backup directory is required to upgrade an existing workspace");
        }
        const auto backup =
            *options.backupDirectory / ("pre-migration-v" + std::to_string(*version) + "-" +
                                        fileTimestamp(options.now) + ".db");
        if (auto saved = backupDatabase(database, backup); !saved) {
            return tl::unexpected<core::Error>(std::move(saved.error()));
        }
        result.backup = backup;
    }

    for (const Migration& migration : migrations) {
        if (migration.version <= *version) {
            continue;
        }
        if (auto ran = runMigration(database, migration); !ran) {
            return tl::unexpected<core::Error>(std::move(ran.error()));
        }
        result.toVersion = migration.version;
    }
    return result;
}

} // namespace studyapp::persistence
