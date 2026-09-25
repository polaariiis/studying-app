#include <studyapp/ui/MainWindow.hpp>

#include "CanvasWidget.hpp"
#include "SessionDocumentPort.hpp"

#include <studyapp/application/ComponentVersions.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/ui/AppIcon.hpp>
#include <studyapp/ui/CanvasPlaceholder.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>

#include <string_view>

namespace studyapp::ui {

namespace {

const QString kGeometryKey = QStringLiteral("mainWindow/geometry");
const QString kStateKey = QStringLiteral("mainWindow/state");
const QString kThemeKey = QStringLiteral("appearance/theme");
const QString kHudKey = QStringLiteral("canvas/debugHud");

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

MainWindow::MainWindow(ThemeManager& themes, QSettings& settings, QWidget* parent)
    : QMainWindow(parent), themes_(&themes), settings_(&settings) {
    setUp();
}

MainWindow::MainWindow(ThemeManager& themes, QSettings& settings, const WorkspaceContext& workspace,
                       QWidget* parent)
    : QMainWindow(parent), themes_(&themes), settings_(&settings),
      workspace_(std::make_unique<WorkspaceContext>(workspace)) {
    port_ = std::make_unique<SessionDocumentPort>(workspace_->session);
    controller_ = std::make_unique<canvas::CanvasController>(*port_, workspace_->ids);
    // Every applied patch — canvas edits, undo and redo — keeps the canvas in step.
    workspace_->session.setPatchListener([this](const document::Patch& patch) {
        controller_->onDocumentChanged(patch);
        onDocumentChanged();
    });
    setUp();
    controller_->setPage(workspace_->page);
}

MainWindow::~MainWindow() {
    if (workspace_) {
        workspace_->session.setPatchListener({});
    }
    // The canvas widget refers to the controller: destroy it while the controller exists.
    delete canvasWidget_;
    canvasWidget_ = nullptr;
}

void MainWindow::setUp() {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(toQString(core::build::kProductName));
    setWindowIcon(applicationIcon());
    resize(1100, 720);

    createActions();
    if (workspace_) {
        createCanvasActions();
    }
    createMenus();
    createToolBar();
    createCentralWidget();
    createStatusBar();
    readSettings();

    connect(themes_, &ThemeManager::themeChanged, this, &MainWindow::syncThemeActions);
    connect(themes_, &ThemeManager::themeChanged, this, &MainWindow::applyCanvasTheme);
    syncThemeActions();
    applyCanvasTheme();
    updateEditActions();
    updateSaveStatus();
}

void MainWindow::createActions() {
    quitAction_ = new QAction(tr("&Quit"), this);
    quitAction_->setObjectName(QStringLiteral("actionQuit"));
    quitAction_->setShortcuts(QKeySequence::Quit);
    quitAction_->setMenuRole(QAction::QuitRole);
    connect(quitAction_, &QAction::triggered, this, &QWidget::close);

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

    toggleThemeAction_ = new QAction(tr("Toggle Light/Dark"), this);
    toggleThemeAction_->setObjectName(QStringLiteral("actionToggleTheme"));
    toggleThemeAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    toggleThemeAction_->setStatusTip(tr("Switch between the light and dark theme"));
    connect(toggleThemeAction_, &QAction::triggered, themes_, &ThemeManager::toggleLightDark);

    aboutAction_ = new QAction(tr("&About %1").arg(toQString(core::build::kProductName)), this);
    aboutAction_->setObjectName(QStringLiteral("actionAbout"));
    aboutAction_->setMenuRole(QAction::AboutRole);
    connect(aboutAction_, &QAction::triggered, this, &MainWindow::showAbout);

    aboutQtAction_ = new QAction(tr("About &Qt"), this);
    aboutQtAction_->setObjectName(QStringLiteral("actionAboutQt"));
    aboutQtAction_->setMenuRole(QAction::AboutQtRole);
    connect(aboutQtAction_, &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::createCanvasActions() {
    const auto make = [this](const QString& text, const QString& name, const QKeySequence& key) {
        auto* action = new QAction(text, this);
        action->setObjectName(name);
        action->setShortcut(key);
        return action;
    };

    // Changes are saved continuously; Save only makes sure nothing is pending.
    saveAction_ = make(tr("&Save"), QStringLiteral("actionSave"), QKeySequence::Save);
    saveAction_->setStatusTip(tr("Changes are saved automatically; this writes anything pending"));
    connect(saveAction_, &QAction::triggered, this, [this] {
        (void)workspace_->session.flush();
        updateSaveStatus();
    });

    undoAction_ = make(tr("&Undo"), QStringLiteral("actionUndo"), QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, [this] {
        if (!controller_->isGestureActive()) {
            (void)workspace_->session.undo();
        }
    });
    redoAction_ = make(tr("&Redo"), QStringLiteral("actionRedo"), QKeySequence::Redo);
    redoAction_->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});
    connect(redoAction_, &QAction::triggered, this, [this] {
        if (!controller_->isGestureActive()) {
            (void)workspace_->session.redo();
        }
    });
    deleteAction_ = make(tr("&Delete"), QStringLiteral("actionDelete"), QKeySequence::Delete);
    connect(deleteAction_, &QAction::triggered, this,
            [this] { (void)controller_->deleteSelection(); });
    selectAllAction_ =
        make(tr("Select &All"), QStringLiteral("actionSelectAll"), QKeySequence::SelectAll);
    connect(selectAllAction_, &QAction::triggered, this, [this] {
        controller_->setTool(canvas::ToolKind::Select);
        for (QAction* action : toolGroup_->actions()) {
            action->setChecked(action->data().toInt() ==
                               static_cast<int>(canvas::ToolKind::Select));
        }
        controller_->selectAll();
        if (canvasWidget_ != nullptr) {
            canvasWidget_->refreshCursor();
        }
    });

    toolGroup_ = new QActionGroup(this);
    toolGroup_->setExclusive(true);
    const auto makeTool = [&](const QString& text, const QString& name, Qt::Key key,
                              canvas::ToolKind tool) {
        QAction* action = make(text, name, QKeySequence(key));
        action->setCheckable(true);
        action->setData(static_cast<int>(tool));
        action->setActionGroup(toolGroup_);
        connect(action, &QAction::triggered, this, [this, tool] {
            controller_->setTool(tool);
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

    zoomInAction_ = make(tr("Zoom &In"), QStringLiteral("actionZoomIn"), QKeySequence::ZoomIn);
    connect(zoomInAction_, &QAction::triggered, this, [this] { controller_->zoomBy(1.25); });
    zoomOutAction_ = make(tr("Zoom &Out"), QStringLiteral("actionZoomOut"), QKeySequence::ZoomOut);
    connect(zoomOutAction_, &QAction::triggered, this, [this] { controller_->zoomBy(0.8); });
    resetViewAction_ = make(tr("&Reset View"), QStringLiteral("actionResetView"),
                            QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(resetViewAction_, &QAction::triggered, this, [this] { controller_->resetView(); });
    fitAction_ = make(tr("Zoom to &Fit"), QStringLiteral("actionZoomToFit"),
                      QKeySequence(Qt::CTRL | Qt::Key_1));
    connect(fitAction_, &QAction::triggered, this, [this] { controller_->zoomToFit(); });

    hudAction_ = make(tr("Debug &HUD"), QStringLiteral("actionDebugHud"), QKeySequence(Qt::Key_F3));
    hudAction_->setCheckable(true);
    connect(hudAction_, &QAction::toggled, this, [this](bool on) {
        if (canvasWidget_ != nullptr) {
            canvasWidget_->setHudVisible(on);
        }
    });

    // Page format (templates and page settings UI arrive with navigation in Phase 5).
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
            const document::PageInfo* page =
                workspace_->session.workspace().findPage(workspace_->page);
            setPageFormat(static_cast<int>(pattern),
                          page != nullptr && page->extent == document::PageExtent::Bounded);
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
            const document::PageInfo* page =
                workspace_->session.workspace().findPage(workspace_->page);
            setPageFormat(page != nullptr ? static_cast<int>(page->background.pattern) : 0,
                          bounded);
            controller_->resetView();
        });
    }
}

void MainWindow::createMenus() {
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("menuFile"));
    if (saveAction_ != nullptr) {
        fileMenu->addAction(saveAction_);
        fileMenu->addSeparator();
    }
    fileMenu->addAction(quitAction_);

    if (workspace_) {
        QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
        editMenu->setObjectName(QStringLiteral("menuEdit"));
        editMenu->addAction(undoAction_);
        editMenu->addAction(redoAction_);
        editMenu->addSeparator();
        editMenu->addAction(deleteAction_);
        editMenu->addAction(selectAllAction_);

        QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
        toolsMenu->setObjectName(QStringLiteral("menuTools"));
        toolsMenu->addActions(toolGroup_->actions());
    }

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("menuView"));
    QMenu* themeMenu = viewMenu->addMenu(tr("&Theme"));
    themeMenu->setObjectName(QStringLiteral("menuTheme"));
    themeMenu->addActions(themeGroup_->actions());
    viewMenu->addSeparator();
    viewMenu->addAction(toggleThemeAction_);
    if (workspace_) {
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
    }

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setObjectName(QStringLiteral("menuHelp"));
    helpMenu->addAction(aboutAction_);
    helpMenu->addAction(aboutQtAction_);
}

void MainWindow::createToolBar() {
    QToolBar* toolBar = addToolBar(tr("Main"));
    toolBar->setObjectName(QStringLiteral("mainToolBar"));
    toolBar->setMovable(false);
    if (workspace_) {
        toolBar->addActions(toolGroup_->actions());
        toolBar->addSeparator();
        toolBar->addAction(undoAction_);
        toolBar->addAction(redoAction_);
        toolBar->addSeparator();
    }
    toolBar->addAction(toggleThemeAction_);
}

void MainWindow::createCentralWidget() {
    if (!workspace_) {
        setCentralWidget(new CanvasPlaceholder(*themes_, this));
        return;
    }
    canvasWidget_ = new CanvasWidget(*controller_, this);
    setCentralWidget(canvasWidget_);
    canvasWidget_->setFocus();
}

void MainWindow::createStatusBar() {
    if (workspace_) {
        saveStatusLabel_ = new QLabel(this);
        saveStatusLabel_->setObjectName(QStringLiteral("saveStatusLabel"));
        statusBar()->addWidget(saveStatusLabel_);
    }
    auto* versionLabel =
        new QLabel(QStringLiteral("%1 %2").arg(toQString(core::build::kProductName),
                                               toQString(core::build::kVersion)),
                   this);
    versionLabel->setObjectName(QStringLiteral("versionLabel"));
    statusBar()->addPermanentWidget(versionLabel);
}

void MainWindow::readSettings() {
    themes_->setMode(themeModeFromSettingsValue(settings_->value(kThemeKey).toString()));
    restoreGeometry(settings_->value(kGeometryKey).toByteArray());
    restoreState(settings_->value(kStateKey).toByteArray());
    if (hudAction_ != nullptr) {
        hudAction_->setChecked(settings_->value(kHudKey, false).toBool());
    }
}

void MainWindow::writeSettings() {
    settings_->setValue(kThemeKey, toSettingsValue(themes_->mode()));
    settings_->setValue(kGeometryKey, saveGeometry());
    settings_->setValue(kStateKey, saveState());
    if (hudAction_ != nullptr) {
        settings_->setValue(kHudKey, hudAction_->isChecked());
    }
    settings_->sync();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    writeSettings();
    if (workspace_) {
        // Writes are continuous; this only catches anything a failed write left pending.
        // P3-01 (what to offer when a write keeps failing) is designed in Phase 5; until
        // then the failure is logged and shown, never silently ignored.
        if (auto flushed = workspace_->session.flush(); !flushed) {
            core::logError("ui", "closing with unsaved changes: " + flushed.error().message);
        }
    }
    event->accept();
}

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
}

void MainWindow::applyCanvasTheme() {
    if (!controller_) {
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
    controller_->setColors(colors);
    if (canvasWidget_ != nullptr) {
        canvasWidget_->refreshCursor(); // the eraser ring takes the new colour
        canvasWidget_->update();
    }
}

void MainWindow::onDocumentChanged() {
    updateEditActions();
    updateSaveStatus();
}

void MainWindow::updateEditActions() {
    if (!workspace_) {
        return;
    }
    application::WorkspaceSession& session = workspace_->session;
    const bool writable = !session.isReadOnly();
    undoAction_->setEnabled(writable && session.canUndo());
    redoAction_->setEnabled(writable && session.canRedo());
    const auto label = [](std::string_view text) {
        return toQString(text);
    };
    const document::Command* nextUndo = session.history().nextUndo();
    const document::Command* nextRedo = session.history().nextRedo();
    undoAction_->setText(nextUndo != nullptr ? tr("&Undo %1").arg(label(nextUndo->label))
                                             : tr("&Undo"));
    redoAction_->setText(nextRedo != nullptr ? tr("&Redo %1").arg(label(nextRedo->label))
                                             : tr("&Redo"));
    deleteAction_->setEnabled(writable);

    const document::PageInfo* page = session.workspace().findPage(workspace_->page);
    if (page != nullptr) {
        for (QAction* action : backgroundGroup_->actions()) {
            action->setChecked(action->data().toInt() ==
                               static_cast<int>(page->background.pattern));
            action->setEnabled(writable);
        }
        for (QAction* action : extentGroup_->actions()) {
            action->setChecked((action->data().toInt() == 1) ==
                               (page->extent == document::PageExtent::Bounded));
            action->setEnabled(writable);
        }
    }
}

void MainWindow::updateSaveStatus() {
    if (saveStatusLabel_ == nullptr) {
        return;
    }
    const application::WorkspaceSession& session = workspace_->session;
    if (session.isReadOnly()) {
        saveStatusLabel_->setText(tr("Read-only"));
    } else if (const auto& error = session.lastWriteError()) {
        saveStatusLabel_->setText(tr("Saving failed (%1 pending): %2")
                                      .arg(session.pendingWriteCount())
                                      .arg(toQString(error->message)));
    } else {
        saveStatusLabel_->setText(tr("All changes saved"));
    }
}

void MainWindow::setPageFormat(int backgroundPattern, bool bounded) {
    const document::Workspace& workspace = workspace_->session.workspace();
    const document::PageInfo* page = workspace.findPage(workspace_->page);
    if (page == nullptr) {
        return;
    }
    document::commands::PageFormat format{.extent = bounded ? document::PageExtent::Bounded
                                                            : document::PageExtent::Infinite,
                                          .size = bounded ? document::kA4PortraitSize : page->size,
                                          .background = page->background};
    format.background.pattern = static_cast<document::BackgroundPattern>(backgroundPattern);
    auto command =
        document::commands::setPageFormat(workspace, workspace_->page, format, workspace_->clock);
    if (command) {
        (void)workspace_->session.execute(std::move(*command));
    }
    updateEditActions();
}

void MainWindow::runPanBenchmark(int frames, double zoomFactor,
                                 std::function<void(const PanBenchmarkResult&)> done) {
    if (canvasWidget_ != nullptr) {
        if (zoomFactor > 0.0) {
            controller_->zoomBy(zoomFactor);
        } else {
            controller_->zoomToFit(); // 0 or negative: everything on the page in view
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
