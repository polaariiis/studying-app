#pragma once

#include <studyapp/application/WorkspaceLock.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/UndoStack.hpp>
#include <studyapp/document/Workspace.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::application {

/// A file being imported as an asset, copied and hashed but not stored yet
/// (WorkspaceSession::prepareAssetImport).
struct StagedAssetFile {
    core::AssetId id;
    std::filesystem::path file; ///< in the workspace's temporary directory
    std::array<std::uint8_t, 32> sha256{};
    std::uint64_t byteSize = 0;
    std::string originalName;
};

/// Result of WorkspaceSession::checkIntegrity(): empty when everything is fine.
struct IntegrityReport {
    std::vector<std::string> problems; ///< one line each, for the user
};

/// Automatic backups (Phase 9, docs/DATABASE_SCHEMA.md §9): when a session that changed the
/// workspace is closed, a database snapshot is written to backups/ if the newest one is at
/// least this old; kBackupDays daily and kBackupWeeks weekly snapshots are kept.
inline constexpr std::chrono::hours kAutoBackupInterval{24};
inline constexpr int kBackupDays = 7;
inline constexpr int kBackupWeeks = 4;

/// One full-text search hit (Phase 8): the indexed record and its bm25 score (lower is
/// better). Resolve it against the session's Workspace (application::search does).
struct SearchHit {
    enum class Kind : std::uint8_t {
        PageTitle,
        TextBox,
        Task
    };
    Kind kind = Kind::PageTitle;
    core::Uuid owner; ///< page, text box element or task
    double score = 0.0;
};

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
    /// True if `root` contains a StudyBoard workspace (its database), without opening it.
    [[nodiscard]] static bool isWorkspace(const std::filesystem::path& root);

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

    /// Called with every patch applied to the workspace — by execute(), undo() (the inverse
    /// patch) and redo() — right after the in-memory change and before it is written, so
    /// views (the canvas) stay in step with the document. One listener; empty to remove.
    using PatchListener = std::function<void(const document::Patch&)>;
    void setPatchListener(PatchListener listener);

    /// Writes all pending patches ("Save"). Returns the first write error, if any.
    [[nodiscard]] core::Result<void> flush();
    [[nodiscard]] std::size_t pendingWriteCount() const noexcept;
    [[nodiscard]] const std::optional<core::Error>& lastWriteError() const noexcept;

    /// Imports an image/PDF into the workspace's content-addressed asset store.
    [[nodiscard]] core::Result<core::AssetId> importAsset(const std::filesystem::path& source,
                                                          std::string_view mediaType);
    [[nodiscard]] core::Result<std::filesystem::path> assetPath(core::AssetId asset);
    /// importAsset() in two parts, so the expensive part runs off the GUI thread (Phase 9):
    /// the returned job copies and hashes `source` into this workspace's temporary
    /// directory — file I/O only, it uses no session state and may run on any thread, also
    /// after the session is gone (the staged file is then left for temporary-directory
    /// cleanup). finishAssetImport() then stores it (deduplicated) on the session's thread.
    [[nodiscard]] std::function<core::Result<StagedAssetFile>()>
    prepareAssetImport(const std::filesystem::path& source);
    [[nodiscard]] core::Result<core::AssetId> finishAssetImport(const StagedAssetFile& staged,
                                                                std::string_view mediaType);
    /// Removes a staged file that will not be stored.
    static void discardStagedAsset(const StagedAssetFile& staged) noexcept;

    // ---- maintenance (Phase 9) --------------------------------------------------------------
    /// Writes a snapshot of the database to backups/ now (after flushing) and rotates old
    /// ones; returns the snapshot's path. Errors: Unsupported (read-only or closed), IoError.
    [[nodiscard]] core::Result<std::filesystem::path> backUpNow();
    /// Checks the database (`PRAGMA integrity_check`) and every asset file (exists, size,
    /// SHA-256). Only reads.
    [[nodiscard]] core::Result<IntegrityReport> checkIntegrity();

    // ---- bundles (Phase 8; docs/DATABASE_SCHEMA.md §12) ------------------------------------
    /// Writes a bundle (a zip archive) of the whole workspace — a consistent snapshot of the
    /// database after flushing, and every asset — or, with `notebook`, of that notebook with
    /// the tags and assets it uses. Only reads the workspace (also when it is read-only);
    /// `target` must lie outside the workspace directory and is replaced only when complete.
    [[nodiscard]] core::Result<void>
    exportBundle(const std::filesystem::path& target,
                 std::optional<core::NotebookId> notebook = std::nullopt);
    /// Copies every notebook of a bundle (workspace or notebook bundle) into this workspace
    /// as one undoable command ("Import notebook"), with the assets and tags they use;
    /// conflicting ids are remapped (document::commands::importNotebooks). The bundle is
    /// checked before anything is changed (persistence::extractBundle). Returns the new
    /// notebooks. Errors: those of extractBundle, InvalidArgument (no notebook in it). Assets
    /// are imported before the command; if it then fails they stay unreferenced until asset
    /// garbage collection removes them (as after an undone image insertion).
    [[nodiscard]] core::Result<std::vector<core::NotebookId>>
    importBundle(const std::filesystem::path& bundle);
    /// True if `path` lies inside this workspace's directory — or cannot be resolved, so
    /// that callers refuse to write there (exports and bundles never write into it).
    [[nodiscard]] bool isInsideWorkspace(const std::filesystem::path& path) const;
    /// Unpacks a bundle as a new workspace directory `root` (missing or empty), checked as by
    /// persistence::extractBundle; open() it afterwards.
    [[nodiscard]] static core::Result<void> extractBundle(const std::filesystem::path& bundle,
                                                          const std::filesystem::path& root);

    /// Full-text search over page titles, text boxes and tasks (docs/DATABASE_SCHEMA.md §6):
    /// up to `limit` hits, best first. The index is kept in step with every written patch;
    /// a workspace opened read-write whose index is missing or outdated is re-indexed once
    /// when opened. A read-only session searches the index as it is on disk.
    /// False for a workspace from before Phase 8 opened read-only: its index is built the
    /// first time it is opened for editing, and until then search finds nothing.
    [[nodiscard]] bool isSearchIndexed();
    [[nodiscard]] core::Result<std::vector<SearchHit>> search(std::string_view text,
                                                              std::size_t limit);

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
