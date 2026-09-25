#pragma once

#include <studyapp/render/Renderer.hpp>

#include <functional>
#include <memory>
#include <string>

namespace studyapp::render_gl {

/// OpenGL 3.3 core implementation of render::Renderer (docs/RENDERING.md §6).
///
/// The only code in the application that calls OpenGL. This header is deliberately free of
/// Qt and GL types; the implementation uses QOpenGLFunctions_3_3_Core from the context
/// that is current when initialize() is called.
///
/// Resources: one VAO/VBO/IBO per mesh in generational slots (stale handles are ignored),
/// two programs (solid, pattern) compiled from GLSL 330 sources compiled into the binary
/// as Qt resources, and an empty VAO for the background's full-screen triangle.
///
/// Lifetime: every GL object belongs to the context current at initialize(). releaseAll()
/// must run while that context is current; the destructor does not touch GL. If the
/// context is about to be destroyed while resources exist (e.g. the widget moves to
/// another window), the renderer makes it current, releases everything and calls the
/// context-lost handler so owners of handles can forget them.
class OpenGLRenderer final : public render::Renderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer() override;
    OpenGLRenderer(const OpenGLRenderer&) = delete;
    OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;
    OpenGLRenderer(OpenGLRenderer&&) = delete;
    OpenGLRenderer& operator=(OpenGLRenderer&&) = delete;

    [[nodiscard]] core::Result<void> initialize() override;
    [[nodiscard]] bool isInitialized() const noexcept override;
    void resize(int framebufferWidth, int framebufferHeight) override;

    [[nodiscard]] render::MeshHandle createMesh(const render::MeshData& mesh) override;
    void updateMesh(render::MeshHandle handle, const render::MeshData& mesh) override;
    void destroyMesh(render::MeshHandle handle) override;

    void render(const render::RenderFrame& frame) override;
    [[nodiscard]] render::RenderStats lastFrameStats() const noexcept override;
    void releaseAll() noexcept override;

    /// Called after the renderer released its resources because its context is going away.
    void setContextLostHandler(std::function<void()> handler);

    /// "OpenGL <version> (<renderer>)" once initialized, for diagnostics and the HUD.
    [[nodiscard]] const std::string& description() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace studyapp::render_gl
