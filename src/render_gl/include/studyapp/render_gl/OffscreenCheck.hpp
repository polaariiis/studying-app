#pragma once

#include <studyapp/core/Error.hpp>

#include <string>

namespace studyapp::render_gl {

/// Installation check (the application's --self-test, docs/BUILDING.md §8): creates an
/// OpenGL 3.3 core context on an offscreen surface, initialises OpenGLRenderer (its shaders
/// compile), draws one square into an offscreen framebuffer and reads it back. Proves that
/// the platform, the OpenGL driver and the C++ runtime the process loaded can render the
/// canvas, on any vendor's driver (software ones included), without a window.
///
/// Needs a QGuiApplication; runs on its thread. Returns OpenGLRenderer::description().
/// Unsupported when no 3.3 core context is available; IoError when drawing fails.
[[nodiscard]] core::Result<std::string> checkOffscreenRendering();

} // namespace studyapp::render_gl
