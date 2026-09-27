#include "SessionImageSource.hpp"

#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QPointer>
#include <QString>

#include <algorithm>
#include <cstring>

namespace studyapp::ui {

namespace {

render::ImageData decode(const QString& file, int maxSide) {
    render::ImageData pixels;
    QImageReader reader(file);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && std::max(size.width(), size.height()) > maxSide) {
        reader.setScaledSize(size.scaled(maxSide, maxSide, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        return pixels; // missing, corrupted or not an image
    }
    if (std::max(image.width(), image.height()) > maxSide) { // formats that ignore scaling
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
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

SessionImageSource::SessionImageSource(application::WorkspaceSession& session,
                                       std::size_t uncollectedBudgetBytes)
    : session_(&session), uncollectedBudgetBytes_(uncollectedBudgetBytes) {
    pool_.setMaxThreadCount(2); // decoding is memory-bound; a few large files at a time
}

SessionImageSource::~SessionImageSource() {
    pool_.waitForDone(); // jobs post to context_, which is destroyed with this object
}

void SessionImageSource::waitForDecodes() {
    pool_.waitForDone();
}

std::size_t SessionImageSource::uncollectedBytes() const {
    std::lock_guard lock(results_->mutex);
    return results_->doneBytes;
}

std::optional<render::ImageData> SessionImageSource::load(core::AssetId asset, int maxSide) {
    const Key key{asset, maxSide};
    {
        std::lock_guard lock(results_->mutex);
        if (const auto it = results_->done.find(key); it != results_->done.end()) {
            render::ImageData pixels = std::move(it->second);
            results_->doneBytes -= pixels.byteSize();
            results_->done.erase(it);
            return pixels;
        }
        if (results_->running.contains(key)) {
            return std::nullopt; // still decoding
        }
    }
    const auto path = session_->assetPath(asset);
    if (!path || maxSide <= 0) {
        return render::ImageData{}; // unknown asset: the canvas shows a frame
    }
    {
        std::lock_guard lock(results_->mutex);
        results_->running.insert(key);
    }
    const QString file = QString::fromStdU16String(path->u16string());
    std::shared_ptr<Results> results = results_;
    QPointer<QObject> context(&context_);
    pool_.start([results, key, file, context, budget = uncollectedBudgetBytes_, this] {
        render::ImageData pixels = decode(file, key.second);
        {
            std::lock_guard lock(results->mutex);
            results->running.erase(key);
            results->doneBytes += pixels.byteSize();
            results->done[key] = std::move(pixels);
            results->doneOrder.push_back(key);
            // Bounded: the oldest uncollected results go (not the one just decoded).
            while (results->doneBytes > budget && results->doneOrder.size() > 1) {
                const auto old = results->done.find(results->doneOrder.front());
                results->doneOrder.pop_front();
                if (old != results->done.end() && old->first != key) {
                    results->doneBytes -= old->second.byteSize();
                    results->done.erase(old);
                }
            }
            if (results->doneOrder.size() > 2 * results->done.size() + 16) {
                // Drop the names of collected results (keeps the queue as small as the map).
                std::erase_if(results->doneOrder,
                              [&](const Key& k) { return !results->done.contains(k); });
            }
        }
        QMetaObject::invokeMethod(context.data(), [this] { notifyReady(); }, Qt::QueuedConnection);
    });
    return std::nullopt;
}

} // namespace studyapp::ui
