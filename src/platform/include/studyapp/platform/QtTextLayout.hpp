#pragma once

#include <studyapp/canvas/TextLayout.hpp>

#include <string_view>

namespace studyapp::platform {

/// canvas::TextLayout on top of QTextDocument (docs/CANVAS.md §9): the application font at
/// the box's font size (pixels per world unit), wrapped at the box width minus padding. The
/// text editor overlay (ui::CanvasWidget) and exports (ui::PageExport) use the same font,
/// size and width, so the text wraps the same way while it is edited, drawn and exported.
class QtTextLayout final : public canvas::TextLayout {
public:
    [[nodiscard]] float heightFor(std::string_view text, float width, float fontSize) override;
    [[nodiscard]] render::ImageData rasterize(std::string_view text, const core::Vec2& size,
                                              float fontSize, float pixelsPerUnit) override;
};

} // namespace studyapp::platform
