#pragma once

#include <studyapp/canvas/ShapeBuilder.hpp>
#include <studyapp/canvas/StrokeBuilder.hpp>
#include <studyapp/core/Color.hpp>
#include <studyapp/document/Element.hpp>

#include <cstdint>

namespace studyapp::canvas {

/// Default highlighter: a warm yellow at 45 % opacity, a broad band (world units).
inline constexpr std::uint8_t kHighlighterAlpha = 0x73;
inline constexpr PenStyle kDefaultHighlighter{
    .brush = document::Brush::Highlighter,
    .color = core::Color::fromRgba(0xF2, 0xC9, 0x4C, kHighlighterAlpha),
    .width = 14.0F};

/// What the eraser removes: the ink it covers (vector pieces remain), or whole strokes.
enum class EraserMode : std::uint8_t {
    Partial,
    WholeStroke,
};

/// Configuration of the drawing tools: what the next stroke looks like (docs/CANVAS.md
/// §13). It is tool state, not document data — every stroke stores its own brush, colour
/// and width in the document, so changing the settings never changes existing content,
/// and rendering reads only the document. The CanvasController owns the active settings;
/// the UI edits and remembers them. New tools add their styles here.
///
/// Note: a designated initializer such as `{.highlighter = {.width = 8}}` starts from
/// PenStyle{} (the pen's defaults: black, opaque) and resets the other tool's style too;
/// to change one field, copy the current settings (or kDefaultHighlighter) and edit it.
struct ToolSettings {
    PenStyle pen{};                                  ///< ToolKind::Pen
    PenStyle highlighter = kDefaultHighlighter;      ///< ToolKind::Highlighter
    EraserMode eraser = EraserMode::Partial;         ///< ToolKind::Eraser and a pen's eraser end
    ShapeStyle shape{};                              ///< ToolKind::Shape
    float textSize = document::kDefaultTextFontSize; ///< ToolKind::Text: new boxes' font size

    [[nodiscard]] friend bool operator==(const ToolSettings&, const ToolSettings&) = default;
};

/// `size` as a valid text font size (document::isValidTextFontSize): rounded to a whole
/// number and clamped to [kMinTextFontSize, kMaxTextFontSize]; non-finite: the default.
[[nodiscard]] float sanitizedTextFontSize(float size) noexcept;

/// Stroke widths accepted (pen and highlighter), in world units at pressure 1.
inline constexpr float kMinPenWidth = 0.25F;
inline constexpr float kMaxPenWidth = 64.0F;

/// `settings` made valid: widths clamped to [kMinPenWidth, kMaxPenWidth] (a non-finite width
/// becomes the tool's default); a fully transparent colour would draw nothing, so the pen
/// becomes opaque and the highlighter gets kHighlighterAlpha; each tool always draws its
/// own brush (document::Brush::Pen, document::Brush::Highlighter). Shapes: the outline
/// width is clamped the same way (an outline is always drawn), a transparent outline
/// becomes opaque, and an unknown kind becomes a rectangle. The text size is made valid with
/// sanitizedTextFontSize().
[[nodiscard]] ToolSettings sanitized(ToolSettings settings) noexcept;

} // namespace studyapp::canvas
