#include <studyapp/application/WorkspaceSession.hpp>

#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/document/Editor.hpp>
#include <studyapp/persistence/AssetStore.hpp>
#include <studyapp/persistence/Backups.hpp>
#include <studyapp/persistence/Bundle.hpp>
#include <studyapp/persistence/Migrations.hpp>
#include <studyapp/persistence/WorkspaceFile.hpp>
#include <studyapp/persistence/WorkspaceLayout.hpp>
#include <studyapp/persistence/WorkspaceStore.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <random>
#include <system_error>
#include <unordered_map>
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
            std::optional<core::AssetId> asset;
            if (const auto* pageChange = std::get_if<document::PageChange>(&change);
                pageChange != nullptr && pageChange->after && pageChange->after->document) {
                asset = pageChange->after->document->asset; // a PDF page (Phase 8)
            }
            const auto* elementChange = std::get_if<document::ElementChange>(&change);
            if (elementChange != nullptr && elementChange->after) {
                if (const auto* image =
                        std::get_if<document::Image>(&elementChange->after->payload)) {
                    asset = image->asset;
                }
            }
            if (!asset) {
                continue;
            }
            auto exists = assets.exists(*asset);
            if (!exists) {
                return forward(exists);
            }
            if (!*exists) {
                return makeError(ErrorCode::NotFound, "asset " + asset->toString() +
                                                          " has not been imported into the "
                                                          "workspace");
            }
        }
        return {};
    }

    /// Re-indexes a workspace whose search index is missing or from another version (D28).
    /// Failures are logged: the workspace stays usable, search may be incomplete.
    void ensureSearchIndex() {
        if (file.isReadOnly()) {
            return;
        }
        auto current = store.search().isCurrent();
        if (current && *current) {
            return;
        }
        auto transaction = persistence::Transaction::begin(
            file.database(), persistence::Transaction::Kind::Immediate);
        Result<void> rebuilt = transaction ? store.search().rebuild(*transaction)
                                           : Result<void>{tl::unexpected(transaction.error())};
        if (rebuilt && transaction) {
            rebuilt = transaction->commit();
        }
        if (!rebuilt) {
            core::logError(kLogCategory,
                           "building the search index failed: " + rebuilt.error().message);
        }
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

    /// Snapshot + rotation. Old snapshots that cannot be removed are logged, not errors.
    Result<std::filesystem::path> backUp() {
        if (auto flushed = writePending(); !flushed) {
            return forward(flushed);
        }
        // Snapshot names carry the time to the millisecond; a second one within the same
        // millisecond takes the next free name.
        auto target = file.backup(persistence::kAutoBackupLabel, clock->now());
        for (int step = 1;
             !target && target.error().code == ErrorCode::AlreadyExists && step < 1000; ++step) {
            target = file.backup(persistence::kAutoBackupLabel,
                                 clock->now() + std::chrono::milliseconds{step});
        }
        if (!target) {
            return forward(target);
        }
        if (auto backups = persistence::listBackups(file.layout(), persistence::kAutoBackupLabel)) {
            for (const auto& old :
                 persistence::backupsToRemove(*backups, kBackupDays, kBackupWeeks)) {
                std::error_code ec;
                if (!std::filesystem::remove(old, ec) && ec) {
                    core::logWarning(kLogCategory, "cannot remove an old backup: " + ec.message());
                }
            }
        }
        return target;
    }

    /// The automatic backup on close: only after changes, at most once per interval.
    void backUpIfDue() {
        if (!changed || file.isReadOnly()) {
            return;
        }
        auto backups = persistence::listBackups(file.layout(), persistence::kAutoBackupLabel);
        if (backups && !backups->empty() &&
            clock->now() - backups->back().time < kAutoBackupInterval) {
            return;
        }
        if (auto made = backUp(); !made) {
            core::logWarning(kLogCategory, "automatic backup failed: " + made.error().message);
        }
    }

    void persist(document::Patch patch) {
        changed = true;
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
    bool changed = false; ///< an edit, undo or redo happened in this session
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
    impl->ensureSearchIndex(); // empty: only records the index version
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
    impl->ensureSearchIndex();
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
    impl_->backUpIfDue(); // never blocks closing: failures are logged
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

Result<std::filesystem::path> WorkspaceSession::backUpNow() {
    if (auto writable = impl_->checkWritable(); !writable) {
        return forward(writable);
    }
    return impl_->backUp();
}

Result<IntegrityReport> WorkspaceSession::checkIntegrity() {
    if (impl_->closed) {
        return makeError(ErrorCode::Unsupported, "the workspace session is closed");
    }
    IntegrityReport report;
    auto check = impl_->file.database().prepare("PRAGMA integrity_check");
    if (!check) {
        return forward(check);
    }
    for (;;) {
        auto row = check->step();
        if (!row) {
            return forward(row);
        }
        if (!*row) {
            break;
        }
        if (const std::string line(check->columnText(0)); line != "ok") {
            report.problems.push_back("Database: " + line);
        }
    }
    auto assets = impl_->assets.verify();
    if (!assets) {
        return forward(assets);
    }
    for (const persistence::AssetProblem& problem : *assets) {
        const char* what = problem.kind == persistence::AssetProblem::Kind::MissingFile ? "missing"
                           : problem.kind == persistence::AssetProblem::Kind::WrongSize
                               ? "has the wrong size"
                               : "is damaged (its content does not match its hash)";
        report.problems.push_back("Asset " + problem.asset.toString() + " " + what);
    }
    return report;
}

bool WorkspaceSession::isSearchIndexed() {
    return !impl_->closed && impl_->store.search().isCurrent().value_or(false);
}

std::function<Result<StagedAssetFile>()>
WorkspaceSession::prepareAssetImport(const std::filesystem::path& source) {
    const core::AssetId id = core::AssetId::generate(*impl_->ids); // ids: this thread only
    return [root = impl_->root, source, id]() -> Result<StagedAssetFile> {
        auto staged = persistence::AssetStore::stage(root, source, id.toString());
        if (!staged) {
            return forward(staged);
        }
        return StagedAssetFile{.id = id,
                               .file = staged->file,
                               .sha256 = staged->sha256,
                               .byteSize = staged->byteSize,
                               .originalName = staged->originalName};
    };
}

namespace {

persistence::StagedAsset toPersistence(const StagedAssetFile& staged) {
    return {.file = staged.file,
            .sha256 = staged.sha256,
            .byteSize = staged.byteSize,
            .originalName = staged.originalName};
}

} // namespace

Result<core::AssetId> WorkspaceSession::finishAssetImport(const StagedAssetFile& staged,
                                                          std::string_view mediaType) {
    if (auto writable = impl_->checkWritable(); !writable) {
        discardStagedAsset(staged);
        return forward(writable);
    }
    return impl_->assets.commit(toPersistence(staged), mediaType, staged.id, *impl_->clock);
}

void WorkspaceSession::discardStagedAsset(const StagedAssetFile& staged) noexcept {
    persistence::AssetStore::discard(toPersistence(staged));
}

Result<std::vector<SearchHit>> WorkspaceSession::search(std::string_view text, std::size_t limit) {
    if (impl_->closed) {
        return makeError(ErrorCode::Unsupported, "the workspace session is closed");
    }
    auto hits = impl_->store.search().query(text, limit);
    if (!hits) {
        return forward(hits);
    }
    std::vector<SearchHit> out;
    out.reserve(hits->size());
    for (const persistence::SearchHit& hit : *hits) {
        const auto kind = hit.kind == persistence::SearchKind::PageTitle
                              ? SearchHit::Kind::PageTitle
                          : hit.kind == persistence::SearchKind::TextBox ? SearchHit::Kind::TextBox
                                                                         : SearchHit::Kind::Task;
        out.push_back({.kind = kind, .owner = hit.owner, .score = hit.score});
    }
    return out;
}

Result<std::filesystem::path> WorkspaceSession::assetPath(core::AssetId asset) {
    if (impl_->closed) {
        return makeError(ErrorCode::Unsupported, "the workspace session is closed");
    }
    return impl_->assets.pathOf(asset);
}

// ---------------------------------------------------------------------------- bundles

namespace {

/// A private directory in the system's temporary directory, removed with everything in it.
class Staging {
public:
    /// A new directory with a random name (never one that exists: another process or a
    /// test using deterministic ids could own it).
    static Result<Staging> create() {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return makeError(ErrorCode::IoError, "no temporary directory: " + ec.message());
        }
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::uint64_t name = (static_cast<std::uint64_t>(random()) << 32U) ^
                                       static_cast<std::uint64_t>(random());
            std::array<char, 17> hex{};
            std::snprintf(hex.data(), hex.size(), "%016llx", static_cast<unsigned long long>(name));
            Staging staging;
            staging.path_ = base / ("studyboard-" + std::string(hex.data()));
            if (std::filesystem::create_directory(staging.path_, ec) && !ec) {
                return staging;
            }
            staging.path_.clear(); // not ours: never removed
        }
        return makeError(ErrorCode::IoError, "cannot create a temporary directory");
    }
    Staging(Staging&& other) noexcept : path_(std::exchange(other.path_, {})) {}
    Staging& operator=(Staging&&) = delete;
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
    ~Staging() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    Staging() = default;
    std::filesystem::path path_;
};

/// `path` lies inside `directory` (after resolving both as far as they exist); true when
/// either cannot be resolved (the caller then refuses to write).
bool isInside(const std::filesystem::path& path, const std::filesystem::path& directory) {
    std::error_code pathError;
    std::error_code directoryError;
    const auto resolvedPath = std::filesystem::weakly_canonical(path, pathError);
    const auto resolvedDirectory = std::filesystem::weakly_canonical(directory, directoryError);
    if (pathError || directoryError || resolvedPath.empty()) {
        return true;
    }
    const auto relative =
        resolvedPath.lexically_normal().lexically_relative(resolvedDirectory.lexically_normal());
    return relative.empty() || *relative.begin() != "..";
}

/// Assets referenced by the notebook's pages (PDFs) and images, each once.
std::vector<core::AssetId> assetsOf(const document::Workspace& ws, core::NotebookId notebook) {
    std::vector<core::AssetId> assets;
    for (const core::SectionId section : ws.sectionsOf(notebook)) {
        for (const core::PageId page : ws.pagesOf(section)) {
            if (const auto& document = ws.findPage(page)->document) {
                assets.push_back(document->asset);
            }
            for (const core::LayerId layer : ws.layersOf(page)) {
                for (const core::ElementId element : ws.elementsOf(layer)) {
                    if (const auto* image =
                            std::get_if<document::Image>(&ws.findElement(element)->payload)) {
                        assets.push_back(image->asset);
                    }
                }
            }
        }
    }
    std::sort(assets.begin(), assets.end());
    assets.erase(std::unique(assets.begin(), assets.end()), assets.end());
    return assets;
}

Result<std::vector<persistence::BundleAsset>> bundleAssetsOf(persistence::AssetStore& store) {
    auto rows = store.list();
    if (!rows) {
        return forward(rows);
    }
    std::vector<persistence::BundleAsset> assets;
    assets.reserve(rows->size());
    for (const persistence::AssetInfo& row : *rows) {
        const auto relative = persistence::AssetStore::relativePath(row.sha256, row.mediaType);
        assets.push_back({.relative = relative, .file = store.root() / relative});
    }
    return assets;
}

} // namespace

Result<void> WorkspaceSession::saveCopy(const std::filesystem::path& root) {
    if (impl_->closed) {
        return makeError(ErrorCode::Unsupported, "the workspace session is closed");
    }
    if (isInside(root, impl_->root)) {
        return makeError(ErrorCode::InvalidArgument,
                         "a copy cannot be saved inside the workspace directory");
    }
    const document::Workspace& ws = impl_->workspace;
    document::WorkspaceInfo info = ws.info();
    info.id = core::WorkspaceId::generate(*impl_->ids);
    auto file = persistence::WorkspaceFile::create(root, info, core::build::kVersion);
    if (!file) {
        return forward(file);
    }
    // Assets keep their ids (the records reference them); files are copied and re-hashed.
    persistence::AssetStore assets(file->database(), root);
    std::vector<core::AssetId> used;
    for (const core::NotebookId notebook : ws.notebooks()) {
        const auto ofNotebook = assetsOf(ws, notebook);
        used.insert(used.end(), ofNotebook.begin(), ofNotebook.end());
    }
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    for (const core::AssetId asset : used) {
        auto row = impl_->assets.find(asset);
        auto path = impl_->assets.pathOf(asset);
        if (!row || !*row || !path) {
            return makeError(ErrorCode::NotFound, "asset " + asset.toString() + " is missing");
        }
        auto staged = persistence::AssetStore::stage(root, *path, asset.toString());
        if (!staged) {
            return forward(staged);
        }
        if (auto stored = assets.commit(*staged, (*row)->mediaType, asset, *impl_->clock);
            !stored) {
            return forward(stored);
        }
    }
    persistence::WorkspaceStore store(file->database(), *impl_->clock);
    if (auto written = store.write(document::commands::creationPatch(ws)); !written) {
        return written;
    }
    return file->close();
}

bool WorkspaceSession::isInsideWorkspace(const std::filesystem::path& path) const {
    return isInside(path, impl_->root);
}

Result<void> WorkspaceSession::exportBundle(const std::filesystem::path& target,
                                            std::optional<core::NotebookId> notebook) {
    if (impl_->closed) {
        return makeError(ErrorCode::Unsupported, "the workspace session is closed");
    }
    if (isInside(target, impl_->root)) {
        return makeError(ErrorCode::InvalidArgument,
                         "a bundle cannot be written inside the workspace directory");
    }
    if (notebook && impl_->workspace.findNotebook(*notebook) == nullptr) {
        return makeError(ErrorCode::NotFound, "the notebook does not exist");
    }
    if (!impl_->file.isReadOnly()) {
        if (auto flushed = impl_->writePending(); !flushed) {
            return makeError(flushed.error().code,
                             "unsaved changes cannot be exported: " + flushed.error().message);
        }
    }
    auto staging = Staging::create();
    if (!staging) {
        return forward(staging);
    }
    const int schema = persistence::currentSchemaVersion();
    if (!notebook) {
        const auto snapshot = staging->path() / "workspace.db";
        if (auto copied = persistence::backupDatabase(impl_->file.database(), snapshot); !copied) {
            return copied;
        }
        auto assets = bundleAssetsOf(impl_->assets);
        if (!assets) {
            return forward(assets);
        }
        return persistence::writeBundle(target, persistence::BundleKind::Workspace, schema,
                                        snapshot, *assets);
    }

    // A notebook: copied into a new workspace of its own (through the ordinary command and
    // persistence paths), which is then bundled like a whole workspace.
    const document::Workspace& ws = impl_->workspace;
    const auto root = staging->path() / "workspace";
    document::WorkspaceInfo info{.id = core::WorkspaceId::generate(*impl_->ids),
                                 .name = ws.findNotebook(*notebook)->title,
                                 .created = impl_->clock->now()};
    auto file = persistence::WorkspaceFile::create(root, info, core::build::kVersion);
    if (!file) {
        return forward(file);
    }
    persistence::AssetStore assets(file->database(), root);
    std::unordered_map<core::AssetId, core::AssetId> copies;
    for (const core::AssetId asset : assetsOf(ws, *notebook)) {
        auto row = impl_->assets.find(asset);
        auto path = impl_->assets.pathOf(asset);
        if (!row || !*row || !path) {
            return makeError(ErrorCode::NotFound,
                             "asset " + asset.toString() + " of the notebook is missing");
        }
        auto copy = assets.import(*path, (*row)->mediaType, *impl_->ids, *impl_->clock);
        if (!copy) {
            return forward(copy);
        }
        copies.emplace(asset, *copy);
    }
    document::Workspace empty(info);
    const std::vector<core::NotebookId> which{*notebook};
    auto copied = document::commands::importNotebooks(empty, ws, which, copies, *impl_->ids);
    if (!copied) {
        return forward(copied);
    }
    persistence::WorkspaceStore store(file->database(), *impl_->clock);
    if (auto written = store.write(copied->command.patch); !written) {
        return written;
    }
    auto bundleAssets = bundleAssetsOf(assets);
    if (!bundleAssets) {
        return forward(bundleAssets);
    }
    if (auto closed = file->close(); !closed) {
        return closed;
    }
    return persistence::writeBundle(target, persistence::BundleKind::Notebook, schema,
                                    persistence::WorkspaceLayout{root}.database(), *bundleAssets);
}

Result<std::vector<core::NotebookId>>
WorkspaceSession::importBundle(const std::filesystem::path& bundle) {
    if (auto writable = impl_->checkWritable(); !writable) {
        return forward(writable);
    }
    auto staging = Staging::create();
    if (!staging) {
        return forward(staging);
    }
    const auto root = staging->path() / "bundle";
    if (auto extracted = persistence::extractBundle(bundle, root); !extracted) {
        return forward(extracted);
    }
    auto file = persistence::WorkspaceFile::open(root, persistence::AccessMode::ReadOnly,
                                                 impl_->clock->now(), core::build::kVersion);
    if (!file) {
        return forward(file);
    }
    auto source = persistence::WorkspaceStore(file->database(), *impl_->clock).load();
    if (!source) {
        return forward(source);
    }
    const auto notebooks = source->notebooks();
    if (notebooks.empty()) {
        return makeError(ErrorCode::InvalidArgument, "the bundle holds no notebook");
    }
    // The assets they use, imported first (content-addressed: known content is reused).
    persistence::AssetStore bundleAssets(file->database(), root);
    std::unordered_map<core::AssetId, core::AssetId> assets;
    for (const core::NotebookId notebook : notebooks) {
        for (const core::AssetId asset : assetsOf(*source, notebook)) {
            if (assets.contains(asset)) {
                continue;
            }
            auto row = bundleAssets.find(asset);
            auto path = bundleAssets.pathOf(asset);
            if (!row || !*row || !path) {
                return makeError(ErrorCode::ParseError, "not a valid StudyBoard bundle: asset " +
                                                            asset.toString() + " is missing");
            }
            auto imported =
                impl_->assets.import(*path, (*row)->mediaType, *impl_->ids, *impl_->clock);
            if (!imported) {
                return forward(imported);
            }
            assets.emplace(asset, *imported);
        }
    }
    const std::vector<core::NotebookId> which(notebooks.begin(), notebooks.end());
    auto imported =
        document::commands::importNotebooks(impl_->workspace, *source, which, assets, *impl_->ids);
    if (!imported) {
        return forward(imported);
    }
    if (auto executed = execute(std::move(imported->command)); !executed) {
        return forward(executed);
    }
    (void)file->close();
    return std::move(imported->id);
}

Result<void> WorkspaceSession::extractBundle(const std::filesystem::path& bundle,
                                             const std::filesystem::path& root) {
    auto extracted = persistence::extractBundle(bundle, root);
    if (!extracted) {
        return forward(extracted);
    }
    return {};
}

} // namespace studyapp::application
