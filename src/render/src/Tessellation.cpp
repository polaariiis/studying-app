#include <studyapp/render/Tessellation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace studyapp::render {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
/// cos(30°): turns gentler than this share a vertex pair (bounded miter).
const float kSharedJoinCos = std::cos(kPi / 6.0F);

bool isFinite(const core::Vec2& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

class MeshBuilder {
public:
    explicit MeshBuilder(MeshData& mesh) : mesh_(&mesh) {}

    std::uint32_t vertex(const core::Vec2& v) {
        mesh_->vertices.push_back(v);
        mesh_->bounds = mesh_->bounds.including(v);
        return static_cast<std::uint32_t>(mesh_->vertices.size() - 1);
    }
    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        mesh_->indices.insert(mesh_->indices.end(), {a, b, c});
    }
    /// Quad between the vertex pairs (l0, r0) and (l1, r1).
    void quad(std::uint32_t l0, std::uint32_t r0, std::uint32_t l1, std::uint32_t r1) {
        triangle(l0, r0, l1);
        triangle(l1, r0, r1);
    }
    void disc(const core::Vec2& center, float radius, int segments) {
        const std::uint32_t c = vertex(center);
        const std::uint32_t first = vertex(center + core::Vec2{radius, 0.0F});
        std::uint32_t previous = first;
        for (int i = 1; i < segments; ++i) {
            const float angle = 2.0F * kPi * static_cast<float>(i) / static_cast<float>(segments);
            const std::uint32_t next =
                vertex(center + core::Vec2{std::cos(angle), std::sin(angle)} * radius);
            triangle(c, previous, next);
            previous = next;
        }
        triangle(c, previous, first);
    }
    /// Half disc around `center`, sweeping from `from` through `through` to −`from`
    /// (both unit vectors, perpendicular).
    void halfDisc(const core::Vec2& center, float radius, const core::Vec2& from,
                  const core::Vec2& through, int segments) {
        const std::uint32_t c = vertex(center);
        std::uint32_t previous = vertex(center + from * radius);
        for (int i = 1; i <= segments; ++i) {
            const float t = kPi * static_cast<float>(i) / static_cast<float>(segments);
            const std::uint32_t next =
                vertex(center + (from * std::cos(t) + through * std::sin(t)) * radius);
            triangle(c, previous, next);
            previous = next;
        }
    }

private:
    MeshData* mesh_;
};

core::Vec2 perpendicular(const core::Vec2& d) noexcept {
    return {-d.y, d.x};
}

} // namespace

int circleSegments(float radius, const TessellationOptions& options) noexcept {
    const float radiusPx = radius * options.pixelsPerUnit;
    const float tolerance = std::max(options.roundTolerancePx, 1e-3F);
    if (!std::isfinite(radiusPx) || radiusPx <= tolerance) {
        return 6;
    }
    const float step = 2.0F * std::acos(1.0F - tolerance / radiusPx);
    if (!(step > 0.0F)) {
        return 96;
    }
    const float segments = std::ceil(2.0F * kPi / step);
    return static_cast<int>(std::clamp(segments, 6.0F, 96.0F));
}

MeshData tessellatePolyline(std::span<const WidthPoint> input, const TessellationOptions& options) {
    // Usable points, consecutive duplicates merged (keeping the larger radius).
    std::vector<WidthPoint> points;
    points.reserve(input.size());
    for (const WidthPoint& p : input) {
        if (!isFinite(p.position) || !std::isfinite(p.radius) || p.radius <= 0.0F) {
            continue;
        }
        if (!points.empty()) {
            const float mergeDistance = 1e-4F * std::max(1.0F, p.radius);
            if ((p.position - points.back().position).lengthSquared() <
                mergeDistance * mergeDistance) {
                points.back().radius = std::max(points.back().radius, p.radius);
                continue;
            }
        }
        points.push_back(p);
    }

    MeshData mesh;
    if (points.empty()) {
        return mesh;
    }
    MeshBuilder build(mesh);
    if (points.size() == 1) {
        build.disc(points[0].position, points[0].radius, circleSegments(points[0].radius, options));
        return mesh;
    }

    const std::size_t n = points.size();
    std::vector<core::Vec2> directions(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        directions[i] = (points[i + 1].position - points[i].position).normalized();
    }
    const auto capSegments = [&](float radius) {
        return std::max(2, circleSegments(radius, options) / 2);
    };

    // Start cap and first vertex pair.
    const WidthPoint& first = points.front();
    core::Vec2 normal = perpendicular(directions.front());
    build.halfDisc(first.position, first.radius, normal, -directions.front(),
                   capSegments(first.radius));
    std::uint32_t left = build.vertex(first.position + normal * first.radius);
    std::uint32_t right = build.vertex(first.position - normal * first.radius);

    for (std::size_t k = 1; k + 1 < n; ++k) {
        const WidthPoint& p = points[k];
        const core::Vec2 before = perpendicular(directions[k - 1]);
        const core::Vec2 after = perpendicular(directions[k]);
        if (directions[k - 1].dot(directions[k]) > kSharedJoinCos) {
            // Gentle turn: one shared pair along the averaged normal. dot(miter, after) is
            // cos(turn / 2) >= cos(15°), so the offset is at most 1.04 × radius.
            const core::Vec2 miter = (before + after).normalized();
            const float length = p.radius / std::max(miter.dot(after), 0.5F);
            const std::uint32_t l = build.vertex(p.position + miter * length);
            const std::uint32_t r = build.vertex(p.position - miter * length);
            build.quad(left, right, l, r);
            left = l;
            right = r;
        } else {
            // Sharp turn: finish the segment, cover the corner with a disc, restart.
            const std::uint32_t l = build.vertex(p.position + before * p.radius);
            const std::uint32_t r = build.vertex(p.position - before * p.radius);
            build.quad(left, right, l, r);
            build.disc(p.position, p.radius, circleSegments(p.radius, options));
            left = build.vertex(p.position + after * p.radius);
            right = build.vertex(p.position - after * p.radius);
        }
    }

    // Last segment and end cap.
    const WidthPoint& last = points.back();
    normal = perpendicular(directions.back());
    const std::uint32_t l = build.vertex(last.position + normal * last.radius);
    const std::uint32_t r = build.vertex(last.position - normal * last.radius);
    build.quad(left, right, l, r);
    build.halfDisc(last.position, last.radius, -normal, directions.back(),
                   capSegments(last.radius));
    return mesh;
}

MeshData tessellateConvexFill(std::span<const core::Vec2> polygon) {
    MeshData mesh;
    if (polygon.size() < 3) {
        return mesh;
    }
    MeshBuilder build(mesh);
    const std::uint32_t first = build.vertex(polygon[0]);
    std::uint32_t previous = build.vertex(polygon[1]);
    for (std::size_t i = 2; i < polygon.size(); ++i) {
        const std::uint32_t next = build.vertex(polygon[i]);
        build.triangle(first, previous, next);
        previous = next;
    }
    return mesh;
}

MeshData tessellateRectOutline(const core::Rect& rect, float halfWidth) {
    MeshData mesh;
    if (rect.isEmpty() || !(halfWidth > 0.0F)) {
        return mesh;
    }
    const core::Rect outer = rect.expanded(halfWidth);
    const core::Rect inner = rect.expanded(-halfWidth);
    MeshBuilder build(mesh);
    const auto corners = [&](const core::Rect& r) {
        return std::array<std::uint32_t, 4>{build.vertex(r.min), build.vertex({r.max.x, r.min.y}),
                                            build.vertex(r.max), build.vertex({r.min.x, r.max.y})};
    };
    const auto o = corners(outer);
    if (inner.isEmpty() || inner.width() <= 0.0F || inner.height() <= 0.0F) {
        build.quad(o[0], o[1], o[3], o[2]);
        return mesh;
    }
    const auto in = corners(inner);
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t j = (i + 1) % 4;
        build.quad(o[i], in[i], o[j], in[j]);
    }
    return mesh;
}

std::vector<core::Vec2> ellipsePolygon(const core::Rect& bounds,
                                       const TessellationOptions& options) {
    std::vector<core::Vec2> polygon;
    if (bounds.isEmpty()) {
        return polygon;
    }
    const core::Vec2 center = bounds.center();
    const core::Vec2 radii = bounds.size() * 0.5F;
    const int segments = circleSegments(std::max(radii.x, radii.y), options);
    polygon.reserve(static_cast<std::size_t>(segments));
    for (int i = 0; i < segments; ++i) {
        const float angle = 2.0F * kPi * static_cast<float>(i) / static_cast<float>(segments);
        polygon.push_back(center +
                          core::Vec2{std::cos(angle) * radii.x, std::sin(angle) * radii.y});
    }
    return polygon;
}

void appendMesh(MeshData& target, const MeshData& source) {
    const auto base = static_cast<std::uint32_t>(target.vertices.size());
    target.vertices.insert(target.vertices.end(), source.vertices.begin(), source.vertices.end());
    target.indices.reserve(target.indices.size() + source.indices.size());
    for (const std::uint32_t index : source.indices) {
        target.indices.push_back(base + index);
    }
    target.bounds = target.bounds.united(source.bounds);
}

} // namespace studyapp::render
