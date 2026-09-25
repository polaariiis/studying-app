#pragma once

#include <filesystem>

namespace studyapp::persistence {

/// On-disk layout of a workspace directory (docs/ARCHITECTURE.md §11):
///
///   <root>/
///   ├── workspace.db     SQLite database (WAL; -wal/-shm files are transient)
///   ├── assets/          content-addressed binaries: assets/ab/cd/<sha256>.<ext>
///   ├── backups/         consistent snapshots (VACUUM INTO)
///   ├── temporary/       in-progress imports; emptied when opened with the lock held
///   └── .lock            exclusive-writer lock (managed by the application layer)
///
/// All paths inside the workspace are relative to the root, so a workspace can be moved or
/// copied as a whole.
struct WorkspaceLayout {
    std::filesystem::path root;

    [[nodiscard]] std::filesystem::path database() const { return root / "workspace.db"; }
    [[nodiscard]] std::filesystem::path assets() const { return root / "assets"; }
    [[nodiscard]] std::filesystem::path backups() const { return root / "backups"; }
    [[nodiscard]] std::filesystem::path temporary() const { return root / "temporary"; }
    [[nodiscard]] std::filesystem::path lockFile() const { return root / ".lock"; }
};

} // namespace studyapp::persistence
