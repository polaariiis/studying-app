#pragma once

#include <QColor>
#include <QIcon>

namespace studyapp::ui {

/// Icon of the compact theme toggle: a moon while the light theme is shown ("switch to
/// dark"), a sun while the dark theme is shown. Line art in `color` (a text token), drawn
/// at high resolution so it stays sharp at any device pixel ratio.
[[nodiscard]] QIcon themeToggleIcon(bool darkThemeShown, const QColor& color);

} // namespace studyapp::ui
