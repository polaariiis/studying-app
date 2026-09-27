#pragma once

#include <studyapp/canvas/TextLayout.hpp>

#include <string_view>

namespace studyapp::platform {

/// canvas::TextLayout on top of QTextDocument (docs/CANVAS.md §9): the application font at
/// canvas::kTextSize pixels per world unit, wrapped at the box width minus padding. The text
/// editor overlay (ui::CanvasWidget) uses a QTextEdit with the same font and width, so the
/// text wraps the same way while it is edited and once it is drawn.
class QtTextLayout final : public canvas::TextLayout {
public:
    [[nodiscard]] float heightFor(std::string_view text, float width) override;
    [[nodiscard]] render::ImageData rasterize(std::string_view text, const core::Vec2& size,
                                              float pixelsPerUnit) override;
};

} // namespace studyapp::platform
