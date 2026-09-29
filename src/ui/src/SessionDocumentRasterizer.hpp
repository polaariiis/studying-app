#pragma once

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/canvas/DocumentRasterizer.hpp>
#include <studyapp/core/Error.hpp>

#include <QObject>
#include <QThreadPool>

#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace studyapp::ui {

// PDF documents through QtPdf (PDFium) — Phase 8, docs/CANVAS.md §10. A PDF is untrusted
// input: it is only ever read, page counts and sizes are bounded, and a page that cannot be
// rendered shows as blank paper.

/// Most pages an imported PDF may have.
inline constexpr int kMaxPdfPages = 5000;
/// Largest page side accepted, in points (PDF's own limit: 14 400 pt = 200 in).
inline constexpr double kMaxPdfPagePoints = 14400.0;

struct PdfInfo {
    std::vector<core::DVec2> pageSizes; ///< world units (1/96 in), one per page
};

/// Reads a PDF's page count and page sizes. Errors: IoError (unreadable, not a PDF, corrupt),
/// Unsupported (password-protected), InvalidArgument (no pages, too many, a page of no
/// valid size or larger than kMaxPdfPagePoints).
[[nodiscard]] core::Result<PdfInfo> inspectPdf(const std::filesystem::path& file);

/// canvas::DocumentRasterizer over the open workspace's PDF assets. Tiles are rendered on one
/// worker thread (PDFium is not reentrant; QtPdf serializes it anyway) in request order, so
/// the page preview the canvas asks for first comes first; requests the canvas no longer
/// wants (keepOnly) are dropped before they are rendered. A few documents stay open on the
/// worker; the asset files are never written.
class SessionDocumentRasterizer final : public canvas::DocumentRasterizer {
public:
    /// Rendered tiles the canvas has not collected yet are kept up to this many bytes (and
    /// kUncollectedTiles tiles), oldest dropped first.
    static constexpr std::size_t kUncollectedBudgetBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kUncollectedTiles = 256;
    /// Documents kept open on the worker, least recently used closed first.
    static constexpr std::size_t kOpenDocuments = 4;

    explicit SessionDocumentRasterizer(application::WorkspaceSession& session);
    ~SessionDocumentRasterizer() override;
    SessionDocumentRasterizer(const SessionDocumentRasterizer&) = delete;
    SessionDocumentRasterizer& operator=(const SessionDocumentRasterizer&) = delete;
    SessionDocumentRasterizer(SessionDocumentRasterizer&&) = delete;
    SessionDocumentRasterizer& operator=(SessionDocumentRasterizer&&) = delete;

    [[nodiscard]] std::optional<render::ImageData>
    tile(const canvas::DocumentTileKey& key) override;
    void keepOnly(std::span<const canvas::DocumentTileKey> wanted) override;

    /// Blocks until queued tiles are rendered (tests).
    void waitForTiles();
    /// Bytes of rendered tiles not collected yet (tests).
    [[nodiscard]] std::size_t uncollectedBytes() const;
    /// Tiles queued or being rendered (tests).
    [[nodiscard]] std::size_t queuedTiles() const;
    /// Tiles rendered since the start (tests).
    [[nodiscard]] std::size_t renderedTiles() const;

private:
    using Key = canvas::DocumentTileKey;
    using KeyHash = canvas::DocumentTileKeyHash;
    struct Worker; ///< documents open on the worker thread
    /// Shared with the worker job, which may outlive a call but not the rasterizer.
    struct Shared {
        mutable std::mutex mutex;
        std::deque<std::pair<Key, QString>> queue; ///< oldest request first
        std::unordered_set<Key, KeyHash> pending;  ///< queued or being rendered
        std::unordered_map<Key, render::ImageData, KeyHash> done;
        std::deque<Key> doneOrder; ///< oldest first; may name keys already collected
        std::size_t doneBytes = 0;
        std::size_t rendered = 0;
        bool draining = false; ///< a worker job is running
        bool stopping = false;
    };

    void drain(); ///< worker job: renders queued tiles until none are left

    application::WorkspaceSession* session_;
    std::unordered_map<core::AssetId, QString> files_; ///< GUI thread: asset → file
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    std::unique_ptr<Worker> worker_; ///< touched by the worker job only
    QObject context_;                ///< delivers "rendered" on the GUI thread
    QThreadPool pool_;
};

} // namespace studyapp::ui
