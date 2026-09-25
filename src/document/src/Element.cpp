#include <studyapp/document/Element.hpp>

#include <array>
#include <cmath>
#include <type_traits>
#include <utility>

namespace studyapp::document {

StrokePoints makeStrokePoints(std::vector<StrokePoint> points) {
    return std::make_shared<const std::vector<StrokePoint>>(std::move(points));
}

bool operator==(const Stroke& lhs, const Stroke& rhs) {
    if (lhs.brush != rhs.brush || lhs.color != rhs.color || lhs.baseWidth != rhs.baseWidth) {
        return false;
    }
    if (lhs.points == rhs.points) {
        return true; // same shared array (the common case for undo snapshots)
    }
    if (!lhs.points || !rhs.points) {
        return false;
    }
    return *lhs.points == *rhs.points;
}

ElementKind kindOf(const ElementPayload& payload) {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Stroke>) {
                return ElementKind::Stroke;
            } else if constexpr (std::is_same_v<T, TextBox>) {
                return ElementKind::TextBox;
            } else if constexpr (std::is_same_v<T, Shape>) {
                return ElementKind::Shape;
            } else if constexpr (std::is_same_v<T, Image>) {
                return ElementKind::Image;
            } else {
                static_assert(std::is_same_v<T, Connector>, "unhandled element payload");
                return ElementKind::Connector;
            }
        },
        payload);
}

std::string_view toString(ElementKind kind) noexcept {
    switch (kind) {
    case ElementKind::Stroke:
        return "stroke";
    case ElementKind::TextBox:
        return "text box";
    case ElementKind::Shape:
        return "shape";
    case ElementKind::Image:
        return "image";
    case ElementKind::Connector:
        return "connector";
    }
    return "element";
}

core::DRect localBounds(const ElementPayload& payload) {
    const auto box = [](const core::Vec2& size) {
        return core::DRect::fromPoints({0.0, 0.0}, core::vectorCast<double>(size));
    };
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Stroke>) {
                auto bounds = core::DRect::emptyBounds();
                if (value.points) {
                    for (const StrokePoint& p : *value.points) {
                        bounds = bounds.including({p.x, p.y});
                    }
                }
                return bounds.expanded(value.baseWidth / 2.0);
            } else if constexpr (std::is_same_v<T, Connector>) {
                return core::DRect::fromPoints(value.start.position, value.end.position)
                    .expanded(value.width / 2.0);
            } else {
                static_assert(std::is_same_v<T, TextBox> || std::is_same_v<T, Shape> ||
                                  std::is_same_v<T, Image>,
                              "unhandled element payload");
                return box(value.size);
            }
        },
        payload);
}

core::Affine2 localToWorld(const Transform& transform) noexcept {
    return core::Affine2::translation(transform.position) *
           core::Affine2::rotation(static_cast<double>(transform.rotation)) *
           core::Affine2::scaling(static_cast<double>(transform.scale.x),
                                  static_cast<double>(transform.scale.y));
}

core::DRect worldBounds(const Element& element) {
    const core::DRect local = localBounds(element.payload);
    if (local.isEmpty() || std::holds_alternative<Connector>(element.payload)) {
        return local;
    }
    const core::Affine2 toWorld = localToWorld(element.transform);
    const std::array<core::DVec2, 4> corners{{
        {local.min.x, local.min.y},
        {local.max.x, local.min.y},
        {local.min.x, local.max.y},
        {local.max.x, local.max.y},
    }};
    auto bounds = core::DRect::emptyBounds();
    for (const core::DVec2& corner : corners) {
        bounds = bounds.including(toWorld.apply(corner));
    }
    return bounds;
}

} // namespace studyapp::document
