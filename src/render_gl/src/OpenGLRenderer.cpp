#include <studyapp/render_gl/OpenGLRenderer.hpp>

#include <studyapp/core/Log.hpp>

#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QString>
#include <QSurface>
#include <QSurfaceFormat>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace studyapp::render_gl {

using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

static_assert(sizeof(core::Vec2) == 2 * sizeof(float), "vertex positions must be tightly packed");
static_assert(sizeof(core::Color) == 4, "vertex colours must be tightly packed RGBA8");

struct MeshSlot {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    GLuint cbo = 0; ///< per-vertex colours (0 if the mesh has none)
    GLsizei indexCount = 0;
    std::uint32_t generation = 1;
    bool alive = false;
    bool hasColors = false;
    std::uint64_t bytes = 0;
};

std::string toStd(const QByteArray& bytes) {
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

Result<QByteArray> loadShaderSource(const char* resource) {
    QFile file(QString::fromLatin1(resource));
    if (!file.open(QIODevice::ReadOnly)) {
        return makeError(ErrorCode::NotFound,
                         std::string("shader resource ") + resource + " is missing");
    }
    return file.readAll();
}

float channel(std::uint8_t value) noexcept {
    return static_cast<float>(value) / 255.0F;
}

} // namespace

struct OpenGLRenderer::Impl {
    QOpenGLContext* context = nullptr;
    QSurface* surface = nullptr;
    QOpenGLFunctions_3_3_Core* gl = nullptr;
    QMetaObject::Connection contextConnection;

    GLuint solidProgram = 0;
    GLuint patternProgram = 0;
    GLuint emptyVao = 0;
    struct {
        GLint model = -1, projection = -1, color = -1, invert = -1, vertexColors = -1;
    } solid;
    struct {
        GLint viewport = -1, dpr = -1, framebufferHeight = -1, zoom = -1, desk = -1, paper = -1,
              patternColor = -1, pattern = -1, spacing = -1, phase = -1, bounded = -1,
              pageRect = -1, invert = -1;
    } pattern;

    // GL_TIME_ELAPSED queries (core in 3.3), used round-robin so reading a result never
    // stalls on a frame that is still in flight.
    static constexpr std::size_t kTimerQueries = 3;
    std::array<GLuint, kTimerQueries> timerQueries{};
    std::array<bool, kTimerQueries> timerPending{};
    std::size_t timerIndex = 0;
    double gpuMs = -1.0;

    std::vector<MeshSlot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::uint32_t liveMeshes = 0;
    std::uint64_t meshBytes = 0;
    int framebufferWidth = 1;
    int framebufferHeight = 1;
    render::RenderStats stats;
    std::function<void()> contextLost;
    std::string description;
    bool initialized = false;
    bool reportedGlError = false;

    Result<GLuint> compile(GLenum type, const char* resource) {
        auto source = loadShaderSource(resource);
        if (!source) {
            return tl::unexpected(source.error());
        }
        const GLuint shader = gl->glCreateShader(type);
        const char* text = source->constData();
        const auto length = static_cast<GLint>(source->size());
        gl->glShaderSource(shader, 1, &text, &length);
        gl->glCompileShader(shader);
        GLint ok = GL_FALSE;
        gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            GLint logLength = 0;
            gl->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
            QByteArray log(std::max(logLength, 1), '\0');
            gl->glGetShaderInfoLog(shader, logLength, nullptr, log.data());
            gl->glDeleteShader(shader);
            return makeError(ErrorCode::Internal,
                             std::string("compiling ") + resource + " failed: " + toStd(log));
        }
        return shader;
    }

    Result<GLuint> link(const char* vertex, const char* fragment) {
        auto vs = compile(GL_VERTEX_SHADER, vertex);
        if (!vs) {
            return tl::unexpected(vs.error());
        }
        auto fs = compile(GL_FRAGMENT_SHADER, fragment);
        if (!fs) {
            gl->glDeleteShader(*vs);
            return tl::unexpected(fs.error());
        }
        const GLuint program = gl->glCreateProgram();
        gl->glAttachShader(program, *vs);
        gl->glAttachShader(program, *fs);
        gl->glBindAttribLocation(program, 0, "aPosition");
        gl->glBindAttribLocation(program, 1, "aColor");
        gl->glLinkProgram(program);
        gl->glDetachShader(program, *vs);
        gl->glDetachShader(program, *fs);
        gl->glDeleteShader(*vs);
        gl->glDeleteShader(*fs);
        GLint ok = GL_FALSE;
        gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            GLint logLength = 0;
            gl->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
            QByteArray log(std::max(logLength, 1), '\0');
            gl->glGetProgramInfoLog(program, logLength, nullptr, log.data());
            gl->glDeleteProgram(program);
            return makeError(ErrorCode::Internal, std::string("linking ") + vertex + " + " +
                                                      fragment + " failed: " + toStd(log));
        }
        return program;
    }

    MeshSlot* slotFor(render::MeshHandle handle) noexcept {
        if (!handle.isValid() || handle.index >= slots.size()) {
            return nullptr;
        }
        MeshSlot& slot = slots[handle.index];
        return slot.alive && slot.generation == handle.generation ? &slot : nullptr;
    }

    void upload(MeshSlot& slot, const render::MeshData& mesh, GLenum usage) {
        meshBytes -= slot.bytes;
        gl->glBindVertexArray(slot.vao);
        gl->glBindBuffer(GL_ARRAY_BUFFER, slot.vbo);
        gl->glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(core::Vec2)),
                         mesh.vertices.data(), usage);
        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, slot.ibo);
        gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)),
                         mesh.indices.data(), usage);
        const bool colored = !mesh.colors.empty() && mesh.colors.size() == mesh.vertices.size();
        if (colored) {
            if (slot.cbo == 0) {
                gl->glGenBuffers(1, &slot.cbo);
            }
            gl->glBindBuffer(GL_ARRAY_BUFFER, slot.cbo);
            gl->glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(mesh.colors.size() * sizeof(core::Color)),
                             mesh.colors.data(), usage);
            gl->glEnableVertexAttribArray(1);
            gl->glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(core::Color),
                                      nullptr);
        } else {
            gl->glDisableVertexAttribArray(1);
        }
        slot.hasColors = colored;
        gl->glBindVertexArray(0);
        slot.indexCount = static_cast<GLsizei>(mesh.indices.size());
        slot.bytes = mesh.byteSize();
        meshBytes += slot.bytes;
    }

    void draw(const render::DrawItem& item) {
        MeshSlot* slot = slotFor(item.mesh);
        if (slot == nullptr || slot->indexCount == 0) {
            return;
        }
        const core::Affine2f& t = item.transform;
        // Column-major 3×3: columns (a, b, 0), (c, d, 0), (tx, ty, 1).
        const GLfloat model[9] = {t.a, t.b, 0.0F, t.c, t.d, 0.0F, t.tx, t.ty, 1.0F};
        gl->glUniformMatrix3fv(solid.model, 1, GL_FALSE, model);
        gl->glUniform1i(solid.vertexColors, slot->hasColors ? 1 : 0);
        gl->glUniform4f(solid.color, channel(item.color.r), channel(item.color.g),
                        channel(item.color.b), channel(item.color.a) * item.opacity);
        gl->glBindVertexArray(slot->vao);
        gl->glDrawElements(GL_TRIANGLES, slot->indexCount, GL_UNSIGNED_INT, nullptr);
        ++stats.drawCalls;
        stats.triangles += static_cast<std::uint64_t>(slot->indexCount / 3);
    }

    void setColor(GLint location, const core::Color& color) {
        gl->glUniform4f(location, channel(color.r), channel(color.g), channel(color.b),
                        channel(color.a));
    }
};

OpenGLRenderer::OpenGLRenderer() : impl_(std::make_unique<Impl>()) {}

OpenGLRenderer::~OpenGLRenderer() {
    // GL objects must be released by releaseAll() while the context is current; here the
    // context may already be gone, so only the signal connection is dropped.
    QObject::disconnect(impl_->contextConnection);
}

bool OpenGLRenderer::isInitialized() const noexcept {
    return impl_->initialized;
}

const std::string& OpenGLRenderer::description() const noexcept {
    return impl_->description;
}

void OpenGLRenderer::setContextLostHandler(std::function<void()> handler) {
    impl_->contextLost = std::move(handler);
}

Result<void> OpenGLRenderer::initialize() {
    Impl& d = *impl_;
    if (d.initialized) {
        return {};
    }
    d.context = QOpenGLContext::currentContext();
    if (d.context == nullptr) {
        return makeError(ErrorCode::Internal, "no current OpenGL context");
    }
    const QSurfaceFormat format = d.context->format();
    if (d.context->isOpenGLES() ||
        std::pair(format.majorVersion(), format.minorVersion()) < std::pair(3, 3)) {
        return makeError(ErrorCode::Unsupported,
                         "OpenGL 3.3 core is required, but the context provides " +
                             std::string(d.context->isOpenGLES() ? "OpenGL ES " : "OpenGL ") +
                             std::to_string(format.majorVersion()) + "." +
                             std::to_string(format.minorVersion()));
    }
    d.gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(d.context);
    if (d.gl == nullptr || !d.gl->initializeOpenGLFunctions()) {
        return makeError(ErrorCode::Unsupported, "OpenGL 3.3 core functions are unavailable");
    }
    d.surface = d.context->surface();

    auto solid = d.link(":/shaders/solid.vert", ":/shaders/solid.frag");
    if (!solid) {
        return tl::unexpected(solid.error());
    }
    auto pattern = d.link(":/shaders/pattern.vert", ":/shaders/pattern.frag");
    if (!pattern) {
        d.gl->glDeleteProgram(*solid);
        return tl::unexpected(pattern.error());
    }
    d.solidProgram = *solid;
    d.patternProgram = *pattern;
    const auto uniform = [&](GLuint program, const char* name) {
        return d.gl->glGetUniformLocation(program, name);
    };
    d.solid = {uniform(d.solidProgram, "uModel"), uniform(d.solidProgram, "uProjection"),
               uniform(d.solidProgram, "uColor"), uniform(d.solidProgram, "uInvertLightness"),
               uniform(d.solidProgram, "uVertexColors")};
    d.pattern = {uniform(d.patternProgram, "uViewport"),
                 uniform(d.patternProgram, "uDevicePixelRatio"),
                 uniform(d.patternProgram, "uFramebufferHeight"),
                 uniform(d.patternProgram, "uZoom"),
                 uniform(d.patternProgram, "uDeskColor"),
                 uniform(d.patternProgram, "uPaperColor"),
                 uniform(d.patternProgram, "uPatternColor"),
                 uniform(d.patternProgram, "uPattern"),
                 uniform(d.patternProgram, "uSpacing"),
                 uniform(d.patternProgram, "uPhase"),
                 uniform(d.patternProgram, "uBounded"),
                 uniform(d.patternProgram, "uPageRect"),
                 uniform(d.patternProgram, "uInvertLightness")};
    d.gl->glGenVertexArrays(1, &d.emptyVao);
    d.gl->glGenQueries(static_cast<GLsizei>(d.timerQueries.size()), d.timerQueries.data());
    d.timerPending.fill(false);
    d.gpuMs = -1.0;

    const auto text = [&](GLenum name) {
        const auto* value = reinterpret_cast<const char*>(d.gl->glGetString(name));
        return value != nullptr ? std::string(value) : std::string("?");
    };
    d.description = "OpenGL " + text(GL_VERSION) + " (" + text(GL_RENDERER) + ")";

    // Context going away while we hold resources (e.g. re-parenting): release in time.
    d.contextConnection = QObject::connect(d.context, &QOpenGLContext::aboutToBeDestroyed, [this] {
        Impl& state = *impl_;
        if (!state.initialized) {
            return;
        }
        state.context->makeCurrent(state.surface);
        releaseAll();
        if (state.contextLost) {
            state.contextLost();
        }
    });
    d.initialized = true;
    core::logInfo("render", d.description);
    return {};
}

void OpenGLRenderer::resize(int framebufferWidth, int framebufferHeight) {
    impl_->framebufferWidth = std::max(1, framebufferWidth);
    impl_->framebufferHeight = std::max(1, framebufferHeight);
}

render::MeshHandle OpenGLRenderer::createMesh(const render::MeshData& mesh) {
    Impl& d = *impl_;
    if (!d.initialized) {
        return {};
    }
    std::uint32_t index = 0;
    if (!d.freeSlots.empty()) {
        index = d.freeSlots.back();
        d.freeSlots.pop_back();
    } else {
        index = static_cast<std::uint32_t>(d.slots.size());
        d.slots.emplace_back();
    }
    MeshSlot& slot = d.slots[index];
    d.gl->glGenVertexArrays(1, &slot.vao);
    d.gl->glGenBuffers(1, &slot.vbo);
    d.gl->glGenBuffers(1, &slot.ibo);
    d.gl->glBindVertexArray(slot.vao);
    d.gl->glBindBuffer(GL_ARRAY_BUFFER, slot.vbo);
    d.gl->glEnableVertexAttribArray(0);
    d.gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(core::Vec2), nullptr);
    d.gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, slot.ibo); // recorded in the VAO
    d.gl->glBindVertexArray(0);
    slot.alive = true;
    slot.bytes = 0;
    d.upload(slot, mesh, GL_STATIC_DRAW);
    ++d.liveMeshes;
    return {.index = index, .generation = slot.generation};
}

void OpenGLRenderer::updateMesh(render::MeshHandle handle, const render::MeshData& mesh) {
    Impl& d = *impl_;
    if (!d.initialized) {
        return;
    }
    if (MeshSlot* slot = d.slotFor(handle)) {
        d.upload(*slot, mesh, GL_DYNAMIC_DRAW);
    }
}

void OpenGLRenderer::destroyMesh(render::MeshHandle handle) {
    Impl& d = *impl_;
    if (!d.initialized) {
        return;
    }
    MeshSlot* slot = d.slotFor(handle);
    if (slot == nullptr) {
        return;
    }
    d.gl->glDeleteVertexArrays(1, &slot->vao);
    d.gl->glDeleteBuffers(1, &slot->vbo);
    d.gl->glDeleteBuffers(1, &slot->ibo);
    if (slot->cbo != 0) {
        d.gl->glDeleteBuffers(1, &slot->cbo);
    }
    d.meshBytes -= slot->bytes;
    --d.liveMeshes;
    *slot = MeshSlot{.generation = slot->generation + 1}; // old handles become stale
    d.freeSlots.push_back(handle.index);
}

void OpenGLRenderer::render(const render::RenderFrame& frame) {
    Impl& d = *impl_;
    if (!d.initialized) {
        return;
    }
    d.stats = {};
    QOpenGLFunctions_3_3_Core& gl = *d.gl;
    // Collect the oldest finished timer query, then time this frame with it.
    const std::size_t query = d.timerIndex;
    d.timerIndex = (d.timerIndex + 1) % d.timerQueries.size();
    if (d.timerPending[query]) {
        GLint available = 0;
        gl.glGetQueryObjectiv(d.timerQueries[query], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available != 0) {
            GLuint64 elapsedNs = 0;
            gl.glGetQueryObjectui64v(d.timerQueries[query], GL_QUERY_RESULT, &elapsedNs);
            d.gpuMs = static_cast<double>(elapsedNs) / 1e6;
        }
    }
    gl.glBeginQuery(GL_TIME_ELAPSED, d.timerQueries[query]);
    const float width = std::max(frame.viewportSize.x, 1.0F);
    const float height = std::max(frame.viewportSize.y, 1.0F);
    const int invert =
        frame.contentColorTransform == render::ColorTransform::InvertLightness ? 1 : 0;

    gl.glViewport(0, 0, d.framebufferWidth, d.framebufferHeight);
    gl.glDisable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE);
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glDisable(GL_STENCIL_TEST);

    // 1. Background: paper, page edges and pattern (opaque, covers every pixel).
    const render::Background& bg = frame.background;
    gl.glDisable(GL_BLEND);
    gl.glUseProgram(d.patternProgram);
    gl.glUniform2f(d.pattern.viewport, width, height);
    gl.glUniform1f(d.pattern.dpr, frame.devicePixelRatio);
    gl.glUniform1f(d.pattern.framebufferHeight, static_cast<float>(d.framebufferHeight));
    gl.glUniform1f(d.pattern.zoom, frame.zoom);
    d.setColor(d.pattern.desk, bg.deskColor);
    d.setColor(d.pattern.paper, bg.paperColor);
    d.setColor(d.pattern.patternColor, bg.patternColor);
    gl.glUniform1i(d.pattern.pattern, static_cast<GLint>(bg.pattern));
    gl.glUniform1f(d.pattern.spacing, bg.spacing);
    gl.glUniform2f(d.pattern.phase, bg.patternPhase.x, bg.patternPhase.y);
    gl.glUniform1i(d.pattern.bounded, bg.bounded ? 1 : 0);
    gl.glUniform4f(d.pattern.pageRect, bg.pageRect.min.x, bg.pageRect.min.y, bg.pageRect.max.x,
                   bg.pageRect.max.y);
    gl.glUniform1i(d.pattern.invert, invert);
    gl.glBindVertexArray(d.emptyVao);
    gl.glDrawArrays(GL_TRIANGLES, 0, 3);
    ++d.stats.drawCalls;

    // 2. Content: camera-relative world → clip (y flipped: view y points down).
    gl.glEnable(GL_BLEND);
    gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.glUseProgram(d.solidProgram);
    gl.glUniform4f(d.solid.projection, 2.0F * frame.zoom / width, -2.0F * frame.zoom / height, 0.0F,
                   0.0F);
    gl.glUniform1i(d.solid.invert, invert);
    for (const render::DrawItem& item : frame.content) {
        d.draw(item);
    }

    // 3. Overlays: view pixels → clip.
    gl.glUniform4f(d.solid.projection, 2.0F / width, -2.0F / height, -1.0F, 1.0F);
    gl.glUniform1i(d.solid.invert, 0);
    for (const render::DrawItem& item : frame.overlay) {
        d.draw(item);
    }
    gl.glBindVertexArray(0);
    gl.glUseProgram(0);
    gl.glEndQuery(GL_TIME_ELAPSED);
    d.timerPending[query] = true;

    d.stats.liveMeshes = d.liveMeshes;
    d.stats.gpuMs = d.gpuMs;
    d.stats.meshBytes = d.meshBytes;
    if (!d.reportedGlError) {
        if (const GLenum error = gl.glGetError(); error != GL_NO_ERROR) {
            d.reportedGlError = true; // report once; a broken driver must not flood the log
            core::logError("render", "OpenGL error 0x" + QString::number(error, 16).toStdString());
        }
    }
}

render::RenderStats OpenGLRenderer::lastFrameStats() const noexcept {
    return impl_->stats;
}

void OpenGLRenderer::releaseAll() noexcept {
    Impl& d = *impl_;
    if (!d.initialized) {
        return;
    }
    // Slots are kept (with bumped generations) rather than cleared, so a handle from before
    // the release can never match a mesh created after re-initialisation.
    d.freeSlots.clear();
    for (std::uint32_t i = 0; i < d.slots.size(); ++i) {
        MeshSlot& slot = d.slots[i];
        if (slot.alive) {
            d.gl->glDeleteVertexArrays(1, &slot.vao);
            d.gl->glDeleteBuffers(1, &slot.vbo);
            d.gl->glDeleteBuffers(1, &slot.ibo);
            if (slot.cbo != 0) {
                d.gl->glDeleteBuffers(1, &slot.cbo);
            }
        }
        slot = MeshSlot{.generation = slot.generation + 1};
        d.freeSlots.push_back(i);
    }
    d.liveMeshes = 0;
    d.meshBytes = 0;
    d.gl->glDeleteProgram(d.solidProgram);
    d.gl->glDeleteProgram(d.patternProgram);
    d.gl->glDeleteVertexArrays(1, &d.emptyVao);
    d.gl->glDeleteQueries(static_cast<GLsizei>(d.timerQueries.size()), d.timerQueries.data());
    d.timerQueries.fill(0);
    d.timerPending.fill(false);
    d.solidProgram = 0;
    d.patternProgram = 0;
    d.emptyVao = 0;
    QObject::disconnect(d.contextConnection);
    d.initialized = false;
}

} // namespace studyapp::render_gl
