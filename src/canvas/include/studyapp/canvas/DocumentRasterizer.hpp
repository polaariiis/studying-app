#pragma once

#include <studyapp/core/Ids.hpp>
#include <studyapp/render/Renderer.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace studyapp::canvas {

// Pages of imported documents (PDF) behind a page's content (docs/CANVAS.md §10, Phase 8).
// A document page is drawn from tiles: square pieces of kDocumentTilePx pixels of the page
// rendered at 2^level pixels per world unit, so zooming re-renders only per factor 2 and
// only the tiles in view. A whole-page preview (one tile at a low level) is drawn under the
// tiles, so nothing is blank while finer tiles are rendered.

inline constexpr int kDocumentTilePx = 512;
/// Finest detail: 8 pixels per world unit (≈ 770 dpi); zooming further magnifies.
inline constexpr int kMaxDocumentLevel = 3;
/// Coarsest level ever rendered.
inline constexpr int kMinDocumentLevel = -8;
/// GPU memory kept for document tiles; least recently drawn tiles are released beyond it.
inline constexpr std::size_t kDocumentTextureBudgetBytes = 192U * 1024U * 1024U;

struct DocumentTileKey {
    core::AssetId asset;
    std::int32_t page = 0;
    std::int32_t level = 0; ///< 2^level pixels per world unit
    std::int32_t column = 0;
    std::int32_t row = 0;

    [[nodiscard]] friend bool operator==(const DocumentTileKey&, const DocumentTileKey&) = default;
};

struct DocumentTileKeyHash {
    [[nodiscard]] std::size_t operator()(const DocumentTileKey& key) const noexcept {
        std::size_t hash = std::hash<core::AssetId>{}(key.asset);
        for (const std::int32_t v : {key.page, key.level, key.column, key.row}) {
            hash = hash * 1099511628211ULL ^ static_cast<std::uint32_t>(v);
        }
        return hash;
    }
};

/// Pixels per world unit of `level`.
[[nodiscard]] inline double documentScale(int level) noexcept {
    return std::ldexp(1.0, level);
}

/// The level whose tiles have at least `devicePixelsPerUnit` resolution, within the limits.
[[nodiscard]] int documentLevelFor(double devicePixelsPerUnit) noexcept;

/// The coarsest level at which a page of `size` world units fits one tile (the preview).
[[nodiscard]] int documentPreviewLevel(const core::DVec2& size) noexcept;

/// Renders pages of document assets into tiles; implemented with QtPdf by
/// `ui::SessionDocumentRasterizer` (D44).
class DocumentRasterizer {
public:
    DocumentRasterizer() = default;
    virtual ~DocumentRasterizer() = default;
    DocumentRasterizer(const DocumentRasterizer&) = delete;
    DocumentRasterizer& operator=(const DocumentRasterizer&) = delete;
    DocumentRasterizer(DocumentRasterizer&&) = delete;
    DocumentRasterizer& operator=(DocumentRasterizer&&) = delete;

    /// The tile's pixels (RGBA8, premultiplied): at most kDocumentTilePx square, smaller at the
    /// right and bottom edges of the page; empty if the document or page cannot be rendered
    /// (missing, corrupt, not a document). nullopt: rendering elsewhere — the ready handler is
    /// called when asking again will succeed.
    [[nodiscard]] virtual std::optional<render::ImageData> tile(const DocumentTileKey& key) = 0;
    /// The tiles the canvas still wants: queued requests for others may be dropped (the view
    /// moved on). Called once per frame that requested tiles.
    virtual void keepOnly(std::span<const DocumentTileKey> wanted) = 0;

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
