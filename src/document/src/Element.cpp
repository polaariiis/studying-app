#include <studyapp/document/Element.hpp>

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

} // namespace studyapp::document
