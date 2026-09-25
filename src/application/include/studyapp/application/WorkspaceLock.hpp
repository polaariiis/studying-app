#pragma once

#include <studyapp/core/Error.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace studyapp::application {

// Port for the exclusive-writer workspace lock (docs/ARCHITECTURE.md §11.1). The lock is
// an OS-level lock held for the whole session, so a crashed process releases it; its
// owner metadata lets the UI say who holds it and lets the adapter classify a leftover
// lock as stale. The implementation lives in `platform` (QLockFile); tests may provide
// their own.

/// Who holds (or held) a lock, as recorded in the lock file.
struct LockOwner {
    std::int64_t processId = 0;
    std::string hostName;
    std::string applicationName;

    [[nodiscard]] friend bool operator==(const LockOwner&, const LockOwner&) = default;
};

enum class LockState {
    Free,   ///< no lock file
    Active, ///< held by a live process, or by another host (never auto-classified stale)
    Stale,  ///< left behind: the recorded process on this host no longer runs
};

struct LockStatus {
    LockState state = LockState::Free;
    std::optional<LockOwner> owner; ///< known for Active/Stale when the file is readable
};

/// A held lock; released when destroyed.
class WorkspaceLock {
public:
    WorkspaceLock() = default;
    virtual ~WorkspaceLock() = default;
    WorkspaceLock(const WorkspaceLock&) = delete;
    WorkspaceLock& operator=(const WorkspaceLock&) = delete;
    WorkspaceLock(WorkspaceLock&&) = delete;
    WorkspaceLock& operator=(WorkspaceLock&&) = delete;
};

class WorkspaceLocker {
public:
    WorkspaceLocker() = default;
    virtual ~WorkspaceLocker() = default;
    WorkspaceLocker(const WorkspaceLocker&) = delete;
    WorkspaceLocker& operator=(const WorkspaceLocker&) = delete;
    WorkspaceLocker(WorkspaceLocker&&) = delete;
    WorkspaceLocker& operator=(WorkspaceLocker&&) = delete;

    /// Classifies the lock without changing it.
    [[nodiscard]] virtual core::Result<LockStatus>
    inspect(const std::filesystem::path& lockFile) = 0;

    /// Acquires the lock. Fails with Conflict if it is Active, or if it is Stale and
    /// `takeOverStale` is false (recovery is an explicit decision, never automatic).
    [[nodiscard]] virtual core::Result<std::unique_ptr<WorkspaceLock>>
    acquire(const std::filesystem::path& lockFile, bool takeOverStale) = 0;
};

} // namespace studyapp::application
