#include "CanvasWidget.hpp"

#include "CanvasInputAdapter.hpp"

#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/render_gl/OpenGLRenderer.hpp>

#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSurfaceFormat>
#include <QTabletEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace studyapp::ui {

namespace {

constexpr std::size_t kFpsWindow = 60;
/// Frames further apart than this are separated by idle time (nothing asked for a
/// repaint), not by slow rendering; such gaps are left out of the HUD's frame averages.
constexpr double kIdleGapMs = 100.0;
constexpr int kBenchmarkWarmUpFrames = 10;

Qt::CursorShape toQtCursor(canvas::CursorShape shape) noexcept {
    switch (shape) {
    case canvas::CursorShape::Arrow:
        return Qt::ArrowCursor;
    case canvas::CursorShape::Crosshair:
    case canvas::CursorShape::ZoomIn:
        return Qt::CrossCursor;
    case canvas::CursorShape::OpenHand:
        return Qt::OpenHandCursor;
    case canvas::CursorShape::ClosedHand:
        return Qt::ClosedHandCursor;
    case canvas::CursorShape::SizeAll:
        return Qt::SizeAllCursor;
    }
    return Qt::ArrowCursor;
}

QString megabytes(std::uint64_t bytes) {
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1) +
           QStringLiteral(" MB");
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index =
        static_cast<std::size_t>(std::clamp(fraction * static_cast<double>(values.size() - 1), 0.0,
                                            static_cast<double>(values.size() - 1)));
    return values[index];
}

} // namespace

CanvasWidget::CanvasWidget(canvas::CanvasController& controller, QWidget* parent)
    : QOpenGLWidget(parent), controller_(&controller) {
    setObjectName(QStringLiteral("canvasWidget"));
    // OpenGL 3.3 core with 4× MSAA (docs/RENDERING.md §6.1). Vsync stays on (default).
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    // MSAA sample count; STUDYAPP_MSAA_SAMPLES overrides it (diagnostics, weak GPUs).
    const int samples = qEnvironmentVariableIsSet("STUDYAPP_MSAA_SAMPLES")
                            ? qEnvironmentVariableIntValue("STUDYAPP_MSAA_SAMPLES")
                            : 4;
    format.setSamples(std::max(samples, 0));
    format.setStencilBufferSize(8);
    setFormat(format);

    setMouseTracking(true); // hover moves drive the eraser cursor
    setAttribute(Qt::WA_TabletTracking);
    setFocusPolicy(Qt::StrongFocus);

    hud_ = new QLabel(this);
    hud_->setObjectName(QStringLiteral("canvasHud"));
    hud_->setAttribute(Qt::WA_TransparentForMouseEvents);
    hud_->move(8, 8);
    hud_->hide();

    clock_.start();
    // Render on demand: the controller asks for a frame when something changed; Qt
    // coalesces repeated requests into one paint, which uses the latest state. While input
    // keeps arriving this runs at the display rate (the swap waits for vsync); when it
    // stops, nothing is drawn.
    controller_->setRedrawCallback([this] { onRedrawRequested(); });
    connect(this, &QOpenGLWidget::frameSwapped, this, &CanvasWidget::onFrameSwapped);
    refreshCursor();
}

void CanvasWidget::onRedrawRequested() {
    if (!requestPending_) {
        requestPending_ = true;
        requestedAtMs_ = static_cast<double>(clock_.nsecsElapsed()) / 1e6;
    }
    update();
}

void CanvasWidget::onFrameSwapped() {
    if (paintedRequestAtMs_) {
        const double now = static_cast<double>(clock_.nsecsElapsed()) / 1e6;
        recentLatenciesMs_.push_back(now - *paintedRequestAtMs_);
        if (recentLatenciesMs_.size() > kFpsWindow) {
            recentLatenciesMs_.erase(recentLatenciesMs_.begin());
        }
        paintedRequestAtMs_.reset();
    }
}

CanvasWidget::~CanvasWidget() {
    makeCurrent();
    releaseGraphics();
    doneCurrent();
    controller_->setRedrawCallback({});
}

void CanvasWidget::releaseGraphics() {
    if (renderer_ && renderer_->isInitialized()) {
        renderer_->releaseAll();
    }
    controller_->onGraphicsReset();
}

void CanvasWidget::setHudVisible(bool visible) {
    hud_->setVisible(visible);
    update();
}

bool CanvasWidget::isHudVisible() const noexcept {
    return hud_->isVisible();
}

// ---------------------------------------------------------------------------- GL lifecycle

void CanvasWidget::initializeGL() {
    if (!renderer_) {
        renderer_ = std::make_unique<render_gl::OpenGLRenderer>();
        // The context can be recreated (e.g. re-parenting): the renderer releases its
        // resources in time and the canvas forgets its handles, then initializeGL runs again.
        renderer_->setContextLostHandler([this] { controller_->onGraphicsReset(); });
    }
    if (auto started = renderer_->initialize(); !started) {
        graphicsError_ = QString::fromStdString(started.error().message);
        core::logError("canvas", "cannot start the renderer: " + started.error().message);
        hud_->setText(tr("The canvas cannot be displayed: %1").arg(graphicsError_));
        hud_->adjustSize();
        hud_->show();
        return;
    }
    graphicsError_.clear();
}

void CanvasWidget::resizeGL(int /*width*/, int /*height*/) {
    syncViewport(); // Qt passes logical sizes; the framebuffer size is derived in paintGL
}

void CanvasWidget::syncViewport() {
    const double dpr = devicePixelRatioF();
    const core::DVec2 logical{static_cast<double>(width()), static_cast<double>(height())};
    const canvas::Camera& camera = controller_->camera();
    if (camera.viewportSize() != logical || camera.devicePixelRatio() != dpr) {
        controller_->setViewport(logical, dpr);
    }
}

void CanvasWidget::paintGL() {
    if (!renderer_ || !renderer_->isInitialized()) {
        return;
    }
    const double nowMs = static_cast<double>(clock_.nsecsElapsed()) / 1e6;
    const double intervalMs = lastPaintMs_ >= 0.0 ? nowMs - lastPaintMs_ : 0.0;
    lastPaintMs_ = nowMs;
    lastIntervalMs_ = intervalMs;
    if (intervalMs > kIdleGapMs) {
        activeIntervalsMs_.clear(); // the canvas was idle: a new burst of activity starts
    } else if (intervalMs > 0.0) {
        activeIntervalsMs_.push_back(intervalMs);
        if (activeIntervalsMs_.size() > kFpsWindow) {
            activeIntervalsMs_.erase(activeIntervalsMs_.begin());
        }
    }
    // Paints Qt makes on its own (expose, resize) count from the paint itself.
    paintedRequestAtMs_ = requestPending_ ? requestedAtMs_ : nowMs;
    requestPending_ = false;

    if (benchmark_) {
        // Oscillate by ±30 px so everything that was in view stays in view: the
        // benchmark measures redrawing the same content every frame.
        const int frame = benchmark_->total - benchmark_->remaining;
        const double direction = (frame / 10) % 2 == 0 ? 1.0 : -1.0;
        controller_->panBy({6.0 * direction, 2.0 * direction});
    }

    QElapsedTimer cpu;
    cpu.start();
    syncViewport();
    // Framebuffer = logical size × device pixel ratio (what QOpenGLWidget allocates).
    const double dpr = devicePixelRatioF();
    renderer_->resize(static_cast<int>(std::lround(width() * dpr)),
                      static_cast<int>(std::lround(height() * dpr)));
    const render::RenderFrame frame = controller_->buildFrame(*renderer_);
    renderer_->render(frame);
    const double cpuMs = static_cast<double>(cpu.nsecsElapsed()) / 1e6;

    if (hud_->isVisible()) {
        updateHud(cpuMs);
    }

    if (benchmark_) {
        // The first frames upload meshes (warm-up); they are not part of the measurement.
        if (benchmark_->total - benchmark_->remaining >= kBenchmarkWarmUpFrames) {
            benchmark_->intervalsMs.push_back(intervalMs);
            benchmark_->cpuMs.push_back(cpuMs);
            benchmark_->drawItems += frame.content.size();
            if (const double gpu = renderer_->lastFrameStats().gpuMs; gpu >= 0.0) {
                benchmark_->gpuMs.push_back(gpu);
            }
        }
        if (--benchmark_->remaining > 0) {
            QTimer::singleShot(0, this, [this] { update(); });
        } else {
            const Benchmark& b = *benchmark_;
            PanBenchmarkResult result;
            result.frames = b.total;
            if (!b.intervalsMs.empty()) {
                result.averageIntervalMs =
                    std::accumulate(b.intervalsMs.begin(), b.intervalsMs.end(), 0.0) /
                    static_cast<double>(b.intervalsMs.size());
                result.worstIntervalMs =
                    *std::max_element(b.intervalsMs.begin(), b.intervalsMs.end());
                result.p95IntervalMs = percentile(b.intervalsMs, 0.95);
            }
            if (!b.cpuMs.empty()) {
                result.averageCpuMs = std::accumulate(b.cpuMs.begin(), b.cpuMs.end(), 0.0) /
                                      static_cast<double>(b.cpuMs.size());
                result.worstCpuMs = *std::max_element(b.cpuMs.begin(), b.cpuMs.end());
                result.averageDrawItems = static_cast<std::size_t>(
                    b.drawItems / static_cast<std::uint64_t>(b.cpuMs.size()));
            }
            if (!b.gpuMs.empty()) {
                result.averageGpuMs = std::accumulate(b.gpuMs.begin(), b.gpuMs.end(), 0.0) /
                                      static_cast<double>(b.gpuMs.size());
            }
            result.frames = static_cast<int>(b.intervalsMs.size());
            result.sceneElements = controller_->stats().sceneElements;
            auto done = std::move(benchmark_->done);
            benchmark_.reset();
            QTimer::singleShot(0, this, [done = std::move(done), result] {
                if (done) {
                    done(result);
                }
            });
        }
    }
}

void CanvasWidget::runPanBenchmark(int frames,
                                   std::function<void(const PanBenchmarkResult&)> done) {
    benchmark_ = std::make_unique<Benchmark>();
    benchmark_->remaining = std::max(frames, 2) + kBenchmarkWarmUpFrames;
    benchmark_->total = benchmark_->remaining;
    benchmark_->done = std::move(done);
    update();
}

void CanvasWidget::updateHud(double cpuMs) {
    const canvas::CanvasStats stats = controller_->stats();
    const render::RenderStats gpu = renderer_->lastFrameStats();
    const canvas::Camera& camera = controller_->camera();
    const auto mean = [](const std::vector<double>& values) {
        return values.empty() ? 0.0
                              : std::accumulate(values.begin(), values.end(), 0.0) /
                                    static_cast<double>(values.size());
    };
    // Frame rate only over consecutive frames: with rendering on demand, the time between
    // two frames separated by idleness says nothing about how fast frames are made.
    QString frameLine;
    if (activeIntervalsMs_.empty()) {
        frameLine = QStringLiteral("frame  first after %1 ms idle (renders on demand)")
                        .arg(QString::number(lastIntervalMs_, 'f', 0));
    } else {
        const double average = mean(activeIntervalsMs_);
        frameLine =
            QStringLiteral("frame  %1 fps while active (avg %2 ms, last %3 ms, %4 frames; "
                           "gaps > %5 ms are idle, renders on demand)")
                .arg(QString::number(1000.0 / average, 'f', 0), QString::number(average, 'f', 1),
                     QString::number(lastIntervalMs_, 'f', 1))
                .arg(static_cast<qulonglong>(activeIntervalsMs_.size()))
                .arg(QString::number(kIdleGapMs, 'f', 0));
    }
    const QString latencyLine =
        QStringLiteral("input  repaint request to presented: avg %1 ms, worst %2 ms (%3 frames)")
            .arg(QString::number(mean(recentLatenciesMs_), 'f', 1),
                 QString::number(
                     recentLatenciesMs_.empty()
                         ? 0.0
                         : *std::max_element(recentLatenciesMs_.begin(), recentLatenciesMs_.end()),
                     'f', 1))
            .arg(static_cast<qulonglong>(recentLatenciesMs_.size()));
    const auto section = [this](const char* name) {
        const core::Profiler::Section* s = controller_->profiler().find(name);
        return s != nullptr ? QString::number(s->lastMs(), 'f', 2) : QStringLiteral("-");
    };
    const QString text =
        frameLine + QLatin1Char('\n') + latencyLine + QLatin1Char('\n') +
        QStringLiteral("cpu    %4 ms  build %5  query %6  prepare %7\n"
                       "camera zoom %8  centre (%9, %10)\n"
                       "view   %11x%12 px  dpr %13  framebuffer %14x%15\n"
                       "scene  %16 elements  visible %17  drawn %18  selected %19\n"
                       "gpu    %20 draw calls  %21 triangles  %22 meshes  %23  %31 ms\n"
                       "cache  %24 entries  %25  built %26  uploaded %27\n"
                       "tool   %28  live points %29\n"
                       "%30")
            .arg(QString::number(cpuMs, 'f', 2), section("build frame"), section("scene query"),
                 section("prepare content"))
            .arg(QString::number(camera.zoom(), 'f', 3), QString::number(camera.center().x, 'f', 1),
                 QString::number(camera.center().y, 'f', 1))
            .arg(width())
            .arg(height())
            .arg(QString::number(camera.devicePixelRatio(), 'f', 2))
            .arg(static_cast<int>(std::lround(camera.deviceSize().x)))
            .arg(static_cast<int>(std::lround(camera.deviceSize().y)))
            .arg(static_cast<qulonglong>(stats.sceneElements))
            .arg(static_cast<qulonglong>(stats.visibleElements))
            .arg(static_cast<qulonglong>(stats.drawItems))
            .arg(static_cast<qulonglong>(stats.selected))
            .arg(static_cast<qulonglong>(gpu.drawCalls))
            .arg(static_cast<qulonglong>(gpu.triangles))
            .arg(static_cast<qulonglong>(gpu.liveMeshes))
            .arg(megabytes(gpu.meshBytes))
            .arg(static_cast<qulonglong>(stats.cache.entries))
            .arg(megabytes(stats.cache.cpuBytes))
            .arg(static_cast<qulonglong>(stats.cache.builtLastFrame))
            .arg(static_cast<qulonglong>(stats.cache.uploadedLastFrame))
            .arg(QString::fromUtf8(canvas::toString(stats.tool).data(),
                                   static_cast<qsizetype>(canvas::toString(stats.tool).size())))
            .arg(static_cast<qulonglong>(stats.livePoints))
            .arg(QString::fromStdString(renderer_->description()))
            .arg(gpu.gpuMs >= 0.0 ? QString::number(gpu.gpuMs, 'f', 2) : QStringLiteral("-"));
    hud_->setText(text);
    hud_->adjustSize();
}

// ---------------------------------------------------------------------------- input

void CanvasWidget::refreshCursor() {
    const canvas::CursorShape shape = controller_->cursor();
    if (shape == canvas::CursorShape::EraserRing) {
        setCursor(eraserCursor());
    } else {
        setCursor(toQtCursor(shape));
    }
}

const QCursor& CanvasWidget::eraserCursor() {
    const double dpr = devicePixelRatioF();
    const core::Color color = controller_->colors().eraser;
    const QRgb rgb = qRgba(color.r, color.g, color.b, color.a);
    if (eraserCursorDpr_ == dpr && eraserCursorColor_ == rgb) {
        return eraserCursor_;
    }
    // The ring as a cursor image, drawn at the screen's pixel density. A thin contrasting
    // halo keeps it visible on ink and on both paper themes.
    const double radius = canvas::kEraserRadiusViewPx;
    const int size = 2 * static_cast<int>(std::ceil(radius + 2.0)) + 1; // odd: exact centre
    QPixmap pixmap(
        QSize(static_cast<int>(std::ceil(size * dpr)), static_cast<int>(std::ceil(size * dpr))));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    {
        const QColor ring(rgb);
        const QColor halo =
            ring.lightnessF() < 0.5 ? QColor(255, 255, 255, 170) : QColor(0, 0, 0, 170);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(Qt::NoBrush);
        const QPointF centre(size / 2.0, size / 2.0);
        painter.setPen(QPen(halo, 3.0));
        painter.drawEllipse(centre, radius, radius);
        painter.setPen(QPen(ring, 1.25));
        painter.drawEllipse(centre, radius, radius);
    }
    // Hot spot in device-independent pixels: the ring's centre is the eraser's centre.
    eraserCursor_ = QCursor(pixmap, size / 2, size / 2);
    eraserCursorDpr_ = dpr;
    eraserCursorColor_ = rgb;
    return eraserCursor_;
}

void CanvasWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (const auto pointer = toPointerEvent(*event)) {
        pointerDown_ = true;
        controller_->onPointer(*pointer);
        event->accept();
    }
    refreshCursor();
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    mousePressEvent(event); // a double click is a second press for the canvas
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (const auto pointer = toPointerEvent(*event)) {
        controller_->onPointer(*pointer);
        event->accept();
    }
    refreshCursor();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (const auto pointer = toPointerEvent(*event)) {
        pointerDown_ = event->buttons() != Qt::NoButton;
        controller_->onPointer(*pointer);
        event->accept();
    }
    refreshCursor();
}

void CanvasWidget::tabletEvent(QTabletEvent* event) {
    if (const auto pointer = toPointerEvent(*event)) {
        if (pointer->phase == canvas::PointerPhase::Down) {
            setFocus(Qt::MouseFocusReason);
            pointerDown_ = true;
        } else if (pointer->phase == canvas::PointerPhase::Up) {
            pointerDown_ = false;
        }
        controller_->onPointer(*pointer);
        event->accept(); // no synthesised mouse events for handled pen input
        refreshCursor();
        return;
    }
    event->ignore();
}

void CanvasWidget::wheelEvent(QWheelEvent* event) {
    controller_->onWheel(toWheelEvent(*event));
    event->accept();
}

void CanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (const auto key = toKeyEvent(*event, true)) {
        controller_->onKey(*key);
        event->accept();
        refreshCursor();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* event) {
    if (const auto key = toKeyEvent(*event, false)) {
        controller_->onKey(*key);
        event->accept();
        refreshCursor();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

void CanvasWidget::focusOutEvent(QFocusEvent* event) {
    // A gesture interrupted by losing focus (e.g. a dialog) must not commit half-done.
    if (pointerDown_) {
        pointerDown_ = false;
        controller_->onPointer({.phase = canvas::PointerPhase::Cancel});
    }
    controller_->onKey({.key = canvas::Key::Space, .pressed = false});
    QOpenGLWidget::focusOutEvent(event);
}

bool CanvasWidget::event(QEvent* event) {
    if (event->type() == QEvent::NativeGesture) {
        if (const auto zoom = toZoomGesture(*static_cast<QNativeGestureEvent*>(event))) {
            controller_->onZoomGesture(*zoom);
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

} // namespace studyapp::ui
