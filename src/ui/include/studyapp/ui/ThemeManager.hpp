#pragma once

#include <studyapp/ui/DesignTokens.hpp>

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QString>

namespace studyapp::ui {

enum class ThemeMode {
    System, ///< follow the operating system's light/dark setting
    Light,
    Dark,
};

/// Stable string used to persist a mode in QSettings ("system", "light", "dark").
[[nodiscard]] QString toSettingsValue(ThemeMode mode);
/// Inverse of toSettingsValue(); unknown values map to ThemeMode::System.
[[nodiscard]] ThemeMode themeModeFromSettingsValue(const QString& value);

[[nodiscard]] QColor toQColor(const core::Color& color);

/// QPalette derived from design tokens (neutral: no coloured highlight or links).
[[nodiscard]] QPalette makePalette(const ColorTokens& tokens);

/// Application stylesheet: the `:/themes/studyboard.qss` template with @token@
/// placeholders replaced from `tokens` and metricTokens().
[[nodiscard]] QString makeStyleSheet(const ColorTokens& tokens);

/// Applies the application-wide light or dark theme: Fusion style, a token-derived
/// QPalette and the token-derived stylesheet. In System mode it follows
/// QStyleHints::colorScheme() and reacts to changes of the OS setting.
///
/// Requires a QApplication. Owned by the composition root (app/main.cpp).
class ThemeManager final : public QObject {
    Q_OBJECT

public:
    explicit ThemeManager(QObject* parent = nullptr);

    [[nodiscard]] ThemeMode mode() const noexcept { return mode_; }
    /// The effective appearance after resolving System mode.
    [[nodiscard]] bool isDark() const noexcept { return dark_; }
    /// Colour tokens of the effective appearance.
    [[nodiscard]] const ColorTokens& tokens() const noexcept;

    void setMode(ThemeMode mode);
    /// Switches to an explicit Light or Dark mode, opposite to the current appearance.
    void toggleLightDark();

Q_SIGNALS:
    /// Emitted when the mode or the effective appearance changes.
    void themeChanged();

private:
    void apply();
    void onSystemColorSchemeChanged();

    ThemeMode mode_ = ThemeMode::System;
    bool dark_ = false;
    bool applying_ = false;
};

} // namespace studyapp::ui
