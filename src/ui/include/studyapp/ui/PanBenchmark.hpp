#pragma once

#include <cstddef>

namespace studyapp::ui {

/// Result of the automated pan benchmark (MainWindow::runPanBenchmark): the canvas pans
/// continuously for a number of frames while frame timing is recorded. Frame intervals
/// include vsync, so ≈16.7 ms means the 60 fps display rate was reached.
struct PanBenchmarkResult {
    int frames = 0;
    double averageIntervalMs = 0.0; ///< time between presented frames
    double worstIntervalMs = 0.0;
    double p95IntervalMs = 0.0;
    double averageCpuMs = 0.0; ///< buildFrame + render submission on the GUI thread
    double worstCpuMs = 0.0;
    double averageGpuMs = -1.0; ///< GL_TIME_ELAPSED per frame; < 0 if unavailable
    std::size_t sceneElements = 0;
    std::size_t averageDrawItems = 0;
};

} // namespace studyapp::ui
