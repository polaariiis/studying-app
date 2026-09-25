#include <studyapp/ui/ThemeManager.hpp>

#include <studyapp/core/Log.hpp>

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QGuiApplication>
#include <QStyleHints>

namespace studyapp::ui {

namespace {

struct PaletteColors {
    QColor window;
    QColor windowText;
    QColor base;
    QColor alternateBase;
    QColor button;
    QColor buttonText;
    QColor text;
    QColor placeholderText;
    QColor highlight;
    QColor highlightedText;
    QColor link;
    QColor light;
    QColor midlight;
    QColor mid;
    QColor dark;
    QColor shadow;
    QColor disabledText;
};

QPalette makePalette(const PaletteColors& c) {
    QPalette palette;
    palette.setColor(QPalette::Window, c.window);
    palette.setColor(QPalette::WindowText, c.windowText);
    palette.setColor(QPalette::Base, c.base);
    palette.setColor(QPalette::AlternateBase, c.alternateBase);
    palette.setColor(QPalette::ToolTipBase, c.base);
    palette.setColor(QPalette::ToolTipText, c.text);
    palette.setColor(QPalette::Button, c.button);
    palette.setColor(QPalette::ButtonText, c.buttonText);
    palette.setColor(QPalette::Text, c.text);
    palette.setColor(QPalette::BrightText, QColor(0xE5, 0x48, 0x4D));
    palette.setColor(QPalette::PlaceholderText, c.placeholderText);
    palette.setColor(QPalette::Highlight, c.highlight);
    palette.setColor(QPalette::HighlightedText, c.highlightedText);
    palette.setColor(QPalette::Link, c.link);
    palette.setColor(QPalette::LinkVisited, c.link);
    palette.setColor(QPalette::Light, c.light);
    palette.setColor(QPalette::Midlight, c.midlight);
    palette.setColor(QPalette::Mid, c.mid);
    palette.setColor(QPalette::Dark, c.dark);
    palette.setColor(QPalette::Shadow, c.shadow);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, c.disabledText);
    }
    return palette;
}

QString loadStyleSheet(bool dark) {
    QFile file(dark ? QStringLiteral(":/themes/dark.qss") : QStringLiteral(":/themes/light.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        core::logWarning("ui", "theme stylesheet resource is missing");
        return {};
    }
    return QString::fromUtf8(file.readAll());
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

QPalette lightPalette() {
    return makePalette({
        .window = QColor(0xF5, 0xF6, 0xF7),
        .windowText = QColor(0x1F, 0x23, 0x28),
        .base = QColor(0xFF, 0xFF, 0xFF),
        .alternateBase = QColor(0xF0, 0xF1, 0xF3),
        .button = QColor(0xEC, 0xEE, 0xF0),
        .buttonText = QColor(0x1F, 0x23, 0x28),
        .text = QColor(0x1F, 0x23, 0x28),
        .placeholderText = QColor(0x6E, 0x77, 0x81),
        .highlight = QColor(0x2F, 0x65, 0xCA),
        .highlightedText = QColor(0xFF, 0xFF, 0xFF),
        .link = QColor(0x1A, 0x5F, 0xB4),
        .light = QColor(0xFF, 0xFF, 0xFF),
        .midlight = QColor(0xD0, 0xD7, 0xDE),
        .mid = QColor(0x8C, 0x95, 0x9F),
        .dark = QColor(0x9A, 0xA0, 0xA6),
        .shadow = QColor(0x7A, 0x7F, 0x85),
        .disabledText = QColor(0x8C, 0x95, 0x9F),
    });
}

QPalette darkPalette() {
    return makePalette({
        .window = QColor(0x1E, 0x1F, 0x22),
        .windowText = QColor(0xDF, 0xE1, 0xE5),
        .base = QColor(0x2B, 0x2D, 0x30),
        .alternateBase = QColor(0x31, 0x33, 0x38),
        .button = QColor(0x2B, 0x2D, 0x30),
        .buttonText = QColor(0xDF, 0xE1, 0xE5),
        .text = QColor(0xDF, 0xE1, 0xE5),
        .placeholderText = QColor(0x8C, 0x8F, 0x94),
        .highlight = QColor(0x2F, 0x65, 0xCA),
        .highlightedText = QColor(0xFF, 0xFF, 0xFF),
        .link = QColor(0x58, 0x9D, 0xF6),
        .light = QColor(0x43, 0x45, 0x4A),
        .midlight = QColor(0x3C, 0x3F, 0x44),
        .mid = QColor(0x8C, 0x8F, 0x94),
        .dark = QColor(0x15, 0x16, 0x18),
        .shadow = QColor(0x00, 0x00, 0x00),
        .disabledText = QColor(0x6F, 0x73, 0x7A),
    });
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
    // Fusion renders palettes consistently on every platform, so both themes look the same
    // on Windows, macOS and Linux.
    QApplication::setStyle(QStringLiteral("Fusion"));
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            &ThemeManager::onSystemColorSchemeChanged);
    apply();
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

    QApplication::setPalette(dark_ ? darkPalette() : lightPalette());
    qApp->setStyleSheet(loadStyleSheet(dark_));
    applying_ = false;

    Q_EMIT themeChanged();
}

} // namespace studyapp::ui
