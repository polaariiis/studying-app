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

#include <algorithm>
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

/// Content parts per depth range (render::RenderFrame::content: each part covers a pixel at
/// most once). Part k of a frame is drawn at window depth (k + 1) / kDepthParts and passes
/// only where the depth buffer holds less: its own earlier fragments are rejected, later
/// parts pass. 2^20 steps are 16 units apart in a 24-bit depth buffer and exact in float;
/// the shader uses the same constant (solid.vert). A frame with more parts clears the
/// depth buffer and starts over (only overlaps across the reset are not suppressed).
///
/// Needs a depth buffer of at least 24 bits: with fewer, neighbouring parts would round to
/// the same depth and later content would be rejected where it overlaps earlier content.
/// The renderer checks the bound framebuffer and draws without the rule otherwise (only the
/// self-overlap suppression is lost). One mesh's parts must fit the range (batches hold at
/// most RenderBatches::kBatchSize elements).
constexpr std::uint32_t kDepthParts = 1U << 20U;
constexpr GLint kMinDepthBits = 24;

struct MeshSlot {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    GLuint cbo = 0; ///< per-vertex colours (0 if the mesh has none)
    GLuint pbo = 0; ///< per-vertex part numbers (0 if the mesh has none)
    GLsizei indexCount = 0;
    std::uint32_t partCount = 1;
    std::uint32_t generation = 1;
    bool alive = false;
    bool hasColors = false;
    bool hasParts = false;
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

struct TextureSlot {
    GLuint id = 0;
    std::uint32_t generation = 1;
    bool alive = false;
    std::uint64_t bytes = 0;
};

struct OpenGLRenderer::Impl {
    QOpenGLContext* context = nullptr;
    QSurface* surface = nullptr;
    QOpenGLFunctions_3_3_Core* gl = nullptr;
    QMetaObject::Connection contextConnection;

    GLuint solidProgram = 0;
    GLuint patternProgram = 0;
    GLuint texturedProgram = 0;
    GLuint emptyVao = 0;
    // The unit square [0, 1]² that textured items draw (two triangles).
    GLuint quadVao = 0;
    GLuint quadVbo = 0;
    GLuint quadIbo = 0;
    GLint maxTextureSize = 0;
    struct {
        GLint model = -1, projection = -1, color = -1, invert = -1, firstPart = -1, sampler = -1;
    } textured;
    struct {
        GLint model = -1, projection = -1, color = -1, invert = -1, vertexColors = -1,
              firstPart = -1, vertexParts = -1;
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
    std::vector<TextureSlot> textureSlots;
    std::vector<std::uint32_t> freeTextureSlots;
    std::uint32_t liveTextures = 0;
    std::uint64_t textureBytes = 0;
    GLuint currentProgram = 0; ///< while drawing content: solid or textured
    std::uint32_t liveMeshes = 0;
    std::uint64_t meshBytes = 0;
    std::uint32_t nextPart = 0; ///< depth part of the next content item this frame
    GLint depthBits = -1;       ///< of the framebuffer drawn into; -1 = not queried yet
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
        gl->glBindAttribLocation(program, 2, "aPart");
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
        const bool parted = !mesh.parts.empty() && mesh.parts.size() == mesh.vertices.size();
        if (parted) {
            if (slot.pbo == 0) {
                gl->glGenBuffers(1, &slot.pbo);
            }
            gl->glBindBuffer(GL_ARRAY_BUFFER, slot.pbo);
            gl->glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(mesh.parts.size() * sizeof(std::uint32_t)),
                             mesh.parts.data(), usage);
            gl->glEnableVertexAttribArray(2);
            gl->glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(std::uint32_t), nullptr);
            slot.partCount = std::ranges::max(mesh.parts) + 1U;
            slot.hasParts = true;
        } else {
            gl->glDisableVertexAttribArray(2);
            if (slot.pbo != 0) {
                gl->glDeleteBuffers(1, &slot.pbo);
                slot.pbo = 0;
            }
            slot.partCount = 1;
            slot.hasParts = false;
        }
        gl->glBindVertexArray(0);
        slot.indexCount = static_cast<GLsizei>(mesh.indices.size());
        slot.bytes = mesh.byteSize();
        meshBytes += slot.bytes;
    }

    TextureSlot* textureFor(render::TextureHandle handle) noexcept {
        if (!handle.isValid() || handle.index >= textureSlots.size()) {
            return nullptr;
        }
        TextureSlot& slot = textureSlots[handle.index];
        return slot.alive && slot.generation == handle.generation ? &slot : nullptr;
    }

    void useProgram(GLuint program) {
        if (currentProgram != program) {
            gl->glUseProgram(program);
            currentProgram = program;
        }
    }

    /// Takes the next depth part, clearing the depth buffer when the range is used up.
    std::uint32_t takeParts(std::uint32_t parts) {
        if (nextPart > kDepthParts - parts) {
            gl->glClear(GL_DEPTH_BUFFER_BIT);
            nextPart = 0;
        }
        const std::uint32_t first = nextPart;
        nextPart += parts;
        return first;
    }

    /// A textured content item: the unit quad with the item's texture.
    void drawTexturedContent(const render::DrawItem& item) {
        const TextureSlot* texture = textureFor(item.texture);
        if (texture == nullptr) {
            return;
        }
        useProgram(texturedProgram);
        gl->glUniform1f(textured.firstPart, static_cast<float>(takeParts(1)));
        const core::Affine2f& t = item.transform;
        const GLfloat model[9] = {t.a, t.b, 0.0F, t.c, t.d, 0.0F, t.tx, t.ty, 1.0F};
        gl->glUniformMatrix3fv(textured.model, 1, GL_FALSE, model);
        gl->glUniform4f(textured.color, channel(item.color.r), channel(item.color.g),
                        channel(item.color.b), channel(item.color.a) * item.opacity);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, texture->id);
        gl->glBindVertexArray(quadVao);
        gl->glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        ++stats.drawCalls;
        stats.triangles += 2;
    }

    /// A content item, drawn at the next depth parts (render::RenderFrame::content).
    void drawContent(const render::DrawItem& item) {
        if (item.texture.isValid()) {
            drawTexturedContent(item);
            return;
        }
        const MeshSlot* slot = slotFor(item.mesh);
        if (slot == nullptr || slot->indexCount == 0) {
            return;
        }
        useProgram(solidProgram);
        const std::uint32_t parts = std::min(slot->partCount, kDepthParts);
        gl->glUniform1f(solid.firstPart, static_cast<float>(takeParts(parts)));
        draw(*slot, item);
    }

    /// An overlay item (no depth).
    void drawOverlay(const render::DrawItem& item) {
        if (const MeshSlot* slot = slotFor(item.mesh); slot != nullptr && slot->indexCount != 0) {
            draw(*slot, item);
        }
    }

    void draw(const MeshSlot& slot, const render::DrawItem& item) {
        const core::Affine2f& t = item.transform;
        // Column-major 3×3: columns (a, b, 0), (c, d, 0), (tx, ty, 1).
        const GLfloat model[9] = {t.a, t.b, 0.0F, t.c, t.d, 0.0F, t.tx, t.ty, 1.0F};
        gl->glUniformMatrix3fv(solid.model, 1, GL_FALSE, model);
        gl->glUniform1i(solid.vertexColors, slot.hasColors ? 1 : 0);
        gl->glUniform1i(solid.vertexParts, slot.hasParts ? 1 : 0);
        gl->glUniform4f(solid.color, channel(item.color.r), channel(item.color.g),
                        channel(item.color.b), channel(item.color.a) * item.opacity);
        gl->glBindVertexArray(slot.vao);
        gl->glDrawElements(GL_TRIANGLES, slot.indexCount, GL_UNSIGNED_INT, nullptr);
        ++stats.drawCalls;
        stats.triangles += static_cast<std::uint64_t>(slot.indexCount / 3);
    }

    /// Depth bits of the framebuffer bound for drawing (Qt's FBO for a QOpenGLWidget).
    GLint queryDepthBits() {
        GLint framebuffer = 0;
        gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        GLint bits = 0;
        if (framebuffer != 0) {
            GLint type = GL_NONE;
            gl->glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                      GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            if (type != GL_NONE) {
                gl->glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                          GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE,
                                                          &bits);
            }
        } else {
            gl->glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                                      GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
        }
        return bits;
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
    auto textured = d.link(":/shaders/textured.vert", ":/shaders/textured.frag");
    if (!textured) {
        d.gl->glDeleteProgram(*solid);
        d.gl->glDeleteProgram(*pattern);
        return tl::unexpected(textured.error());
    }
    d.solidProgram = *solid;
    d.patternProgram = *pattern;
    d.texturedProgram = *textured;
    const auto uniform = [&](GLuint program, const char* name) {
        return d.gl->glGetUniformLocation(program, name);
    };
    d.solid = {
        uniform(d.solidProgram, "uModel"),        uniform(d.solidProgram, "uProjection"),
        uniform(d.solidProgram, "uColor"),        uniform(d.solidProgram, "uInvertLightness"),
        uniform(d.solidProgram, "uVertexColors"), uniform(d.solidProgram, "uFirstPart"),
        uniform(d.solidProgram, "uVertexParts")};
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
    d.textured = {
        uniform(d.texturedProgram, "uModel"),     uniform(d.texturedProgram, "uProjection"),
        uniform(d.texturedProgram, "uColor"),     uniform(d.texturedProgram, "uInvertLightness"),
        uniform(d.texturedProgram, "uFirstPart"), uniform(d.texturedProgram, "uTexture")};
    d.gl->glUseProgram(d.texturedProgram);
    d.gl->glUniform1i(d.textured.sampler, 0);
    d.gl->glUseProgram(0);
    {
        static constexpr std::array<GLfloat, 8> corners{0, 0, 1, 0, 1, 1, 0, 1};
        static constexpr std::array<GLuint, 6> triangles{0, 1, 2, 0, 2, 3};
        d.gl->glGenVertexArrays(1, &d.quadVao);
        d.gl->glGenBuffers(1, &d.quadVbo);
        d.gl->glGenBuffers(1, &d.quadIbo);
        d.gl->glBindVertexArray(d.quadVao);
        d.gl->glBindBuffer(GL_ARRAY_BUFFER, d.quadVbo);
        d.gl->glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners.data(), GL_STATIC_DRAW);
        d.gl->glEnableVertexAttribArray(0);
        d.gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), nullptr);
        d.gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.quadIbo);
        d.gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(triangles), triangles.data(),
                           GL_STATIC_DRAW);
        d.gl->glBindVertexArray(0);
    }
    d.gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &d.maxTextureSize);
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
    if (slot->pbo != 0) {
        d.gl->glDeleteBuffers(1, &slot->pbo);
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

    // 2. Content: camera-relative world → clip (y flipped: view y points down). Each part
    //    covers a pixel once: depth test "greater" against increasing per-part depths.
    gl.glEnable(GL_BLEND);
    gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (d.depthBits < 0) {
        d.depthBits = d.queryDepthBits();
        if (d.depthBits < kMinDepthBits) {
            core::logWarning("render", "depth buffer has " + std::to_string(d.depthBits) +
                                           " bits; translucent ink may darken where it "
                                           "overlaps itself");
        }
    }
    const bool singleCoverage = d.depthBits >= kMinDepthBits;
    if (singleCoverage) {
        gl.glDepthMask(GL_TRUE);
        gl.glClearDepth(0.0);
        gl.glClear(GL_DEPTH_BUFFER_BIT);
        gl.glEnable(GL_DEPTH_TEST);
        gl.glDepthFunc(GL_GREATER);
    }
    gl.glUseProgram(d.texturedProgram);
    gl.glUniform4f(d.textured.projection, 2.0F * frame.zoom / width, -2.0F * frame.zoom / height,
                   0.0F, 0.0F);
    gl.glUniform1i(d.textured.invert, invert);
    gl.glUseProgram(d.solidProgram);
    d.currentProgram = d.solidProgram;
    gl.glUniform4f(d.solid.projection, 2.0F * frame.zoom / width, -2.0F * frame.zoom / height, 0.0F,
                   0.0F);
    gl.glUniform1i(d.solid.invert, invert);
    d.nextPart = 0;
    for (const render::DrawItem& item : frame.content) {
        d.drawContent(item);
    }
    gl.glDisable(GL_DEPTH_TEST);

    // 3. Overlays: view pixels → clip.
    d.useProgram(d.solidProgram);
    gl.glUniform4f(d.solid.projection, 2.0F / width, -2.0F / height, -1.0F, 1.0F);
    gl.glUniform1i(d.solid.invert, 0);
    for (const render::DrawItem& item : frame.overlay) {
        d.drawOverlay(item);
    }
    gl.glBindVertexArray(0);
    gl.glUseProgram(0);
    gl.glEndQuery(GL_TIME_ELAPSED);
    d.timerPending[query] = true;

    d.currentProgram = 0;
    d.stats.liveMeshes = d.liveMeshes;
    d.stats.gpuMs = d.gpuMs;
    d.stats.meshBytes = d.meshBytes;
    d.stats.liveTextures = d.liveTextures;
    d.stats.textureBytes = d.textureBytes;
    if (!d.reportedGlError) {
        if (const GLenum error = gl.glGetError(); error != GL_NO_ERROR) {
            d.reportedGlError = true; // report once; a broken driver must not flood the log
            core::logError("render", "OpenGL error 0x" + QString::number(error, 16).toStdString());
        }
    }
}

render::TextureHandle OpenGLRenderer::createTexture(const render::ImageData& image) {
    Impl& d = *impl_;
    if (!d.initialized || image.empty() || image.width > d.maxTextureSize ||
        image.height > d.maxTextureSize) {
        return {};
    }
    std::uint32_t index = 0;
    if (!d.freeTextureSlots.empty()) {
        index = d.freeTextureSlots.back();
        d.freeTextureSlots.pop_back();
    } else {
        index = static_cast<std::uint32_t>(d.textureSlots.size());
        d.textureSlots.emplace_back();
    }
    TextureSlot& slot = d.textureSlots[index];
    QOpenGLFunctions_3_3_Core& gl = *d.gl;
    gl.glGenTextures(1, &slot.id);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, slot.id);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, image.pixels.data());
    gl.glGenerateMipmap(GL_TEXTURE_2D);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glBindTexture(GL_TEXTURE_2D, 0);
    slot.alive = true;
    slot.bytes = image.byteSize() + image.byteSize() / 3; // with mipmaps
    d.textureBytes += slot.bytes;
    ++d.liveTextures;
    return {.index = index, .generation = slot.generation};
}

void OpenGLRenderer::destroyTexture(render::TextureHandle handle) {
    Impl& d = *impl_;
    if (!d.initialized) {
        return;
    }
    TextureSlot* slot = d.textureFor(handle);
    if (slot == nullptr) {
        return;
    }
    d.gl->glDeleteTextures(1, &slot->id);
    d.textureBytes -= slot->bytes;
    --d.liveTextures;
    *slot = TextureSlot{.generation = slot->generation + 1};
    d.freeTextureSlots.push_back(handle.index);
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
            if (slot.pbo != 0) {
                d.gl->glDeleteBuffers(1, &slot.pbo);
            }
        }
        slot = MeshSlot{.generation = slot.generation + 1};
        d.freeSlots.push_back(i);
    }
    d.liveMeshes = 0;
    d.meshBytes = 0;
    d.freeTextureSlots.clear();
    for (std::uint32_t i = 0; i < d.textureSlots.size(); ++i) {
        TextureSlot& slot = d.textureSlots[i];
        if (slot.alive) {
            d.gl->glDeleteTextures(1, &slot.id);
        }
        slot = TextureSlot{.generation = slot.generation + 1};
        d.freeTextureSlots.push_back(i);
    }
    d.liveTextures = 0;
    d.textureBytes = 0;
    d.depthBits = -1; // a new context may bring another framebuffer format
    d.gl->glDeleteProgram(d.solidProgram);
    d.gl->glDeleteProgram(d.patternProgram);
    d.gl->glDeleteProgram(d.texturedProgram);
    d.gl->glDeleteVertexArrays(1, &d.quadVao);
    d.gl->glDeleteBuffers(1, &d.quadVbo);
    d.gl->glDeleteBuffers(1, &d.quadIbo);
    d.texturedProgram = 0;
    d.quadVao = 0;
    d.quadVbo = 0;
    d.quadIbo = 0;
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
