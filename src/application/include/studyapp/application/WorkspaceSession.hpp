#pragma once

#include <studyapp/application/WorkspaceLock.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/UndoStack.hpp>
#include <studyapp/document/Workspace.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace studyapp::application {

/// Services a session needs from its environment (constructor injection; the clock and
/// id generator are deterministic in tests).
struct SessionServices {
    const core::Clock& clock;
    core::IdGenerator& ids;
    WorkspaceLocker& locker;
};

enum class AccessMode {
    ReadWrite,
    ReadOnly, ///< no lock is taken, nothing is written, edits are rejected
};

struct OpenOptions {
    AccessMode mode = AccessMode::ReadWrite;
    /// Take over a *stale* lock (the user chose "Recover"): runs an integrity check and
    /// empties temporary/ before loading. An active lock is never taken over.
    bool recoverStaleLock = false;
};

/// An open workspace: the in-memory document, its undo history and its database.
///
/// Every edit goes Command → Patch → Workspace (via document::Editor) → persistence:
/// after the Editor has applied a command (or an undo/redo), the session writes exactly the
/// patch that was applied — the command's patch, or its inverse for undo — in one database
/// transaction (docs/DATABASE_SCHEMA.md §7.1). There is no second mutation path and no
/// per-command SQL. Persistence is continuous and, in Phase 3, synchronous.
///
/// Write failures: the in-memory edit stands (data in memory is never discarded), the
/// patch stays queued in order, the error is logged and reported by lastWriteError(), and
/// the next edit, flush() or close() retries. pendingWriteCount() > 0 therefore always
/// means "the database is behind memory".
///
/// Not thread-safe; used on the GUI thread (docs/ARCHITECTURE.md §10).
class WorkspaceSession {
public:
    /// Creates a new workspace in `root` (must not exist or be empty) and opens it.
    [[nodiscard]] static core::Result<std::unique_ptr<WorkspaceSession>>
    create(const std::filesystem::path& root, std::string name, SessionServices services);

    /// Opens an existing workspace. Read-write fails with Conflict if the lock is held by a
    /// live process, or is stale and `options.recoverStaleLock` is false; inspectLock()
    /// tells which, so the UI can offer read-only / recover / cancel.
    [[nodiscard]] static core::Result<std::unique_ptr<WorkspaceSession>>
    open(const std::filesystem::path& root, OpenOptions options, SessionServices services);

    [[nodiscard]] static core::Result<LockStatus> inspectLock(const std::filesystem::path& root,
                                                              WorkspaceLocker& locker);

    /// Flushes pending writes (best effort; failures are logged) and releases the lock.
    ~WorkspaceSession();
    WorkspaceSession(const WorkspaceSession&) = delete;
    WorkspaceSession& operator=(const WorkspaceSession&) = delete;
    WorkspaceSession(WorkspaceSession&&) = delete;
    WorkspaceSession& operator=(WorkspaceSession&&) = delete;

    [[nodiscard]] const document::Workspace& workspace() const noexcept;
    [[nodiscard]] const document::UndoStack& history() const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] bool isReadOnly() const noexcept;
    /// True if this session took over a stale lock and ran recovery.
    [[nodiscard]] bool recoveredStaleLock() const noexcept;

    /// Applies the command in memory, records it for undo and persists its patch.
    /// Errors: Unsupported (read-only/closed), NotFound (image references an asset that was
    /// never imported), or the document's own validation errors; nothing changes then.
    [[nodiscard]] core::Result<void> execute(document::Command command);
    [[nodiscard]] core::Result<void> undo();
    [[nodiscard]] core::Result<void> redo();
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;

    /// Writes all pending patches ("Save"). Returns the first write error, if any.
    [[nodiscard]] core::Result<void> flush();
    [[nodiscard]] std::size_t pendingWriteCount() const noexcept;
    [[nodiscard]] const std::optional<core::Error>& lastWriteError() const noexcept;

    /// Imports an image/PDF into the workspace's content-addressed asset store.
    [[nodiscard]] core::Result<core::AssetId> importAsset(const std::filesystem::path& source,
                                                          std::string_view mediaType);
    [[nodiscard]] core::Result<std::filesystem::path> assetPath(core::AssetId asset);

    /// Flushes, closes the database and releases the lock. If flushing fails the session
    /// stays open (nothing is lost) and the error is returned.
    [[nodiscard]] core::Result<void> close();
    [[nodiscard]] bool isClosed() const noexcept;

private:
    struct Impl;
    explicit WorkspaceSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace studyapp::application
