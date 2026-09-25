#pragma once

#include <studyapp/ui/PanBenchmark.hpp>

#include <QColor>
#include <QCursor>
#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QLabel;

namespace studyapp::canvas {
class CanvasController;
} // namespace studyapp::canvas

namespace studyapp::render_gl {
class OpenGLRenderer;
} // namespace studyapp::render_gl

namespace studyapp::ui {

/// The canvas surface: the Qt boundary of the canvas engine (docs/CANVAS.md §0).
///
/// Responsibilities, and nothing more:
///   * host the OpenGL 3.3 core context and own the render_gl::OpenGLRenderer;
///   * translate Qt input into canvas events (CanvasInputAdapter) and forward them;
///   * report the logical size and device pixel ratio to the controller and the
///     framebuffer size (logical × DPR) to the renderer on every paint;
///   * render on demand (update() when the controller asks) and show the debug HUD;
///   * map canvas cursor shapes to platform cursors (the eraser ring is a cursor image).
/// Camera math, tools, document edits and GL calls all live elsewhere.
class CanvasWidget final : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit CanvasWidget(canvas::CanvasController& controller, QWidget* parent = nullptr);
    ~CanvasWidget() override;
    CanvasWidget(const CanvasWidget&) = delete;
    CanvasWidget& operator=(const CanvasWidget&) = delete;
    CanvasWidget(CanvasWidget&&) = delete;
    CanvasWidget& operator=(CanvasWidget&&) = delete;

    void setHudVisible(bool visible);
    [[nodiscard]] bool isHudVisible() const noexcept;

    /// Applies the controller's cursor now (after a tool or colour change made elsewhere,
    /// e.g. a keyboard shortcut, without waiting for the next pointer event).
    void refreshCursor();

    /// Pans continuously for `frames` frames (back and forth), then reports frame timings.
    void runPanBenchmark(int frames, std::function<void(const PanBenchmarkResult&)> done);

    /// Empty while the renderer works; otherwise why it could not start.
    [[nodiscard]] const QString& graphicsError() const noexcept { return graphicsError_; }

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;

private:
    void syncViewport();
    void updateHud(double cpuMs);
    void releaseGraphics();
    void onRedrawRequested();
    void onFrameSwapped();
    [[nodiscard]] const QCursor& eraserCursor();

    canvas::CanvasController* controller_;
    std::unique_ptr<render_gl::OpenGLRenderer> renderer_;
    QLabel* hud_ = nullptr;
    QString graphicsError_;
    bool pointerDown_ = false;

    // Frame timing for the HUD (on-demand rendering: gaps between frames are idle time,
    // not frame time, and are kept out of the averages).
    QElapsedTimer clock_;                      ///< monotonic, started at construction
    double lastPaintMs_ = -1.0;                ///< clock_ time of the previous paint
    double lastIntervalMs_ = 0.0;              ///< since the previous paint, idle gaps included
    std::vector<double> activeIntervalsMs_;    ///< consecutive frames of the current activity
    bool requestPending_ = false;              ///< a repaint was requested and not yet painted
    double requestedAtMs_ = 0.0;               ///< when the pending request was made
    std::optional<double> paintedRequestAtMs_; ///< request time of the frame being presented
    std::vector<double> recentLatenciesMs_;    ///< request → frame presented

    QCursor eraserCursor_;
    double eraserCursorDpr_ = 0.0;
    QRgb eraserCursorColor_ = 0;

    struct Benchmark {
        int remaining = 0;
        int total = 0;
        std::vector<double> intervalsMs;
        std::vector<double> cpuMs;
        std::vector<double> gpuMs;
        std::uint64_t drawItems = 0;
        std::function<void(const PanBenchmarkResult&)> done;
    };
    std::unique_ptr<Benchmark> benchmark_;
};

} // namespace studyapp::ui
