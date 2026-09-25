#pragma once

#include <QMainWindow>

class QAction;
class QActionGroup;
class QSettings;

namespace studyapp::ui {

class ThemeManager;

/// Application shell: menus, toolbar, status bar and a placeholder central area.
/// Feature UI (notebooks, canvas, planner) is added in later phases.
///
/// Dependencies are injected; the window does not own them. Window geometry and the
/// chosen theme are stored in the given QSettings.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(ThemeManager& themes, QSettings& settings, QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createActions();
    void createMenus();
    void createToolBar();
    void createCentralPlaceholder();
    void createStatusBar();
    void readSettings();
    void writeSettings();
    void syncThemeActions();
    void showAbout();

    ThemeManager* themes_;
    QSettings* settings_;

    QAction* quitAction_ = nullptr;
    QActionGroup* themeGroup_ = nullptr;
    QAction* themeSystemAction_ = nullptr;
    QAction* themeLightAction_ = nullptr;
    QAction* themeDarkAction_ = nullptr;
    QAction* toggleThemeAction_ = nullptr;
    QAction* aboutAction_ = nullptr;
    QAction* aboutQtAction_ = nullptr;
};

} // namespace studyapp::ui
