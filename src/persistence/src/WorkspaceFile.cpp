#include <studyapp/persistence/WorkspaceFile.hpp>

#include "StoreSupport.hpp"
#include "Utf8Path.hpp"

#include <studyapp/persistence/CatalogStore.hpp>
#include <studyapp/persistence/Transaction.hpp>

#include <string>
#include <system_error>
#include <vector>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;
using detail::forward;
using detail::utf8;

namespace {

Result<void> createDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return makeError(ErrorCode::IoError,
                         "cannot create '" + utf8(directory) + "': " + ec.message());
    }
    return {};
}

Result<void> createSubdirectories(const WorkspaceLayout& layout) {
    for (const auto& directory : {layout.assets(), layout.backups(), layout.temporary()}) {
        if (auto created = createDirectory(directory); !created) {
            return created;
        }
    }
    return {};
}

/// Entries of `directory`, iterated with error codes: a range-for over a
/// directory_iterator throws from `++` if the directory changes or fails mid-scan.
std::vector<std::filesystem::path> listDirectory(const std::filesystem::path& directory,
                                                 std::error_code& ec) {
    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        entries.push_back(it->path());
    }
    return entries;
}

/// True if `file` is an SQLite database without any schema yet: what an interrupted
/// WorkspaceFile::create leaves behind (at most page_size/journal_mode were written; the
/// first migration and everything after it commit atomically). Anything else — including a
/// file that is not an SQLite database — is not considered, so it is never reinitialised.
bool isUninitialisedDatabase(const std::filesystem::path& file) {
    auto database = Database::open(file, OpenMode::ReadWrite);
    if (!database) {
        return false;
    }
    const auto version = database->queryInt("PRAGMA user_version");
    const auto objects = database->queryInt("SELECT count(*) FROM sqlite_schema");
    const auto applicationId = database->queryInt("PRAGMA application_id");
    return version && *version == 0 && objects && *objects == 0 && applicationId &&
           (*applicationId == 0 || *applicationId == kApplicationId);
}

/// Removes what WorkspaceFile::create made if creation fails part-way.
class CreationRollback {
public:
    CreationRollback(std::filesystem::path root, bool rootExisted)
        : root_(std::move(root)), rootExisted_(rootExisted) {}
    ~CreationRollback() {
        if (committed_) {
            return;
        }
        std::error_code ignored;
        if (!rootExisted_) {
            std::filesystem::remove_all(root_, ignored);
            return;
        }
        // The directory existed (empty but for a lock file, or the leftovers of an
        // interrupted creation): remove everything except the lock file.
        for (const auto& path : listDirectory(root_, ignored)) {
            if (path.filename() != WorkspaceLayout{root_}.lockFile().filename()) {
                std::filesystem::remove_all(path, ignored);
            }
        }
    }
    CreationRollback(const CreationRollback&) = delete;
    CreationRollback& operator=(const CreationRollback&) = delete;
    CreationRollback(CreationRollback&&) = delete;
    CreationRollback& operator=(CreationRollback&&) = delete;

    void commit() noexcept { committed_ = true; }

private:
    std::filesystem::path root_;
    bool rootExisted_;
    bool committed_ = false;
};

} // namespace

Result<void> WorkspaceFile::checkCanCreate(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return {};
    }
    const auto notEmpty = [&] {
        return makeError(ErrorCode::AlreadyExists,
                         "'" + utf8(root) +
                             "' is not empty; a new workspace needs an empty or new directory");
    };
    if (!std::filesystem::is_directory(root, ec)) {
        return makeError(ErrorCode::AlreadyExists, "'" + utf8(root) + "' is not a directory");
    }
    const auto entries = listDirectory(root, ec);
    if (ec) {
        return makeError(ErrorCode::IoError, "cannot list '" + utf8(root) + "': " + ec.message());
    }
    // Allowed: the lock file (the caller may already hold it) and the leftovers of an
    // interrupted creation — empty subdirectories and an uninitialised database.
    const WorkspaceLayout layout{root};
    const std::string db = layout.database().filename().string();
    for (const auto& entry : entries) {
        const auto name = entry.filename();
        if (name == layout.lockFile().filename()) {
            continue;
        }
        if (name == layout.assets().filename() || name == layout.backups().filename() ||
            name == layout.temporary().filename()) {
            if (!std::filesystem::is_directory(entry, ec) ||
                !std::filesystem::is_empty(entry, ec)) {
                return notEmpty();
            }
            continue;
        }
        if (name == db || name == db + "-wal" || name == db + "-shm" || name == db + "-journal") {
            continue; // checked below
        }
        return notEmpty();
    }
    if (std::filesystem::exists(layout.database(), ec) &&
        !isUninitialisedDatabase(layout.database())) {
        return notEmpty();
    }
    return {};
}

Result<WorkspaceFile> WorkspaceFile::create(const std::filesystem::path& root,
                                            const document::WorkspaceInfo& info,
                                            std::string_view appVersion) {
    if (auto creatable = checkCanCreate(root); !creatable) {
        return forward(creatable);
    }
    std::error_code ec;
    const bool existed = std::filesystem::exists(root, ec);
    const WorkspaceLayout layout{root};
    CreationRollback rollback(root, existed);
    if (auto created = createDirectory(root); !created) {
        return forward(created);
    }
    if (auto created = createSubdirectories(layout); !created) {
        return forward(created);
    }

    auto database = Database::open(layout.database(), OpenMode::Create);
    if (!database) {
        return forward(database);
    }
    // The metadata is written in the transaction of the last migration: a crash leaves either
    // no schema at all (reinitialised by the next create) or a complete workspace.
    MigrationOptions options;
    options.initializeNew = [&](Transaction& transaction) {
        return CatalogStore(*database).createInfo(transaction, info, appVersion);
    };
    auto migrated = migrate(*database, builtinMigrations(), options);
    if (!migrated) {
        return forward(migrated);
    }
    if (migrated->fromVersion != 0) {
        return makeError(ErrorCode::AlreadyExists,
                         "'" + utf8(root) + "' already contains a workspace database");
    }
    rollback.commit();
    return WorkspaceFile(layout, std::move(*database), std::move(*migrated));
}

Result<WorkspaceFile> WorkspaceFile::open(const std::filesystem::path& root, AccessMode mode,
                                          core::Timestamp now, std::string_view appVersion) {
    const WorkspaceLayout layout{root};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(layout.database(), ec)) {
        return makeError(ErrorCode::NotFound,
                         "'" + utf8(root) + "' is not a StudyBoard workspace (no workspace.db)");
    }
    const bool readOnly = mode == AccessMode::ReadOnly;
    auto database =
        Database::open(layout.database(), readOnly ? OpenMode::ReadOnly : OpenMode::ReadWrite);
    if (!database) {
        return forward(database);
    }
    auto version = schemaVersion(*database);
    if (!version) {
        return forward(version); // e.g. not an SQLite database: refused, file untouched
    }
    if (*version == 0) {
        // Never initialise a database on open: only create() does that, together with the
        // workspace metadata. This is what an interrupted creation leaves behind.
        return makeError(ErrorCode::Unsupported,
                         "the workspace in '" + utf8(root) +
                             "' was not created completely; create a new workspace there");
    }
    if (!readOnly) {
        if (auto created = createSubdirectories(layout); !created) {
            return forward(created);
        }
    }
    auto migrated = migrate(*database, builtinMigrations(),
                            MigrationOptions{.backupDirectory = layout.backups(), .now = now});
    if (!migrated) {
        return forward(migrated);
    }
    if (!readOnly) {
        auto transaction = Transaction::begin(*database);
        if (!transaction) {
            return forward(transaction);
        }
        if (auto recorded = CatalogStore(*database).recordWriterVersion(*transaction, appVersion);
            !recorded) {
            return forward(recorded);
        }
        if (auto committed = transaction->commit(); !committed) {
            return forward(committed);
        }
    }
    return WorkspaceFile(layout, std::move(*database), std::move(*migrated));
}

Result<std::filesystem::path> WorkspaceFile::backup(std::string_view label, core::Timestamp now) {
    if (auto created = createDirectory(layout_.backups()); !created) {
        return forward(created);
    }
    const auto target = layout_.backups() / (std::string(label) + "-" + fileTimestamp(now) + ".db");
    if (auto saved = backupDatabase(database_, target); !saved) {
        return forward(saved);
    }
    return target;
}

Result<void> WorkspaceFile::integrityCheck() {
    std::vector<std::string> problems;
    {
        auto statement = database_.prepare("PRAGMA quick_check");
        if (!statement) {
            return forward(statement);
        }
        while (true) {
            auto row = statement->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            const std::string line(statement->columnText(0));
            if (line != "ok") {
                problems.push_back(line);
            }
        }
    }
    {
        auto statement = database_.prepare("PRAGMA foreign_key_check");
        if (!statement) {
            return forward(statement);
        }
        while (true) {
            auto row = statement->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            problems.push_back("foreign key violation in table '" +
                               std::string(statement->columnText(0)) + "' (references '" +
                               std::string(statement->columnText(2)) + "')");
        }
    }
    if (problems.empty()) {
        return {};
    }
    std::string message = "workspace integrity check failed: " + problems.front();
    if (problems.size() > 1) {
        message += " (and " + std::to_string(problems.size() - 1) + " more)";
    }
    return makeError(ErrorCode::InvalidArgument, std::move(message));
}

Result<void> WorkspaceFile::cleanTemporary() {
    std::error_code ec;
    if (!std::filesystem::exists(layout_.temporary(), ec)) {
        return {};
    }
    const auto entries = listDirectory(layout_.temporary(), ec);
    if (ec) {
        return makeError(ErrorCode::IoError,
                         "cannot list '" + utf8(layout_.temporary()) + "': " + ec.message());
    }
    for (const auto& entry : entries) {
        std::filesystem::remove_all(entry, ec);
        if (ec) {
            return makeError(ErrorCode::IoError,
                             "cannot remove '" + utf8(entry) + "': " + ec.message());
        }
    }
    return {};
}

} // namespace studyapp::persistence
