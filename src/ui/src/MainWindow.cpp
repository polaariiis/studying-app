#include <studyapp/ui/MainWindow.hpp>

#include <studyapp/application/ComponentVersions.hpp>
#include <studyapp/core/BuildInfo.hpp>
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

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

MainWindow::MainWindow(ThemeManager& themes, QSettings& settings, QWidget* parent)
    : QMainWindow(parent), themes_(&themes), settings_(&settings) {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(toQString(core::build::kProductName));
    setWindowIcon(applicationIcon());
    resize(1100, 720);

    createActions();
    createMenus();
    createToolBar();
    createCentralPlaceholder();
    createStatusBar();
    readSettings();

    connect(themes_, &ThemeManager::themeChanged, this, &MainWindow::syncThemeActions);
    syncThemeActions();
}

MainWindow::~MainWindow() = default;

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

void MainWindow::createMenus() {
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("menuFile"));
    fileMenu->addAction(quitAction_);

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("menuView"));
    QMenu* themeMenu = viewMenu->addMenu(tr("&Theme"));
    themeMenu->setObjectName(QStringLiteral("menuTheme"));
    themeMenu->addActions(themeGroup_->actions());
    viewMenu->addSeparator();
    viewMenu->addAction(toggleThemeAction_);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setObjectName(QStringLiteral("menuHelp"));
    helpMenu->addAction(aboutAction_);
    helpMenu->addAction(aboutQtAction_);
}

void MainWindow::createToolBar() {
    QToolBar* toolBar = addToolBar(tr("Main"));
    toolBar->setObjectName(QStringLiteral("mainToolBar"));
    toolBar->setMovable(false);
    toolBar->addAction(toggleThemeAction_);
}

void MainWindow::createCentralPlaceholder() {
    setCentralWidget(new CanvasPlaceholder(*themes_, this));
}

void MainWindow::createStatusBar() {
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
}

void MainWindow::writeSettings() {
    settings_->setValue(kThemeKey, toSettingsValue(themes_->mode()));
    settings_->setValue(kGeometryKey, saveGeometry());
    settings_->setValue(kStateKey, saveState());
    settings_->sync();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    writeSettings();
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
