#include <studyapp/ui/MainWindow.hpp>

#include "CanvasWidget.hpp"
#include "NavigationPanel.hpp"
#include "SessionDocumentPort.hpp"
#include "ThemeIcons.hpp"
#include "WorkspaceTreeModel.hpp"

#include <studyapp/application/ComponentVersions.hpp>
#include <studyapp/application/PageNavigator.hpp>
#include <studyapp/application/StartPage.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/ui/AppIcon.hpp>
#include <studyapp/ui/CanvasPlaceholder.hpp>
#include <studyapp/ui/ShellDialogs.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <string_view>
#include <unordered_map>

namespace studyapp::ui {

using application::HierarchyItem;
using application::WorkspaceStructure;

namespace {

const QString kGeometryKey = QStringLiteral("mainWindow/geometry");
const QString kStateKey = QStringLiteral("mainWindow/state");
const QString kSplitterKey = QStringLiteral("mainWindow/navigationSplitter");
const QString kNavigationKey = QStringLiteral("mainWindow/navigationVisible");
const QString kThemeKey = QStringLiteral("appearance/theme");
const QString kHudKey = QStringLiteral("canvas/debugHud");
const QString kRecentKey = QStringLiteral("workspaces/recent");
const QString kLastKey = QStringLiteral("workspaces/last");
constexpr int kMaxRecent = 8;

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString pathText(const std::filesystem::path& path) {
    return QDir::toNativeSeparators(QString::fromStdU16String(path.u16string()));
}

std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.toStdU16String());
}

QString normalized(const std::filesystem::path& root) {
    return QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(pathText(root)).absoluteFilePath()));
}

QString errorText(const core::Error& error) {
    return toQString(error.message);
}

/// The same directory, also when spelled differently (case on Windows/macOS, links).
bool sameDirectory(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec) {
        return true;
    }
    return normalized(a) == normalized(b);
}

} // namespace

/// Everything that belongs to one open workspace; recreated when another one is opened.
struct MainWindow::OpenWorkspace {
    struct View {
        core::DVec2 center;
        double zoom = 1.0;
    };

    OpenWorkspace(application::WorkspaceSession& shown,
                  std::unique_ptr<application::WorkspaceSession> ownedSession,
                  const core::Clock& shellClock, core::IdGenerator& shellIds)
        : owned(std::move(ownedSession)), session(&shown), clock(&shellClock), ids(&shellIds),
          port(std::make_unique<SessionDocumentPort>(shown)),
          controller(std::make_unique<canvas::CanvasController>(*port, shellIds)),
          navigator(shown.workspace()), structure(shown, shellClock, shellIds) {}

    std::unique_ptr<application::WorkspaceSession> owned; ///< null when borrowed
    application::WorkspaceSession* session;
    const core::Clock* clock;
    core::IdGenerator* ids;
    std::unique_ptr<SessionDocumentPort> port;
    std::unique_ptr<canvas::CanvasController> controller;
    application::PageNavigator navigator;
    WorkspaceStructure structure;
    /// Where the user left each page in this session (not persisted).
    std::unordered_map<core::PageId, View> views;
};

// ---------------------------------------------------------------------------- construction

MainWindow::MainWindow(ThemeManager& themes, QSettings& settings, QWidget* parent)
    : QMainWindow(parent), themes_(&themes), settings_(&settings), dialogs_(makeQtShellDialogs()) {
    setUp();
}

MainWindow::MainWindow(ThemeManager& themes, QSettings& settings, const ShellServices& services,
                       std::unique_ptr<ShellDialogs> dialogs, QWidget* parent)
    : QMainWindow(parent), themes_(&themes), settings_(&settings), services_(services),
      dialogs_(dialogs ? std::move(dialogs) : makeQtShellDialogs()) {
    setUp();
}

MainWindow::MainWindow(ThemeManager& themes, QSettings& settings, const WorkspaceContext& workspace,
                       QWidget* parent)
    : QMainWindow(parent), themes_(&themes), settings_(&settings), dialogs_(makeQtShellDialogs()) {
    setUp();
    attach(
        std::make_unique<OpenWorkspace>(workspace.session, nullptr, workspace.clock, workspace.ids),
        workspace.page);
}

MainWindow::~MainWindow() {
    detach(true); // best effort: pending writes are flushed; failures are logged
}

void MainWindow::setUp() {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowIcon(applicationIcon());
    resize(1200, 760);

    createActions();
    createMenus();
    createToolBar();
    createCentralWidget();
    createStatusBar();
    readSettings();

    connect(themes_, &ThemeManager::themeChanged, this, &MainWindow::syncThemeActions);
    connect(themes_, &ThemeManager::themeChanged, this, &MainWindow::applyCanvasTheme);
    syncThemeActions();
    applyCanvasTheme();
    updateRecentMenu();
    updateActions();
    updateStatus();
}

void MainWindow::createActions() {
    const auto make = [this](const QString& text, const QString& name,
                             const QKeySequence& key = {}) {
        auto* action = new QAction(text, this);
        action->setObjectName(name);
        if (!key.isEmpty()) {
            action->setShortcut(key);
        }
        return action;
    };

    // ---- File
    newWorkspaceAction_ = make(tr("&New Workspace…"), QStringLiteral("actionNewWorkspace"));
    newWorkspaceAction_->setStatusTip(tr("Create a new workspace folder"));
    connect(newWorkspaceAction_, &QAction::triggered, this, &MainWindow::newWorkspace);
    openWorkspaceAction_ =
        make(tr("&Open Workspace…"), QStringLiteral("actionOpenWorkspace"), QKeySequence::Open);
    connect(openWorkspaceAction_, &QAction::triggered, this, &MainWindow::openWorkspaceDialog);
    closeWorkspaceAction_ = make(tr("&Close Workspace"), QStringLiteral("actionCloseWorkspace"),
                                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    connect(closeWorkspaceAction_, &QAction::triggered, this, [this] {
        if (closeWorkspace()) {
            settings_->remove(kLastKey); // closed on purpose: start on the welcome screen
        }
    });
    // Changes are saved continuously; Save only makes sure nothing is pending.
    saveAction_ = make(tr("&Save"), QStringLiteral("actionSave"), QKeySequence::Save);
    saveAction_->setStatusTip(tr("Changes are saved automatically; this writes anything pending"));
    connect(saveAction_, &QAction::triggered, this, [this] {
        if (open_) {
            if (auto flushed = open_->session->flush(); !flushed) {
                reportFailure(tr("Some changes could not be saved yet. They are kept and saved "
                                 "with the next change."),
                              errorText(flushed.error()));
            }
            updateStatus();
        }
    });
    quitAction_ = make(tr("E&xit"), QStringLiteral("actionQuit"));
    quitAction_->setShortcuts(QKeySequence::Quit);
    quitAction_->setMenuRole(QAction::QuitRole);
    connect(quitAction_, &QAction::triggered, this, &QWidget::close);

    // ---- Edit
    undoAction_ = make(tr("&Undo"), QStringLiteral("actionUndo"), QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, &MainWindow::undo);
    redoAction_ = make(tr("&Redo"), QStringLiteral("actionRedo"));
    // Ctrl+Y everywhere, plus the platform's Redo keys. A key registered twice on one
    // action is ambiguous to Qt and fires nothing (QKeySequence::Redo is Ctrl+Y on Windows).
    QList<QKeySequence> redoKeys = QKeySequence::keyBindings(QKeySequence::Redo);
    if (!redoKeys.contains(QKeySequence(Qt::CTRL | Qt::Key_Y))) {
        redoKeys.append(QKeySequence(Qt::CTRL | Qt::Key_Y));
    }
    redoAction_->setShortcuts(redoKeys);
    connect(redoAction_, &QAction::triggered, this, &MainWindow::redo);
    // Canvas edits: their shortcuts work while the canvas has focus (the tree has its own
    // Delete for pages, sections and notebooks).
    deleteAction_ = make(tr("&Delete"), QStringLiteral("actionDelete"), QKeySequence::Delete);
    deleteAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteAction_, &QAction::triggered, this, [this] {
        if (open_) {
            (void)open_->controller->deleteSelection();
        }
    });
    selectAllAction_ =
        make(tr("Select &All"), QStringLiteral("actionSelectAll"), QKeySequence::SelectAll);
    selectAllAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(selectAllAction_, &QAction::triggered, this, [this] {
        if (!open_) {
            return;
        }
        open_->controller->setTool(canvas::ToolKind::Select);
        for (QAction* action : toolGroup_->actions()) {
            action->setChecked(action->data().toInt() ==
                               static_cast<int>(canvas::ToolKind::Select));
        }
        open_->controller->selectAll();
        if (canvasWidget_ != nullptr) {
            canvasWidget_->refreshCursor();
        }
    });

    // ---- Notebook (structure)
    newPageAction_ = make(tr("New &Page"), QStringLiteral("actionNewPage"), QKeySequence::New);
    connect(newPageAction_, &QAction::triggered, this, &MainWindow::newPage);
    newSectionAction_ = make(tr("New &Section"), QStringLiteral("actionNewSection"),
                             QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(newSectionAction_, &QAction::triggered, this, &MainWindow::newSection);
    newNotebookAction_ = make(tr("New &Notebook"), QStringLiteral("actionNewNotebook"),
                              QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(newNotebookAction_, &QAction::triggered, this, &MainWindow::newNotebook);
    renameAction_ = make(tr("&Rename"), QStringLiteral("actionRename"), QKeySequence(Qt::Key_F2));
    connect(renameAction_, &QAction::triggered, this, &MainWindow::renameCurrent);
    deleteItemAction_ =
        make(tr("De&lete…"), QStringLiteral("actionDeleteItem"), QKeySequence::Delete);
    deleteItemAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut); // the tree
    connect(deleteItemAction_, &QAction::triggered, this, &MainWindow::deleteCurrent);
    moveUpAction_ = make(tr("Move &Up"), QStringLiteral("actionMoveUp"),
                         QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Up));
    connect(moveUpAction_, &QAction::triggered, this, [this] { moveCurrent(-1); });
    moveDownAction_ = make(tr("Move &Down"), QStringLiteral("actionMoveDown"),
                           QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Down));
    connect(moveDownAction_, &QAction::triggered, this, [this] { moveCurrent(+1); });
    previousPageAction_ = make(tr("Pre&vious Page"), QStringLiteral("actionPreviousPage"),
                               QKeySequence(Qt::CTRL | Qt::Key_PageUp));
    connect(previousPageAction_, &QAction::triggered, this, [this] {
        if (open_) {
            if (const auto page = open_->navigator.previousPage()) {
                openPage(*page);
            }
        }
    });
    nextPageAction_ = make(tr("Ne&xt Page"), QStringLiteral("actionNextPage"),
                           QKeySequence(Qt::CTRL | Qt::Key_PageDown));
    connect(nextPageAction_, &QAction::triggered, this, [this] {
        if (open_) {
            if (const auto page = open_->navigator.nextPage()) {
                openPage(*page);
            }
        }
    });

    // ---- Tools
    toolGroup_ = new QActionGroup(this);
    toolGroup_->setExclusive(true);
    const auto makeTool = [&](const QString& text, const QString& name, Qt::Key key,
                              canvas::ToolKind tool) {
        QAction* action = make(text, name, QKeySequence(key));
        action->setCheckable(true);
        action->setData(static_cast<int>(tool));
        action->setActionGroup(toolGroup_);
        connect(action, &QAction::triggered, this, [this, tool] {
            if (!open_) {
                return;
            }
            open_->controller->setTool(tool);
            if (canvasWidget_ != nullptr) {
                canvasWidget_->refreshCursor(); // e.g. the eraser ring, without a mouse move
            }
        });
        return action;
    };
    makeTool(tr("&Pen"), QStringLiteral("actionToolPen"), Qt::Key_P, canvas::ToolKind::Pen)
        ->setChecked(true);
    makeTool(tr("&Select"), QStringLiteral("actionToolSelect"), Qt::Key_V,
             canvas::ToolKind::Select);
    makeTool(tr("&Eraser"), QStringLiteral("actionToolEraser"), Qt::Key_E,
             canvas::ToolKind::Eraser);
    makeTool(tr("Pa&n"), QStringLiteral("actionToolPan"), Qt::Key_H, canvas::ToolKind::Pan);
    makeTool(tr("&Zoom"), QStringLiteral("actionToolZoom"), Qt::Key_Z, canvas::ToolKind::Zoom);

    // ---- View
    navigationAction_ = make(tr("&Navigation"), QStringLiteral("actionNavigation"),
                             QKeySequence(Qt::CTRL | Qt::Key_Backslash));
    navigationAction_->setCheckable(true);
    navigationAction_->setChecked(true);
    connect(navigationAction_, &QAction::toggled, this, [this](bool visible) {
        if (navigation_ != nullptr) {
            navigation_->setVisible(visible);
        }
    });

    themeGroup_ = new QActionGroup(this);
    themeGroup_->setExclusive(true);
    const auto makeThemeAction = [this](const QString& text, const QString& name, ThemeMode mode) {
        auto* action = new QAction(text, themeGroup_);
        action->setObjectName(name);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, mode] { themes_->setMode(mode); });
        return action;
    };
    themeSystemAction_ = makeThemeAction(tr("Follow &System"), QStringLiteral("actionThemeSystem"),
                                         ThemeMode::System);
    themeLightAction_ =
        makeThemeAction(tr("&Light"), QStringLiteral("actionThemeLight"), ThemeMode::Light);
    themeDarkAction_ =
        makeThemeAction(tr("&Dark"), QStringLiteral("actionThemeDark"), ThemeMode::Dark);
    toggleThemeAction_ = make(tr("Switch Light/Dark"), QStringLiteral("actionToggleTheme"),
                              QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    connect(toggleThemeAction_, &QAction::triggered, themes_, &ThemeManager::toggleLightDark);

    zoomInAction_ = make(tr("Zoom &In"), QStringLiteral("actionZoomIn"), QKeySequence::ZoomIn);
    connect(zoomInAction_, &QAction::triggered, this, [this] {
        if (open_) {
            open_->controller->zoomBy(1.25);
        }
    });
    zoomOutAction_ = make(tr("Zoom &Out"), QStringLiteral("actionZoomOut"), QKeySequence::ZoomOut);
    connect(zoomOutAction_, &QAction::triggered, this, [this] {
        if (open_) {
            open_->controller->zoomBy(0.8);
        }
    });
    resetViewAction_ = make(tr("&Reset View"), QStringLiteral("actionResetView"),
                            QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(resetViewAction_, &QAction::triggered, this, [this] {
        if (open_) {
            open_->controller->resetView();
        }
    });
    fitAction_ = make(tr("Zoom to &Fit"), QStringLiteral("actionZoomToFit"),
                      QKeySequence(Qt::CTRL | Qt::Key_1));
    connect(fitAction_, &QAction::triggered, this, [this] {
        if (open_) {
            open_->controller->zoomToFit();
        }
    });
    hudAction_ = make(tr("Debug &HUD"), QStringLiteral("actionDebugHud"), QKeySequence(Qt::Key_F3));
    hudAction_->setCheckable(true);
    connect(hudAction_, &QAction::toggled, this, [this](bool on) {
        if (canvasWidget_ != nullptr) {
            canvasWidget_->setHudVisible(on);
        }
    });

    // Page format of the open page.
    backgroundGroup_ = new QActionGroup(this);
    const std::pair<QString, document::BackgroundPattern> patterns[] = {
        {tr("&Blank"), document::BackgroundPattern::None},
        {tr("&Ruled"), document::BackgroundPattern::Ruled},
        {tr("&Grid"), document::BackgroundPattern::Grid},
        {tr("&Dots"), document::BackgroundPattern::Dots},
    };
    for (const auto& [text, pattern] : patterns) {
        auto* action = new QAction(text, backgroundGroup_);
        action->setCheckable(true);
        action->setData(static_cast<int>(pattern));
        connect(action, &QAction::triggered, this, [this, pattern] {
            const auto page = activePage();
            const document::PageInfo* info =
                page ? open_->session->workspace().findPage(*page) : nullptr;
            setPageFormat(static_cast<int>(pattern),
                          info != nullptr && info->extent == document::PageExtent::Bounded);
        });
    }
    extentGroup_ = new QActionGroup(this);
    auto* infinite = new QAction(tr("&Infinite Page"), extentGroup_);
    infinite->setCheckable(true);
    infinite->setData(0);
    auto* a4 = new QAction(tr("&A4 Portrait Page"), extentGroup_);
    a4->setCheckable(true);
    a4->setData(1);
    for (QAction* action : extentGroup_->actions()) {
        connect(action, &QAction::triggered, this, [this, bounded = action->data().toInt() == 1] {
            const auto page = activePage();
            const document::PageInfo* info =
                page ? open_->session->workspace().findPage(*page) : nullptr;
            setPageFormat(info != nullptr ? static_cast<int>(info->background.pattern) : 0,
                          bounded);
            if (open_) {
                open_->controller->resetView();
            }
        });
    }

    // ---- Help
    aboutAction_ = make(tr("&About %1").arg(toQString(core::build::kProductName)),
                        QStringLiteral("actionAbout"));
    aboutAction_->setMenuRole(QAction::AboutRole);
    connect(aboutAction_, &QAction::triggered, this, &MainWindow::showAbout);
    aboutQtAction_ = make(tr("About &Qt"), QStringLiteral("actionAboutQt"));
    aboutQtAction_->setMenuRole(QAction::AboutQtRole);
    connect(aboutQtAction_, &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::createMenus() {
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("menuFile"));
    fileMenu->addAction(newWorkspaceAction_);
    fileMenu->addAction(openWorkspaceAction_);
    recentMenu_ = fileMenu->addMenu(tr("Open &Recent"));
    recentMenu_->setObjectName(QStringLiteral("menuRecent"));
    fileMenu->addAction(closeWorkspaceAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(saveAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction_);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->setObjectName(QStringLiteral("menuEdit"));
    editMenu->addAction(undoAction_);
    editMenu->addAction(redoAction_);
    editMenu->addSeparator();
    editMenu->addAction(deleteAction_);
    editMenu->addAction(selectAllAction_);

    QMenu* notebookMenu = menuBar()->addMenu(tr("&Notebook"));
    notebookMenu->setObjectName(QStringLiteral("menuNotebook"));
    notebookMenu->addAction(newPageAction_);
    notebookMenu->addAction(newSectionAction_);
    notebookMenu->addAction(newNotebookAction_);
    notebookMenu->addSeparator();
    notebookMenu->addAction(renameAction_);
    notebookMenu->addAction(deleteItemAction_);
    notebookMenu->addAction(moveUpAction_);
    notebookMenu->addAction(moveDownAction_);
    notebookMenu->addSeparator();
    notebookMenu->addAction(previousPageAction_);
    notebookMenu->addAction(nextPageAction_);

    QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("menuTools"));
    toolsMenu->addActions(toolGroup_->actions());

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("menuView"));
    viewMenu->addAction(navigationAction_);
    viewMenu->addSeparator();
    QMenu* themeMenu = viewMenu->addMenu(tr("&Theme"));
    themeMenu->setObjectName(QStringLiteral("menuTheme"));
    themeMenu->addActions(themeGroup_->actions());
    themeMenu->addSeparator();
    themeMenu->addAction(toggleThemeAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(zoomInAction_);
    viewMenu->addAction(zoomOutAction_);
    viewMenu->addAction(resetViewAction_);
    viewMenu->addAction(fitAction_);
    viewMenu->addSeparator();
    QMenu* pageMenu = viewMenu->addMenu(tr("&Page"));
    pageMenu->setObjectName(QStringLiteral("menuPage"));
    pageMenu->addActions(backgroundGroup_->actions());
    pageMenu->addSeparator();
    pageMenu->addActions(extentGroup_->actions());
    viewMenu->addSeparator();
    viewMenu->addAction(hudAction_);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setObjectName(QStringLiteral("menuHelp"));
    helpMenu->addAction(aboutAction_);
    helpMenu->addAction(aboutQtAction_);
}

void MainWindow::createToolBar() {
    QToolBar* toolBar = addToolBar(tr("Main"));
    toolBar->setObjectName(QStringLiteral("mainToolBar"));
    toolBar->setMovable(false);
    toolBar->addActions(toolGroup_->actions());
    toolBar->addSeparator();
    toolBar->addAction(undoAction_);
    toolBar->addAction(redoAction_);
    // The compact theme toggle sits at the far right.
    auto* spacer = new QWidget(toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolBar->addWidget(spacer);
    toolBar->addAction(toggleThemeAction_);
    if (QWidget* button = toolBar->widgetForAction(toggleThemeAction_)) {
        button->setObjectName(QStringLiteral("themeToggleButton"));
    }
}

void MainWindow::createCentralWidget() {
    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("centralStack"));

    welcome_ = new CanvasPlaceholder(*themes_, stack_);
    welcome_->setActionsAvailable(services_.has_value());
    connect(welcome_, &CanvasPlaceholder::newWorkspaceRequested, this, &MainWindow::newWorkspace);
    connect(welcome_, &CanvasPlaceholder::openWorkspaceRequested, this,
            &MainWindow::openWorkspaceDialog);
    connect(welcome_, &CanvasPlaceholder::openRecentRequested, this,
            [this](const QString& path) { (void)openWorkspace(toPath(path)); });
    stack_->addWidget(welcome_);

    treeModel_ = new WorkspaceTreeModel(this);
    shell_ = new QSplitter(Qt::Horizontal, stack_);
    shell_->setObjectName(QStringLiteral("shellSplitter"));
    shell_->setChildrenCollapsible(false);
    navigation_ = new NavigationPanel(*treeModel_, shell_);
    navigation_->setMinimumWidth(160);
    navigation_->newMenu()->addAction(newPageAction_);
    navigation_->newMenu()->addAction(newSectionAction_);
    navigation_->newMenu()->addAction(newNotebookAction_);
    navigation_->tree()->addAction(deleteItemAction_); // its Delete shortcut lives here
    canvasArea_ = new QWidget(shell_);
    canvasArea_->setObjectName(QStringLiteral("canvasArea"));
    auto* canvasLayout = new QVBoxLayout(canvasArea_);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    shell_->addWidget(navigation_);
    shell_->addWidget(canvasArea_);
    shell_->setStretchFactor(0, 0);
    shell_->setStretchFactor(1, 1);
    shell_->setSizes({240, 960});
    stack_->addWidget(shell_);
    setCentralWidget(stack_);

    QTreeView* tree = navigation_->tree();
    connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onTreeCurrentChanged);
    connect(tree, &QTreeView::customContextMenuRequested, this, &MainWindow::showTreeContextMenu);

    treeModel_->setRenameHandler([this](const HierarchyItem& item, const QString& title) {
        if (!open_) {
            return false;
        }
        if (auto renamed = open_->structure.rename(item, title.trimmed().toStdString()); !renamed) {
            statusBar()->showMessage(tr("Not renamed: %1").arg(errorText(renamed.error())), 5000);
            return false;
        }
        return true;
    });
    treeModel_->setMoveHandler([this](const HierarchyItem& item,
                                      const std::optional<HierarchyItem>& parent,
                                      std::size_t index) {
        if (!open_) {
            return false;
        }
        core::Result<void> moved;
        if (const auto* page = std::get_if<core::PageId>(&item)) {
            moved = open_->structure.movePage(*page, std::get<core::SectionId>(*parent), index);
        } else if (const auto* section = std::get_if<core::SectionId>(&item)) {
            moved =
                open_->structure.moveSection(*section, std::get<core::NotebookId>(*parent), index);
        } else {
            moved = open_->structure.moveWithinParent(item, index);
        }
        if (!moved) {
            reportFailure(tr("The item could not be moved."), errorText(moved.error()));
            return false;
        }
        return true;
    });
}

void MainWindow::createStatusBar() {
    breadcrumbLabel_ = new QLabel(this);
    breadcrumbLabel_->setObjectName(QStringLiteral("breadcrumbLabel"));
    statusBar()->addWidget(breadcrumbLabel_, 1);
    saveStatusLabel_ = new QLabel(this);
    saveStatusLabel_->setObjectName(QStringLiteral("saveStatusLabel"));
    statusBar()->addPermanentWidget(saveStatusLabel_);
    auto* versionLabel =
        new QLabel(QStringLiteral("%1 %2").arg(toQString(core::build::kProductName),
                                               toQString(core::build::kVersion)),
                   this);
    versionLabel->setObjectName(QStringLiteral("versionLabel"));
    statusBar()->addPermanentWidget(versionLabel);
}

// ---------------------------------------------------------------------------- settings

void MainWindow::readSettings() {
    themes_->setMode(themeModeFromSettingsValue(settings_->value(kThemeKey).toString()));
    restoreGeometry(settings_->value(kGeometryKey).toByteArray());
    restoreState(settings_->value(kStateKey).toByteArray());
    shell_->restoreState(settings_->value(kSplitterKey).toByteArray());
    navigationAction_->setChecked(settings_->value(kNavigationKey, true).toBool());
    hudAction_->setChecked(settings_->value(kHudKey, false).toBool());
}

void MainWindow::writeSettings() {
    settings_->setValue(kThemeKey, toSettingsValue(themes_->mode()));
    settings_->setValue(kGeometryKey, saveGeometry());
    settings_->setValue(kStateKey, saveState());
    settings_->setValue(kSplitterKey, shell_->saveState());
    settings_->setValue(kNavigationKey, navigationAction_->isChecked());
    settings_->setValue(kHudKey, hudAction_->isChecked());
    settings_->sync();
}

QStringList MainWindow::recentWorkspaces() const {
    return settings_->value(kRecentKey).toStringList();
}

std::optional<std::filesystem::path> MainWindow::lastWorkspace() const {
    const QString last = settings_->value(kLastKey).toString();
    return last.isEmpty() ? std::nullopt : std::optional(toPath(last));
}

void MainWindow::setRememberWorkspaces(bool remember) {
    rememberWorkspaces_ = remember;
}

void MainWindow::forgetIfLast(const std::filesystem::path& root) {
    if (const auto last = lastWorkspace(); last && sameDirectory(*last, root)) {
        settings_->remove(kLastKey); // it failed to open: do not retry at every start
    }
}

void MainWindow::rememberWorkspace(const std::filesystem::path& root) {
    if (!rememberWorkspaces_) {
        return;
    }
    const QString path = normalized(root);
    QStringList recent = recentWorkspaces();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecent) {
        recent.removeLast();
    }
    settings_->setValue(kRecentKey, recent);
    settings_->setValue(kLastKey, path);
    // Deferred: this may run from a recent-menu action or welcome button that the
    // rebuild deletes.
    QTimer::singleShot(0, this, &MainWindow::updateRecentMenu);
}

void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    const QStringList recent = recentWorkspaces();
    for (const QString& path : recent) {
        QAction* action = recentMenu_->addAction(QFileInfo(path).completeBaseName());
        action->setToolTip(path);
        action->setStatusTip(path);
        // A vanished workspace is reported and dropped from the list by openWorkspace().
        connect(action, &QAction::triggered, this,
                [this, path] { (void)openWorkspace(toPath(path)); });
    }
    recentMenu_->setEnabled(services_.has_value() && !recent.isEmpty());
    welcome_->setRecentWorkspaces(recent);
}

// ---------------------------------------------------------------------------- workspaces

bool MainWindow::hasWorkspace() const noexcept {
    return open_ != nullptr;
}

application::WorkspaceSession* MainWindow::session() const noexcept {
    return open_ ? open_->session : nullptr;
}

bool MainWindow::openWorkspace(const std::filesystem::path& root, OpenMode mode) {
    if (!services_) {
        return false;
    }
    if (open_ && sameDirectory(open_->session->root(), root)) {
        return true; // already open
    }
    if (!application::WorkspaceSession::isWorkspace(root)) {
        if (mode == OpenMode::CreateIfMissing) {
            return createWorkspace(root);
        }
        reportFailure(tr("“%1” is not a StudyBoard workspace.").arg(pathText(root)), {});
        forgetIfLast(root);
        QStringList recent = recentWorkspaces(); // a vanished recent entry goes away
        if (recent.removeAll(normalized(root)) > 0) {
            settings_->setValue(kRecentKey, recent);
            QTimer::singleShot(0, this, &MainWindow::updateRecentMenu);
        }
        return false;
    }

    // Locks (docs/ARCHITECTURE.md §11.1): in use elsewhere → offer read-only; left behind by
    // a crash → offer to check and continue, or read-only.
    application::OpenOptions options;
    if (auto lock = application::WorkspaceSession::inspectLock(root, services_->locker)) {
        const QString owner = lock->owner ? tr("%1, process %2 on %3")
                                                .arg(toQString(lock->owner->applicationName))
                                                .arg(lock->owner->processId)
                                                .arg(toQString(lock->owner->hostName))
                                          : QString();
        if (lock->state == application::LockState::Active) {
            if (!dialogs_->confirmOpenReadOnly(this, owner)) {
                return false;
            }
            options.mode = application::AccessMode::ReadOnly;
        } else if (lock->state == application::LockState::Stale) {
            switch (dialogs_->askStaleLock(this, owner)) {
            case ShellDialogs::StaleLockChoice::Recover:
                options.recoverStaleLock = true;
                break;
            case ShellDialogs::StaleLockChoice::ReadOnly:
                options.mode = application::AccessMode::ReadOnly;
                break;
            case ShellDialogs::StaleLockChoice::Cancel:
                return false;
            }
        }
    }
    auto opened = application::WorkspaceSession::open(
        root, options,
        {.clock = services_->clock, .ids = services_->ids, .locker = services_->locker});
    if (!opened) {
        reportFailure(tr("The workspace “%1” could not be opened.").arg(pathText(root)),
                      errorText(opened.error()));
        forgetIfLast(root);
        return false;
    }
    // The new workspace is open; now let go of the previous one.
    if (!closeWorkspace()) {
        return false; // the user kept the previous one (unsaved changes); the new one closes
    }
    // Opening never writes: an empty workspace opens on an empty canvas (New Page etc. are
    // available); only a new workspace gets a start page.
    application::WorkspaceSession& session = **opened;
    attach(std::make_unique<OpenWorkspace>(session, std::move(*opened), services_->clock,
                                           services_->ids),
           application::firstPage(session.workspace()));
    rememberWorkspace(root);
    return true;
}

bool MainWindow::createWorkspace(const std::filesystem::path& root) {
    if (!services_) {
        return false;
    }
    const std::u8string stem = root.stem().u8string(); // "My Notes.studyws" → "My Notes"
    std::string name(stem.begin(), stem.end());
    if (QString::fromStdString(name).trimmed().isEmpty()) {
        name = "My Workspace";
    }
    auto created = application::WorkspaceSession::create(
        root, name,
        {.clock = services_->clock, .ids = services_->ids, .locker = services_->locker});
    if (!created) {
        reportFailure(tr("The workspace “%1” could not be created.").arg(pathText(root)),
                      errorText(created.error()));
        return false;
    }
    if (!closeWorkspace()) {
        return false;
    }
    application::WorkspaceSession& session = **created;
    std::optional<core::PageId> start;
    if (auto page = application::ensureStartPage(session, services_->clock, services_->ids)) {
        start = *page; // a new workspace starts with Notebook › Notes › Page 1
    }
    attach(std::make_unique<OpenWorkspace>(session, std::move(*created), services_->clock,
                                           services_->ids),
           start);
    rememberWorkspace(root);
    return true;
}

bool MainWindow::closeWorkspace() {
    if (!open_) {
        return true;
    }
    if (open_->owned) {
        // Writes are continuous; this only catches what a failed write left pending
        // (P3-01): never close silently over unsaved changes.
        for (;;) {
            auto flushed = open_->session->flush();
            if (flushed) {
                break;
            }
            switch (dialogs_->askUnsavedChanges(this, open_->session->pendingWriteCount(),
                                                errorText(flushed.error()))) {
            case ShellDialogs::UnsavedChoice::Retry:
                continue;
            case ShellDialogs::UnsavedChoice::Cancel:
                updateStatus();
                return false;
            case ShellDialogs::UnsavedChoice::Discard:
                core::logError("ui", "closing without saving " +
                                         std::to_string(open_->session->pendingWriteCount()) +
                                         " change(s): " + flushed.error().message);
                break;
            }
            break;
        }
    }
    detach(true);
    return true;
}

void MainWindow::attach(std::unique_ptr<OpenWorkspace> workspace,
                        std::optional<core::PageId> page) {
    open_ = std::move(workspace);
    application::WorkspaceSession& session = *open_->session;
    // Every applied patch — canvas edits, structure edits, undo and redo — keeps the
    // canvas, the tree and the active page in step with the document.
    session.setPatchListener([this](const document::Patch& patch) { onPatchApplied(patch); });

    treeModel_->setWorkspace(&session.workspace());
    treeModel_->setReadOnly(session.isReadOnly());
    navigation_->setWorkspaceName(toQString(session.workspace().info().name));

    canvasWidget_ = new CanvasWidget(*open_->controller, canvasArea_);
    canvasArea_->layout()->addWidget(canvasWidget_);
    canvasWidget_->setHudVisible(hudAction_->isChecked());
    canvasWidget_->addAction(deleteAction_); // canvas-only shortcuts
    canvasWidget_->addAction(selectAllAction_);
    applyCanvasTheme();
    if (QAction* checked = toolGroup_->checkedAction()) {
        open_->controller->setTool(static_cast<canvas::ToolKind>(checked->data().toInt()));
    }

    if (!page) {
        page = application::firstPage(session.workspace());
    }
    if (page) {
        (void)open_->navigator.open(*page);
    }
    stack_->setCurrentWidget(shell_);
    showActivePage();
    navigation_->tree()->expandToDepth(0); // notebooks open, sections as they were
    syncTreeToActivePage();
    canvasWidget_->setFocus();
    updateActions();
    updateStatus();
}

void MainWindow::detach(bool closeSession) {
    if (!open_) {
        return;
    }
    open_->session->setPatchListener({});
    treeModel_->setWorkspace(nullptr);
    // The canvas widget refers to the controller: destroy it while the controller exists.
    delete canvasWidget_;
    canvasWidget_ = nullptr;
    if (closeSession && open_->owned) {
        if (auto closed = open_->owned->close(); !closed) {
            core::logError("ui", "closing the workspace: " + closed.error().message);
        }
    }
    open_.reset();
    revealedPage_.reset();
    revealedSection_.reset();
    if (stack_ != nullptr) {
        stack_->setCurrentWidget(welcome_);
    }
    navigation_->setWorkspaceName({});
    updateActions();
    updateStatus();
}

void MainWindow::newWorkspace() {
    if (!services_) {
        return;
    }
    if (const auto root = dialogs_->chooseNewWorkspace(this)) {
        (void)createWorkspace(*root);
    }
}

void MainWindow::openWorkspaceDialog() {
    if (!services_) {
        return;
    }
    if (const auto root = dialogs_->chooseWorkspaceToOpen(this)) {
        (void)openWorkspace(*root);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    writeSettings();
    if (!closeWorkspace()) {
        event->ignore(); // unsaved changes and the user kept the workspace open
        return;
    }
    event->accept();
}

// ---------------------------------------------------------------------------- document

void MainWindow::onPatchApplied(const document::Patch& patch) {
    // While the tree model mirrors the patch, the view may move its current row (e.g.
    // off a removed page); that is not the user choosing a page — the navigator decides.
    applyingPatch_ = true;
    open_->controller->onDocumentChanged(patch);
    treeModel_->onPatch(patch);
    const bool pageChanged = open_->navigator.onPatch(patch);
    applyingPatch_ = false;
    if (pageChanged) {
        showActivePage(); // the open page was deleted: its neighbour opens
    }
    syncTreeToActivePage();
    updateActions();
    updateTitle();          // titles may have changed
    scheduleStatusUpdate(); // the save state: after the write that follows this notification
}

std::optional<core::PageId> MainWindow::activePage() const {
    return open_ ? open_->navigator.activePage() : std::nullopt;
}

bool MainWindow::openPage(core::PageId page) {
    if (!open_) {
        return false;
    }
    if (activePage() == page) {
        syncTreeToActivePage();
        return true;
    }
    rememberView();
    if (auto opened = open_->navigator.open(page); !opened) {
        reportFailure(tr("The page could not be opened."), errorText(opened.error()));
        return false;
    }
    showActivePage();
    syncTreeToActivePage();
    return true;
}

void MainWindow::rememberView() {
    if (const auto page = activePage()) {
        const canvas::Camera& camera = open_->controller->camera();
        open_->views[*page] = {.center = camera.center(), .zoom = camera.zoom()};
    }
}

void MainWindow::showActivePage() {
    const auto page = activePage();
    open_->controller->setPage(page);
    if (page) {
        if (const auto view = open_->views.find(*page); view != open_->views.end()) {
            open_->controller->setView(view->second.center, view->second.zoom);
        }
    }
    treeModel_->setActivePage(page);
    updateActions();
    updateStatus();
}

void MainWindow::revealPage(std::optional<core::PageId> page) {
    if (page && page != activePage()) {
        openPage(*page);
    }
}

void MainWindow::syncTreeToActivePage() {
    if (!open_) {
        return;
    }
    const auto page = activePage();
    if (!page) {
        return;
    }
    QTreeView* tree = navigation_->tree();
    const QModelIndex index = treeModel_->indexOf(HierarchyItem{*page});
    if (!index.isValid()) {
        return;
    }
    // The open page is revealed when it changes or moves (also into a collapsed section);
    // collapsing its section afterwards is respected.
    const document::PageInfo* info = open_->session->workspace().findPage(*page);
    const std::optional<core::SectionId> section =
        info != nullptr ? std::optional(info->section) : std::nullopt;
    if (page != revealedPage_ || section != revealedSection_) {
        for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
            tree->expand(parent);
        }
        revealedPage_ = page;
        revealedSection_ = section;
    }
    if (tree->currentIndex() == index) {
        return;
    }
    // Only follow the page while the user is not working on another item in the tree.
    const std::optional<HierarchyItem> current = treeModel_->itemAt(tree->currentIndex());
    if (current && !std::holds_alternative<core::PageId>(*current) &&
        WorkspaceStructure::exists(open_->session->workspace(), *current)) {
        return;
    }
    syncingTree_ = true;
    tree->scrollTo(index);
    tree->setCurrentIndex(index);
    syncingTree_ = false;
}

void MainWindow::onTreeCurrentChanged() {
    if (syncingTree_ || applyingPatch_ || !open_) {
        return;
    }
    const std::optional<HierarchyItem> item =
        treeModel_->itemAt(navigation_->tree()->currentIndex());
    if (item) {
        if (const auto* page = std::get_if<core::PageId>(&*item)) {
            openPage(*page);
        }
    }
    updateActions();
}

void MainWindow::showTreeContextMenu(const QPoint& position) {
    if (!open_) {
        return;
    }
    QTreeView* tree = navigation_->tree();
    if (const QModelIndex index = tree->indexAt(position); index.isValid()) {
        tree->setCurrentIndex(index);
    }
    QMenu menu(this);
    menu.addAction(newPageAction_);
    menu.addAction(newSectionAction_);
    menu.addAction(newNotebookAction_);
    menu.addSeparator();
    menu.addAction(renameAction_);
    menu.addAction(deleteItemAction_);
    menu.addSeparator();
    menu.addAction(moveUpAction_);
    menu.addAction(moveDownAction_);
    menu.exec(tree->viewport()->mapToGlobal(position));
}

// ---------------------------------------------------------------------------- structure

namespace {

/// The tree's current item, or the open page.
std::optional<HierarchyItem> currentItem(const WorkspaceTreeModel& model, const QTreeView& tree,
                                         std::optional<core::PageId> active) {
    if (auto item = model.itemAt(tree.currentIndex())) {
        return item;
    }
    return active ? std::optional<HierarchyItem>(*active) : std::nullopt;
}

} // namespace

void MainWindow::newPage() {
    if (!open_) {
        return;
    }
    const document::Workspace& ws = open_->session->workspace();
    // Into the current section: the current page's, the current section, or the first
    // section of the current notebook; with no section, a new section (or notebook).
    std::optional<core::SectionId> section;
    std::optional<core::NotebookId> notebook;
    if (const auto item = currentItem(*treeModel_, *navigation_->tree(), activePage())) {
        if (const auto* page = std::get_if<core::PageId>(&*item)) {
            section = ws.findPage(*page)->section;
        } else if (const auto* s = std::get_if<core::SectionId>(&*item)) {
            section = *s;
        } else {
            notebook = std::get<core::NotebookId>(*item);
            if (!ws.sectionsOf(*notebook).empty()) {
                section = ws.sectionsOf(*notebook).front();
            }
        }
    }
    if (!section) {
        if (notebook) {
            if (auto created = open_->structure.createSection(*notebook)) {
                openPage(created->page);
            } else {
                reportFailure(tr("The page could not be created."), errorText(created.error()));
            }
            return;
        }
        newNotebook();
        return;
    }
    if (auto page = open_->structure.createPage(*section)) {
        openPage(*page);
        navigation_->tree()->setCurrentIndex(treeModel_->indexOf(HierarchyItem{*page}));
    } else {
        reportFailure(tr("The page could not be created."), errorText(page.error()));
    }
}

void MainWindow::newSection() {
    if (!open_) {
        return;
    }
    const document::Workspace& ws = open_->session->workspace();
    std::optional<core::NotebookId> notebook;
    if (const auto item = currentItem(*treeModel_, *navigation_->tree(), activePage())) {
        if (const auto* page = std::get_if<core::PageId>(&*item)) {
            notebook = ws.findSection(ws.findPage(*page)->section)->notebook;
        } else if (const auto* section = std::get_if<core::SectionId>(&*item)) {
            notebook = ws.findSection(*section)->notebook;
        } else {
            notebook = std::get<core::NotebookId>(*item);
        }
    }
    if (!notebook) {
        newNotebook();
        return;
    }
    if (auto created = open_->structure.createSection(*notebook)) {
        openPage(created->page);
        navigation_->tree()->setCurrentIndex(treeModel_->indexOf(HierarchyItem{created->page}));
    } else {
        reportFailure(tr("The section could not be created."), errorText(created.error()));
    }
}

void MainWindow::newNotebook() {
    if (!open_) {
        return;
    }
    if (auto created = open_->structure.createNotebook()) {
        openPage(created->page);
        QTreeView* tree = navigation_->tree();
        tree->expand(treeModel_->indexOf(HierarchyItem{created->notebook}));
        tree->setCurrentIndex(treeModel_->indexOf(HierarchyItem{created->page}));
    } else {
        reportFailure(tr("The notebook could not be created."), errorText(created.error()));
    }
}

void MainWindow::renameCurrent() {
    if (!open_ || open_->session->isReadOnly()) {
        return;
    }
    const auto item = currentItem(*treeModel_, *navigation_->tree(), activePage());
    if (!item) {
        return;
    }
    navigationAction_->setChecked(true); // renaming happens in the tree
    QTreeView* tree = navigation_->tree();
    const QModelIndex index = treeModel_->indexOf(*item);
    tree->scrollTo(index);
    tree->setCurrentIndex(index);
    tree->setFocus();
    tree->edit(index);
}

void MainWindow::deleteCurrent() {
    if (!open_ || open_->session->isReadOnly()) {
        return;
    }
    const auto item = currentItem(*treeModel_, *navigation_->tree(), activePage());
    if (!item) {
        return;
    }
    const document::Workspace& ws = open_->session->workspace();
    const QString title = toQString(WorkspaceStructure::displayTitle(ws, *item));
    // Structure deletes are undoable, but take whole subtrees: they are confirmed, except
    // for an empty page.
    QString what;
    bool ask = true;
    if (const auto* notebook = std::get_if<core::NotebookId>(&*item)) {
        std::size_t pages = 0;
        for (const core::SectionId section : ws.sectionsOf(*notebook)) {
            pages += ws.pagesOf(section).size();
        }
        what =
            tr("the notebook “%1” and its %n page(s)", nullptr, static_cast<int>(pages)).arg(title);
    } else if (const auto* section = std::get_if<core::SectionId>(&*item)) {
        what = tr("the section “%1” and its %n page(s)", nullptr,
                  static_cast<int>(ws.pagesOf(*section).size()))
                   .arg(title);
    } else {
        const auto page = std::get<core::PageId>(*item);
        bool empty = true;
        for (const core::LayerId layer : ws.layersOf(page)) {
            empty = empty && ws.elementsOf(layer).empty();
        }
        what = tr("the page “%1”").arg(title);
        ask = !empty;
    }
    if (ask && !dialogs_->confirmDelete(this, what)) {
        return;
    }
    if (auto removed = open_->structure.remove(*item); !removed) {
        reportFailure(tr("%1 could not be deleted.").arg(title), errorText(removed.error()));
    }
}

void MainWindow::moveCurrent(int delta) {
    if (!open_) {
        return;
    }
    const auto item = currentItem(*treeModel_, *navigation_->tree(), activePage());
    if (!item) {
        return;
    }
    if (auto moved = open_->structure.moveBy(*item, delta); !moved) {
        reportFailure(tr("The item could not be moved."), errorText(moved.error()));
    }
}

void MainWindow::undo() {
    if (!open_ || open_->controller->isGestureActive()) {
        return;
    }
    const document::Command* next = open_->session->history().nextUndo();
    if (next == nullptr) {
        return;
    }
    const document::Patch applied = next->patch.inverted();
    if (auto undone = open_->session->undo(); !undone) {
        reportFailure(tr("The last change could not be undone."), errorText(undone.error()));
        return;
    }
    // One history for the whole workspace: show the page the undone edit was on.
    revealPage(application::pageChangedBy(applied, open_->session->workspace()));
}

void MainWindow::redo() {
    if (!open_ || open_->controller->isGestureActive()) {
        return;
    }
    const document::Command* next = open_->session->history().nextRedo();
    if (next == nullptr) {
        return;
    }
    const document::Patch applied = next->patch;
    if (auto redone = open_->session->redo(); !redone) {
        reportFailure(tr("The change could not be redone."), errorText(redone.error()));
        return;
    }
    revealPage(application::pageChangedBy(applied, open_->session->workspace()));
}

void MainWindow::setPageFormat(int backgroundPattern, bool bounded) {
    const auto page = activePage();
    if (!open_ || !page) {
        return;
    }
    const document::Workspace& workspace = open_->session->workspace();
    const document::PageInfo* info = workspace.findPage(*page);
    if (info == nullptr) {
        return;
    }
    document::commands::PageFormat format{.extent = bounded ? document::PageExtent::Bounded
                                                            : document::PageExtent::Infinite,
                                          .size = bounded ? document::kA4PortraitSize : info->size,
                                          .background = info->background};
    format.background.pattern = static_cast<document::BackgroundPattern>(backgroundPattern);
    auto command = document::commands::setPageFormat(workspace, *page, format, *open_->clock);
    core::Result<void> changed =
        command ? open_->session->execute(std::move(*command)) : tl::unexpected(command.error());
    if (!changed) {
        reportFailure(tr("The page format could not be changed."), errorText(changed.error()));
    }
    updateActions();
}

// ---------------------------------------------------------------------------- chrome

void MainWindow::syncThemeActions() {
    switch (themes_->mode()) {
    case ThemeMode::System:
        themeSystemAction_->setChecked(true);
        break;
    case ThemeMode::Light:
        themeLightAction_->setChecked(true);
        break;
    case ThemeMode::Dark:
        themeDarkAction_->setChecked(true);
        break;
    }
    // The compact toggle shows where it leads: a moon in the light theme, a sun in dark.
    const bool dark = themes_->isDark();
    toggleThemeAction_->setIcon(themeToggleIcon(dark, toQColor(themes_->tokens().textSecondary)));
    const QString tip = dark ? tr("Switch to the light theme") : tr("Switch to the dark theme");
    toggleThemeAction_->setToolTip(
        tip + QStringLiteral(" (") +
        toggleThemeAction_->shortcut().toString(QKeySequence::NativeText) + QLatin1Char(')'));
    toggleThemeAction_->setStatusTip(tip);
}

void MainWindow::applyCanvasTheme() {
    if (!open_) {
        return;
    }
    const ColorTokens& tokens = themes_->tokens();
    canvas::CanvasColors colors;
    // The desk around bounded pages must contrast with the paper: light paper is white on
    // a grey desk; dark paper is shown as the dark canvas grey on a lighter grey desk.
    colors.desk = themes_->isDark() ? tokens.surfaceElevated : tokens.hover;
    colors.pattern = tokens.canvasGrid;
    colors.selection = tokens.textSecondary;
    colors.marquee = tokens.textMuted;
    colors.eraser = tokens.textMuted;
    // Dark paper is a display transform; stored colours never change (RENDERING.md §8).
    colors.contentTransform =
        themes_->isDark() ? render::ColorTransform::InvertLightness : render::ColorTransform::None;
    open_->controller->setColors(colors);
    if (canvasWidget_ != nullptr) {
        canvasWidget_->refreshCursor(); // the eraser ring takes the new colour
        canvasWidget_->update();
    }
}

void MainWindow::updateActions() {
    const bool open = open_ != nullptr;
    const bool writable = open && !open_->session->isReadOnly();
    const bool canOpen = services_.has_value();
    newWorkspaceAction_->setEnabled(canOpen);
    openWorkspaceAction_->setEnabled(canOpen);
    closeWorkspaceAction_->setEnabled(open && open_->owned != nullptr);
    saveAction_->setEnabled(writable);

    const auto label = [](std::string_view text) {
        return toQString(text);
    };
    const document::Command* nextUndo = open ? open_->session->history().nextUndo() : nullptr;
    const document::Command* nextRedo = open ? open_->session->history().nextRedo() : nullptr;
    undoAction_->setEnabled(writable && nextUndo != nullptr);
    redoAction_->setEnabled(writable && nextRedo != nullptr);
    undoAction_->setText(nextUndo != nullptr ? tr("&Undo %1").arg(label(nextUndo->label))
                                             : tr("&Undo"));
    redoAction_->setText(nextRedo != nullptr ? tr("&Redo %1").arg(label(nextRedo->label))
                                             : tr("&Redo"));
    deleteAction_->setEnabled(writable);
    selectAllAction_->setEnabled(open);

    const std::optional<HierarchyItem> item =
        open ? currentItem(*treeModel_, *navigation_->tree(), activePage()) : std::nullopt;
    for (QAction* action : {newPageAction_, newSectionAction_, newNotebookAction_}) {
        action->setEnabled(writable);
    }
    for (QAction* action : {renameAction_, deleteItemAction_, moveUpAction_, moveDownAction_}) {
        action->setEnabled(writable && item.has_value());
    }
    previousPageAction_->setEnabled(open && open_->navigator.previousPage().has_value());
    nextPageAction_->setEnabled(open && open_->navigator.nextPage().has_value());

    const bool page = open && activePage().has_value();
    for (QAction* action : toolGroup_->actions()) {
        action->setEnabled(page);
    }
    for (QAction* action : {zoomInAction_, zoomOutAction_, resetViewAction_, fitAction_}) {
        action->setEnabled(page);
    }
    hudAction_->setEnabled(open);
    const document::PageInfo* info =
        page ? open_->session->workspace().findPage(*activePage()) : nullptr;
    for (QAction* action : backgroundGroup_->actions()) {
        action->setEnabled(writable && info != nullptr);
        action->setChecked(info != nullptr &&
                           action->data().toInt() == static_cast<int>(info->background.pattern));
    }
    for (QAction* action : extentGroup_->actions()) {
        action->setEnabled(writable && info != nullptr);
        action->setChecked(info != nullptr && (action->data().toInt() == 1) ==
                                                  (info->extent == document::PageExtent::Bounded));
    }
}

void MainWindow::scheduleStatusUpdate() {
    if (statusUpdatePending_) {
        return;
    }
    statusUpdatePending_ = true;
    QTimer::singleShot(0, this, [this] {
        statusUpdatePending_ = false;
        updateStatus();
    });
}

void MainWindow::updateStatus() {
    updateTitle();
    if (!open_) {
        saveStatusLabel_->clear();
        return;
    }
    const application::WorkspaceSession& session = *open_->session;
    QString status;
    QString tip;
    if (session.isReadOnly()) {
        status = tr("Read-only");
        tip = tr("This workspace is open read-only; changes are not possible.");
    } else if (const auto& error = session.lastWriteError()) {
        status = tr("Not saved yet (%n change(s))", nullptr,
                    static_cast<int>(session.pendingWriteCount()));
        tip = tr("Saving failed; StudyBoard tries again with the next change.\n%1")
                  .arg(errorText(*error));
    } else {
        status = tr("All changes saved");
    }
    if (saveStatusLabel_->text() != status) {
        saveStatusLabel_->setText(status); // unchanged text: no repaint of the window
    }
    saveStatusLabel_->setToolTip(tip);
}

void MainWindow::updateTitle() {
    const QString product = toQString(core::build::kProductName);
    if (!open_) {
        setWindowTitle(product);
        breadcrumbLabel_->clear();
        return;
    }
    const application::WorkspaceSession& session = *open_->session;
    const document::Workspace& ws = session.workspace();
    const QString workspaceName = toQString(ws.info().name);
    QString title = workspaceName;
    QString breadcrumb;
    const auto page = activePage();
    const document::PageInfo* info = page ? ws.findPage(*page) : nullptr;
    const document::SectionInfo* section =
        info != nullptr ? ws.findSection(info->section) : nullptr;
    const document::NotebookInfo* notebook =
        section != nullptr ? ws.findNotebook(section->notebook) : nullptr;
    if (notebook != nullptr) {
        const QString pageTitle = toQString(WorkspaceStructure::displayTitle(ws, *page));
        breadcrumb = toQString(notebook->title) + QStringLiteral("  ›  ") +
                     toQString(section->title) + QStringLiteral("  ›  ") + pageTitle;
        title = pageTitle + QStringLiteral(" — ") + workspaceName;
    }
    if (session.isReadOnly()) {
        title += tr(" (read-only)");
    }
    title += QStringLiteral(" — ") + product;
    if (windowTitle() != title) {
        setWindowTitle(title);
    }
    if (breadcrumbLabel_->text() != breadcrumb) {
        breadcrumbLabel_->setText(breadcrumb); // unchanged text: no repaint of the window
    }
}

void MainWindow::reportFailure(const QString& summary, const QString& details) {
    core::logWarning("ui", summary.toStdString() +
                               (details.isEmpty() ? std::string() : ": " + details.toStdString()));
    dialogs_->showError(this, summary, details);
}

void MainWindow::runPanBenchmark(int frames, double zoomFactor,
                                 std::function<void(const PanBenchmarkResult&)> done) {
    if (canvasWidget_ != nullptr && open_) {
        if (zoomFactor > 0.0) {
            open_->controller->zoomBy(zoomFactor);
        } else {
            open_->controller->zoomToFit(); // 0 or negative: everything on the page in view
        }
        canvasWidget_->runPanBenchmark(frames, std::move(done));
    }
}

QImage MainWindow::grabCanvas() {
    return canvasWidget_ != nullptr ? canvasWidget_->grabFramebuffer() : QImage{};
}

void MainWindow::showAbout() {
    QString components;
    for (const auto& component : application::componentVersions()) {
        components += QStringLiteral("<li>%1 %2</li>")
                          .arg(QString::fromStdString(component.name).toHtmlEscaped(),
                               QString::fromStdString(component.version).toHtmlEscaped());
    }
    components += QStringLiteral("<li>Qt %1</li>").arg(QString::fromLatin1(qVersion()));

    const QString productName = toQString(core::build::kProductName);
    QMessageBox::about(
        this, tr("About %1").arg(productName),
        tr("<h3>%1 %2</h3>"
           "<p>A local-first desktop application for notes, drawing and study planning.</p>"
           "<p>Components:</p><ul>%3</ul>"
           "<p>Released under the MIT License. <a href=\"%4\">%4</a></p>")
            .arg(productName, toQString(core::build::kVersion), components,
                 toQString(core::build::kHomepageUrl)));
}

} // namespace studyapp::ui
