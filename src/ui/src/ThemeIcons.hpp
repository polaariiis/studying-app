#pragma once

#include <studyapp/document/Element.hpp>

#include <QColor>
#include <QIcon>

namespace studyapp::ui {

/// Icon of the compact theme toggle: a moon while the light theme is shown ("switch to
/// dark"), a sun while the dark theme is shown. Line art in `color` (a text token), drawn
/// at high resolution so it stays sharp at any device pixel ratio.
[[nodiscard]] QIcon themeToggleIcon(bool darkThemeShown, const QColor& color);

/// A round colour swatch with a thin `border` (so black reads on dark chrome too).
[[nodiscard]] QIcon swatchIcon(const QColor& fill, const QColor& border);
/// A horizontal line `thickness` logical pixels thick, for width choices.
[[nodiscard]] QIcon lineWidthIcon(double thickness, const QColor& color);
/// The pen options button: a dot in the ink colour, sized by the width.
[[nodiscard]] QIcon penOptionsIcon(const QColor& ink, double width, const QColor& border);
/// A shape kind as line art in `color` (optionally with a light fill).
[[nodiscard]] QIcon shapeIcon(document::ShapeKind kind, const QColor& color, bool filled = false);
/// Text size (style button of the text tool): a letter "A" drawn larger for larger sizes.
[[nodiscard]] QIcon textSizeIcon(float fontSize, const QColor& color);
/// A shape kind as line art in `color` (optionally with a light fill).

} // namespace studyapp::ui
