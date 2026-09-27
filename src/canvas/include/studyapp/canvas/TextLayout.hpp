#pragma once

#include <studyapp/core/Ids.hpp>
#include <studyapp/core/Vec2.hpp>
#include <studyapp/render/Renderer.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace studyapp::canvas {

// Text on the canvas (docs/CANVAS.md §9). Phase 6 text boxes hold plain UTF-8 text in one
// style; the rich-text model comes later. Text is laid out and rasterised by a TextLayout
// the UI provides (Qt); the canvas only decides when (text, size or zoom bucket changed)
// and caches the result as a texture.

inline constexpr float kTextSize = 16.0F;          ///< font pixel size, world units
inline constexpr float kTextPadding = 4.0F;        ///< between the box and the text
inline constexpr float kDefaultTextWidth = 240.0F; ///< a click creates a box this wide
inline constexpr float kMinTextWidth = 24.0F;
/// Rasters never exceed this many pixels per side (the zoom resolution is capped instead).
inline constexpr int kMaxTextRasterPx = 4096;

class TextLayout {
public:
    TextLayout() = default;
    virtual ~TextLayout() = default;
    TextLayout(const TextLayout&) = delete;
    TextLayout& operator=(const TextLayout&) = delete;
    TextLayout(TextLayout&&) = delete;
    TextLayout& operator=(TextLayout&&) = delete;

    /// Height (world units, padding included, at least one line) of `text` wrapped within
    /// a box `width` wide.
    [[nodiscard]] virtual float heightFor(std::string_view text, float width) = 0;
    /// The box's content: `size` world units at `pixelsPerUnit`, RGBA8 premultiplied, the
    /// text in black on transparent (the display transform recolours it on dark paper).
    [[nodiscard]] virtual render::ImageData rasterize(std::string_view text, const core::Vec2& size,
                                                      float pixelsPerUnit) = 0;
};

/// A text box being edited (in the UI's editor overlay; not document data until it is
/// finished, docs/CANVAS.md §9).
struct TextEdit {
    std::optional<core::ElementId> element; ///< this text box; nullopt: a new one
    core::DVec2 position{};                 ///< world, the box's top-left corner
    float width = kDefaultTextWidth;        ///< world units
    std::string text;                       ///< UTF-8, as the editor starts
};

/// Height of `text` without a layout (headless use): one line of 1.4 × kTextSize per line
/// break, plus padding. The UI's TextLayout is used whenever it is present.
[[nodiscard]] float fallbackTextHeight(std::string_view text) noexcept;

} // namespace studyapp::canvas
