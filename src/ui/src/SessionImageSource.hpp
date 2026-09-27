#pragma once

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/canvas/ImageSource.hpp>

#include <QObject>
#include <QThreadPool>

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace studyapp::ui {

/// canvas::ImageSource over the open workspace's asset files (docs/CANVAS.md §10). The asset
/// path is looked up on the GUI thread (the session's database); the file is decoded with
/// QImageReader on a worker thread (EXIF orientation applied, downscaled to what the canvas
/// asks for). The first load of an asset at a resolution returns "not ready" and starts the
/// decode; when it finishes, the ready handler asks the canvas for a frame and the next
/// load returns the pixels. Measured: decoding a 6000 × 4000 PNG on the GUI thread stalled
/// the first frame for ≈ 310 ms.
class SessionImageSource final : public canvas::ImageSource {
public:
    /// Decoded images nobody collected (the page changed, or the zoom asked for another
    /// size, while decoding) are kept up to this many bytes, oldest dropped first; a
    /// dropped one is decoded again if it is asked for. As much as the canvas keeps in
    /// textures: room for several full-size decodes waiting for their frame (the canvas
    /// takes a limited number of refinements per frame).
    static constexpr std::size_t kUncollectedBudgetBytes = canvas::kImageTextureBudgetBytes;

    explicit SessionImageSource(application::WorkspaceSession& session,
                                std::size_t uncollectedBudgetBytes = kUncollectedBudgetBytes);
    ~SessionImageSource() override;
    SessionImageSource(const SessionImageSource&) = delete;
    SessionImageSource& operator=(const SessionImageSource&) = delete;
    SessionImageSource(SessionImageSource&&) = delete;
    SessionImageSource& operator=(SessionImageSource&&) = delete;

    [[nodiscard]] std::optional<render::ImageData> load(core::AssetId asset, int maxSide) override;

    /// Blocks until running decodes are done (tests).
    void waitForDecodes();
    /// Bytes of decoded images the canvas has not collected yet (tests).
    [[nodiscard]] std::size_t uncollectedBytes() const;

private:
    using Key = std::pair<core::AssetId, int>;
    /// Shared with the decoding jobs, which may outlive a call but not the source.
    struct Results {
        mutable std::mutex mutex;
        std::set<Key> running;
        std::map<Key, render::ImageData> done;
        std::deque<Key> doneOrder; ///< oldest first; may name keys already collected
        std::size_t doneBytes = 0;
    };

    application::WorkspaceSession* session_;
    std::size_t uncollectedBudgetBytes_;
    std::shared_ptr<Results> results_ = std::make_shared<Results>();
    QObject context_; ///< delivers "decoded" on the GUI thread
    QThreadPool pool_;
};

} // namespace studyapp::ui
