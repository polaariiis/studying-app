#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/document/Records.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Migrations.hpp>
#include <studyapp/persistence/WorkspaceLayout.hpp>

#include <filesystem>
#include <string_view>

namespace studyapp::persistence {

enum class AccessMode {
    ReadWrite,
    ReadOnly, ///< SQLITE_OPEN_READONLY; nothing in the workspace is modified
};

/// An open workspace directory (docs/ARCHITECTURE.md §11): its layout and the read-write
/// (or read-only) connection to `workspace.db`, with the schema at the current version.
///
/// Locking is not done here: the exclusive-writer lock (§11.1) is an OS-level lock that
/// the application layer acquires through a platform adapter *before* opening the
/// workspace read-write.
class WorkspaceFile {
public:
    /// Creates a new workspace in `root`, which must not exist or be an empty directory
    /// (a `.lock` file already held by the caller is allowed): the directory structure,
    /// the database with the current schema, and the metadata. On failure, everything
    /// created is removed again.
    [[nodiscard]] static core::Result<WorkspaceFile> create(const std::filesystem::path& root,
                                                            const document::WorkspaceInfo& info,
                                                            std::string_view appVersion);

    /// Opens an existing workspace. Read-write: upgrades the schema if needed (after a
    /// backup in backups/), recreates missing subdirectories and records `appVersion` as
    /// the last writer. Fails with NotFound if `root` holds no workspace database.
    [[nodiscard]] static core::Result<WorkspaceFile> open(const std::filesystem::path& root,
                                                          AccessMode mode, core::Timestamp now,
                                                          std::string_view appVersion);

    [[nodiscard]] const WorkspaceLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] Database& database() noexcept { return database_; }
    [[nodiscard]] bool isReadOnly() const noexcept { return database_.isReadOnly(); }
    [[nodiscard]] const MigrationResult& migration() const noexcept { return migration_; }

    /// Consistent snapshot in backups/: `<label>-<timestamp>.db`.
    [[nodiscard]] core::Result<std::filesystem::path> backup(std::string_view label,
                                                             core::Timestamp now);

    /// `PRAGMA quick_check` and `PRAGMA foreign_key_check`; InvalidArgument listing the
    /// first problems if either reports any.
    [[nodiscard]] core::Result<void> integrityCheck();

    /// Empties temporary/ (only safe while holding the workspace lock).
    [[nodiscard]] core::Result<void> cleanTemporary();

    [[nodiscard]] core::Result<void> close() { return database_.close(); }

private:
    WorkspaceFile(WorkspaceLayout layout, Database database, MigrationResult migration) noexcept
        : layout_(std::move(layout)), database_(std::move(database)),
          migration_(std::move(migration)) {}

    WorkspaceLayout layout_;
    Database database_;
    MigrationResult migration_;
};

} // namespace studyapp::persistence
