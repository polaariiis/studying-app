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

    // Render on demand: the controller asks for a frame when something changed; Qt
    // coalesces repeated requests into one paint.
    controller_->setRedrawCallback([this] { update(); });
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
    frameClock_.start();
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
    const double intervalMs = static_cast<double>(frameClock_.nsecsElapsed()) / 1e6;
    frameClock_.restart();
    lastIntervalMs_ = intervalMs;
    recentIntervalsMs_.push_back(intervalMs);
    if (recentIntervalsMs_.size() > kFpsWindow) {
        recentIntervalsMs_.erase(recentIntervalsMs_.begin());
    }

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
    const double average =
        recentIntervalsMs_.empty()
            ? 0.0
            : std::accumulate(recentIntervalsMs_.begin(), recentIntervalsMs_.end(), 0.0) /
                  static_cast<double>(recentIntervalsMs_.size());
    const auto section = [this](const char* name) {
        const core::Profiler::Section* s = controller_->profiler().find(name);
        return s != nullptr ? QString::number(s->lastMs(), 'f', 2) : QStringLiteral("-");
    };
    const QString text =
        QStringLiteral("frame  %1 ms since the last (avg %2 ms = %3 fps; renders on demand)\n"
                       "cpu    %4 ms  build %5  query %6  prepare %7\n"
                       "camera zoom %8  centre (%9, %10)\n"
                       "view   %11x%12 px  dpr %13  framebuffer %14x%15\n"
                       "scene  %16 elements  visible %17  drawn %18  selected %19\n"
                       "gpu    %20 draw calls  %21 triangles  %22 meshes  %23  %31 ms\n"
                       "cache  %24 entries  %25  built %26  uploaded %27\n"
                       "tool   %28  live points %29\n"
                       "%30")
            .arg(QString::number(lastIntervalMs_, 'f', 1), QString::number(average, 'f', 1),
                 QString::number(average > 0.0 ? 1000.0 / average : 0.0, 'f', 0),
                 QString::number(cpuMs, 'f', 2), section("build frame"), section("scene query"),
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

void CanvasWidget::updateCursor() {
    setCursor(toQtCursor(controller_->cursor()));
}

void CanvasWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (const auto pointer = toPointerEvent(*event)) {
        pointerDown_ = true;
        controller_->onPointer(*pointer);
        event->accept();
    }
    updateCursor();
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    mousePressEvent(event); // a double click is a second press for the canvas
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (const auto pointer = toPointerEvent(*event)) {
        controller_->onPointer(*pointer);
        event->accept();
    }
    updateCursor();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (const auto pointer = toPointerEvent(*event)) {
        pointerDown_ = event->buttons() != Qt::NoButton;
        controller_->onPointer(*pointer);
        event->accept();
    }
    updateCursor();
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
        updateCursor();
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
        updateCursor();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* event) {
    if (const auto key = toKeyEvent(*event, false)) {
        controller_->onKey(*key);
        event->accept();
        updateCursor();
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
