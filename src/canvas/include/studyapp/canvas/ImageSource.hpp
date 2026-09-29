#pragma once

#include <studyapp/core/Ids.hpp>
#include <studyapp/render/Renderer.hpp>

#include <cstddef>
#include <functional>
#include <optional>

namespace studyapp::canvas {

// Images on the canvas (docs/CANVAS.md §10). Image elements reference content-addressed
// assets; the canvas asks an ImageSource (the UI decodes the workspace's asset files) for
// pixels at the resolution the view needs and keeps one texture per asset, shared by every
// element that shows it.

/// Decoded textures never exceed this many pixels per side.
inline constexpr int kMaxImageTexturePx = 4096;
/// GPU memory kept for image textures; least recently drawn ones are released beyond it.
inline constexpr std::size_t kImageTextureBudgetBytes = std::size_t{256} * 1024U * 1024U;

class ImageSource {
public:
    ImageSource() = default;
    virtual ~ImageSource() = default;
    ImageSource(const ImageSource&) = delete;
    ImageSource& operator=(const ImageSource&) = delete;
    ImageSource(ImageSource&&) = delete;
    ImageSource& operator=(ImageSource&&) = delete;

    /// The asset's pixels for display, RGBA8 premultiplied, at most `maxSide` pixels on the
    /// longer side (downscaled with the aspect ratio kept; never upscaled). Empty if the
    /// asset is missing, unreadable or not an image. nullopt: not ready yet — decoding
    /// continues elsewhere and the ready handler is called when asking again will succeed
    /// (so decoding a large file never stalls a frame).
    [[nodiscard]] virtual std::optional<render::ImageData> load(core::AssetId asset,
                                                                int maxSide) = 0;
    /// Called (on the canvas's thread) when a load that returned nullopt can be retried.
    void setReadyHandler(std::function<void()> handler) { ready_ = std::move(handler); }

protected:
    void notifyReady() const {
        if (ready_) {
            ready_();
        }
    }

private:
    std::function<void()> ready_;
};

} // namespace studyapp::canvas
