#include <studyapp/render_gl/OffscreenCheck.hpp>

#include <studyapp/render_gl/OpenGLRenderer.hpp>

#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>

#include <array>
#include <string>
#include <utility>

namespace studyapp::render_gl {

using core::ErrorCode;
using core::makeError;

namespace {

/// A black square in the middle of a white frame, drawn by the canvas renderer.
core::Result<int> drawSquare(OpenGLRenderer& renderer, QOpenGLFramebufferObject& target, int side) {
    render::MeshData mesh;
    const float half = static_cast<float>(side) / 4.0F;
    mesh.vertices = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.bounds = core::Rect{{-half, -half}, {half, half}};
    const render::MeshHandle handle = renderer.createMesh(mesh);
    if (!handle.isValid()) {
        return makeError(ErrorCode::IoError, "cannot create a mesh");
    }
    const std::array<render::DrawItem, 1> items{
        render::DrawItem{.mesh = handle, .color = core::Color{0, 0, 0, 255}}};
    render::RenderFrame frame;
    frame.viewportSize = {static_cast<float>(side), static_cast<float>(side)};
    frame.background.deskColor = core::Color::white();
    frame.background.paperColor = core::Color::white();
    frame.content = items;
    renderer.render(frame);
    const QImage image = target.toImage();
    const int centre = image.pixelColor(side / 2, side / 2).value();
    const int corner = image.pixelColor(1, 1).value();
    if (centre > 64 || corner < 192) {
        return makeError(ErrorCode::IoError, "the test drawing is wrong (centre " +
                                                 std::to_string(centre) + ", corner " +
                                                 std::to_string(corner) + ")");
    }
    return centre;
}

} // namespace

core::Result<std::string> checkOffscreenRendering() {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    if (!surface.isValid()) {
        return makeError(ErrorCode::Unsupported, "no offscreen OpenGL surface");
    }
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        return makeError(ErrorCode::Unsupported, "cannot create an OpenGL context");
    }
    if (!context.makeCurrent(&surface)) {
        return makeError(ErrorCode::Unsupported, "cannot make the OpenGL context current");
    }
    const QSurfaceFormat actual = context.format();
    if (std::pair(actual.majorVersion(), actual.minorVersion()) < std::pair(3, 3)) {
        context.doneCurrent();
        return makeError(ErrorCode::Unsupported, "OpenGL " + std::to_string(actual.majorVersion()) +
                                                     "." + std::to_string(actual.minorVersion()) +
                                                     "; 3.3 core is needed");
    }
    constexpr int kSide = 32;
    core::Result<std::string> result = std::string{};
    {
        QOpenGLFramebufferObject target(kSide, kSide);
        if (!target.isValid() || !target.bind()) {
            result = makeError(ErrorCode::IoError, "cannot create an offscreen framebuffer");
        } else {
            OpenGLRenderer renderer;
            if (auto initialized = renderer.initialize(); !initialized) {
                result = tl::unexpected(initialized.error());
            } else {
                renderer.resize(kSide, kSide);
                if (auto drawn = drawSquare(renderer, target, kSide); !drawn) {
                    result = tl::unexpected(drawn.error());
                } else {
                    result = renderer.description();
                }
                renderer.releaseAll(); // while the context is current
            }
            target.release();
        }
    }
    context.doneCurrent();
    return result;
}

} // namespace studyapp::render_gl
