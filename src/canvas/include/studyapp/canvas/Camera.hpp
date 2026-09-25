#pragma once

#include <studyapp/core/Affine2.hpp>
#include <studyapp/core/Rect.hpp>
#include <studyapp/core/Vec2.hpp>

namespace studyapp::canvas {

/// The view onto a page: the single owner of every world ↔ view ↔ device conversion
/// (docs/CANVAS.md §1–2). Rendering and input both go through the same Camera value, so
/// what is drawn and what the pointer hits cannot disagree.
///
/// Spaces (y points down in all of them):
///   world   document coordinates (double), 1 unit = 1 logical px at zoom 1
///   view    logical widget pixels, origin at the widget's top-left corner
///   device  physical framebuffer pixels = view × devicePixelRatio
///
///   view   = (world − center) × zoom + viewportSize / 2
///   device = view × devicePixelRatio
///
/// The OpenGL y flip (framebuffer origin bottom-left) is the renderer's projection
/// concern and never appears here.
class Camera {
public:
    static constexpr double kMinZoom = 0.02;
    static constexpr double kMaxZoom = 64.0;

    [[nodiscard]] core::DVec2 center() const noexcept { return center_; }
    [[nodiscard]] double zoom() const noexcept { return zoom_; }
    [[nodiscard]] core::DVec2 viewportSize() const noexcept { return viewport_; }
    [[nodiscard]] double devicePixelRatio() const noexcept { return dpr_; }
    /// Framebuffer size in physical pixels (what the GL viewport must be).
    [[nodiscard]] core::DVec2 deviceSize() const noexcept { return viewport_ * dpr_; }

    [[nodiscard]] core::DVec2 worldToView(const core::DVec2& world) const noexcept;
    [[nodiscard]] core::DVec2 viewToWorld(const core::DVec2& view) const noexcept;
    [[nodiscard]] core::DVec2 viewToDevice(const core::DVec2& view) const noexcept {
        return view * dpr_;
    }
    [[nodiscard]] core::DVec2 deviceToView(const core::DVec2& device) const noexcept {
        return device / dpr_;
    }
    /// world → view as an affine transform.
    [[nodiscard]] core::Affine2 worldToViewTransform() const noexcept;
    /// The world rectangle covered by the viewport.
    [[nodiscard]] core::DRect visibleWorldRect() const noexcept;
    /// A length in view pixels expressed in world units (e.g. hit tolerances).
    [[nodiscard]] double viewToWorldLength(double viewPixels) const noexcept {
        return viewPixels / zoom_;
    }

    /// Resizing keeps the world point at the viewport centre fixed. Non-positive or
    /// non-finite values are ignored.
    void setViewport(const core::DVec2& logicalSize, double devicePixelRatio) noexcept;
    void setCenter(const core::DVec2& center) noexcept;
    /// Clamped to [kMinZoom, kMaxZoom]; keeps the centre fixed.
    void setZoom(double zoom) noexcept;

    /// Moves the content by `viewDelta` pixels (the content follows the pointer).
    void panBy(const core::DVec2& viewDelta) noexcept;
    /// Multiplies the zoom by `factor` (clamped) while keeping the world point under
    /// `viewAnchor` fixed on screen.
    void zoomAt(const core::DVec2& viewAnchor, double factor) noexcept;
    /// Centres `world` and zooms so it fits the viewport with `marginPx` on each side.
    void fitRect(const core::DRect& world, double marginPx) noexcept;
    /// Zoom 1 with the world origin at the viewport's top-left corner.
    void reset() noexcept;

    [[nodiscard]] friend bool operator==(const Camera&, const Camera&) = default;

private:
    core::DVec2 center_{0.0, 0.0};
    double zoom_ = 1.0;
    core::DVec2 viewport_{1.0, 1.0};
    double dpr_ = 1.0;
};

} // namespace studyapp::canvas
