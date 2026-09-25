#pragma once

#include <studyapp/core/Color.hpp>
#include <studyapp/core/FractionalIndex.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/core/Vec2.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace studyapp::document {

// Canvas elements (docs/DATA_MODEL.md §4.1): a common header plus a closed set of payload
// kinds in a std::variant. Phase 2 defines the *data* of each kind only; rendering, hit
// testing and editing tools arrive with the canvas (Phase 4/6). Fields that later phases
// need (rich text, polygon vertices, arrow caps, ...) are added with those phases.

/// Placement of an element: position in world units (double, for the infinite canvas),
/// rotation about the origin, and scale. No skew, by design.
struct Transform {
    core::DVec2 position{};
    float rotation = 0.0F; ///< radians
    core::Vec2 scale{1.0F, 1.0F};

    [[nodiscard]] friend bool operator==(const Transform&, const Transform&) = default;
};

/// One sampled point of a stroke, in element-local coordinates.
struct StrokePoint {
    float x = 0.0F;
    float y = 0.0F;
    float pressure = 1.0F; ///< 0..1

    [[nodiscard]] friend bool operator==(const StrokePoint&, const StrokePoint&) = default;
};

/// Immutable point data shared between the document and undo history snapshots, so that
/// copying an Element never copies points (docs/DATA_MODEL.md §4.1). Edits create a new
/// point array rather than mutating an existing one.
using StrokePoints = std::shared_ptr<const std::vector<StrokePoint>>;

[[nodiscard]] StrokePoints makeStrokePoints(std::vector<StrokePoint> points);

enum class Brush : std::uint8_t {
    Pen,
    Pencil,
    Highlighter,
    Marker
};

struct Stroke {
    Brush brush = Brush::Pen;
    core::Color color = core::Color::black();
    float baseWidth = 2.0F; ///< world units at pressure 1; > 0
    StrokePoints points;    ///< non-null, at least one point

    /// Compares point *values*, not pointer identity.
    // (No [[nodiscard]]: attributes are only allowed on friend declarations that are definitions.)
    friend bool operator==(const Stroke& lhs, const Stroke& rhs);
};

bool operator==(const Stroke& lhs, const Stroke& rhs);

/// Typed text. Plain text in Phase 2; the rich-text model is introduced in Phase 6.
struct TextBox {
    core::Vec2 size{}; ///< box size in local units; components >= 0
    std::string text;  ///< UTF-8

    [[nodiscard]] friend bool operator==(const TextBox&, const TextBox&) = default;
};

enum class ShapeKind : std::uint8_t {
    Rectangle,
    Ellipse,
    Line
};

struct Shape {
    ShapeKind kind = ShapeKind::Rectangle;
    core::Vec2 size{}; ///< components >= 0
    std::optional<core::Color> strokeColor = core::Color::black();
    float strokeWidth = 2.0F; ///< >= 0
    std::optional<core::Color> fillColor{};

    [[nodiscard]] friend bool operator==(const Shape&, const Shape&) = default;
};

/// Raster image referencing an external, content-addressed asset (docs/DATABASE_SCHEMA.md
/// §3). The asset store arrives in Phase 3; Phase 2 only requires a non-null id.
struct Image {
    core::AssetId asset;
    core::Vec2 size{}; ///< displayed size in local units; components >= 0

    [[nodiscard]] friend bool operator==(const Image&, const Image&) = default;
};

struct ConnectorEnd {
    core::DVec2 position{};                    ///< world position (cached when attached)
    std::optional<core::ElementId> attachedTo; ///< non-connector element on the same page

    [[nodiscard]] friend bool operator==(const ConnectorEnd&, const ConnectorEnd&) = default;
};

struct Connector {
    ConnectorEnd start;
    ConnectorEnd end;
    core::Color color = core::Color::black();
    float width = 2.0F; ///< > 0

    [[nodiscard]] friend bool operator==(const Connector&, const Connector&) = default;
};

using ElementPayload = std::variant<Stroke, TextBox, Shape, Image, Connector>;

/// Stable numeric kinds (they will be stored in the database; never reuse a value).
enum class ElementKind : std::uint8_t {
    Stroke = 1,
    TextBox = 2,
    Shape = 3,
    Image = 4,
    Connector = 5,
};

[[nodiscard]] ElementKind kindOf(const ElementPayload& payload);
[[nodiscard]] std::string_view toString(ElementKind kind) noexcept;

struct Element {
    core::ElementId id;
    core::LayerId layer;     ///< parent
    core::FractionalIndex z; ///< order within the layer (painter's order)
    Transform transform{};
    bool locked = false;
    ElementPayload payload;

    [[nodiscard]] ElementKind kind() const { return kindOf(payload); }

    [[nodiscard]] friend bool operator==(const Element&, const Element&) = default;
};

} // namespace studyapp::document
