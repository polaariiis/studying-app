#include <studyapp/ui/MainWindow.hpp>

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
        QVERIFY(window.findChild<QMenu*>(QStringLiteral("menuFile")) != nullptr);
        QVERIFY(window.findChild<QMenu*>(QStringLiteral("menuView")) != nullptr);
        QVERIFY(window.findChild<QMenu*>(QStringLiteral("menuHelp")) != nullptr);
        QVERIFY(window.findChild<QToolBar*>(QStringLiteral("mainToolBar")) != nullptr);
        QVERIFY(window.centralWidget() != nullptr);
        QCOMPARE(window.centralWidget()->objectName(), QStringLiteral("placeholder"));
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
