#include <studyapp/ui/ThemeManager.hpp>

#include <studyapp/core/Log.hpp>

#include <QApplication>
#include <QFile>
#include <QGuiApplication>
#include <QStyleHints>

#include <initializer_list>
#include <utility>

namespace studyapp::ui {

namespace {

QString loadTemplate() {
    QFile file(QStringLiteral(":/themes/studyboard.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        core::logWarning("ui", "theme stylesheet resource is missing");
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString cssColor(const core::Color& color) {
    return toQColor(color).name(QColor::HexRgb);
}

} // namespace

QString toSettingsValue(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Light:
        return QStringLiteral("light");
    case ThemeMode::Dark:
        return QStringLiteral("dark");
    case ThemeMode::System:
        break;
    }
    return QStringLiteral("system");
}

ThemeMode themeModeFromSettingsValue(const QString& value) {
    if (value == QLatin1String("light")) {
        return ThemeMode::Light;
    }
    if (value == QLatin1String("dark")) {
        return ThemeMode::Dark;
    }
    return ThemeMode::System;
}

QColor toQColor(const core::Color& color) {
    return QColor(color.r, color.g, color.b, color.a);
}

QPalette makePalette(const ColorTokens& t) {
    QPalette palette;
    const auto set = [&](QPalette::ColorRole role, const core::Color& color) {
        palette.setColor(role, toQColor(color));
    };
    set(QPalette::Window, t.background);
    set(QPalette::WindowText, t.textPrimary);
    set(QPalette::Base, t.surface);
    set(QPalette::AlternateBase, t.background);
    set(QPalette::ToolTipBase, t.surfaceElevated);
    set(QPalette::ToolTipText, t.textPrimary);
    set(QPalette::PlaceholderText, t.textMuted);
    set(QPalette::Text, t.textPrimary);
    set(QPalette::Button, t.surface);
    set(QPalette::ButtonText, t.textPrimary);
    set(QPalette::BrightText, t.error);
    // Selection is tonal, not coloured.
    set(QPalette::Highlight, t.borderStrong);
    set(QPalette::HighlightedText, t.selectedText);
    // Links use the text colour (rich text underlines them), not a brand blue.
    set(QPalette::Link, t.textPrimary);
    set(QPalette::LinkVisited, t.textSecondary);
    set(QPalette::Light, t.surface);
    set(QPalette::Midlight, t.border);
    set(QPalette::Mid, t.textMuted);
    set(QPalette::Dark, t.borderStrong);
    set(QPalette::Shadow, t.borderStrong);
    set(QPalette::Accent, t.control);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, toQColor(t.textDisabled));
    }
    return palette;
}

QString makeStyleSheet(const ColorTokens& t) {
    QString sheet = loadTemplate();
    const MetricTokens& m = metricTokens();
    const std::initializer_list<std::pair<const char*, QString>> values = {
        {"background", cssColor(t.background)},
        {"surfaceElevated", cssColor(t.surfaceElevated)},
        {"surface", cssColor(t.surface)},
        {"borderStrong", cssColor(t.borderStrong)},
        {"border", cssColor(t.border)},
        {"textPrimary", cssColor(t.textPrimary)},
        {"textSecondary", cssColor(t.textSecondary)},
        {"textMuted", cssColor(t.textMuted)},
        {"textDisabled", cssColor(t.textDisabled)},
        {"hover", cssColor(t.hover)},
        {"selectedText", cssColor(t.selectedText)},
        {"selected", cssColor(t.selected)},
        {"radiusSmall", QString::number(m.radiusSmall)},
        {"radiusMedium", QString::number(m.radiusMedium)},
    };
    for (const auto& [name, value] : values) {
        sheet.replace(QLatin1Char('@') + QLatin1String(name) + QLatin1Char('@'), value);
    }
    return sheet;
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
    // Fusion renders palettes consistently on every platform.
    QApplication::setStyle(QStringLiteral("Fusion"));
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            &ThemeManager::onSystemColorSchemeChanged);
    apply();
}

const ColorTokens& ThemeManager::tokens() const noexcept {
    return dark_ ? darkColorTokens() : lightColorTokens();
}

void ThemeManager::setMode(ThemeMode mode) {
    if (mode == mode_) {
        return;
    }
    mode_ = mode;
    apply();
}

void ThemeManager::toggleLightDark() {
    setMode(dark_ ? ThemeMode::Light : ThemeMode::Dark);
}

void ThemeManager::onSystemColorSchemeChanged() {
    if (!applying_ && mode_ == ThemeMode::System) {
        apply();
    }
}

void ThemeManager::apply() {
    applying_ = true;
    QStyleHints* hints = QGuiApplication::styleHints();

    // Let native window decorations (e.g. the Windows title bar) follow explicit choices.
    switch (mode_) {
    case ThemeMode::Light:
        hints->setColorScheme(Qt::ColorScheme::Light);
        break;
    case ThemeMode::Dark:
        hints->setColorScheme(Qt::ColorScheme::Dark);
        break;
    case ThemeMode::System:
        hints->unsetColorScheme();
        break;
    }

    dark_ = mode_ == ThemeMode::Dark ||
            (mode_ == ThemeMode::System && hints->colorScheme() == Qt::ColorScheme::Dark);

    const ColorTokens& t = tokens();
    QApplication::setPalette(makePalette(t));
    qApp->setStyleSheet(makeStyleSheet(t));
    applying_ = false;

    Q_EMIT themeChanged();
}

} // namespace studyapp::ui
