#include <studyapp/canvas/DocumentRasterizer.hpp>

#include <algorithm>
#include <cmath>

namespace studyapp::canvas {

int documentLevelFor(double devicePixelsPerUnit) noexcept {
    if (!(devicePixelsPerUnit > 0.0) || !std::isfinite(devicePixelsPerUnit)) {
        return kMinDocumentLevel;
    }
    const auto level = static_cast<int>(std::ceil(std::log2(devicePixelsPerUnit) - 1e-9));
    return std::clamp(level, kMinDocumentLevel, kMaxDocumentLevel);
}

int documentPreviewLevel(const core::DVec2& size) noexcept {
    const double longest = std::max(size.x, size.y);
    if (!(longest > 0.0) || !std::isfinite(longest)) {
        return kMinDocumentLevel;
    }
    const auto level = static_cast<int>(std::floor(std::log2(kDocumentTilePx / longest)));
    return std::clamp(level, kMinDocumentLevel, kMaxDocumentLevel);
}

} // namespace studyapp::canvas
