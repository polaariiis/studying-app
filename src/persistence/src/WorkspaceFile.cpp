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
        // The directory existed (empty but for a lock file): remove only the new content.
        std::vector<std::filesystem::path> created;
        for (const auto& entry : std::filesystem::directory_iterator(root_, ignored)) {
            if (entry.path().filename() != WorkspaceLayout{root_}.lockFile().filename()) {
                created.push_back(entry.path());
            }
        }
        for (const auto& path : created) {
            std::filesystem::remove_all(path, ignored);
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

Result<WorkspaceFile> WorkspaceFile::create(const std::filesystem::path& root,
                                            const document::WorkspaceInfo& info,
                                            std::string_view appVersion) {
    std::error_code ec;
    const bool existed = std::filesystem::exists(root, ec);
    if (existed) {
        if (!std::filesystem::is_directory(root, ec)) {
            return makeError(ErrorCode::AlreadyExists, "'" + utf8(root) + "' is not a directory");
        }
        // The only entry allowed is the lock file, which the caller may already hold.
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (entry.path().filename() != WorkspaceLayout{root}.lockFile().filename()) {
                return makeError(ErrorCode::AlreadyExists,
                                 "'" + utf8(root) +
                                     "' is not empty; a new workspace needs an empty or new "
                                     "directory");
            }
        }
        if (ec) {
            return makeError(ErrorCode::IoError,
                             "cannot list '" + utf8(root) + "': " + ec.message());
        }
    }
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
    auto migrated = migrate(*database, builtinMigrations());
    if (!migrated) {
        return forward(migrated);
    }
    {
        auto transaction = Transaction::begin(*database);
        if (!transaction) {
            return forward(transaction);
        }
        if (auto written = CatalogStore(*database).createInfo(*transaction, info, appVersion);
            !written) {
            return forward(written);
        }
        if (auto committed = transaction->commit(); !committed) {
            return forward(committed);
        }
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
    if (!readOnly) {
        if (auto created = createSubdirectories(layout); !created) {
            return forward(created);
        }
    }
    auto database =
        Database::open(layout.database(), readOnly ? OpenMode::ReadOnly : OpenMode::ReadWrite);
    if (!database) {
        return forward(database);
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
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(layout_.temporary(), ec)) {
        entries.push_back(entry.path());
    }
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
