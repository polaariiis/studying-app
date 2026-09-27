// End-to-end canvas test through the real Qt widget, OpenGL renderer and workspace
// session: scripted mouse/wheel input → canvas → command → workspace → SQLite, checked in
// the document, in rendered pixels and after reopening the workspace.
//
// Needs an OpenGL 3.3 context. On platforms without one (e.g. QT_QPA_PLATFORM=offscreen
// on CI) the test skips itself; run the executable directly on a desktop to exercise it.

#include "CanvasWidget.hpp"

#include <studyapp/application/StartPage.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/platform/QtTextLayout.hpp>
#include <studyapp/platform/QtWorkspaceLocker.hpp>
#include <studyapp/render_gl/OpenGLRenderer.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/ui/MainWindow.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLWidget>
#include <QPixmap>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>

using namespace studyapp;

namespace {

std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.toStdU16String());
}

/// World position of each end of the stroke `id`.
std::pair<core::DVec2, core::DVec2> strokeEnds(const document::Workspace& ws, core::ElementId id) {
    const document::Element& element = *ws.findElement(id);
    const auto& points = *std::get<document::Stroke>(element.payload).points;
    const core::Affine2 toWorld = document::localToWorld(element.transform);
    return {toWorld.apply({points.front().x, points.front().y}),
            toWorld.apply({points.back().x, points.back().y})};
}

std::vector<core::ElementId> strokesOn(const document::Workspace& ws, core::PageId page) {
    std::vector<core::ElementId> ids;
    for (const core::LayerId layer : ws.layersOf(page)) {
        for (const core::ElementId id : ws.elementsOf(layer)) {
            ids.push_back(id);
        }
    }
    return ids;
}

void drag(QWidget* widget, QPoint from, QPoint to, int steps = 12) {
    QTest::mousePress(widget, Qt::LeftButton, {}, from);
    for (int i = 1; i <= steps; ++i) {
        QTest::mouseMove(widget, from + (to - from) * i / steps, 2);
    }
    QTest::mouseRelease(widget, Qt::LeftButton, {}, to);
}

void wheel(QWidget* widget, QPoint at, int angle) {
    QWheelEvent event(QPointF(at), widget->mapToGlobal(QPointF(at)), QPoint(), QPoint(0, angle),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(widget, &event);
}

} // namespace

class CanvasWidgetTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void drawZoomUndoAndReopen() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        const application::SessionServices services{clock, ids, locker};

        auto created = application::WorkspaceSession::create(root, "Canvas test", services);
        QVERIFY(created.has_value());
        std::unique_ptr<application::WorkspaceSession> session = std::move(*created);
        auto page = application::ensureStartPage(*session, clock, ids);
        QVERIFY(page.has_value());

        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ui::ThemeManager themes;
        auto window = std::make_unique<ui::MainWindow>(
            themes, settings,
            ui::WorkspaceContext{.session = *session, .ids = ids, .clock = clock, .page = *page});
        window->resize(900, 700);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));

        auto* canvas = window->findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        QTest::qWait(50);
        if (!canvas->isValid() || canvas->grabFramebuffer().isNull() ||
            canvas->context() == nullptr ||
            std::pair(canvas->format().majorVersion(), canvas->format().minorVersion()) <
                std::pair(3, 3)) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }

        // 1. Draw at zoom 1: the start page shows the world origin at the top-left.
        drag(canvas, {100, 120}, {320, 200});
        auto ids1 = strokesOn(session->workspace(), *page);
        QCOMPARE(ids1.size(), std::size_t{1});
        auto [start, end] = strokeEnds(session->workspace(), ids1[0]);
        QVERIFY(std::abs(start.x - 100) < 1e-6 && std::abs(start.y - 120) < 1e-6);
        QVERIFY(std::abs(end.x - 320) < 1e-3 && std::abs(end.y - 200) < 1e-3);

        // The ink is visible where it was drawn: the pixel on the stroke differs from the
        // paper next to it (sampled in device pixels).
        const QImage image = canvas->grabFramebuffer();
        const qreal dpr = canvas->devicePixelRatioF();
        const auto sample = [&](QPointF logical) {
            return image.pixelColor((logical * dpr).toPoint());
        };
        const QColor ink = sample({210, 160});   // on the straight stroke
        const QColor paper = sample({210, 400}); // empty paper (away from the dot grid)
        QVERIFY2(ink != paper, "the stroke is not visible where it was drawn");

        // 2. Zoom in around the cursor, then draw: still aligned with the pointer.
        const QPoint anchor{450, 350};
        wheel(canvas, anchor, 240);
        const double zoom = std::pow(1.0015, 240);
        drag(canvas, {400, 300}, {600, 300});
        auto ids2 = strokesOn(session->workspace(), *page);
        QCOMPARE(ids2.size(), std::size_t{2});
        const core::DVec2 expected{anchor.x() + (400.0 - anchor.x()) / zoom,
                                   anchor.y() + (300.0 - anchor.y()) / zoom};
        const auto [zoomedStart, zoomedEnd] = strokeEnds(session->workspace(), ids2[1]);
        QVERIFY2(std::abs(zoomedStart.x - expected.x) < 1e-6 &&
                     std::abs(zoomedStart.y - expected.y) < 1e-6,
                 "pointer and world disagree after zooming");
        (void)zoomedEnd;

        // 3. Undo through the window's action removes the last stroke; saved continuously.
        auto* undo = window->findChild<QAction*>(QStringLiteral("actionUndo"));
        QVERIFY(undo != nullptr && undo->isEnabled());
        undo->trigger();
        QCOMPARE(strokesOn(session->workspace(), *page).size(), std::size_t{1});
        QCOMPARE(session->pendingWriteCount(), std::size_t{0});

        const document::Element saved = *session->workspace().findElement(ids1[0]);
        window.reset();
        QVERIFY(session->close().has_value());
        session.reset();

        // 4. Reopen: the ink is identical.
        auto reopened = application::WorkspaceSession::open(root, {}, services);
        QVERIFY(reopened.has_value());
        const auto reloaded = strokesOn((*reopened)->workspace(), *page);
        QCOMPARE(reloaded.size(), std::size_t{1});
        QVERIFY(*(*reopened)->workspace().findElement(ids1[0]) == saved);
    }

    // Background modes and bounded pages, checked by sampling pixels. With
    // STUDYAPP_TEST_SCREENSHOTS=<dir> every mode is also saved as a PNG for review.
    void backgroundsAndBoundedPages() {
        QTemporaryDir dir;
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        auto created =
            application::WorkspaceSession::create(root, "Backgrounds", {clock, ids, locker});
        QVERIFY(created.has_value());
        auto& session = **created;
        auto page = application::ensureStartPage(session, clock, ids);
        QVERIFY(page.has_value());
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        // The window applies the theme stored in its settings; STUDYAPP_TEST_THEME=dark
        // renders the dark-paper variant for review.
        const QString theme = qEnvironmentVariable("STUDYAPP_TEST_THEME", QStringLiteral("light"));
        settings.setValue(QStringLiteral("appearance/theme"), theme);
        ui::ThemeManager themes;
        ui::MainWindow window(
            themes, settings,
            ui::WorkspaceContext{.session = session, .ids = ids, .clock = clock, .page = *page});
        window.resize(900, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* canvas = window.findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        QTest::qWait(50);
        if (!canvas->isValid() || canvas->grabFramebuffer().isNull()) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }
        const QString shots = qEnvironmentVariable("STUDYAPP_TEST_SCREENSHOTS");
        const auto format = [&](document::BackgroundPattern pattern, bool bounded) {
            const document::PageInfo& info = *session.workspace().findPage(*page);
            document::commands::PageFormat f{
                .extent = bounded ? document::PageExtent::Bounded : document::PageExtent::Infinite,
                .size = bounded ? document::kA4PortraitSize : info.size,
                .background = info.background};
            f.background.pattern = pattern;
            auto command = document::commands::setPageFormat(session.workspace(), *page, f, clock);
            QVERIFY(command.has_value());
            QVERIFY(session.execute(std::move(*command)).has_value());
        };
        const auto grab = [&](const char* name) {
            canvas->update();
            QTest::qWait(30);
            QImage image = canvas->grabFramebuffer();
            if (!shots.isEmpty()) {
                image.save(QDir(shots).filePath(theme + QStringLiteral("-") +
                                                QString::fromLatin1(name) +
                                                QStringLiteral(".png")));
            }
            return image;
        };
        const qreal dpr = canvas->devicePixelRatioF();
        const auto at = [&](const QImage& image, qreal x, qreal y) {
            return image.pixelColor(QPointF(x * dpr, y * dpr).toPoint());
        };

        // Blank infinite page: uniform paper.
        format(document::BackgroundPattern::None, false);
        const QImage blank = grab("blank");
        const QColor paper = at(blank, 300, 300);
        QCOMPARE(at(blank, 301, 312), paper);

        // Ruled: lines every 24 world units (zoom 1: every 24 px) across the paper.
        format(document::BackgroundPattern::Ruled, false);
        const QImage ruled = grab("ruled");
        QVERIFY2(at(ruled, 300, 48) != paper, "ruled line missing");
        QCOMPARE(at(ruled, 300, 60), paper); // between lines

        // Grid: vertical lines too.
        format(document::BackgroundPattern::Grid, false);
        const QImage grid = grab("grid");
        QVERIFY(at(grid, 48, 300) != paper);
        QVERIFY(at(grid, 300, 48) != paper);

        // Dots: only at lattice points.
        format(document::BackgroundPattern::Dots, false);
        const QImage dots = grab("dots");
        QVERIFY(at(dots, 48, 48) != paper);
        QCOMPARE(at(dots, 48, 60), paper);

        // Bounded A4 page, fitted: desk outside, paper inside.
        format(document::BackgroundPattern::Ruled, true);
        auto* reset = window.findChild<QAction*>(QStringLiteral("actionResetView"));
        QVERIFY(reset != nullptr);
        reset->trigger();
        const QImage bounded = grab("bounded-a4-ruled");
        QVERIFY2(at(bounded, 5, 350) != at(bounded, 450, 350), "page edge not visible");

        // Drawing on the bounded page still lands where the pointer is.
        drag(canvas, {400, 200}, {500, 260});
        QCOMPARE(strokesOn(session.workspace(), *page).size(), std::size_t{1});
        const QImage inked = grab("bounded-a4-ink");
        QVERIFY(at(inked, 450, 230) != at(bounded, 450, 230));

        // Selection is indicated by a frame around the selected stroke.
        auto* select = window.findChild<QAction*>(QStringLiteral("actionToolSelect"));
        QVERIFY(select != nullptr);
        select->trigger();
        QTest::mouseClick(canvas, Qt::LeftButton, {}, QPoint(450, 230));
        const QImage selected = grab("selected");
        // The frame's top edge lies a few pixels above the stroke's top (y = 200).
        bool frameFound = false;
        for (qreal y = 188; y <= 200 && !frameFound; y += 0.5) {
            frameFound = at(selected, 450, y) != at(inked, 450, y);
        }
        QVERIFY2(frameFound, "selection frame not drawn");

        // Debug HUD: can be shown, reports the frame, and can be hidden again.
        auto* hudAction = window.findChild<QAction*>(QStringLiteral("actionDebugHud"));
        auto* hud = window.findChild<QLabel*>(QStringLiteral("canvasHud"));
        QVERIFY(hudAction != nullptr && hud != nullptr);
        hudAction->setChecked(true);
        canvas->update();
        QTest::qWait(50);
        QVERIFY(hud->isVisible());
        QVERIFY(hud->text().contains(QStringLiteral("frame")));
        QVERIFY(hud->text().contains(QStringLiteral("OpenGL")));
        if (!shots.isEmpty()) {
            window.grab().save(QDir(shots).filePath(theme + QStringLiteral("-hud.png")));
        }
        hudAction->setChecked(false);
        QVERIFY(!hud->isVisible());
    }

    // Resizing changes only the viewport: after several resizes (at this desktop's device
    // pixel ratio) ink still appears exactly under the pointer that drew it, and the page
    // content is not rebuilt.
    void resizeKeepsInputAndRenderingAligned() {
        QTemporaryDir dir;
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        auto created = application::WorkspaceSession::create(root, "Resize", {clock, ids, locker});
        QVERIFY(created.has_value());
        auto& session = **created;
        auto page = application::ensureStartPage(session, clock, ids);
        QVERIFY(page.has_value());
        { // blank paper, so sampled pixels are either ink or paper
            const document::PageInfo& info = *session.workspace().findPage(*page);
            document::commands::PageFormat format{
                .extent = info.extent, .size = info.size, .background = info.background};
            format.background.pattern = document::BackgroundPattern::None;
            auto command =
                document::commands::setPageFormat(session.workspace(), *page, format, clock);
            QVERIFY(command.has_value());
            QVERIFY(session.execute(std::move(*command)).has_value());
        }
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ui::ThemeManager themes;
        ui::MainWindow window(
            themes, settings,
            ui::WorkspaceContext{.session = session, .ids = ids, .clock = clock, .page = *page});
        window.resize(900, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* canvas = window.findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        QTest::qWait(50);
        if (!canvas->isValid() || canvas->grabFramebuffer().isNull()) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }
        drag(canvas, {150, 150}, {350, 150});
        for (const QSize size : {QSize(700, 520), QSize(1100, 800), QSize(820, 640)}) {
            window.resize(size);
            QTest::qWait(30);
        }
        drag(canvas, {150, 260}, {350, 260});
        QCOMPARE(strokesOn(session.workspace(), *page).size(), std::size_t{2});
        canvas->update();
        QTest::qWait(30);
        const QImage image = canvas->grabFramebuffer();
        const qreal dpr = canvas->devicePixelRatioF();
        // Framebuffer = logical size × DPR.
        QCOMPARE(image.size(),
                 QSize(qRound(canvas->width() * dpr), qRound(canvas->height() * dpr)));
        const auto at = [&](qreal x, qreal y) {
            return image.pixelColor(QPointF(x * dpr, y * dpr).toPoint());
        };
        const QColor paper = at(250, 205);
        // The canvas keeps its centre across resizes, so the stroke drawn before moved by
        // half the size change; the stroke drawn after lies under the pointer.
        QVERIFY2(at(250, 260) != paper, "stroke drawn after resizing is not under the pointer");
        QCOMPARE(at(250, 280), paper);
    }

    // Rendering stays on demand inside the shell: using the navigation tree (focus changes,
    // selecting a notebook, collapsing and expanding) presents no canvas frame; opening a
    // page does.
    void navigationDoesNotRepaintTheCanvas() {
        QTemporaryDir dir;
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        auto created = application::WorkspaceSession::create(root, "Idle", {clock, ids, locker});
        QVERIFY(created.has_value());
        auto& session = **created;
        auto page = application::ensureStartPage(session, clock, ids);
        QVERIFY(page.has_value());
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ui::ThemeManager themes;
        ui::MainWindow window(
            themes, settings,
            ui::WorkspaceContext{.session = session, .ids = ids, .clock = clock, .page = *page});
        window.resize(900, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* canvas = window.findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        auto* tree = window.findChild<QTreeView*>(QStringLiteral("workspaceTree"));
        QVERIFY(canvas != nullptr && tree != nullptr);
        QTest::qWait(50);
        if (!canvas->isValid() || canvas->grabFramebuffer().isNull()) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }
        auto* newPage = window.findChild<QAction*>(QStringLiteral("actionNewPage"));
        newPage->trigger(); // a second page to switch to
        QVERIFY(window.openPage(*page));
        canvas->setFocus();
        QTest::qWait(100); // let pending frames present

        // Counted in canvas paints: frameSwapped also fires when only the window (e.g. the
        // tree) is presented.
        auto* canvasWidget = static_cast<ui::CanvasWidget*>(canvas);
        const std::uint64_t before = canvasWidget->paintCount();
        tree->setFocus(); // the canvas loses focus
        const QModelIndex notebook = tree->model()->index(0, 0);
        tree->setCurrentIndex(notebook); // selecting a notebook opens nothing
        tree->collapse(notebook);
        tree->expand(notebook);
        QTest::mouseMove(tree->viewport(), tree->visualRect(notebook).center());
        canvas->setFocus(); // and gets it back
        QTest::qWait(100);
        QCOMPARE(canvasWidget->paintCount(), before);

        auto* nextPage = window.findChild<QAction*>(QStringLiteral("actionNextPage"));
        nextPage->trigger(); // switching pages draws the new page
        QTest::qWait(100);
        QVERIFY(canvasWidget->paintCount() > before);
    }

    // Phase 6, step 1: ink is rendered in the chosen pen style (colour, width).
    void inkIsDrawnInTheChosenStyle() {
        QTemporaryDir dir;
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        auto created = application::WorkspaceSession::create(root, "Ink", {clock, ids, locker});
        QVERIFY(created.has_value());
        auto& session = **created;
        auto page = application::ensureStartPage(session, clock, ids);
        QVERIFY(page.has_value());
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ui::ThemeManager themes;
        ui::MainWindow window(
            themes, settings,
            ui::WorkspaceContext{.session = session, .ids = ids, .clock = clock, .page = *page});
        window.resize(900, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* canvas = window.findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        QTest::qWait(50);
        if (!canvas->isValid() || canvas->grabFramebuffer().isNull()) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }
        window.findChild<QAction*>(QStringLiteral("actionPenColor_blue"))->trigger();
        window.findChild<QAction*>(QStringLiteral("actionPenWidth_thick"))->trigger();
        drag(canvas, {100, 210}, {400, 210});
        canvas->update();
        QTest::qWait(30);
        const QImage image = canvas->grabFramebuffer();
        const qreal dpr = canvas->devicePixelRatioF();
        const QColor ink = image.pixelColor((QPointF(250, 210) * dpr).toPoint());
        QVERIFY2(ink.blue() > ink.red() + 40 && ink.blue() > ink.green() + 20,
                 qPrintable(ink.name()));
        // Thick: several device pixels tall (1.2 is "fine"; 4 world units at zoom 1).
        const QColor edge = image.pixelColor((QPointF(250, 211.5) * dpr).toPoint());
        QVERIFY2(edge.blue() > edge.red() + 20, qPrintable(edge.name()));
    }

    // Phase 6, step 2: the highlighter is broad, translucent ink. A stroke is even where it
    // overlaps itself (joins, caps, a stroke doubling back): each stroke covers a pixel
    // once. Separate strokes build up where they cross, and ink under them stays visible.
    // Checked drawn element by element and through batches (many visible elements).
    void highlighterIsEvenTranslucentAndBuildsUpAcrossStrokes() {
        QTemporaryDir dir;
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        auto created = application::WorkspaceSession::create(root, "Mark", {clock, ids, locker});
        QVERIFY(created.has_value());
        auto& session = **created;
        auto page = application::ensureStartPage(session, clock, ids);
        QVERIFY(page.has_value());
        { // blank paper, so sampled pixels are either ink or paper
            const document::PageInfo& info = *session.workspace().findPage(*page);
            document::commands::PageFormat format{
                .extent = info.extent, .size = info.size, .background = info.background};
            format.background.pattern = document::BackgroundPattern::None;
            auto command =
                document::commands::setPageFormat(session.workspace(), *page, format, clock);
            QVERIFY(command.has_value());
            QVERIFY(session.execute(std::move(*command)).has_value());
        }
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ui::ThemeManager themes;
        ui::MainWindow window(
            themes, settings,
            ui::WorkspaceContext{.session = session, .ids = ids, .clock = clock, .page = *page});
        window.resize(900, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* canvas = window.findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        QTest::qWait(50);
        if (!canvas->isValid() || canvas->grabFramebuffer().isNull()) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }

        // A thick black pen line, then highlighter strokes (default: yellow, 14 px at zoom 1).
        window.findChild<QAction*>(QStringLiteral("actionPenWidth_thick"))->trigger();
        drag(canvas, {300, 120}, {300, 300});
        window.findChild<QAction*>(QStringLiteral("actionToolHighlighter"))->trigger();
        // One stroke that runs right and doubles back over itself.
        QTest::mousePress(canvas, Qt::LeftButton, {}, {100, 200});
        for (int i = 1; i <= 16; ++i) {
            QTest::mouseMove(canvas, QPoint(100 + 25 * i, 200), 2);
        }
        for (int i = 1; i <= 12; ++i) {
            QTest::mouseMove(canvas, QPoint(500 - 25 * i, 200), 2);
        }
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, {200, 200});
        // A second stroke crossing the first.
        drag(canvas, {420, 140}, {420, 260});
        QCOMPARE(strokesOn(session.workspace(), *page).size(), std::size_t{3});

        const auto check = [&](const QString& mode) {
            canvas->update();
            QTest::qWait(30);
            const QImage image = canvas->grabFramebuffer();
            const qreal dpr = canvas->devicePixelRatioF();
            const auto at = [&](qreal x, qreal y) {
                return image.pixelColor(QPointF(x * dpr, y * dpr).toPoint());
            };
            const auto near = [](const QColor& a, const QColor& b) {
                return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                                 std::abs(a.blue() - b.blue())}) <= 3;
            };
            const QColor paper = at(150, 260);
            const QColor once = at(150, 200); // the forward pass only
            // Translucent yellow on white: light, warm, clearly not opaque yellow (#F2C94C).
            QVERIFY2(once != paper, qPrintable(mode));
            QVERIFY2(once.red() > 200 && once.green() > 180 && once.blue() < once.red() - 40,
                     qPrintable(QStringLiteral("%1: %2").arg(mode, once.name())));
            QVERIFY2(once.blue() > 120,
                     qPrintable(QStringLiteral("%1: %2").arg(mode, once.name())));
            // Even along the stroke, at its joins and where it runs over itself again.
            for (const qreal x : {125.0, 137.0, 175.0, 250.0, 275.0, 350.0, 375.0, 480.0}) {
                const QColor c = at(x, 200);
                QVERIFY2(near(c, once), qPrintable(QStringLiteral("%1: x=%2 %3 vs %4")
                                                       .arg(mode)
                                                       .arg(x)
                                                       .arg(c.name(), once.name())));
            }
            // Broad: 14 px tall, centred on the stroke; not wider than that.
            QVERIFY2(near(at(150, 205), once), qPrintable(mode));
            QVERIFY2(near(at(150, 195), once), qPrintable(mode));
            QVERIFY2(near(at(150, 212), paper), qPrintable(mode));
            // The pen line under the highlighter stays dark and visible.
            const QColor inkUnder = at(300, 200);
            QVERIFY2(inkUnder.value() < 140, qPrintable(inkUnder.name()));
            QVERIFY2(inkUnder.red() > inkUnder.blue() + 20, qPrintable(inkUnder.name())); // tinted
            // Two strokes build up where they cross.
            const QColor twice = at(420, 200);
            QVERIFY2(
                twice.blue() < once.blue() - 20,
                qPrintable(QStringLiteral("%1: %2 vs %3").arg(mode, twice.name(), once.name())));
        };
        check(QStringLiteral("per element"));

        // Enough visible elements to be drawn through batches (strokes share batch meshes).
        const core::LayerId layer = session.workspace().layersOf(*page).front();
        for (int i = 0; i < 1045; ++i) {
            auto dot = document::commands::createElement(
                session.workspace(), layer,
                {.transform = {.position = {20.0 + 4.0 * (i % 19), 330.0 + 4.0 * (i / 19)}},
                 .payload = document::Stroke{.baseWidth = 1.0F,
                                             .points = document::makeStrokePoints({{0, 0, 1}})}},
                ids);
            QVERIFY(dot.has_value());
            QVERIFY(session.execute(std::move(dot->command)).has_value());
        }
        window.findChild<QAction*>(QStringLiteral("actionDebugHud"))->setChecked(true);
        auto* hud = canvas->findChild<QLabel*>(QStringLiteral("canvasHud"));
        QVERIFY(hud != nullptr);
        canvas->update();
        QTRY_VERIFY_WITH_TIMEOUT(hud->text().contains(QStringLiteral("batched")), 5000);
        window.findChild<QAction*>(QStringLiteral("actionDebugHud"))->setChecked(false);
        check(QStringLiteral("batched"));
    }

    // Phase 6, step 5: text boxes are drawn from a raster of their text (a texture), in
    // their place, dark on light paper; nothing is drawn outside the text.
    void textBoxesAreDrawnAsText() {
        QTemporaryDir dir;
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        auto created = application::WorkspaceSession::create(root, "Text", {clock, ids, locker});
        QVERIFY(created.has_value());
        auto& session = **created;
        auto page = application::ensureStartPage(session, clock, ids);
        QVERIFY(page.has_value());
        { // blank paper
            const document::PageInfo& info = *session.workspace().findPage(*page);
            document::commands::PageFormat format{
                .extent = info.extent, .size = info.size, .background = info.background};
            format.background.pattern = document::BackgroundPattern::None;
            auto command =
                document::commands::setPageFormat(session.workspace(), *page, format, clock);
            QVERIFY(command.has_value());
            QVERIFY(session.execute(std::move(*command)).has_value());
        }
        auto box = document::commands::createElement(
            session.workspace(), session.workspace().layersOf(*page).front(),
            {.transform = {.position = {100, 100}},
             .payload = document::TextBox{.size = {300, 60}, .text = "MMMMMMMM WWWWWWWW"}},
            ids);
        QVERIFY(box.has_value());
        QVERIFY(session.execute(std::move(box->command)).has_value());
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ui::ThemeManager themes;
        platform::QtTextLayout layout;
        ui::MainWindow window(
            themes, settings,
            ui::WorkspaceContext{.session = session, .ids = ids, .clock = clock, .page = *page});
        window.setTextLayout(&layout);
        window.resize(900, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* canvas = window.findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        QTest::qWait(50);
        if (!canvas->isValid() || canvas->grabFramebuffer().isNull()) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }
        canvas->update();
        QTest::qWait(30);
        const QImage image = canvas->grabFramebuffer();
        const qreal dpr = canvas->devicePixelRatioF();
        int dark = 0;
        int darkOutside = 0;
        for (int y = 90; y < 180; ++y) {
            for (int x = 90; x < 420; ++x) {
                const QColor c = image.pixelColor((QPointF(x, y) * dpr).toPoint());
                if (c.value() < 100) {
                    const bool inside = x >= 100 && x < 400 && y >= 100 && y < 160;
                    (inside ? dark : darkOutside) += 1;
                }
            }
        }
        QVERIFY2(dark > 200, qPrintable(QString::number(dark))); // glyphs are there
        QCOMPARE(darkOutside, 0);
    }

    // Phase 6, step 6: an image element shows its asset's pixels (a texture decoded from the
    // workspace's asset file) in its box; a missing asset shows the neutral frame.
    void imagesAreDrawnFromTheirAssets() {
        QTemporaryDir dir;
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        auto created = application::WorkspaceSession::create(root, "Images", {clock, ids, locker});
        QVERIFY(created.has_value());
        auto& session = **created;
        auto page = application::ensureStartPage(session, clock, ids);
        QVERIFY(page.has_value());
        QImage blue(40, 20, QImage::Format_RGB32);
        blue.fill(QColor(30, 60, 200));
        const QString png = dir.filePath(QStringLiteral("blue.png"));
        QVERIFY(blue.save(png));
        auto asset = session.importAsset(toPath(png), "image/png");
        QVERIFY(asset.has_value());
        const core::LayerId layer = session.workspace().layersOf(*page).front();
        auto image = document::commands::createElement(
            session.workspace(), layer,
            {.transform = {.position = {100, 100}},
             .payload = document::Image{.asset = *asset, .size = {200, 100}}},
            ids);
        QVERIFY(image.has_value());
        QVERIFY(session.execute(std::move(image->command)).has_value());
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ui::ThemeManager themes;
        ui::MainWindow window(
            themes, settings,
            ui::WorkspaceContext{.session = session, .ids = ids, .clock = clock, .page = *page});
        window.resize(900, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* canvas = window.findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        QTest::qWait(50);
        if (!canvas->isValid() || canvas->grabFramebuffer().isNull()) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }
        // Decoded off the GUI thread: the frame is shown first, the pixels a frame later.
        const qreal dpr = canvas->devicePixelRatioF();
        const auto inside = [&] {
            return canvas->grabFramebuffer().pixelColor((QPointF(200, 150) * dpr).toPoint());
        };
        QTRY_VERIFY2_WITH_TIMEOUT(inside().blue() > 150 && inside().red() < 90,
                                  qPrintable(inside().name()), 3000);
        const QImage frame = canvas->grabFramebuffer();
        const QColor outside = frame.pixelColor((QPointF(200, 220) * dpr).toPoint());
        QVERIFY2(outside.blue() < 150 || outside.red() > 150, qPrintable(outside.name()));
    }

    // One mesh handle updated between plain data and batch data (per-vertex part numbers):
    // the part attribute buffer is created, dropped and created again, and each draw uses
    // the data it was last given. Two overlapping translucent squares are blended twice
    // as two parts, once as one plain mesh.
    void meshesSwitchBetweenPlainAndPartNumberedData() {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        QOpenGLContext context;
        context.setFormat(format);
        if (!surface.isValid() || !context.create() || !context.makeCurrent(&surface) ||
            std::pair(context.format().majorVersion(), context.format().minorVersion()) <
                std::pair(3, 3)) {
            QSKIP("no OpenGL 3.3 context on this platform");
        }
        QOpenGLFramebufferObjectFormat target;
        target.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        constexpr int kSide = 64;
        QOpenGLFramebufferObject fbo(kSide, kSide, target);
        QVERIFY(fbo.isValid() && fbo.bind());
        {
            render_gl::OpenGLRenderer renderer;
            QVERIFY(renderer.initialize().has_value());
            renderer.resize(kSide, kSide);
            const auto squares = [](bool parted) {
                render::MeshData mesh;
                for (std::uint32_t k = 0; k < 2; ++k) {
                    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                    mesh.vertices.insert(mesh.vertices.end(),
                                         {{-16, -16}, {16, -16}, {16, 16}, {-16, 16}});
                    mesh.indices.insert(mesh.indices.end(),
                                        {base, base + 1, base + 2, base, base + 2, base + 3});
                    if (parted) {
                        mesh.parts.insert(mesh.parts.end(), 4, k);
                    }
                }
                mesh.bounds = core::Rect{{-16, -16}, {16, 16}};
                return mesh;
            };
            render::RenderFrame frame;
            frame.viewportSize = {kSide, kSide};
            frame.background.deskColor = core::Color::white();
            frame.background.paperColor = core::Color::white();
            const auto centre = [&](render::MeshHandle mesh) {
                const std::array<render::DrawItem, 1> items{
                    render::DrawItem{.mesh = mesh, .color = core::Color{0, 0, 0, 128}}};
                frame.content = items;
                renderer.render(frame);
                return fbo.toImage().pixelColor(kSide / 2, kSide / 2).value();
            };
            const render::MeshHandle mesh = renderer.createMesh(squares(false));
            QVERIFY(mesh.isValid());
            const int once = centre(mesh);
            QVERIFY2(once > 110 && once < 145, qPrintable(QString::number(once)));
            renderer.updateMesh(mesh, squares(true)); // plain → parts: blended twice
            const int twice = centre(mesh);
            QVERIFY2(twice > 45 && twice < 80, qPrintable(QString::number(twice)));
            renderer.updateMesh(mesh, squares(false)); // parts → plain: once again
            QCOMPARE(centre(mesh), once);
            renderer.updateMesh(mesh, squares(true));
            QCOMPARE(centre(mesh), twice);
            renderer.releaseAll();
        }
        fbo.release();
        context.doneCurrent();
    }

    // The eraser's reach is shown as the platform cursor (no OpenGL needed): it follows the
    // pointer without render latency and the window system removes it when the pointer
    // leaves the canvas. Choosing the tool applies it at once, without a pointer move.
    void eraserRingIsThePlatformCursor() {
        QTemporaryDir dir;
        const auto root = toPath(dir.filePath(QStringLiteral("ws")));
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        auto created = application::WorkspaceSession::create(root, "Cursor", {clock, ids, locker});
        QVERIFY(created.has_value());
        auto& session = **created;
        auto page = application::ensureStartPage(session, clock, ids);
        QVERIFY(page.has_value());
        QSettings settings(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        ui::ThemeManager themes;
        ui::MainWindow window(
            themes, settings,
            ui::WorkspaceContext{.session = session, .ids = ids, .clock = clock, .page = *page});
        auto* canvas = window.findChild<QOpenGLWidget*>(QStringLiteral("canvasWidget"));
        auto* eraser = window.findChild<QAction*>(QStringLiteral("actionToolEraser"));
        auto* pen = window.findChild<QAction*>(QStringLiteral("actionToolPen"));
        QVERIFY(canvas != nullptr && eraser != nullptr && pen != nullptr);

        QCOMPARE(canvas->cursor().shape(), Qt::CrossCursor); // the pen
        eraser->trigger();
        const QCursor ring = canvas->cursor();
        QCOMPARE(ring.shape(), Qt::BitmapCursor);
        const QPixmap pixmap = ring.pixmap();
        QVERIFY(!pixmap.isNull());
        const QSizeF logical = pixmap.deviceIndependentSize();
        QCOMPARE(logical.width(), logical.height());
        // The hot spot (the pointer position) is the ring's centre; the ring (radius 8 px)
        // is drawn around it and the centre is transparent, so the ink stays visible.
        QCOMPARE(ring.hotSpot(), QPoint(static_cast<int>(logical.width()) / 2,
                                        static_cast<int>(logical.height()) / 2));
        const QImage image = pixmap.toImage();
        const qreal dpr = pixmap.devicePixelRatio();
        const QPointF centre(logical.width() / 2.0, logical.height() / 2.0);
        QCOMPARE(image.pixelColor((centre * dpr).toPoint()).alpha(), 0);
        QVERIFY(image.pixelColor(((centre + QPointF(8.0, 0.0)) * dpr).toPoint()).alpha() > 0);

        pen->trigger();
        QCOMPARE(canvas->cursor().shape(), Qt::CrossCursor);
    }
};

QTEST_MAIN(CanvasWidgetTest)
#include "CanvasWidgetTest.moc"
