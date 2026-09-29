#include "SessionDocumentRasterizer.hpp"

#include <studyapp/document/Records.hpp>

#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QPointer>
#include <QRect>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>

namespace studyapp::ui {

namespace {

using core::ErrorCode;
using core::makeError;

QString toQString(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// Largest rendered page side in pixels (a tile is cut from a page of this size at most).
constexpr double kMaxScaledSide = 1 << 20;

render::ImageData renderTile(QPdfDocument& pdf, const canvas::DocumentTileKey& key) {
    render::ImageData pixels;
    if (key.page < 0 || key.page >= pdf.pageCount() || key.column < 0 || key.row < 0) {
        return pixels;
    }
    const QSizeF points = pdf.pagePointSize(key.page);
    const double scale = document::kUnitsPerPoint * canvas::documentScale(key.level);
    const double width = std::ceil(points.width() * scale);
    const double height = std::ceil(points.height() * scale);
    if (!(width >= 1.0 && height >= 1.0 && width <= kMaxScaledSide && height <= kMaxScaledSide)) {
        return pixels;
    }
    const QSize page(static_cast<int>(width), static_cast<int>(height));
    const QRect clip =
        QRect(key.column * canvas::kDocumentTilePx, key.row * canvas::kDocumentTilePx,
              canvas::kDocumentTilePx, canvas::kDocumentTilePx) &
        QRect(QPoint(0, 0), page);
    if (clip.isEmpty()) {
        return pixels;
    }
    QPdfDocumentRenderOptions options;
    options.setScaledSize(page);
    options.setScaledClipRect(clip);
    QImage image = pdf.render(key.page, clip.size(), options);
    if (image.isNull()) {
        return pixels;
    }
    // PDF pages are white paper; PDFium leaves unpainted areas transparent. Opaque tiles also
    // hide the coarse preview drawn under them (it would show through as a halo).
    QImage paper(image.size(), QImage::Format_RGBA8888_Premultiplied);
    paper.fill(Qt::white);
    {
        QPainter painter(&paper);
        painter.drawImage(0, 0, image);
    }
    image = std::move(paper);
    pixels.width = image.width();
    pixels.height = image.height();
    const auto rowBytes = static_cast<std::size_t>(pixels.width) * 4U;
    pixels.pixels.resize(rowBytes * static_cast<std::size_t>(pixels.height));
    for (int y = 0; y < pixels.height; ++y) {
        std::memcpy(pixels.pixels.data() + static_cast<std::size_t>(y) * rowBytes,
                    image.constScanLine(y), rowBytes);
    }
    return pixels;
}

} // namespace

core::Result<PdfInfo> inspectPdf(const std::filesystem::path& file) {
    QPdfDocument pdf;
    switch (pdf.load(toQString(file))) {
    case QPdfDocument::Error::None:
        break;
    case QPdfDocument::Error::IncorrectPassword:
        return makeError(ErrorCode::Unsupported, "password-protected PDFs are not supported");
    case QPdfDocument::Error::UnsupportedSecurityScheme:
        return makeError(ErrorCode::Unsupported, "the PDF's security scheme is not supported");
    case QPdfDocument::Error::FileNotFound:
        return makeError(ErrorCode::IoError, "the file could not be opened");
    default:
        return makeError(ErrorCode::IoError, "the file is not a readable PDF document");
    }
    const int count = pdf.pageCount();
    if (count <= 0) {
        return makeError(ErrorCode::InvalidArgument, "the PDF has no pages");
    }
    if (count > kMaxPdfPages) {
        return makeError(ErrorCode::InvalidArgument,
                         "the PDF has " + std::to_string(count) + " pages; at most " +
                             std::to_string(kMaxPdfPages) + " are supported");
    }
    PdfInfo info;
    info.pageSizes.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const QSizeF points = pdf.pagePointSize(i);
        const double w = points.width();
        const double h = points.height();
        if (!(std::isfinite(w) && std::isfinite(h) && w > 0.0 && h > 0.0 &&
              w <= kMaxPdfPagePoints && h <= kMaxPdfPagePoints)) {
            return makeError(ErrorCode::InvalidArgument,
                             "page " + std::to_string(i + 1) + " of the PDF has no supported size");
        }
        info.pageSizes.push_back({w * document::kUnitsPerPoint, h * document::kUnitsPerPoint});
    }
    return info;
}

// ---------------------------------------------------------------------------- rasterizer

struct SessionDocumentRasterizer::Worker {
    std::list<std::pair<QString, std::unique_ptr<QPdfDocument>>> open; ///< most recent first

    /// The open document of `file` (nullptr if it cannot be read).
    QPdfDocument* document(const QString& file) {
        const auto it = std::find_if(open.begin(), open.end(),
                                     [&](const auto& entry) { return entry.first == file; });
        if (it != open.end()) {
            open.splice(open.begin(), open, it);
            return open.front().second.get();
        }
        auto pdf = std::make_unique<QPdfDocument>();
        const bool loaded = pdf->load(file) == QPdfDocument::Error::None;
        // A failure is remembered while the queue is drained (not reopened per tile).
        open.emplace_front(file, loaded ? std::move(pdf) : nullptr);
        while (open.size() > kOpenDocuments) {
            open.pop_back();
        }
        return open.front().second.get();
    }
};

SessionDocumentRasterizer::SessionDocumentRasterizer(application::WorkspaceSession& session)
    : session_(&session), worker_(std::make_unique<Worker>()) {
    pool_.setMaxThreadCount(1);
    pool_.setExpiryTimeout(-1); // one thread for good: the documents stay on it
}

SessionDocumentRasterizer::~SessionDocumentRasterizer() {
    {
        std::lock_guard lock(shared_->mutex);
        shared_->stopping = true;
        shared_->queue.clear();
    }
    pool_.waitForDone();
    // Close the documents on the worker thread that opened them.
    pool_.start([this] { worker_->open.clear(); });
    pool_.waitForDone();
}

void SessionDocumentRasterizer::waitForTiles() {
    pool_.waitForDone();
}

std::size_t SessionDocumentRasterizer::uncollectedBytes() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->doneBytes;
}

std::size_t SessionDocumentRasterizer::queuedTiles() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->pending.size();
}

std::size_t SessionDocumentRasterizer::renderedTiles() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->rendered;
}

std::optional<render::ImageData>
SessionDocumentRasterizer::tile(const canvas::DocumentTileKey& key) {
    {
        std::lock_guard lock(shared_->mutex);
        if (const auto it = shared_->done.find(key); it != shared_->done.end()) {
            render::ImageData pixels = std::move(it->second);
            shared_->doneBytes -= pixels.byteSize();
            shared_->done.erase(it);
            return pixels;
        }
        if (shared_->pending.contains(key)) {
            return std::nullopt; // queued or being rendered
        }
    }
    auto file = files_.find(key.asset);
    if (file == files_.end()) {
        const auto path = session_->assetPath(key.asset);
        if (!path) {
            return render::ImageData{}; // not an asset of this workspace: blank paper
        }
        file = files_.emplace(key.asset, toQString(*path)).first;
    }
    bool start = false;
    {
        std::lock_guard lock(shared_->mutex);
        shared_->queue.emplace_back(key, file->second);
        shared_->pending.insert(key);
        start = !shared_->draining;
        shared_->draining = true;
    }
    if (start) {
        pool_.start([this] { drain(); });
    }
    return std::nullopt;
}

void SessionDocumentRasterizer::keepOnly(std::span<const canvas::DocumentTileKey> wanted) {
    std::lock_guard lock(shared_->mutex);
    if (shared_->queue.empty()) {
        return;
    }
    const std::unordered_set<Key, KeyHash> keep(wanted.begin(), wanted.end());
    std::erase_if(shared_->queue, [&](const auto& request) {
        if (keep.contains(request.first)) {
            return false;
        }
        shared_->pending.erase(request.first); // asked again later, it is queued again
        return true;
    });
}

void SessionDocumentRasterizer::drain() {
    const std::shared_ptr<Shared> shared = shared_;
    QPointer<QObject> context(&context_);
    for (;;) {
        Key key;
        QString file;
        {
            std::lock_guard lock(shared->mutex);
            if (shared->queue.empty() || shared->stopping) {
                shared->draining = false;
                // Files that could not be read are tried again next time (e.g. once a
                // virus scanner or sync client released them).
                std::erase_if(worker_->open, [](const auto& entry) { return !entry.second; });
                return;
            }
            key = shared->queue.front().first;
            file = std::move(shared->queue.front().second);
            shared->queue.pop_front();
        }
        QPdfDocument* pdf = worker_->document(file);
        render::ImageData pixels = pdf != nullptr ? renderTile(*pdf, key) : render::ImageData{};
        {
            std::lock_guard lock(shared->mutex);
            shared->pending.erase(key);
            ++shared->rendered;
            shared->doneBytes += pixels.byteSize();
            shared->done[key] = std::move(pixels);
            shared->doneOrder.push_back(key);
            // Bounded: the oldest uncollected tiles go (not the one just rendered).
            while ((shared->doneBytes > kUncollectedBudgetBytes ||
                    shared->done.size() > kUncollectedTiles) &&
                   shared->doneOrder.size() > 1) {
                const Key oldest = shared->doneOrder.front();
                shared->doneOrder.pop_front();
                const auto old = shared->done.find(oldest);
                if (old != shared->done.end() && !(oldest == key)) {
                    shared->doneBytes -= old->second.byteSize();
                    shared->done.erase(old);
                }
            }
            if (shared->doneOrder.size() > 2 * shared->done.size() + 16) {
                std::erase_if(shared->doneOrder,
                              [&](const Key& k) { return !shared->done.contains(k); });
            }
        }
        QMetaObject::invokeMethod(context.data(), [this] { notifyReady(); }, Qt::QueuedConnection);
    }
}

} // namespace studyapp::ui
