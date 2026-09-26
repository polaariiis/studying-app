#pragma once

#include <studyapp/core/Ids.hpp>
#include <studyapp/ui/PanBenchmark.hpp>
#include <studyapp/ui/ShellDialogs.hpp>

#include <QImage>
#include <QMainWindow>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

class QAction;
class QActionGroup;
class QLabel;
class QMenu;
class QSettings;
class QSplitter;
class QStackedWidget;

namespace studyapp::application {
class WorkspaceLocker;
class WorkspaceSession;
} // namespace studyapp::application

namespace studyapp::core {
class Clock;
class IdGenerator;
} // namespace studyapp::core

namespace studyapp::document {
class Patch;
} // namespace studyapp::document

namespace studyapp::ui {

class CanvasPlaceholder;
class CanvasWidget;
class NavigationPanel;
class ThemeManager;
class WorkspaceTreeModel;

/// What the shell needs to open and create workspaces. Owned by the composition root; it
/// must outlive the window.
struct ShellServices {
    const core::Clock& clock;
    core::IdGenerator& ids;
    application::WorkspaceLocker& locker;
};

/// A workspace session owned by the caller, shown by the window without taking it over
/// (tests and tools). The window never closes it.
struct WorkspaceContext {
    application::WorkspaceSession& session;
    core::IdGenerator& ids;
    const core::Clock& clock;
    core::PageId page; ///< the page shown first
};

/// The application shell (docs/ARCHITECTURE.md §3.5): menus, toolbar, the navigation
/// tree, the canvas and the status bar.
///
/// Ownership: the window owns the open workspace — its WorkspaceSession (unless borrowed
/// through WorkspaceContext), the canvas controller, the PageNavigator (active page) and
/// the WorkspaceStructure actions — and recreates them when another workspace is opened.
/// Switching pages reuses them; the canvas widget stays.
///
/// Every edit — canvas, structure, undo/redo — goes through the session (Command → Patch
/// → Workspace → persistence). Each applied patch is routed to the canvas, the tree and
/// the navigator. Changes are saved continuously: there is no save question; only a
/// failed write asks what to do when the workspace is closed.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// Welcome screen only; it cannot open workspaces (tests of the chrome).
    MainWindow(ThemeManager& themes, QSettings& settings, QWidget* parent = nullptr);
    /// The full shell. `dialogs` answers the shell's questions (nullptr: native dialogs).
    MainWindow(ThemeManager& themes, QSettings& settings, const ShellServices& services,
               std::unique_ptr<ShellDialogs> dialogs = nullptr, QWidget* parent = nullptr);
    /// Shows a session owned by the caller (see WorkspaceContext).
    MainWindow(ThemeManager& themes, QSettings& settings, const WorkspaceContext& workspace,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    // ---- workspaces ----------------------------------------------------------------------
    enum class OpenMode {
        Existing,
        CreateIfMissing, ///< create the workspace if `root` holds none (first start)
    };
    /// Opens the workspace in `root` (asking about locks when needed) and shows its first
    /// page; the previously open workspace is closed. false if it was not opened.
    bool openWorkspace(const std::filesystem::path& root, OpenMode mode = OpenMode::Existing);
    /// Creates a workspace in `root` (named after the directory) with a start page.
    bool createWorkspace(const std::filesystem::path& root);
    /// Closes the open workspace after writing anything pending. false if changes could
    /// not be written and the user chose to keep the workspace open.
    bool closeWorkspace();
    [[nodiscard]] bool hasWorkspace() const noexcept;
    [[nodiscard]] application::WorkspaceSession* session() const noexcept;
    /// Recently opened workspace directories, most recent first.
    [[nodiscard]] QStringList recentWorkspaces() const;
    /// The workspace open when the application last quit, if any.
    [[nodiscard]] std::optional<std::filesystem::path> lastWorkspace() const;
    /// Whether opened workspaces go into the recent list and become the start-up
    /// workspace (default true; development runs such as benchmarks turn it off).
    void setRememberWorkspaces(bool remember);

    // ---- navigation ----------------------------------------------------------------------
    [[nodiscard]] std::optional<core::PageId> activePage() const;
    /// Shows `page` on the canvas (where the user left it, if it was open before).
    bool openPage(core::PageId page);

    // ---- diagnostics ---------------------------------------------------------------------
    /// Zooms the canvas by `zoomFactor` (<= 0: zoom to fit), then pans it back and forth for
    /// `frames` frames and reports timings (no-op without a canvas).
    void runPanBenchmark(int frames, double zoomFactor,
                         std::function<void(const PanBenchmarkResult&)> done);
    /// The rendered canvas (for diagnostics); a null image without a canvas.
    [[nodiscard]] QImage grabCanvas();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct OpenWorkspace;

    void setUp();
    void createActions();
    void createMenus();
    void createToolBar();
    void createCentralWidget();
    void createStatusBar();
    void readSettings();
    void writeSettings();

    void attach(std::unique_ptr<OpenWorkspace> workspace, std::optional<core::PageId> page);
    void detach(bool closeSession);
    void rememberWorkspace(const std::filesystem::path& root);
    void forgetIfLast(const std::filesystem::path& root);
    void onPatchApplied(const document::Patch& patch);
    void showActivePage();
    void rememberView();
    void revealPage(std::optional<core::PageId> page);
    void syncTreeToActivePage();
    void onTreeCurrentChanged();
    void showTreeContextMenu(const QPoint& position);

    // User actions.
    void newWorkspace();
    void openWorkspaceDialog();
    void undo();
    void redo();
    void newPage();
    void newSection();
    void newNotebook();
    void renameCurrent();
    void deleteCurrent();
    void moveCurrent(int delta);
    void setPageFormat(int backgroundPattern, bool bounded);

    // Chrome.
    void syncThemeActions();
    void applyCanvasTheme();
    void updateActions();
    void scheduleStatusUpdate();
    void updateStatus();
    void updateTitle();
    void updateRecentMenu();
    void showAbout();
    void reportFailure(const QString& summary, const QString& details);

    ThemeManager* themes_;
    QSettings* settings_;
    std::optional<ShellServices> services_;
    std::unique_ptr<ShellDialogs> dialogs_;
    std::unique_ptr<OpenWorkspace> open_;
    bool syncingTree_ = false;
    bool applyingPatch_ = false; ///< inside onPatchApplied (the tree model is catching up)
    bool statusUpdatePending_ = false;
    bool rememberWorkspaces_ = true;
    // The open page as last revealed in the tree (expanded ancestors).
    std::optional<core::PageId> revealedPage_;
    std::optional<core::SectionId> revealedSection_;

    // Central area.
    QStackedWidget* stack_ = nullptr;
    CanvasPlaceholder* welcome_ = nullptr;
    QSplitter* shell_ = nullptr;
    NavigationPanel* navigation_ = nullptr;
    WorkspaceTreeModel* treeModel_ = nullptr;
    QWidget* canvasArea_ = nullptr;
    CanvasWidget* canvasWidget_ = nullptr;

    // File.
    QAction* newWorkspaceAction_ = nullptr;
    QAction* openWorkspaceAction_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QAction* closeWorkspaceAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* quitAction_ = nullptr;
    // Edit.
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* selectAllAction_ = nullptr;
    // Notebook (structure).
    QAction* newPageAction_ = nullptr;
    QAction* newSectionAction_ = nullptr;
    QAction* newNotebookAction_ = nullptr;
    QAction* renameAction_ = nullptr;
    QAction* deleteItemAction_ = nullptr;
    QAction* moveUpAction_ = nullptr;
    QAction* moveDownAction_ = nullptr;
    QAction* previousPageAction_ = nullptr;
    QAction* nextPageAction_ = nullptr;
    // Tools and view.
    QActionGroup* toolGroup_ = nullptr;
    QAction* navigationAction_ = nullptr;
    QActionGroup* themeGroup_ = nullptr;
    QAction* themeSystemAction_ = nullptr;
    QAction* themeLightAction_ = nullptr;
    QAction* themeDarkAction_ = nullptr;
    QAction* toggleThemeAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* resetViewAction_ = nullptr;
    QAction* fitAction_ = nullptr;
    QAction* hudAction_ = nullptr;
    QActionGroup* backgroundGroup_ = nullptr;
    QActionGroup* extentGroup_ = nullptr;
    // Help.
    QAction* aboutAction_ = nullptr;
    QAction* aboutQtAction_ = nullptr;

    QLabel* breadcrumbLabel_ = nullptr;
    QLabel* saveStatusLabel_ = nullptr;
};

} // namespace studyapp::ui
