// End-to-end canvas test through the real Qt widget, OpenGL renderer and workspace
// session: scripted mouse/wheel input → canvas → command → workspace → SQLite, checked in
// the document, in rendered pixels and after reopening the workspace.
//
// Needs an OpenGL 3.3 context. On platforms without one (e.g. QT_QPA_PLATFORM=offscreen
// on CI) the test skips itself; run the executable directly on a desktop to exercise it.

#include <studyapp/application/StartPage.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/platform/QtWorkspaceLocker.hpp>
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
#include <QOpenGLWidget>
#include <QPixmap>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

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
