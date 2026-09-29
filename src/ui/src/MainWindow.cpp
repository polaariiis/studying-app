#include <studyapp/ui/MainWindow.hpp>

#include "CanvasWidget.hpp"
#include "NavigationPanel.hpp"
#include "PageExport.hpp"
#include "PlannerPanel.hpp"
#include "SessionDocumentPort.hpp"
#include "SessionDocumentRasterizer.hpp"
#include "SessionImageSource.hpp"
#include "ThemeIcons.hpp"
#include "WorkspaceTreeModel.hpp"

#include <studyapp/application/ComponentVersions.hpp>
#include <studyapp/application/PageNavigator.hpp>
#include <studyapp/application/Search.hpp>
#include <studyapp/application/StartPage.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/ui/AppIcon.hpp>
#include <studyapp/ui/CanvasPlaceholder.hpp>
#include <studyapp/ui/DesignTokens.hpp>
#include <studyapp/ui/ShellDialogs.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QPrinter>
#include <QProgressDialog>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QThreadPool>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
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
const QString kPlannerKey = QStringLiteral("mainWindow/plannerVisible");
const QString kThemeKey = QStringLiteral("appearance/theme");
const QString kHudKey = QStringLiteral("canvas/debugHud");
const QString kPenColorKey = QStringLiteral("tools/penColor");
const QString kPenWidthKey = QStringLiteral("tools/penWidth");
const QString kHighlighterColorKey = QStringLiteral("tools/highlighterColor");
const QString kHighlighterWidthKey = QStringLiteral("tools/highlighterWidth");
const QString kEraserModeKey = QStringLiteral("tools/eraserMode"); // "partial" | "strokes"
const QString kShapeKindKey = QStringLiteral("tools/shapeKind");
const QString kShapeColorKey = QStringLiteral("tools/shapeColor");
const QString kShapeWidthKey = QStringLiteral("tools/shapeWidth");
const QString kShapeFillKey = QStringLiteral("tools/shapeFill");

struct ShapeKindName {
    document::ShapeKind kind;
    const char* name; ///< settings value and object-name suffix
    const char* label;
};
constexpr ShapeKindName kShapeKinds[] = {
    {document::ShapeKind::Line, "line", QT_TRANSLATE_NOOP("MainWindow", "&Line")},
    {document::ShapeKind::Arrow, "arrow", QT_TRANSLATE_NOOP("MainWindow", "&Arrow")},
    {document::ShapeKind::Rectangle, "rectangle", QT_TRANSLATE_NOOP("MainWindow", "&Rectangle")},
    {document::ShapeKind::Ellipse, "ellipse", QT_TRANSLATE_NOOP("MainWindow", "&Ellipse")},
};
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

core::Color toCoreColor(const QColor& color) noexcept {
    return core::Color::fromRgba(
        static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
        static_cast<std::uint8_t>(color.blue()), static_cast<std::uint8_t>(color.alpha()));
}

/// How a (translucent) ink looks on white paper, for swatches on any chrome colour.
QColor onPaper(const QColor& ink) {
    const float a = ink.alphaF();
    return QColor::fromRgbF(1.0F - a * (1.0F - ink.redF()), 1.0F - a * (1.0F - ink.greenF()),
                            1.0F - a * (1.0F - ink.blueF()));
}

/// Width shown by style icons: highlighter bands are drawn at the scale of pen widths.
double iconWidth(canvas::ToolKind tool, float width) noexcept {
    constexpr double kHighlighterIconScale = 1.0 / 6.0;
    return tool == canvas::ToolKind::Highlighter ? width * kHighlighterIconScale : width;
}

/// The patch changes what search finds or shows: titles, text boxes, tasks, or the
/// notebook/section names shown as context. Ink, shapes and styling do not.
bool affectsSearch(const document::Patch& patch) {
    const auto isText = [](const std::optional<document::Element>& element) {
        return element && std::holds_alternative<document::TextBox>(element->payload);
    };
    for (const document::AnyChange& change : patch.changes()) {
        if (const auto* element = std::get_if<document::ElementChange>(&change)) {
            if (isText(element->before) || isText(element->after)) {
                return true;
            }
            continue;
        }
        if (std::holds_alternative<document::NotebookChange>(change) ||
            std::holds_alternative<document::SectionChange>(change) ||
            std::holds_alternative<document::PageChange>(change) ||
            std::holds_alternative<document::TaskChange>(change) ||
            std::holds_alternative<document::CourseChange>(change) ||
            std::holds_alternative<document::ProjectChange>(change)) {
            return true;
        }
    }
    return false;
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
          images(std::make_unique<SessionImageSource>(shown)),
          documents(std::make_unique<SessionDocumentRasterizer>(shown)),
          controller(std::make_unique<canvas::CanvasController>(*port, shellIds)),
          navigator(shown.workspace()), structure(shown, shellClock, shellIds),
          planner(shown, shellClock, shellIds) {}

    std::unique_ptr<application::WorkspaceSession> owned; ///< null when borrowed
    application::WorkspaceSession* session;
    const core::Clock* clock;
    core::IdGenerator* ids;
    std::unique_ptr<SessionDocumentPort> port;
    std::unique_ptr<SessionImageSource> images;
    std::unique_ptr<SessionDocumentRasterizer> documents; ///< PDF pages (Phase 8)
    std::unique_ptr<canvas::CanvasController> controller;
    application::PageNavigator navigator;
    WorkspaceStructure structure;
    application::Planner planner;
    /// Where the user left each page in this session (not persisted).
    std::unordered_map<core::PageId, View> views;
    /// Expires when this workspace is closed: background results for it are dropped.
    std::shared_ptr<void> alive = std::make_shared<int>(0);
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
    insertImageAction_ = make(tr("Insert &Image…"), QStringLiteral("actionInsertImage"),
                              QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    connect(insertImageAction_, &QAction::triggered, this, &MainWindow::insertImage);
    importPdfAction_ = make(tr("&Import PDF…"), QStringLiteral("actionImportPdf"));
    connect(importPdfAction_, &QAction::triggered, this, &MainWindow::importPdf);
    exportPageAction_ = make(tr("&Export Page\u2026"), QStringLiteral("actionExportPage"),
                             QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(exportPageAction_, &QAction::triggered, this, [this] { exportPages(false); });
    exportSectionAction_ =
        make(tr("Export &Section as PDF\u2026"), QStringLiteral("actionExportSection"));
    connect(exportSectionAction_, &QAction::triggered, this, [this] { exportPages(true); });
    printAction_ = make(tr("&Print\u2026"), QStringLiteral("actionPrint"), QKeySequence::Print);
    exportWorkspaceAction_ =
        make(tr("Export &Workspace\u2026"), QStringLiteral("actionExportWorkspace"));
    connect(exportWorkspaceAction_, &QAction::triggered, this, [this] { exportBundle(false); });
    exportNotebookAction_ =
        make(tr("Export &Notebook\u2026"), QStringLiteral("actionExportNotebook"));
    connect(exportNotebookAction_, &QAction::triggered, this, [this] { exportBundle(true); });
    importNotebookAction_ =
        make(tr("Import N&otebook\u2026"), QStringLiteral("actionImportNotebook"));
    connect(importNotebookAction_, &QAction::triggered, this, &MainWindow::importNotebookBundle);
    backUpAction_ = make(tr("&Back Up Now"), QStringLiteral("actionBackUp"));
    connect(backUpAction_, &QAction::triggered, this, &MainWindow::backUpNow);
    checkWorkspaceAction_ =
        make(tr("C&heck Workspace\u2026"), QStringLiteral("actionCheckWorkspace"));
    connect(checkWorkspaceAction_, &QAction::triggered, this, &MainWindow::checkWorkspace);
    openBundleAction_ =
        make(tr("Open &Bundle as Workspace\u2026"), QStringLiteral("actionOpenBundle"));
    connect(openBundleAction_, &QAction::triggered, this, &MainWindow::openBundleAsWorkspace);
    connect(printAction_, &QAction::triggered, this, &MainWindow::printPages);
    // Cut, copy and paste of canvas elements (the canvas clipboard, not the system one; text
    // being edited takes these keys itself).
    cutAction_ = make(tr("Cu&t"), QStringLiteral("actionCut"), QKeySequence::Cut);
    cutAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(cutAction_, &QAction::triggered, this, [this] {
        if (open_) {
            (void)open_->controller->cutSelection();
        }
    });
    copyAction_ = make(tr("&Copy"), QStringLiteral("actionCopy"), QKeySequence::Copy);
    copyAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(copyAction_, &QAction::triggered, this, [this] {
        if (open_) {
            (void)open_->controller->copySelection();
        }
    });
    pasteAction_ = make(tr("&Paste"), QStringLiteral("actionPaste"), QKeySequence::Paste);
    pasteAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(pasteAction_, &QAction::triggered, this, [this] {
        if (open_ && open_->controller->canPaste() && open_->controller->paste()) {
            selectTool(canvas::ToolKind::Select); // the copies are selected, ready to move
        }
    });
    deleteAction_ = make(tr("&Delete"), QStringLiteral("actionDelete"), QKeySequence::Delete);
    deleteAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteAction_, &QAction::triggered, this, [this] {
        if (open_) {
            (void)open_->controller->deleteSelection();
        }
    });
    findAction_ = make(tr("&Find…"), QStringLiteral("actionFind"), QKeySequence::Find);
    connect(findAction_, &QAction::triggered, this, [this] {
        navigationAction_->setChecked(true); // the field is in the navigation panel
        navigation_->searchField()->setFocus(Qt::ShortcutFocusReason);
        navigation_->searchField()->selectAll();
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
            if (tool == canvas::ToolKind::Pen || tool == canvas::ToolKind::Highlighter ||
                tool == canvas::ToolKind::Shape || tool == canvas::ToolKind::Connector) {
                // Connectors are drawn in the shape style's colour and width.
                styleShown_ = static_cast<int>(
                    tool == canvas::ToolKind::Connector ? canvas::ToolKind::Shape : tool);
                syncInkActions(); // the style button shows the chosen drawing tool
            }
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
    makeTool(tr("&Highlighter"), QStringLiteral("actionToolHighlighter"), Qt::Key_M,
             canvas::ToolKind::Highlighter);
    makeTool(tr("Sh&ape"), QStringLiteral("actionToolShape"), Qt::Key_S, canvas::ToolKind::Shape);
    makeTool(tr("&Text"), QStringLiteral("actionToolText"), Qt::Key_T, canvas::ToolKind::Text);
    makeTool(tr("C&onnector"), QStringLiteral("actionToolConnector"), Qt::Key_C,
             canvas::ToolKind::Connector);
    makeTool(tr("&Select"), QStringLiteral("actionToolSelect"), Qt::Key_V,
             canvas::ToolKind::Select);
    makeTool(tr("&Eraser"), QStringLiteral("actionToolEraser"), Qt::Key_E,
             canvas::ToolKind::Eraser);
    makeTool(tr("Pa&n"), QStringLiteral("actionToolPan"), Qt::Key_H, canvas::ToolKind::Pan);
    makeTool(tr("&Zoom"), QStringLiteral("actionToolZoom"), Qt::Key_Z, canvas::ToolKind::Zoom);
    const canvas::ToolSettings defaults;
    pen_ = {.tool = canvas::ToolKind::Pen,
            .color = toQColor(defaults.pen.color),
            .width = defaults.pen.width};
    highlighter_ = {.tool = canvas::ToolKind::Highlighter,
                    .color = toQColor(defaults.highlighter.color),
                    .width = defaults.highlighter.width};
    shape_ = {.tool = canvas::ToolKind::Shape,
              .color = toQColor(defaults.shape.color),
              .width = defaults.shape.width};
    shapeKind_ = static_cast<int>(defaults.shape.kind);
    styleShown_ = static_cast<int>(canvas::ToolKind::Pen);
    createInkActions(pen_);
    createInkActions(highlighter_);
    createInkActions(shape_);
    // Shape kinds first, then (from createInkActions) inks and widths, then the fill.
    shapeKindGroup_ = new QActionGroup(this);
    shapeKindGroup_->setExclusive(true);
    for (const ShapeKindName& entry : kShapeKinds) {
        auto* action =
            new QAction(QCoreApplication::translate("MainWindow", entry.label), shapeKindGroup_);
        action->setObjectName(QStringLiteral("actionShapeKind_") + QString::fromLatin1(entry.name));
        action->setCheckable(true);
        const int kind = static_cast<int>(entry.kind);
        action->setData(kind);
        connect(action, &QAction::triggered, this, [this, kind] {
            shapeKind_ = kind;
            styleShown_ = static_cast<int>(canvas::ToolKind::Shape);
            applyToolSettings();
            selectTool(canvas::ToolKind::Shape);
        });
    }
    QAction* firstInk = shape_.menu->actions().isEmpty() ? nullptr : shape_.menu->actions().front();
    shape_.menu->insertActions(firstInk, shapeKindGroup_->actions());
    shape_.menu->insertSeparator(firstInk);
    shapeFillAction_ = new QAction(tr("&Fill"), this);
    shapeFillAction_->setObjectName(QStringLiteral("actionShapeFill"));
    shapeFillAction_->setCheckable(true);
    connect(shapeFillAction_, &QAction::triggered, this, [this](bool on) {
        shapeFill_ = on;
        styleShown_ = static_cast<int>(canvas::ToolKind::Shape);
        applyToolSettings();
        selectTool(canvas::ToolKind::Shape);
    });
    shape_.menu->addSeparator();
    shape_.menu->addAction(shapeFillAction_);

    // Eraser mode: partial (vector pieces remain) or whole strokes.
    eraserMenu_ = new QMenu(tr("E&raser"), this);
    eraserMenu_->setObjectName(QStringLiteral("menuEraserMode"));
    eraserModeGroup_ = new QActionGroup(this);
    eraserModeGroup_->setExclusive(true);
    for (const bool whole : {false, true}) {
        auto* action = new QAction(whole ? tr("Erase &Whole Strokes") : tr("Erase &Partially"),
                                   eraserModeGroup_);
        action->setObjectName(whole ? QStringLiteral("actionEraserWholeStrokes")
                                    : QStringLiteral("actionEraserPartial"));
        action->setCheckable(true);
        action->setData(whole);
        connect(action, &QAction::triggered, this, [this, whole] {
            eraseWholeStrokes_ = whole;
            applyToolSettings();
            selectTool(canvas::ToolKind::Eraser); // choosing a mode means erasing next
        });
    }
    eraserMenu_->addActions(eraserModeGroup_->actions());

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
    plannerAction_ = make(tr("&Planner"), QStringLiteral("actionPlanner"),
                          QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    plannerAction_->setCheckable(true);
    plannerAction_->setChecked(false);
    connect(plannerAction_, &QAction::toggled, this, [this](bool visible) {
        if (planner_ != nullptr) {
            planner_->setVisible(visible);
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
    // Keyboard access to the panes (Phase 9 accessibility): F6 / Shift+F6 move the focus
    // between the navigation, the canvas and the planner, as in most desktop applications.
    nextPaneAction_ =
        make(tr("Next &Pane"), QStringLiteral("actionNextPane"), QKeySequence(Qt::Key_F6));
    connect(nextPaneAction_, &QAction::triggered, this, [this] { focusPane(+1); });
    previousPaneAction_ = make(tr("Pre&vious Pane"), QStringLiteral("actionPreviousPane"),
                               QKeySequence(Qt::SHIFT | Qt::Key_F6));
    connect(previousPaneAction_, &QAction::triggered, this, [this] { focusPane(-1); });
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
    fileMenu->addAction(importPdfAction_);
    fileMenu->addAction(exportPageAction_);
    fileMenu->addAction(exportSectionAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(printAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(exportWorkspaceAction_);
    fileMenu->addAction(exportNotebookAction_);
    fileMenu->addAction(importNotebookAction_);
    fileMenu->addAction(openBundleAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(backUpAction_);
    fileMenu->addAction(checkWorkspaceAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction_);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->setObjectName(QStringLiteral("menuEdit"));
    editMenu->addAction(undoAction_);
    editMenu->addAction(redoAction_);
    editMenu->addSeparator();
    editMenu->addAction(cutAction_);
    editMenu->addAction(copyAction_);
    editMenu->addAction(pasteAction_);
    editMenu->addAction(deleteAction_);
    editMenu->addAction(selectAllAction_);
    editMenu->addSeparator();
    editMenu->addAction(findAction_);
    editMenu->addSeparator();
    editMenu->addAction(insertImageAction_);

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
    toolsMenu->addSeparator();
    toolsMenu->addMenu(pen_.menu);
    toolsMenu->addMenu(highlighter_.menu);
    toolsMenu->addMenu(shape_.menu);
    toolsMenu->addMenu(eraserMenu_);

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("menuView"));
    viewMenu->addAction(navigationAction_);
    viewMenu->addAction(plannerAction_);
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
    viewMenu->addSeparator();
    viewMenu->addAction(nextPaneAction_);
    viewMenu->addAction(previousPaneAction_);
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
    inkStyleButton_ = new QToolButton(toolBar);
    inkStyleButton_->setObjectName(QStringLiteral("inkStyleButton"));
    inkStyleButton_->setPopupMode(QToolButton::InstantPopup);
    inkStyleButton_->setAutoRaise(true);
    inkStyleButton_->setIconSize(toolBar->iconSize());
    inkStyleButton_->setToolButtonStyle(toolBar->toolButtonStyle());
    connect(toolBar, &QToolBar::iconSizeChanged, inkStyleButton_, &QToolButton::setIconSize);
    connect(toolBar, &QToolBar::toolButtonStyleChanged, inkStyleButton_,
            &QToolButton::setToolButtonStyle);
    toolBar->addWidget(inkStyleButton_);
    syncInkActions();
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
    planner_ = new PlannerPanel(*dialogs_, shell_);
    planner_->setMinimumWidth(240);
    planner_->setVisible(plannerAction_->isChecked());
    planner_->setOpenPageHandler([this](core::PageId page) { (void)openPage(page); });
    planner_->setFailureHandler([this](const QString& summary, const QString& details) {
        reportFailure(summary, details);
    });
    shell_->addWidget(navigation_);
    shell_->addWidget(canvasArea_);
    shell_->addWidget(planner_);
    shell_->setStretchFactor(0, 0);
    shell_->setStretchFactor(1, 1);
    shell_->setStretchFactor(2, 0);
    shell_->setSizes({240, 960, 320});
    stack_->addWidget(shell_);
    setCentralWidget(stack_);

    QTreeView* tree = navigation_->tree();
    connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onTreeCurrentChanged);

    // Live search: after a short pause in typing (one query per pause, not per key).
    searchDelay_ = new QTimer(this);
    searchDelay_->setSingleShot(true);
    searchDelay_->setInterval(150);
    connect(searchDelay_, &QTimer::timeout, this, &MainWindow::runSearch);
    connect(navigation_->searchField(), &QLineEdit::textChanged, this,
            [this] { searchDelay_->start(); });
    connect(navigation_->searchField(), &QLineEdit::returnPressed, this, [this] {
        searchDelay_->stop();
        runSearch();
        if (QTreeWidgetItem* first = navigation_->searchResults()->topLevelItem(0);
            first != nullptr && !first->data(0, Qt::UserRole).toStringList().isEmpty()) {
            activateSearchResult(first);
        }
    });
    navigation_->searchField()->installEventFilter(this);
    connect(navigation_->searchResults(), &QTreeWidget::itemActivated, this,
            &MainWindow::activateSearchResult);
    connect(navigation_->searchResults(), &QTreeWidget::itemClicked, this,
            &MainWindow::activateSearchResult);
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
    plannerAction_->setChecked(settings_->value(kPlannerKey, false).toBool());
    hudAction_->setChecked(settings_->value(kHudKey, false).toBool());
    const auto readInk = [this](InkControls& ink, const QString& colorKey,
                                const QString& widthKey) {
        const QColor color(settings_->value(colorKey).toString());
        if (color.isValid()) {
            ink.color = color;
        }
        bool widthOk = false;
        const float width = settings_->value(widthKey).toFloat(&widthOk);
        if (widthOk) {
            ink.width = width;
        }
    };
    readInk(pen_, kPenColorKey, kPenWidthKey);
    readInk(highlighter_, kHighlighterColorKey, kHighlighterWidthKey);
    eraseWholeStrokes_ = settings_->value(kEraserModeKey).toString() == QStringLiteral("strokes");
    readInk(shape_, kShapeColorKey, kShapeWidthKey);
    const QString storedKind = settings_->value(kShapeKindKey).toString();
    for (const ShapeKindName& entry : kShapeKinds) {
        if (storedKind == QLatin1String(entry.name)) {
            shapeKind_ = static_cast<int>(entry.kind);
        }
    }
    shapeFill_ = settings_->value(kShapeFillKey, false).toBool();
    applyToolSettings();
}

void MainWindow::writeSettings() {
    settings_->setValue(kThemeKey, toSettingsValue(themes_->mode()));
    settings_->setValue(kGeometryKey, saveGeometry());
    settings_->setValue(kStateKey, saveState());
    settings_->setValue(kSplitterKey, shell_->saveState());
    settings_->setValue(kNavigationKey, navigationAction_->isChecked());
    settings_->setValue(kPlannerKey, plannerAction_->isChecked());
    settings_->setValue(kHudKey, hudAction_->isChecked());
    settings_->setValue(kPenColorKey, pen_.color.name(QColor::HexArgb));
    settings_->setValue(kPenWidthKey, pen_.width);
    settings_->setValue(kHighlighterColorKey, highlighter_.color.name(QColor::HexArgb));
    settings_->setValue(kHighlighterWidthKey, highlighter_.width);
    settings_->setValue(kEraserModeKey,
                        eraseWholeStrokes_ ? QStringLiteral("strokes") : QStringLiteral("partial"));
    settings_->setValue(kShapeColorKey, shape_.color.name(QColor::HexArgb));
    settings_->setValue(kShapeWidthKey, shape_.width);
    settings_->setValue(kShapeFillKey, shapeFill_);
    for (const ShapeKindName& entry : kShapeKinds) {
        if (static_cast<int>(entry.kind) == shapeKind_) {
            settings_->setValue(kShapeKindKey, QString::fromLatin1(entry.name));
        }
    }
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
    finishTextEditing(); // written (and flushed below) before the workspace closes
    planner_->finishEditing();
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
    for (QAction* action : {cutAction_, copyAction_, pasteAction_}) {
        canvasWidget_->addAction(action); // canvas-only shortcuts
    }
    canvasWidget_->addAction(deleteAction_);
    canvasWidget_->addAction(selectAllAction_);
    applyCanvasTheme();
    if (QAction* checked = toolGroup_->checkedAction()) {
        open_->controller->setTool(static_cast<canvas::ToolKind>(checked->data().toInt()));
    }
    applyToolSettings(); // tool settings outlive workspaces
    open_->controller->setTextLayout(textLayout_);
    open_->controller->setImageSource(open_->images.get());
    open_->controller->setDocumentRasterizer(open_->documents.get());
    planner_->setReadOnly(session.isReadOnly());
    planner_->setPlanner(&open_->planner, open_->clock);

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
    clearSearch();
    treeModel_->setWorkspace(nullptr);
    planner_->setPlanner(nullptr, nullptr);
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
    planner_->onPatch(patch);
    if (navigation_->isShowingSearchResults() && affectsSearch(patch)) {
        searchDelay_->start(); // results follow the workspace (never stale or dangling)
    }
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
    finishTextEditing(); // what is being typed belongs to the page being left
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

// ---------------------------------------------------------------------------- search

void MainWindow::runSearch() {
    QTreeWidget* results = navigation_->searchResults();
    const QString query = navigation_->searchField()->text().trimmed();
    if (!open_ || query.isEmpty()) {
        results->clear();
        navigation_->showSearchResults(false);
        return;
    }
    auto found = application::search(*open_->session, query.toStdString());
    // Refreshing keeps the chosen result chosen.
    const QVariant chosen = results->currentItem() != nullptr
                                ? results->currentItem()->data(0, Qt::UserRole)
                                : QVariant();
    results->clear();
    navigation_->showSearchResults(true);
    const auto note = [&](const QString& text) {
        auto* item = new QTreeWidgetItem(results);
        item->setText(0, text);
        item->setFlags(Qt::ItemIsEnabled);
    };
    if (!found) {
        note(tr("Search failed: %1").arg(errorText(found.error())));
        return;
    }
    if (found->empty()) {
        note(open_->session->isSearchIndexed()
                 ? tr("No results")
                 : tr("Not indexed yet: open the workspace for editing once to search it"));
        return;
    }
    const auto idText = [](const auto& id) {
        return toQString(id.toString());
    };
    for (const application::SearchResult& result : *found) {
        auto* item = new QTreeWidgetItem(results);
        // Second line: where it is — or, when the match is further into the text than the
        // title shows, the words around it.
        const bool snippetAddsText = !result.snippet.empty() && result.snippet != result.title;
        const QString detail = toQString(snippetAddsText ? result.snippet : result.context);
        item->setText(
            0, toQString(result.title.empty() ? tr("Untitled").toStdString() : result.title) +
                   (detail.isEmpty() ? QString() : QStringLiteral("\n") + detail));
        item->setToolTip(0, toQString(result.context));
        item->setData(0, Qt::UserRole,
                      QStringList{QString::number(static_cast<int>(result.kind)),
                                  result.page ? idText(*result.page) : QString(),
                                  result.element ? idText(*result.element) : QString(),
                                  result.task ? idText(*result.task) : QString()});
        if (chosen.isValid() && item->data(0, Qt::UserRole) == chosen) {
            results->setCurrentItem(item);
        }
    }
}

void MainWindow::activateSearchResult(QTreeWidgetItem* item) {
    const QStringList target =
        item != nullptr ? item->data(0, Qt::UserRole).toStringList() : QStringList{};
    if (!open_ || target.size() != 4) {
        return;
    }
    const auto uuid = [&](int i) {
        return core::Uuid::parse(target[i].toStdString());
    };
    const auto kind = static_cast<application::SearchResult::Kind>(target[0].toInt());
    if (kind == application::SearchResult::Kind::Task) {
        if (auto task = uuid(3)) {
            plannerAction_->setChecked(true);
            planner_->showView(PlannerPanel::View::Tasks);
            planner_->selectTask(core::TaskId{*task});
        }
        return;
    }
    const auto page = uuid(1);
    if (!page || !openPage(core::PageId{*page})) {
        return;
    }
    if (kind == application::SearchResult::Kind::TextBox) {
        if (auto element = uuid(2)) {
            (void)open_->controller->revealElement(core::ElementId{*element});
        }
    }
}

void MainWindow::clearSearch() {
    searchDelay_->stop();
    navigation_->searchField()->clear(); // textChanged restarts the delay: stop it again
    searchDelay_->stop();
    navigation_->searchResults()->clear();
    navigation_->showSearchResults(false);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == navigation_->searchField() && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        clearSearch();
        if (canvasWidget_ != nullptr) {
            canvasWidget_->setFocus(Qt::ShortcutFocusReason);
        }
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::finishTextEditing() {
    if (canvasWidget_ != nullptr) {
        canvasWidget_->finishTextEditing();
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
    planner_->setActivePage(page);
    updateActions();
    updateStatus();
}

void MainWindow::setTimeZone(const study::TimeZone* zone) {
    timeZone_ = zone;
    planner_->setTimeZone(zone);
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

std::optional<core::NotebookId> MainWindow::currentNotebook() const {
    const document::Workspace& ws = open_->session->workspace();
    const auto item = currentItem(*treeModel_, *navigation_->tree(), activePage());
    if (!item) {
        return std::nullopt;
    }
    if (const auto* page = std::get_if<core::PageId>(&*item)) {
        return ws.findSection(ws.findPage(*page)->section)->notebook;
    }
    if (const auto* section = std::get_if<core::SectionId>(&*item)) {
        return ws.findSection(*section)->notebook;
    }
    return std::get<core::NotebookId>(*item);
}

void MainWindow::newSection() {
    if (!open_) {
        return;
    }
    const std::optional<core::NotebookId> notebook = currentNotebook();
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

void MainWindow::setTextLayout(canvas::TextLayout* layout) {
    textLayout_ = layout;
    if (open_) {
        open_->controller->setTextLayout(layout);
    }
}

void MainWindow::createInkActions(InkControls& ink) {
    // A small, fixed choice of inks and widths (DesignTokens): quick to reach, no dialogs.
    const bool highlighter = ink.tool == canvas::ToolKind::Highlighter;
    const bool shape = ink.tool == canvas::ToolKind::Shape;
    const QString prefix = highlighter ? QStringLiteral("actionHighlighter")
                           : shape     ? QStringLiteral("actionShape")
                                       : QStringLiteral("actionPen");
    ink.menu = new QMenu(highlighter ? tr("Highlighter Sty&le")
                         : shape     ? tr("Shape S&tyle")
                                     : tr("Pen St&yle"),
                         this);
    ink.menu->setObjectName(highlighter ? QStringLiteral("menuHighlighterStyle")
                            : shape     ? QStringLiteral("menuShapeStyle")
                                        : QStringLiteral("menuPenStyle"));
    ink.colors = new QActionGroup(this);
    ink.colors->setExclusive(true);
    for (const InkColor& color : highlighter ? highlighterPalette() : inkPalette()) {
        auto* action = new QAction(QCoreApplication::translate("Ink", color.label), ink.colors);
        action->setObjectName(prefix + QStringLiteral("Color_") + QString::fromLatin1(color.name));
        action->setCheckable(true);
        const QColor value = toQColor(color.color);
        action->setData(value);
        connect(action, &QAction::triggered, this, [this, &ink, value] {
            ink.color = value;
            styleShown_ = static_cast<int>(ink.tool);
            applyToolSettings();
            selectTool(ink.tool); // choosing an ink means drawing with it next
        });
    }
    ink.widths = new QActionGroup(this);
    ink.widths->setExclusive(true);
    for (const PenWidthPreset& preset :
         highlighter ? highlighterWidthPresets() : penWidthPresets()) {
        auto* action = new QAction(QCoreApplication::translate("Ink", preset.label), ink.widths);
        action->setObjectName(prefix + QStringLiteral("Width_") + QString::fromLatin1(preset.name));
        action->setCheckable(true);
        action->setData(preset.width);
        const float width = preset.width;
        connect(action, &QAction::triggered, this, [this, &ink, width] {
            ink.width = width;
            styleShown_ = static_cast<int>(ink.tool);
            applyToolSettings();
            selectTool(ink.tool);
        });
    }
    ink.menu->addActions(ink.colors->actions());
    ink.menu->addSeparator();
    ink.menu->addActions(ink.widths->actions());
}

void MainWindow::applyToolSettings() {
    canvas::ToolSettings settings;
    settings.pen = {
        .brush = document::Brush::Pen, .color = toCoreColor(pen_.color), .width = pen_.width};
    settings.highlighter = {.brush = document::Brush::Highlighter,
                            .color = toCoreColor(highlighter_.color),
                            .width = highlighter_.width};
    settings.eraser =
        eraseWholeStrokes_ ? canvas::EraserMode::WholeStroke : canvas::EraserMode::Partial;
    settings.shape = {.kind = static_cast<document::ShapeKind>(shapeKind_),
                      .color = toCoreColor(shape_.color),
                      .width = shape_.width,
                      .fill = shapeFill_};
    settings = canvas::sanitized(settings);
    shapeKind_ = static_cast<int>(settings.shape.kind);
    shape_.color = toQColor(settings.shape.color);
    shape_.width = settings.shape.width;
    pen_.color = toQColor(settings.pen.color);
    pen_.width = settings.pen.width;
    highlighter_.color = toQColor(settings.highlighter.color);
    highlighter_.width = settings.highlighter.width;
    if (open_) {
        open_->controller->setToolSettings(settings);
    }
    syncInkActions();
}

void MainWindow::selectTool(canvas::ToolKind tool) {
    for (QAction* action : toolGroup_->actions()) {
        if (action->data().toInt() == static_cast<int>(tool) && action->isEnabled() &&
            !action->isChecked()) {
            action->trigger();
        }
    }
}

void MainWindow::refreshInkIcons() {
    const QColor border = toQColor(themes_->tokens().borderStrong);
    const QColor line = toQColor(themes_->tokens().textSecondary);
    if (shapeKindGroup_ != nullptr) {
        for (QAction* action : shapeKindGroup_->actions()) {
            action->setIcon(
                shapeIcon(static_cast<document::ShapeKind>(action->data().toInt()), line));
        }
    }
    for (const InkControls* ink : {&pen_, &highlighter_, &shape_}) {
        for (QAction* action : ink->colors->actions()) {
            action->setIcon(swatchIcon(onPaper(action->data().value<QColor>()), border));
        }
        for (QAction* action : ink->widths->actions()) {
            const double width = iconWidth(ink->tool, action->data().toFloat());
            action->setIcon(lineWidthIcon(std::clamp(width * 1.2, 1.0, 6.0), line));
        }
    }
}

void MainWindow::syncInkActions() {
    if (pen_.colors == nullptr || highlighter_.colors == nullptr || shape_.colors == nullptr) {
        return;
    }
    if (shapeKindGroup_ != nullptr) {
        for (QAction* action : shapeKindGroup_->actions()) {
            action->setChecked(action->data().toInt() == shapeKind_);
        }
        shapeFillAction_->setChecked(shapeFill_);
    }
    if (eraserModeGroup_ != nullptr) {
        for (QAction* action : eraserModeGroup_->actions()) {
            action->setChecked(action->data().toBool() == eraseWholeStrokes_);
        }
    }
    for (const InkControls* ink : {&pen_, &highlighter_, &shape_}) {
        for (QAction* action : ink->colors->actions()) {
            action->setChecked(action->data().value<QColor>().rgba() == ink->color.rgba());
        }
        for (QAction* action : ink->widths->actions()) {
            action->setChecked(std::abs(action->data().toFloat() - ink->width) < 0.01F);
        }
    }
    if (inkStyleButton_ == nullptr) {
        return;
    }
    const QColor border = toQColor(themes_->tokens().borderStrong);
    const auto shownTool = static_cast<canvas::ToolKind>(styleShown_);
    const InkControls& shown = shownTool == canvas::ToolKind::Highlighter ? highlighter_
                               : shownTool == canvas::ToolKind::Shape     ? shape_
                                                                          : pen_;
    inkStyleButton_->setMenu(shown.menu);
    if (shownTool == canvas::ToolKind::Shape) {
        inkStyleButton_->setIcon(
            shapeIcon(static_cast<document::ShapeKind>(shapeKind_), shape_.color, shapeFill_));
    } else {
        inkStyleButton_->setIcon(
            penOptionsIcon(onPaper(shown.color), iconWidth(shown.tool, shown.width), border));
    }
    QString name = tr("Custom");
    for (QAction* action : shown.colors->actions()) {
        if (action->isChecked()) {
            name = action->text();
        }
    }
    const QString width = QString::number(shown.width);
    if (shownTool == canvas::ToolKind::Shape) {
        QString kind;
        for (QAction* action : shapeKindGroup_->actions()) {
            if (action->isChecked()) {
                kind = action->text().remove(QLatin1Char('&'));
            }
        }
        inkStyleButton_->setToolTip(tr("Shape style: %1, %2, %3").arg(kind, name, width));
    } else {
        inkStyleButton_->setToolTip(shownTool == canvas::ToolKind::Highlighter
                                        ? tr("Highlighter style: %1, %2").arg(name, width)
                                        : tr("Pen style: %1, %2").arg(name, width));
    }
}

void MainWindow::runInBackground(std::function<std::function<void()>()> work) {
    // `work` runs on a pool thread and returns the part to run back on the GUI thread; that
    // part runs only if this window and the same open workspace are still there.
    const std::weak_ptr<void> workspace = open_->alive;
    const QPointer<MainWindow> window(this);
    ++backgroundJobs_;
    QThreadPool::globalInstance()->start([work = std::move(work), workspace, window] {
        std::function<void()> finish = work();
        QMetaObject::invokeMethod(
            qApp,
            [finish = std::move(finish), workspace, window] {
                if (window == nullptr) {
                    return;
                }
                --window->backgroundJobs_;
                if (!workspace.expired() && finish) {
                    finish();
                }
                window->updateStatus();
            },
            Qt::QueuedConnection);
    });
    updateStatus();
}

void MainWindow::insertImage() {
    if (!open_ || open_->session->isReadOnly() || !activePage()) {
        return;
    }
    const auto chosen = dialogs_->chooseImageToInsert(this);
    if (!chosen) {
        return;
    }
    const QString file = QString::fromStdU16String(chosen->u16string());
    QImageReader reader(file);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90)) {
        size.transpose();
    }
    if (!reader.canRead() || !size.isValid() || size.isEmpty()) {
        dialogs_->showError(this, tr("The image could not be inserted."),
                            tr("%1 is not an image StudyBoard can read.").arg(file));
        return;
    }
    const std::string mediaType = QMimeDatabase().mimeTypeForFile(file).name().toStdString();
    const core::PageId page = *activePage();
    const core::Vec2 pixels{static_cast<float>(size.width()), static_cast<float>(size.height())};
    // Copying and hashing a large file took ≈ 0.4 s for 50 MB: done on a pool thread, then
    // stored and inserted here (docs/PERFORMANCE.md).
    runInBackground([this, stage = open_->session->prepareAssetImport(*chosen), mediaType, page,
                     pixels]() -> std::function<void()> {
        auto staged = std::make_shared<core::Result<application::StagedAssetFile>>(stage());
        return [this, staged, mediaType, page, pixels] {
            const QString failed = tr("The image could not be inserted.");
            if (!*staged) {
                dialogs_->showError(this, failed, errorText(staged->error()));
                return;
            }
            // Content-addressed: importing the same file again reuses the stored copy.
            auto asset = open_->session->finishAssetImport(**staged, mediaType);
            if (!asset) {
                dialogs_->showError(this, failed, errorText(asset.error()));
                return;
            }
            if (activePage() != page && !openPage(page)) {
                return; // the page is gone meanwhile; the asset stays for garbage collection
            }
            if (auto inserted = open_->controller->insertImage(*asset, pixels); !inserted) {
                dialogs_->showError(this, failed, errorText(inserted.error()));
                return;
            }
            selectTool(canvas::ToolKind::Select); // the new image is selected, ready to move
        };
    });
}

void MainWindow::importPdf() {
    if (!open_ || open_->session->isReadOnly()) {
        return;
    }
    const auto chosen = dialogs_->chooseDocumentToImport(this);
    if (!chosen) {
        return;
    }
    // Reading the page sizes and copying the file happen on a pool thread; the section is
    // created here. Only read: the PDF is copied into the workspace and never written.
    runInBackground([this, stage = open_->session->prepareAssetImport(*chosen),
                     source = *chosen]() -> std::function<void()> {
        auto info = std::make_shared<core::Result<PdfInfo>>(inspectPdf(source));
        auto staged = std::make_shared<core::Result<application::StagedAssetFile>>(
            *info ? stage()
                  : core::Result<application::StagedAssetFile>(tl::unexpected(info->error())));
        return [this, info, staged, source] {
            const QString file = QString::fromStdU16String(source.u16string());
            const QString failed = tr("The PDF could not be imported.");
            if (!*info) {
                dialogs_->showError(this, failed, tr("%1: %2").arg(file, errorText(info->error())));
                return;
            }
            if (!*staged) {
                dialogs_->showError(this, failed, errorText(staged->error()));
                return;
            }
            auto asset = open_->session->finishAssetImport(**staged, "application/pdf");
            if (!asset) {
                dialogs_->showError(this, failed, errorText(asset.error()));
                return;
            }
            // Into the current notebook, else the first one; with none, a new notebook
            // "Documents".
            std::optional<core::NotebookId> notebook = currentNotebook();
            if (const auto notebooks = open_->session->workspace().notebooks();
                !notebook && !notebooks.empty()) {
                notebook = notebooks.front();
            }
            auto imported = open_->structure.importDocument(
                notebook, QFileInfo(file).completeBaseName().toStdString(), *asset,
                (*info)->pageSizes);
            if (!imported) {
                dialogs_->showError(this, failed, errorText(imported.error()));
                return;
            }
            openPage(imported->firstPage);
            QTreeView* tree = navigation_->tree();
            tree->expand(treeModel_->indexOf(HierarchyItem{imported->notebook}));
            tree->expand(treeModel_->indexOf(HierarchyItem{imported->section}));
            tree->setCurrentIndex(treeModel_->indexOf(HierarchyItem{imported->firstPage}));
        };
    });
}

namespace {

/// A file name from a title: characters no file system accepts become "_".
QString fileNameFrom(const std::string& title, const QString& fallback) {
    QString name = QString::fromStdString(title).trimmed();
    for (QChar& c : name) {
        if (QStringLiteral("\\/:*?\"<>|").contains(c) || c.unicode() < 0x20) {
            c = u'_';
        }
    }
    return name.isEmpty() ? fallback : name;
}

} // namespace

ExportSources MainWindow::exportSources(std::size_t pages) {
    application::WorkspaceSession* session = open_->session;
    ExportSources sources{
        .assetPath = [session](core::AssetId asset) -> std::optional<std::filesystem::path> {
            auto path = session->assetPath(asset);
            return path ? std::optional(std::move(*path)) : std::nullopt;
        },
        .progress = {}};
    if (pages > 1) {
        // Several pages: progress with Cancel (shown only if it takes a while).
        auto dialog = std::make_shared<QProgressDialog>(tr("Exporting pages\u2026"), tr("Cancel"),
                                                        0, static_cast<int>(pages), this);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setMinimumDuration(500);
        sources.progress = [dialog](std::size_t done, std::size_t) {
            dialog->setValue(static_cast<int>(done));
            return !dialog->wasCanceled();
        };
    }
    return sources;
}

std::vector<core::PageId> MainWindow::pagesToExport(bool wholeSection) const {
    const auto page = activePage();
    if (!open_ || !page) {
        return {};
    }
    if (!wholeSection) {
        return {*page};
    }
    const document::Workspace& ws = open_->session->workspace();
    const auto pages = ws.pagesOf(ws.findPage(*page)->section);
    return {pages.begin(), pages.end()};
}

void MainWindow::exportPages(bool wholeSection) {
    const std::vector<core::PageId> pages = pagesToExport(wholeSection);
    if (pages.empty()) {
        return;
    }
    finishTextEditing(); // what is typed is part of the page
    const document::Workspace& ws = open_->session->workspace();
    const document::PageInfo& page = *ws.findPage(pages.front());
    const QString name = wholeSection
                             ? fileNameFrom(ws.findSection(page.section)->title, tr("Section"))
                             : fileNameFrom(page.title, tr("Page"));
    const auto target = dialogs_->chooseExportTarget(this, name, wholeSection);
    if (!target) {
        return;
    }
    const QString failed = tr("The export could not be written.");
    // The workspace directory belongs to StudyBoard: never write into it.
    if (open_->session->isInsideWorkspace(*target)) {
        dialogs_->showError(this, failed, tr("Choose a location outside the workspace folder."));
        return;
    }
    const QString suffix =
        QFileInfo(QString::fromStdU16String(target->u16string())).suffix().toLower();
    const ExportFormat format = suffix == QStringLiteral("png")   ? ExportFormat::Png
                                : suffix == QStringLiteral("svg") ? ExportFormat::Svg
                                                                  : ExportFormat::Pdf;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto written = ui::exportPages(ws, pages, *target, format, exportSources(pages.size()));
    QApplication::restoreOverrideCursor();
    if (!written) {
        if (written.error().code != core::ErrorCode::Conflict) { // Conflict: cancelled
            dialogs_->showError(this, failed, errorText(written.error()));
        }
        return;
    }
    statusBar()->showMessage(
        tr("Exported to %1").arg(QString::fromStdU16String(target->u16string())), 5000);
}

void MainWindow::printPages() {
    // The pages of the current section: all of them, a range, or the current page.
    const std::vector<core::PageId> section = pagesToExport(true);
    if (section.empty()) {
        return;
    }
    finishTextEditing();
    const document::Workspace& ws = open_->session->workspace();
    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(
        QString::fromStdString(ws.findSection(ws.findPage(section.front())->section)->title));
    if (!dialogs_->setUpPrinter(this, printer, static_cast<int>(section.size()))) {
        return;
    }
    std::vector<core::PageId> chosen;
    switch (printer.printRange()) {
    case QPrinter::CurrentPage:
        chosen = pagesToExport(false);
        break;
    case QPrinter::PageRange: {
        const int from = std::max(1, printer.fromPage());
        const int to = std::min(static_cast<int>(section.size()), printer.toPage());
        for (int i = from; i <= to; ++i) {
            chosen.push_back(section[static_cast<std::size_t>(i - 1)]);
        }
        break;
    }
    default:
        chosen = section;
        break;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto printed =
        ui::printPages(printer, open_->session->workspace(), chosen, exportSources(chosen.size()));
    QApplication::restoreOverrideCursor();
    if (!printed && printed.error().code != core::ErrorCode::Conflict) {
        dialogs_->showError(this, tr("The pages could not be printed."),
                            errorText(printed.error()));
    }
}

// ---------------------------------------------------------------------------- panes

void MainWindow::focusPane(int step) {
    // The panes shown, in order; each takes the focus on its main control.
    std::vector<QWidget*> panes;
    if (navigation_->isVisible()) {
        panes.push_back(navigation_->isShowingSearchResults()
                            ? static_cast<QWidget*>(navigation_->searchResults())
                            : static_cast<QWidget*>(navigation_->tree()));
    }
    if (canvasWidget_ != nullptr && canvasWidget_->isVisible()) {
        panes.push_back(canvasWidget_);
    }
    if (planner_->isVisible()) {
        panes.push_back(planner_);
    }
    if (panes.empty()) {
        return;
    }
    const QWidget* focus = QApplication::focusWidget();
    // Nothing focused in a pane: F6 goes to the first pane, Shift+F6 to the last.
    std::size_t current = step > 0 ? panes.size() - 1 : 0;
    for (std::size_t i = 0; i < panes.size(); ++i) {
        if (focus != nullptr && (focus == panes[i] || panes[i]->isAncestorOf(focus))) {
            current = i;
            break;
        }
    }
    const auto count = static_cast<std::ptrdiff_t>(panes.size());
    const auto next = ((static_cast<std::ptrdiff_t>(current) + step) % count + count) % count;
    QWidget* pane = panes[static_cast<std::size_t>(next)];
    if (pane == planner_) {
        // The planner's first control that takes the focus by keyboard (its view tabs).
        for (QWidget* w = planner_->nextInFocusChain(); w != nullptr && w != planner_;
             w = w->nextInFocusChain()) {
            if (planner_->isAncestorOf(w) && w->isVisible() && w->isEnabled() &&
                (w->focusPolicy() & Qt::TabFocus) != 0) {
                w->setFocus(Qt::TabFocusReason);
                return;
            }
        }
        return;
    }
    pane->setFocus(Qt::TabFocusReason);
}

// ---------------------------------------------------------------------------- maintenance

void MainWindow::backUpNow() {
    if (!open_ || open_->session->isReadOnly()) {
        return;
    }
    finishTextEditing();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto backup = open_->session->backUpNow();
    QApplication::restoreOverrideCursor();
    if (!backup) {
        dialogs_->showError(this, tr("The backup could not be written."),
                            errorText(backup.error()));
        return;
    }
    statusBar()->showMessage(
        tr("Backed up to %1").arg(QString::fromStdU16String(backup->u16string())), 5000);
}

void MainWindow::checkWorkspace() {
    if (!open_) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto report = open_->session->checkIntegrity();
    QApplication::restoreOverrideCursor();
    if (!report) {
        dialogs_->showError(this, tr("The workspace could not be checked."),
                            errorText(report.error()));
        return;
    }
    if (report->problems.empty()) {
        dialogs_->showInformation(this, tr("No problems found."),
                                  tr("The database and every stored file were checked."));
        return;
    }
    QStringList lines;
    for (const std::string& problem : report->problems) {
        lines << toQString(problem);
    }
    dialogs_->showInformation(
        this,
        tr("%n problem(s) found. Backups of the database are in the workspace's backups folder.",
           nullptr, static_cast<int>(lines.size())),
        lines.join(QLatin1Char('\n')));
}

// ---------------------------------------------------------------------------- bundles

void MainWindow::exportBundle(bool notebookOnly) {
    if (!open_) {
        return;
    }
    finishTextEditing();
    const document::Workspace& ws = open_->session->workspace();
    std::optional<core::NotebookId> notebook;
    if (notebookOnly) {
        notebook = currentNotebook();
        if (!notebook && !ws.notebooks().empty()) {
            notebook = ws.notebooks().front();
        }
        if (!notebook) {
            return;
        }
    }
    const QString name = notebook ? fileNameFrom(ws.findNotebook(*notebook)->title, tr("Notebook"))
                                  : fileNameFrom(ws.info().name, tr("Workspace"));
    const auto target = dialogs_->chooseBundleTarget(this, name);
    if (!target) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto written = open_->session->exportBundle(*target, notebook);
    QApplication::restoreOverrideCursor();
    if (!written) {
        dialogs_->showError(this, tr("The bundle could not be written."),
                            errorText(written.error()));
        return;
    }
    statusBar()->showMessage(
        tr("Exported to %1").arg(QString::fromStdU16String(target->u16string())), 5000);
}

void MainWindow::importNotebookBundle() {
    if (!open_ || open_->session->isReadOnly()) {
        return;
    }
    finishTextEditing();
    const auto bundle = dialogs_->chooseBundleToOpen(this);
    if (!bundle) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto imported = open_->session->importBundle(*bundle);
    QApplication::restoreOverrideCursor();
    if (!imported) {
        dialogs_->showError(this, tr("The bundle could not be imported."),
                            errorText(imported.error()));
        return;
    }
    // Show the first imported notebook's first page.
    const document::Workspace& ws = open_->session->workspace();
    const core::NotebookId first = imported->front();
    QTreeView* tree = navigation_->tree();
    tree->expand(treeModel_->indexOf(HierarchyItem{first}));
    for (const core::SectionId section : ws.sectionsOf(first)) {
        if (const auto pages = ws.pagesOf(section); !pages.empty()) {
            openPage(pages.front());
            tree->setCurrentIndex(treeModel_->indexOf(HierarchyItem{pages.front()}));
            break;
        }
    }
}

void MainWindow::openBundleAsWorkspace() {
    if (!services_) {
        return;
    }
    const auto bundle = dialogs_->chooseBundleToOpen(this);
    if (!bundle) {
        return;
    }
    const auto root = dialogs_->chooseNewWorkspace(this);
    if (!root) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto extracted = application::WorkspaceSession::extractBundle(*bundle, *root);
    QApplication::restoreOverrideCursor();
    if (!extracted) {
        dialogs_->showError(this, tr("The bundle could not be opened."),
                            errorText(extracted.error()));
        return;
    }
    (void)openWorkspace(*root);
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
    refreshInkIcons(); // swatch borders follow the theme
    syncInkActions();
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
    importPdfAction_->setEnabled(writable);
    // Exporting and printing only read: also in read-only workspaces.
    const bool hasPage = open && activePage().has_value();
    exportPageAction_->setEnabled(hasPage);
    exportSectionAction_->setEnabled(hasPage);
    printAction_->setEnabled(hasPage);
    exportWorkspaceAction_->setEnabled(open);
    exportNotebookAction_->setEnabled(open && open_->session->workspace().notebookCount() > 0);
    importNotebookAction_->setEnabled(writable);
    openBundleAction_->setEnabled(canOpen);
    backUpAction_->setEnabled(writable);
    checkWorkspaceAction_->setEnabled(open);

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
    cutAction_->setEnabled(writable);
    pasteAction_->setEnabled(writable);
    copyAction_->setEnabled(open);
    selectAllAction_->setEnabled(open);
    findAction_->setEnabled(open);
    navigation_->searchField()->setEnabled(open);

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
    } else if (backgroundJobs_ > 0) {
        status = tr("Importing…");
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
