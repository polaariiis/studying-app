#pragma once

#include <studyapp/core/Ids.hpp>
#include <studyapp/ui/PanBenchmark.hpp>

#include <QImage>
#include <QMainWindow>

#include <functional>
#include <memory>

class QAction;
class QActionGroup;
class QLabel;
class QSettings;

namespace studyapp::application {
class WorkspaceSession;
} // namespace studyapp::application

namespace studyapp::canvas {
class CanvasController;
} // namespace studyapp::canvas

namespace studyapp::core {
class Clock;
class IdGenerator;
} // namespace studyapp::core

namespace studyapp::ui {

class CanvasWidget;
class SessionDocumentPort;
class ThemeManager;

/// The open workspace the window shows. Owned by the composition root; it must outlive
/// the window.
struct WorkspaceContext {
    application::WorkspaceSession& session;
    core::IdGenerator& ids;
    const core::Clock& clock;
    core::PageId page; ///< the page on the canvas (page navigation is Phase 5)
};

/// Application shell: menus, toolbars, status bar and the central area — the canvas when a
/// workspace is open, otherwise a placeholder.
///
/// Dependencies are injected; the window does not own them. Window geometry and the
/// chosen theme are stored in the given QSettings. Canvas edits go through the
/// workspace session (continuous autosave); there is no save dialog.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(ThemeManager& themes, QSettings& settings, QWidget* parent = nullptr);
    MainWindow(ThemeManager& themes, QSettings& settings, const WorkspaceContext& workspace,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    /// Zooms the canvas by `zoomFactor` (<= 0: zoom to fit), then pans it back and forth for
    /// `frames` frames and reports timings (no-op without a canvas).
    void runPanBenchmark(int frames, double zoomFactor,
                         std::function<void(const PanBenchmarkResult&)> done);
    /// The rendered canvas (for diagnostics); a null image without a canvas.
    [[nodiscard]] QImage grabCanvas();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setUp();
    void createActions();
    void createCanvasActions();
    void createMenus();
    void createToolBar();
    void createCentralWidget();
    void createStatusBar();
    void readSettings();
    void writeSettings();
    void syncThemeActions();
    void applyCanvasTheme();
    void onDocumentChanged();
    void updateEditActions();
    void updateSaveStatus();
    void setPageFormat(int backgroundPattern, bool bounded);
    void showAbout();

    ThemeManager* themes_;
    QSettings* settings_;
    std::unique_ptr<WorkspaceContext> workspace_;
    std::unique_ptr<SessionDocumentPort> port_;
    std::unique_ptr<canvas::CanvasController> controller_;
    CanvasWidget* canvasWidget_ = nullptr;

    QAction* quitAction_ = nullptr;
    QActionGroup* themeGroup_ = nullptr;
    QAction* themeSystemAction_ = nullptr;
    QAction* themeLightAction_ = nullptr;
    QAction* themeDarkAction_ = nullptr;
    QAction* toggleThemeAction_ = nullptr;
    QAction* aboutAction_ = nullptr;
    QAction* aboutQtAction_ = nullptr;

    // Canvas (only with a workspace).
    QAction* saveAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* selectAllAction_ = nullptr;
    QActionGroup* toolGroup_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* resetViewAction_ = nullptr;
    QAction* fitAction_ = nullptr;
    QAction* hudAction_ = nullptr;
    QActionGroup* backgroundGroup_ = nullptr;
    QActionGroup* extentGroup_ = nullptr;
    QLabel* saveStatusLabel_ = nullptr;
};

} // namespace studyapp::ui
