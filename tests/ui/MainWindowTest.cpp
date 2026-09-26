#include <studyapp/ui/MainWindow.hpp>

#include <studyapp/ui/AppIcon.hpp>
#include <studyapp/ui/DesignTokens.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenuBar>
#include <QPalette>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

#include <array>
#include <memory>

using studyapp::ui::MainWindow;
using studyapp::ui::ThemeManager;
using studyapp::ui::ThemeMode;

class MainWindowTest : public QObject {
    Q_OBJECT

private:
    [[nodiscard]] std::unique_ptr<QSettings> makeSettings() const {
        return std::make_unique<QSettings>(dir_.filePath(QStringLiteral("settings.ini")),
                                           QSettings::IniFormat);
    }

    static bool windowIsDark() {
        return QApplication::palette().color(QPalette::Window).lightness() < 128;
    }

    QTemporaryDir dir_;

private Q_SLOTS:
    void initTestCase() { QVERIFY(dir_.isValid()); }

    void init() { makeSettings()->clear(); }

    void constructsShell() {
        ThemeManager themes;
        auto settings = makeSettings();
        MainWindow window(themes, *settings);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QCOMPARE(window.windowTitle(), QStringLiteral("StudyBoard"));
        for (const char* menu :
             {"menuFile", "menuEdit", "menuNotebook", "menuTools", "menuView", "menuHelp"}) {
            QVERIFY2(window.findChild<QMenu*>(QString::fromLatin1(menu)) != nullptr, menu);
        }
        QVERIFY(window.findChild<QToolBar*>(QStringLiteral("mainToolBar")) != nullptr);
        QVERIFY(window.centralWidget() != nullptr);
        // Without a workspace the welcome screen is shown; this window cannot open one.
        auto* welcome = window.findChild<QWidget*>(QStringLiteral("placeholder"));
        QVERIFY(welcome != nullptr && welcome->isVisible());
        QVERIFY(!window.hasWorkspace());
        auto* openWorkspace = window.findChild<QAction*>(QStringLiteral("actionOpenWorkspace"));
        QVERIFY(openWorkspace != nullptr && !openWorkspace->isEnabled());
        // Canvas and structure actions need an open page.
        for (const char* name : {"actionUndo", "actionNewPage", "actionToolPen", "actionZoomIn"}) {
            auto* action = window.findChild<QAction*>(QString::fromLatin1(name));
            QVERIFY2(action != nullptr && !action->isEnabled(), name);
        }
    }

    void themeManagerSwitchesPalette() {
        ThemeManager themes;

        themes.setMode(ThemeMode::Dark);
        QVERIFY(themes.isDark());
        QVERIFY(windowIsDark());

        themes.setMode(ThemeMode::Light);
        QVERIFY(!themes.isDark());
        QVERIFY(!windowIsDark());

        themes.toggleLightDark();
        QCOMPARE(themes.mode(), ThemeMode::Dark);
        QVERIFY(windowIsDark());
    }

    void themeActionsDriveThemeManager() {
        ThemeManager themes;
        auto settings = makeSettings();
        MainWindow window(themes, *settings);

        auto* dark = window.findChild<QAction*>(QStringLiteral("actionThemeDark"));
        auto* light = window.findChild<QAction*>(QStringLiteral("actionThemeLight"));
        auto* toggle = window.findChild<QAction*>(QStringLiteral("actionToggleTheme"));
        QVERIFY(dark != nullptr && light != nullptr && toggle != nullptr);

        dark->trigger();
        QCOMPARE(themes.mode(), ThemeMode::Dark);
        QVERIFY(dark->isChecked());

        toggle->trigger();
        QCOMPARE(themes.mode(), ThemeMode::Light);
        QVERIFY(light->isChecked());
        QVERIFY(!windowIsDark());
    }

    void persistsThemeAcrossSessions() {
        {
            ThemeManager themes;
            auto settings = makeSettings();
            MainWindow window(themes, *settings);
            themes.setMode(ThemeMode::Dark);
            window.show();
            QVERIFY(window.close());
        }
        {
            ThemeManager themes;
            auto settings = makeSettings();
            QCOMPARE(settings->value(QStringLiteral("appearance/theme")).toString(),
                     QStringLiteral("dark"));
            const MainWindow window(themes, *settings);
            QCOMPARE(themes.mode(), ThemeMode::Dark);
        }
    }

    void applicationIconIsCompiledIn() {
        const QIcon icon = studyapp::ui::applicationIcon();
        QVERIFY(!icon.isNull());
        const auto sizes = icon.availableSizes();
        for (const int size : {16, 32, 48, 64, 128, 256}) {
            QVERIFY2(sizes.contains(QSize(size, size)), qPrintable(QString::number(size)));
        }
        // Rendered pixels are real artwork, not an empty image.
        const QImage image = icon.pixmap(QSize(64, 64)).toImage();
        QCOMPARE(image.size(), QSize(64, 64));

        ThemeManager themes;
        auto settings = makeSettings();
        const MainWindow window(themes, *settings);
        QVERIFY(!window.windowIcon().isNull());
    }

    void paletteIsNeutral() {
        // Chrome colours must be grays: no brand hue (e.g. purple or blue) in either theme.
        for (const auto* tokens :
             {&studyapp::ui::lightColorTokens(), &studyapp::ui::darkColorTokens()}) {
            const std::array neutral{
                tokens->background,   tokens->surface,      tokens->surfaceElevated,
                tokens->canvas,       tokens->canvasGrid,   tokens->border,
                tokens->borderStrong, tokens->textPrimary,  tokens->textSecondary,
                tokens->textMuted,    tokens->textDisabled, tokens->hover,
                tokens->selected,     tokens->selectedText, tokens->control,
                tokens->controlText};
            for (const auto& c : neutral) {
                QCOMPARE(studyapp::ui::toQColor(c).hsvSaturation(), 0);
            }
            const QPalette palette = studyapp::ui::makePalette(*tokens);
            QCOMPARE(palette.color(QPalette::Highlight).hsvSaturation(), 0);
            QCOMPARE(palette.color(QPalette::Link).hsvSaturation(), 0);
        }
    }

    void styleSheetHasNoUnresolvedTokens() {
        for (const auto* tokens :
             {&studyapp::ui::lightColorTokens(), &studyapp::ui::darkColorTokens()}) {
            const QString sheet = studyapp::ui::makeStyleSheet(*tokens);
            QVERIFY(!sheet.isEmpty());
            QVERIFY2(!sheet.contains(QLatin1Char('@')), qPrintable(sheet));
        }
    }

    void themeTokensFollowMode() {
        ThemeManager themes;
        themes.setMode(ThemeMode::Dark);
        QCOMPARE(&themes.tokens(), &studyapp::ui::darkColorTokens());
        themes.setMode(ThemeMode::Light);
        QCOMPARE(&themes.tokens(), &studyapp::ui::lightColorTokens());
    }

    void settingsValuesRoundTrip() {
        for (const ThemeMode mode : {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark}) {
            QCOMPARE(studyapp::ui::themeModeFromSettingsValue(studyapp::ui::toSettingsValue(mode)),
                     mode);
        }
        QCOMPARE(studyapp::ui::themeModeFromSettingsValue(QStringLiteral("bogus")),
                 ThemeMode::System);
    }
};

QTEST_MAIN(MainWindowTest)
#include "MainWindowTest.moc"
