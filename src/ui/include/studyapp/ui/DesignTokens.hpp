#pragma once

#include <studyapp/core/Color.hpp>

namespace studyapp::ui {

// StudyBoard design tokens: the minimal shared visual vocabulary (docs/ARCHITECTURE.md
// "Design system foundation").
//
// Principles: canvas-first, neutral (black / white / grays), restrained. Colour carries
// meaning only for status (error, warning, success); there is no decorative brand accent,
// no gradients, no glow, no translucency. Active/selected states use tonal contrast and
// borders, not coloured fills.
//
// Tokens are plain core::Color values (not QColor) so the canvas and renderer, which are
// Qt-free, can adopt the same values when they are implemented.

struct ColorTokens {
    // Surfaces
    core::Color background;      ///< application chrome (window, toolbars, status bar)
    core::Color surface;         ///< panels, inputs, lists
    core::Color surfaceElevated; ///< menus, popups, tooltips (separated by border, no shadow)
    core::Color canvas;          ///< the page / canvas area
    core::Color canvasGrid;      ///< subtle grid dots/lines on the canvas

    // Borders
    core::Color border;       ///< subtle separators
    core::Color borderStrong; ///< control outlines, focus/active outlines

    // Text
    core::Color textPrimary;
    core::Color textSecondary;
    core::Color textMuted;
    core::Color textDisabled;

    // Interaction states
    core::Color hover;        ///< hovered control background
    core::Color selected;     ///< selected / active control background
    core::Color selectedText; ///< text on `selected`
    core::Color control;      ///< primary (dark) control, e.g. a default button
    core::Color controlText;  ///< text on `control`

    // Status (functional accents only)
    core::Color error;
    core::Color warning;
    core::Color success;
};

struct MetricTokens {
    int radiusSmall = 3;        ///< controls, menu items
    int radiusMedium = 4;       ///< panels, popups
    int spacingUnit = 4;        ///< base spacing step
    int canvasGridSpacing = 24; ///< logical pixels between grid dots
};

[[nodiscard]] const ColorTokens& lightColorTokens() noexcept;
[[nodiscard]] const ColorTokens& darkColorTokens() noexcept;
[[nodiscard]] const MetricTokens& metricTokens() noexcept;

} // namespace studyapp::ui
