#pragma once

#include <studyapp/application/WorkspaceLock.hpp>

#include <filesystem>
#include <memory>

namespace studyapp::platform {

/// Workspace lock on top of QLockFile (docs/ARCHITECTURE.md §11.1).
///
/// QLockFile holds an OS-level lock for the lifetime of the session (an exclusively
/// opened file on Windows, `flock` on Unix), so the lock disappears with a crashed process,
/// and it records the owner's process id, host name and application name.
///
/// Classification (inspect):
///   * no lock file                                   → Free
///   * recorded host is another machine               → Active (never auto-stale: a
///                                                      workspace on a network drive needs an
///                                                      explicit decision)
///   * this machine, recorded process still running   → Active
///   * this machine, recorded process gone, or the
///     file is unreadable                             → Stale
/// Time-based staleness is disabled (`setStaleLockTime(0)`): a long session is not stale.
class QtWorkspaceLocker final : public application::WorkspaceLocker {
public:
    [[nodiscard]] core::Result<application::LockStatus>
    inspect(const std::filesystem::path& lockFile) override;

    [[nodiscard]] core::Result<std::unique_ptr<application::WorkspaceLock>>
    acquire(const std::filesystem::path& lockFile, bool takeOverStale) override;
};

} // namespace studyapp::platform
