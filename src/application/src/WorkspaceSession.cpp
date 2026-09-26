#include <studyapp/application/WorkspaceSession.hpp>

#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/document/Editor.hpp>
#include <studyapp/persistence/AssetStore.hpp>
#include <studyapp/persistence/WorkspaceFile.hpp>
#include <studyapp/persistence/WorkspaceStore.hpp>

#include <deque>
#include <system_error>
#include <utility>
#include <variant>

namespace studyapp::application {

using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

constexpr std::string_view kLogCategory = "persistence";

template <class T>
tl::unexpected<core::Error> forward(Result<T>& failed) {
    return tl::unexpected<core::Error>(std::move(failed.error()));
}

std::string describe(const std::optional<LockOwner>& owner) {
    if (!owner) {
        return "an unknown process";
    }
    return (owner->applicationName.empty() ? std::string("process") : owner->applicationName) +
           " (pid " + std::to_string(owner->processId) + " on " +
           (owner->hostName.empty() ? std::string("an unknown host") : owner->hostName) + ")";
}

Result<std::unique_ptr<WorkspaceLock>> acquireLock(WorkspaceLocker& locker,
                                                   const std::filesystem::path& lockFile,
                                                   bool recoverStale, bool& recovered) {
    auto status = locker.inspect(lockFile);
    if (!status) {
        return forward(status);
    }
    switch (status->state) {
    case LockState::Active:
        return makeError(ErrorCode::Conflict, "the workspace is open in " +
                                                  describe(status->owner) +
                                                  "; it can be opened read-only");
    case LockState::Stale:
        if (!recoverStale) {
            return makeError(ErrorCode::Conflict, "the workspace was not closed properly by " +
                                                      describe(status->owner) +
                                                      "; open it with recovery or read-only");
        }
        recovered = true;
        return locker.acquire(lockFile, true);
    case LockState::Free:
        break;
    }
    return locker.acquire(lockFile, false);
}

} // namespace

// ---------------------------------------------------------------------------- Impl

struct WorkspaceSession::Impl {
    Impl(std::filesystem::path rootPath, const SessionServices& services,
         std::unique_ptr<WorkspaceLock> heldLock, persistence::WorkspaceFile openFile,
         document::Workspace loaded, bool recoveredLock)
        : root(std::move(rootPath)), clock(&services.clock), ids(&services.ids),
          lock(std::move(heldLock)), file(std::move(openFile)), store(file.database(), *clock),
          assets(file.database(), root), workspace(std::move(loaded)), editor(workspace),
          recovered(recoveredLock) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() = default;

    Result<void> checkWritable() const {
        if (closed) {
            return makeError(ErrorCode::Unsupported, "the workspace session is closed");
        }
        if (file.isReadOnly()) {
            return makeError(ErrorCode::Unsupported, "the workspace is open read-only");
        }
        return {};
    }

    /// Image elements must reference imported assets (the document model only requires a
    /// non-null id; asset existence is a persistence concern, DATA_MODEL.md §4.4).
    Result<void> checkAssetReferences(const document::Patch& patch) {
        for (const auto& change : patch.changes()) {
            const auto* elementChange = std::get_if<document::ElementChange>(&change);
            if (elementChange == nullptr || !elementChange->after) {
                continue;
            }
            const auto* image = std::get_if<document::Image>(&elementChange->after->payload);
            if (image == nullptr) {
                continue;
            }
            auto exists = assets.exists(image->asset);
            if (!exists) {
                return forward(exists);
            }
            if (!*exists) {
                return makeError(ErrorCode::NotFound, "image asset " + image->asset.toString() +
                                                          " has not been imported into the "
                                                          "workspace");
            }
        }
        return {};
    }

    /// Writes queued patches in order; stops at the first failure and keeps the rest.
    Result<void> writePending() {
        while (!pending.empty()) {
            if (auto written = store.write(pending.front()); !written) {
                lastError = written.error();
                core::logError(kLogCategory, "saving changes failed (" +
                                                 std::to_string(pending.size()) +
                                                 " pending): " + written.error().message);
                return written;
            }
            pending.pop_front();
        }
        lastError.reset();
        return {};
    }

    void persist(document::Patch patch) {
        if (patch.empty()) {
            return;
        }
        pending.push_back(patch); // queued first: persistence order is application order
        if (listener) {
            listener(patch);
        }
        // A failure is recorded in lastError and retried later; the edit itself stands.
        (void)writePending();
    }

    std::filesystem::path root;
    const core::Clock* clock;
    core::IdGenerator* ids;
    // Declared before the file so it is released only after the database is closed.
    std::unique_ptr<WorkspaceLock> lock;
    persistence::WorkspaceFile file;
    persistence::WorkspaceStore store;
    persistence::AssetStore assets;
    document::Workspace workspace;
    document::Editor editor;
    std::deque<document::Patch> pending;
    std::optional<core::Error> lastError;
    PatchListener listener;
    bool recovered = false;
    bool closed = false;
};

// ---------------------------------------------------------------------------- lifecycle

Result<std::unique_ptr<WorkspaceSession>>
WorkspaceSession::create(const std::filesystem::path& root, std::string name,
                         SessionServices services) {
    if (auto valid = document::validateName(name, "workspace name"); !valid) {
        return forward(valid);
    }
    const persistence::WorkspaceLayout layout{root};
    std::error_code ec;
    const bool createdRoot = !std::filesystem::exists(root, ec);
    // Checked before taking the lock, so a refused directory is left exactly as it was.
    if (auto creatable = persistence::WorkspaceFile::checkCanCreate(root); !creatable) {
        return forward(creatable);
    }
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return makeError(ErrorCode::IoError,
                         "cannot create the workspace directory: " + ec.message());
    }
    const auto cleanUp = [&](std::unique_ptr<WorkspaceLock>& lock) {
        lock.reset(); // release before removing (Windows cannot delete an open lock file)
        std::error_code ignored;
        if (createdRoot) {
            std::filesystem::remove_all(root, ignored);
        } else {
            std::filesystem::remove(layout.lockFile(), ignored);
        }
    };

    auto lock = services.locker.acquire(layout.lockFile(), false);
    if (!lock) {
        std::unique_ptr<WorkspaceLock> none;
        cleanUp(none);
        return forward(lock);
    }
    document::WorkspaceInfo info{.id = core::WorkspaceId::generate(services.ids),
                                 .name = std::move(name),
                                 .created = services.clock.now()};
    auto file = persistence::WorkspaceFile::create(root, info, core::build::kVersion);
    if (!file) {
        cleanUp(*lock);
        return forward(file);
    }
    auto impl = std::make_unique<Impl>(root, services, std::move(*lock), std::move(*file),
                                       document::Workspace(std::move(info)), false);
    return std::unique_ptr<WorkspaceSession>(new WorkspaceSession(std::move(impl)));
}

Result<std::unique_ptr<WorkspaceSession>> WorkspaceSession::open(const std::filesystem::path& root,
                                                                 OpenOptions options,
                                                                 SessionServices services) {
    const persistence::WorkspaceLayout layout{root};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(layout.database(), ec)) {
        return makeError(ErrorCode::NotFound, "the directory is not a StudyBoard workspace");
    }
    const bool readOnly = options.mode == AccessMode::ReadOnly;

    std::unique_ptr<WorkspaceLock> lock;
    bool recovered = false;
    if (!readOnly) {
        auto acquired =
            acquireLock(services.locker, layout.lockFile(), options.recoverStaleLock, recovered);
        if (!acquired) {
            return forward(acquired);
        }
        lock = std::move(*acquired);
    }

    auto file = persistence::WorkspaceFile::open(
        root, readOnly ? persistence::AccessMode::ReadOnly : persistence::AccessMode::ReadWrite,
        services.clock.now(), core::build::kVersion);
    if (!file) {
        return forward(file);
    }
    if (recovered) {
        core::logWarning(kLogCategory, "recovering a workspace that was not closed properly");
        if (auto checked = file->integrityCheck(); !checked) {
            return forward(checked);
        }
    }
    if (!readOnly) {
        // Safe only while holding the lock (docs/DATABASE_SCHEMA.md §9).
        if (auto cleaned = file->cleanTemporary(); !cleaned) {
            return forward(cleaned);
        }
    }

    auto loaded = persistence::WorkspaceStore(file->database(), services.clock).load();
    if (!loaded) {
        return forward(loaded);
    }
    auto impl = std::make_unique<Impl>(root, services, std::move(lock), std::move(*file),
                                       std::move(*loaded), recovered);
    return std::unique_ptr<WorkspaceSession>(new WorkspaceSession(std::move(impl)));
}

Result<LockStatus> WorkspaceSession::inspectLock(const std::filesystem::path& root,
                                                 WorkspaceLocker& locker) {
    return locker.inspect(persistence::WorkspaceLayout{root}.lockFile());
}

bool WorkspaceSession::isWorkspace(const std::filesystem::path& root) {
    std::error_code ec;
    return std::filesystem::is_regular_file(persistence::WorkspaceLayout{root}.database(), ec);
}

WorkspaceSession::WorkspaceSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WorkspaceSession::~WorkspaceSession() {
    if (impl_ && !impl_->closed) {
        if (auto flushed = impl_->writePending(); !flushed) {
            core::logError(kLogCategory, "closing the workspace with " +
                                             std::to_string(impl_->pending.size()) +
                                             " unsaved change(s): " + flushed.error().message);
        }
    }
}

Result<void> WorkspaceSession::close() {
    if (impl_->closed) {
        return {};
    }
    if (auto flushed = impl_->writePending(); !flushed) {
        return flushed;
    }
    if (auto closedFile = impl_->file.close(); !closedFile) {
        return closedFile;
    }
    impl_->lock.reset();
    impl_->closed = true;
    return {};
}

bool WorkspaceSession::isClosed() const noexcept {
    return impl_->closed;
}

// ---------------------------------------------------------------------------- queries

const document::Workspace& WorkspaceSession::workspace() const noexcept {
    return impl_->workspace;
}
const document::UndoStack& WorkspaceSession::history() const noexcept {
    return impl_->editor.history();
}
const std::filesystem::path& WorkspaceSession::root() const noexcept {
    return impl_->root;
}
bool WorkspaceSession::isReadOnly() const noexcept {
    return impl_->file.isReadOnly();
}
bool WorkspaceSession::recoveredStaleLock() const noexcept {
    return impl_->recovered;
}
bool WorkspaceSession::canUndo() const noexcept {
    return impl_->editor.canUndo();
}
bool WorkspaceSession::canRedo() const noexcept {
    return impl_->editor.canRedo();
}
void WorkspaceSession::setPatchListener(PatchListener listener) {
    impl_->listener = std::move(listener);
}
std::size_t WorkspaceSession::pendingWriteCount() const noexcept {
    return impl_->pending.size();
}
const std::optional<core::Error>& WorkspaceSession::lastWriteError() const noexcept {
    return impl_->lastError;
}

// ---------------------------------------------------------------------------- editing

Result<void> WorkspaceSession::execute(document::Command command) {
    if (auto writable = impl_->checkWritable(); !writable) {
        return writable;
    }
    if (auto assets = impl_->checkAssetReferences(command.patch); !assets) {
        return assets;
    }
    document::Patch applied = command.patch;
    if (auto executed = impl_->editor.execute(std::move(command)); !executed) {
        return executed;
    }
    impl_->persist(std::move(applied));
    return {};
}

Result<void> WorkspaceSession::undo() {
    if (auto writable = impl_->checkWritable(); !writable) {
        return writable;
    }
    const document::Command* entry = impl_->editor.history().nextUndo();
    document::Patch applied = entry != nullptr ? entry->patch.inverted() : document::Patch{};
    if (auto undone = impl_->editor.undo(); !undone) {
        return undone;
    }
    impl_->persist(std::move(applied));
    return {};
}

Result<void> WorkspaceSession::redo() {
    if (auto writable = impl_->checkWritable(); !writable) {
        return writable;
    }
    const document::Command* entry = impl_->editor.history().nextRedo();
    document::Patch applied = entry != nullptr ? entry->patch : document::Patch{};
    if (auto assets = impl_->checkAssetReferences(applied); !assets) {
        return assets;
    }
    if (auto redone = impl_->editor.redo(); !redone) {
        return redone;
    }
    impl_->persist(std::move(applied));
    return {};
}

Result<void> WorkspaceSession::flush() {
    if (impl_->closed) {
        return {};
    }
    return impl_->writePending();
}

// ---------------------------------------------------------------------------- assets

Result<core::AssetId> WorkspaceSession::importAsset(const std::filesystem::path& source,
                                                    std::string_view mediaType) {
    if (auto writable = impl_->checkWritable(); !writable) {
        return forward(writable);
    }
    return impl_->assets.import(source, mediaType, *impl_->ids, *impl_->clock);
}

Result<std::filesystem::path> WorkspaceSession::assetPath(core::AssetId asset) {
    if (impl_->closed) {
        return makeError(ErrorCode::Unsupported, "the workspace session is closed");
    }
    return impl_->assets.pathOf(asset);
}

} // namespace studyapp::application
