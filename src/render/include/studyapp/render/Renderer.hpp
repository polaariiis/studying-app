#pragma once

#include <studyapp/core/Affine2.hpp>
#include <studyapp/core/Color.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/Rect.hpp>
#include <studyapp/core/Vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace studyapp::render {

// Backend-neutral rendering API (docs/RENDERING.md §3). The renderer draws primitives:
// meshes with a transform and a colour, and a procedural page background. It knows
// nothing about strokes, pages or documents; the canvas translates elements into these
// primitives. No GL, no Qt.

/// Opaque, generational reference to a mesh owned by a Renderer. A default-constructed
/// handle is invalid; a handle whose mesh was destroyed is rejected by the backend.
struct MeshHandle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0; ///< 0 = invalid

    [[nodiscard]] bool isValid() const noexcept { return generation != 0; }
    [[nodiscard]] friend bool operator==(const MeshHandle&, const MeshHandle&) = default;
};

/// Opaque, generational reference to a texture owned by a Renderer (like MeshHandle).
struct TextureHandle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0; ///< 0 = invalid

    [[nodiscard]] bool isValid() const noexcept { return generation != 0; }
    [[nodiscard]] friend bool operator==(const TextureHandle&, const TextureHandle&) = default;
};

/// CPU image: RGBA8 with premultiplied alpha, rows from top to bottom, tightly packed.
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; ///< width × height × 4 bytes

    [[nodiscard]] bool empty() const noexcept {
        return width <= 0 || height <= 0 ||
               pixels.size() !=
                   static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    }
    [[nodiscard]] std::size_t byteSize() const noexcept { return pixels.size(); }
};

/// CPU triangle mesh (indexed triangle list) in the space its DrawItem transform maps
/// from: element-local space for document content, view space for overlays.
struct MeshData {
    std::vector<core::Vec2> vertices;
    std::vector<std::uint32_t> indices; ///< three per triangle
    /// Optional per-vertex colours (parallel to `vertices`), e.g. for batches of elements
    /// with different colours. When present, the DrawItem colour multiplies them.
    std::vector<core::Color> colors;
    /// Optional per-vertex part numbers (parallel to `vertices`, 0-based, non-decreasing)
    /// for meshes that combine several pieces of content, e.g. a batch of strokes. Each
    /// part covers a pixel at most once (see RenderFrame::content). Absent: one part.
    std::vector<std::uint32_t> parts;
    core::Rect bounds = core::Rect::emptyBounds();

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
    [[nodiscard]] std::size_t byteSize() const noexcept {
        return vertices.size() * sizeof(core::Vec2) + indices.size() * sizeof(std::uint32_t) +
               colors.size() * sizeof(core::Color) + parts.size() * sizeof(std::uint32_t);
    }
};

/// One mesh to draw, in painter's order.
struct DrawItem {
    MeshHandle mesh{};
    /// Content items: element-local to camera-relative world (world minus camera centre),
    /// so float precision is only needed for element and screen sizes (floating origin,
    /// docs/RENDERING.md §5). Overlay items: mesh space to view pixels.
    core::Affine2f transform{};
    core::Color color = core::Color::black();
    float opacity = 1.0F;
    /// Valid: this item draws the texture on the unit square [0, 1]² (texture row 0 at
    /// y = 0), which `transform` maps like a mesh; `mesh` is ignored and `color` tints it.
    TextureHandle texture{};
};

enum class BackgroundPattern : std::uint8_t {
    None,
    Ruled, ///< horizontal lines
    Grid,
    Dots,
};

/// Procedural page background, drawn behind the content by the pattern program.
struct Background {
    core::Color deskColor;    ///< everything outside a bounded page
    core::Color paperColor;   ///< the page itself (the whole view for infinite pages)
    core::Color patternColor; ///< lines / dots
    BackgroundPattern pattern = BackgroundPattern::None;
    float spacing = 32.0F; ///< world units between lines/dots
    /// Camera centre modulo `spacing`, computed in double precision by the caller, so the
    /// pattern stays anchored in world space without float precision loss far from 0.
    core::Vec2 patternPhase{};
    bool bounded = false;
    core::Rect pageRect{}; ///< camera-relative world rectangle of a bounded page
};

/// How content colours are shown. Display-only; stored colours never change.
enum class ColorTransform : std::uint8_t {
    None,
    InvertLightness, ///< HSL lightness inverted, hue kept: dark-paper display
};

struct RenderFrame {
    core::Vec2 viewportSize{};     ///< logical pixels
    float devicePixelRatio = 1.0F; ///< framebuffer pixels per logical pixel
    float zoom = 1.0F;             ///< view pixels per world unit
    Background background{};
    ColorTransform contentColorTransform = ColorTransform::None;
    /// Camera-relative world, painter's order, culled. Each item (each part of a mesh with
    /// MeshData::parts) colours a pixel at most once: where a piece of content overlaps
    /// itself (a stroke's joins, caps and loops) its first fragment wins, so translucent ink
    /// stays even. Later items still blend over earlier ones, so separate translucent
    /// strokes build up where they cross (docs/RENDERING.md §6.5).
    std::span<const DrawItem> content;
    std::span<const DrawItem> overlay; ///< view space, drawn last
};

struct RenderStats {
    std::uint32_t drawCalls = 0;
    std::uint64_t triangles = 0;
    std::uint32_t liveMeshes = 0;
    std::uint64_t meshBytes = 0; ///< GPU memory held by live meshes (approximate)
    std::uint32_t liveTextures = 0;
    std::uint64_t textureBytes = 0; ///< GPU memory held by live textures (approximate)
    /// GPU time of a recent frame in ms (timer queries lag a frame or two); < 0 if unknown.
    double gpuMs = -1.0;
};

/// Implemented by render_gl::OpenGLRenderer (and by a recording renderer in tests).
/// All calls happen on the thread that owns the graphics context, with it current.
class Renderer {
public:
    Renderer() = default;
    virtual ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    /// Creates GPU programs and state. Must succeed before anything else is called.
    [[nodiscard]] virtual core::Result<void> initialize() = 0;
    [[nodiscard]] virtual bool isInitialized() const noexcept = 0;
    /// Framebuffer size in physical pixels.
    virtual void resize(int framebufferWidth, int framebufferHeight) = 0;

    [[nodiscard]] virtual MeshHandle createMesh(const MeshData& mesh) = 0;
    /// Replaces the geometry of an existing mesh (e.g. the live stroke every frame).
    virtual void updateMesh(MeshHandle handle, const MeshData& mesh) = 0;
    virtual void destroyMesh(MeshHandle handle) = 0;

    /// Uploads `image` (sampled with linear filtering and mipmaps). Invalid handle for an
    /// empty image or one larger than the backend supports.
    [[nodiscard]] virtual TextureHandle createTexture(const ImageData& image) = 0;
    virtual void destroyTexture(TextureHandle handle) = 0;

    virtual void render(const RenderFrame& frame) = 0;
    [[nodiscard]] virtual RenderStats lastFrameStats() const noexcept = 0;

    /// Releases every GPU resource (the context is about to go away). Handles become
    /// invalid; initialize() must be called again before further use.
    virtual void releaseAll() noexcept = 0;
};

} // namespace studyapp::render
