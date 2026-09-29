#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/persistence/WorkspaceLayout.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace studyapp::persistence {

// Automatic backups and their rotation (Phase 9; docs/DATABASE_SCHEMA.md §9). A backup is a
// `VACUUM INTO` snapshot of the database in the workspace's backups/ directory, named
// `<label>-<fileTimestamp>.db`. Assets are not copied: they are immutable and never
// removed automatically (asset garbage collection is not run by the application), so a
// retained snapshot's assets stay in place.

/// Label of automatic and user-requested backups (pre-migration backups use their own).
inline constexpr std::string_view kAutoBackupLabel = "auto";

struct BackupFile {
    std::filesystem::path file;
    core::Timestamp time{};
};

/// fileTimestamp() read back ("YYYYMMDDTHHMMSSmmmZ"); nullopt if `text` is not one.
[[nodiscard]] std::optional<core::Timestamp> parseFileTimestamp(std::string_view text);

/// The backups labelled `label` in `layout`'s backups directory, oldest first; files whose
/// name does not parse are ignored. A missing directory is an empty list.
[[nodiscard]] core::Result<std::vector<BackupFile>> listBackups(const WorkspaceLayout& layout,
                                                                std::string_view label);

/// Rotation: keeps the newest backup of each of the `keepDays` most recent days (UTC) that
/// have one, and the newest of each of the `keepWeeks` most recent weeks; returns the
/// others. The newest backup is always kept. Pure (deterministic, testable).
[[nodiscard]] std::vector<std::filesystem::path>
backupsToRemove(std::span<const BackupFile> backups, int keepDays, int keepWeeks);

} // namespace studyapp::persistence
