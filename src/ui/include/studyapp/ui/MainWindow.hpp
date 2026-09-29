#pragma once

#include <studyapp/core/Ids.hpp>
#include <studyapp/ui/PanBenchmark.hpp>
#include <studyapp/ui/ShellDialogs.hpp>

#include <QColor>
#include <QImage>
#include <QMainWindow>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QTimer;
class QTreeWidgetItem;
class QActionGroup;
class QLabel;
class QMenu;
class QSettings;
class QSplitter;
class QStackedWidget;
class QToolButton;

namespace studyapp::application {
class WorkspaceLocker;
class WorkspaceSession;
} // namespace studyapp::application

namespace studyapp::canvas {
enum class ToolKind : std::uint8_t;
class TextLayout;
} // namespace studyapp::canvas

namespace studyapp::core {
class Clock;
class IdGenerator;
} // namespace studyapp::core

namespace studyapp::document {
class Patch;
} // namespace studyapp::document

namespace studyapp::study {
class TimeZone;
} // namespace studyapp::study

namespace studyapp::ui {

class CanvasPlaceholder;
struct ExportSources;
class CanvasWidget;
class PlannerPanel;
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
    /// Imports still copying files on pool threads (tests wait for 0).
    [[nodiscard]] int backgroundJobCount() const noexcept { return backgroundJobs_; }
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
    /// How text boxes are laid out and drawn (the composition root passes the Qt
    /// implementation; not owned, must outlive the window). Without one, text boxes are
    /// drawn as frames.
    void setTextLayout(canvas::TextLayout* layout);
    /// Local time of the planner (the composition root passes the system zone); UTC until
    /// set.
    void setTimeZone(const study::TimeZone* zone);

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
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    /// Writes the text being typed on the canvas, if any (before the page changes or the
    /// workspace closes).
    void finishTextEditing();
    // Search (Phase 8): the field in the navigation panel, results in place of the tree.
    void runSearch();
    void activateSearchResult(QTreeWidgetItem* item);
    void clearSearch();
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
    void insertImage();
    void importPdf();
    /// Runs `work` on a pool thread; the function it returns runs on the GUI thread if the
    /// same workspace is still open (Phase 9: imports off the GUI thread).
    void runInBackground(std::function<std::function<void()>()> work);
    /// File ▸ Export Page (PDF, PNG, SVG) or, `wholeSection`, Export Section as PDF.
    void exportPages(bool wholeSection);
    void printPages();
    /// File ▸ Export Workspace or (`notebookOnly`) Export Notebook: a bundle file.
    void exportBundle(bool notebookOnly);
    void importNotebookBundle();
    void openBundleAsWorkspace();
    void backUpNow();
    /// Moves the keyboard focus to the next (+1) or previous (-1) pane shown.
    void focusPane(int step);
    void checkWorkspace();
    /// The current page, or every page of its section; empty without a page.
    [[nodiscard]] std::vector<core::PageId> pagesToExport(bool wholeSection) const;
    /// Asset files of the open workspace, and progress with Cancel for several `pages`.
    [[nodiscard]] ExportSources exportSources(std::size_t pages);
    /// The notebook of the tree's current item or the open page.
    [[nodiscard]] std::optional<core::NotebookId> currentNotebook() const;
    struct InkControls;
    void createInkActions(InkControls& ink);
    /// Hands the pen and highlighter styles to the canvas (canvas::ToolSettings).
    void applyToolSettings();
    void selectTool(canvas::ToolKind tool);
    /// Checked inks and widths, and the style button (menu, icon, tooltip). Cheap: runs on
    /// every tool or style change.
    void syncInkActions();
    /// Swatch and width icons of the style menus; they depend on the theme only.
    void refreshInkIcons();

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
    PlannerPanel* planner_ = nullptr;
    QAction* plannerAction_ = nullptr;
    const study::TimeZone* timeZone_ = nullptr;
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
    QAction* cutAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* pasteAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* selectAllAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QTimer* searchDelay_ = nullptr;
    // Notebook (structure).
    QAction* newPageAction_ = nullptr;
    QAction* newSectionAction_ = nullptr;
    QAction* newNotebookAction_ = nullptr;
    QAction* renameAction_ = nullptr;
    QAction* deleteItemAction_ = nullptr;
    QAction* insertImageAction_ = nullptr;
    QAction* importPdfAction_ = nullptr;
    int backgroundJobs_ = 0; ///< imports running on pool threads
    QAction* exportPageAction_ = nullptr;
    QAction* exportSectionAction_ = nullptr;
    QAction* printAction_ = nullptr;
    QAction* exportWorkspaceAction_ = nullptr;
    QAction* exportNotebookAction_ = nullptr;
    QAction* importNotebookAction_ = nullptr;
    QAction* openBundleAction_ = nullptr;
    QAction* backUpAction_ = nullptr;
    QAction* nextPaneAction_ = nullptr;
    QAction* previousPaneAction_ = nullptr;
    QAction* checkWorkspaceAction_ = nullptr;
    QAction* moveUpAction_ = nullptr;
    QAction* moveDownAction_ = nullptr;
    QAction* previousPageAction_ = nullptr;
    QAction* nextPageAction_ = nullptr;
    // Tools and view.
    QActionGroup* toolGroup_ = nullptr;
    // Ink tool styles (canvas::ToolSettings: the pen's and the highlighter's colour and
    // width), kept here across workspaces and remembered per user (QSettings).
    struct InkControls {
        canvas::ToolKind tool{};
        QColor color;
        float width = 0.0F;
        QActionGroup* colors = nullptr;
        QActionGroup* widths = nullptr;
        QMenu* menu = nullptr;
    };
    InkControls pen_;
    InkControls highlighter_;
    /// Shape style (canvas::ShapeStyle): ink and width as for the pen, plus kind and fill.
    InkControls shape_;
    canvas::TextLayout* textLayout_ = nullptr;
    int shapeKind_ = 0; ///< document::ShapeKind value
    bool shapeFill_ = false;
    QActionGroup* shapeKindGroup_ = nullptr;
    QAction* shapeFillAction_ = nullptr;
    /// Eraser mode (canvas::EraserMode), remembered per user like the ink styles.
    bool eraseWholeStrokes_ = false;
    QActionGroup* eraserModeGroup_ = nullptr;
    QMenu* eraserMenu_ = nullptr;
    /// The toolbar's style button: the style of the ink tool chosen last (pen/highlighter).
    /// A plain button with QToolButton::setMenu: QAction::setMenu would make the style
    /// menus' own entries in the Tools menu point at the button.
    QToolButton* inkStyleButton_ = nullptr;
    /// Whose style the button shows: canvas::ToolKind Pen, Highlighter or Shape.
    int styleShown_ = 0;
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
