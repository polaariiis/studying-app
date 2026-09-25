#include <studyapp/canvas/Camera.hpp>

#include <algorithm>
#include <cmath>

namespace studyapp::canvas {

namespace {

bool isPositiveFinite(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

} // namespace

core::DVec2 Camera::worldToView(const core::DVec2& world) const noexcept {
    return (world - center_) * zoom_ + viewport_ * 0.5;
}

core::DVec2 Camera::viewToWorld(const core::DVec2& view) const noexcept {
    return (view - viewport_ * 0.5) / zoom_ + center_;
}

core::Affine2 Camera::worldToViewTransform() const noexcept {
    const core::DVec2 half = viewport_ * 0.5;
    return core::Affine2{
        zoom_, 0.0, 0.0, zoom_, half.x - center_.x * zoom_, half.y - center_.y * zoom_};
}

core::DRect Camera::visibleWorldRect() const noexcept {
    const core::DVec2 half = viewport_ * (0.5 / zoom_);
    return {center_ - half, center_ + half};
}

void Camera::setViewport(const core::DVec2& logicalSize, double devicePixelRatio) noexcept {
    if (isPositiveFinite(logicalSize.x) && isPositiveFinite(logicalSize.y)) {
        viewport_ = logicalSize;
    }
    if (isPositiveFinite(devicePixelRatio)) {
        dpr_ = devicePixelRatio;
    }
}

void Camera::setCenter(const core::DVec2& center) noexcept {
    if (std::isfinite(center.x) && std::isfinite(center.y)) {
        center_ = center;
    }
}

void Camera::setZoom(double zoom) noexcept {
    if (isPositiveFinite(zoom)) {
        zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    }
}

void Camera::panBy(const core::DVec2& viewDelta) noexcept {
    setCenter(center_ - viewDelta / zoom_);
}

void Camera::zoomAt(const core::DVec2& viewAnchor, double factor) noexcept {
    if (!isPositiveFinite(factor)) {
        return;
    }
    const core::DVec2 anchorWorld = viewToWorld(viewAnchor);
    setZoom(zoom_ * factor);
    // Solve worldToView(anchorWorld) == viewAnchor for the new centre.
    setCenter(anchorWorld - (viewAnchor - viewport_ * 0.5) / zoom_);
}

void Camera::fitRect(const core::DRect& world, double marginPx) noexcept {
    if (world.isEmpty()) {
        return;
    }
    const double availableX = std::max(1.0, viewport_.x - 2.0 * marginPx);
    const double availableY = std::max(1.0, viewport_.y - 2.0 * marginPx);
    const double width = std::max(world.width(), 1e-9);
    const double height = std::max(world.height(), 1e-9);
    setZoom(std::min(availableX / width, availableY / height));
    setCenter(world.center());
}

void Camera::reset() noexcept {
    zoom_ = 1.0;
    center_ = viewport_ * 0.5;
}

} // namespace studyapp::canvas
