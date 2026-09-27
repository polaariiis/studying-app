#include <studyapp/canvas/SelectionHandles.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace studyapp::canvas {

namespace {

using core::DVec2;
using document::Element;
using document::ShapeKind;

bool isPlain(const document::Transform& t) noexcept {
    return t.rotation == 0.0F && t.scale.x == 1.0F && t.scale.y == 1.0F;
}

bool isLine(const document::Shape& shape) noexcept {
    return shape.kind == ShapeKind::Line || shape.kind == ShapeKind::Arrow;
}

std::optional<core::Vec2> boxSize(const Element& element) {
    return std::visit(
        [](const auto& payload) -> std::optional<core::Vec2> {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, document::Shape>) {
                return isLine(payload) ? std::nullopt : std::optional{payload.size};
            } else if constexpr (std::is_same_v<T, document::Image> ||
                                 std::is_same_v<T, document::TextBox>) {
                return payload.size;
            } else {
                return std::nullopt;
            }
        },
        element.payload);
}

DVec2 lineEnd(const Element& element, const document::Shape& shape) {
    const double angle = static_cast<double>(element.transform.rotation);
    const double length = static_cast<double>(shape.size.x);
    return element.transform.position + DVec2{std::cos(angle), std::sin(angle)} * length;
}

bool isCorner(HandleKind kind) noexcept {
    return kind == HandleKind::TopLeft || kind == HandleKind::TopRight ||
           kind == HandleKind::BottomRight || kind == HandleKind::BottomLeft;
}

} // namespace

std::vector<Handle> handlesFor(const Element& element) {
    std::vector<Handle> handles;
    if (const auto* connector = std::get_if<document::Connector>(&element.payload)) {
        handles.push_back({HandleKind::Start, connector->start.position});
        handles.push_back({HandleKind::End, connector->end.position});
        return handles;
    }
    if (const auto* shape = std::get_if<document::Shape>(&element.payload);
        shape && isLine(*shape)) {
        // A line's rotation is its direction: only a scale hides its handles.
        if (element.transform.scale.x == 1.0F && element.transform.scale.y == 1.0F) {
            handles.push_back({HandleKind::Start, element.transform.position});
            handles.push_back({HandleKind::End, lineEnd(element, *shape)});
        }
        return handles;
    }
    if (!isPlain(element.transform)) {
        return handles;
    }
    const auto size = boxSize(element);
    if (!size) {
        return handles;
    }
    const DVec2 min = element.transform.position;
    const DVec2 max = min + DVec2{static_cast<double>(size->x), static_cast<double>(size->y)};
    const DVec2 mid = (min + max) * 0.5;
    if (std::holds_alternative<document::TextBox>(element.payload)) {
        handles.push_back({HandleKind::Left, {min.x, mid.y}});
        handles.push_back({HandleKind::Right, {max.x, mid.y}});
        return handles;
    }
    handles.reserve(8);
    handles.push_back({HandleKind::TopLeft, min});
    handles.push_back({HandleKind::Top, {mid.x, min.y}});
    handles.push_back({HandleKind::TopRight, {max.x, min.y}});
    handles.push_back({HandleKind::Right, {max.x, mid.y}});
    handles.push_back({HandleKind::BottomRight, max});
    handles.push_back({HandleKind::Bottom, {mid.x, max.y}});
    handles.push_back({HandleKind::BottomLeft, {min.x, max.y}});
    handles.push_back({HandleKind::Left, {min.x, mid.y}});
    return handles;
}

std::optional<Element> dragHandle(const Element& element, HandleKind handle, const DVec2& world,
                                  const HandleDragOptions& options) {
    const auto handles = handlesFor(element);
    if (std::none_of(handles.begin(), handles.end(),
                     [&](const Handle& h) { return h.kind == handle; })) {
        return std::nullopt;
    }
    Element result = element;
    if (auto* connector = std::get_if<document::Connector>(&result.payload)) {
        document::ConnectorEnd& end =
            handle == HandleKind::Start ? connector->start : connector->end;
        end.position = world;
        end.attachedTo.reset();
        return result;
    }
    if (auto* shape = std::get_if<document::Shape>(&result.payload); shape && isLine(*shape)) {
        DVec2 start = element.transform.position;
        DVec2 end = lineEnd(element, *shape);
        (handle == HandleKind::Start ? start : end) = world;
        const DVec2 d = end - start;
        double angle = std::atan2(d.y, d.x);
        if (options.constrain) {
            constexpr double step = std::numbers::pi / 4.0;
            angle = std::round(angle / step) * step;
            const DVec2 fixed = handle == HandleKind::Start ? end : start;
            const DVec2 dir{std::cos(angle), std::sin(angle)};
            (handle == HandleKind::Start ? start : end) =
                fixed + dir * (d.length() * (handle == HandleKind::Start ? -1.0 : 1.0));
        }
        const double length = std::max((end - start).length(), options.minSize);
        result.transform.position = start;
        result.transform.rotation =
            static_cast<float>(std::atan2((end - start).y, (end - start).x));
        shape->size = {static_cast<float>(length), 0.0F};
        return result;
    }
    const core::Vec2 size = *boxSize(element);
    DVec2 min = element.transform.position;
    DVec2 max = min + DVec2{static_cast<double>(size.x), static_cast<double>(size.y)};
    const bool left = handle == HandleKind::TopLeft || handle == HandleKind::Left ||
                      handle == HandleKind::BottomLeft;
    const bool right = handle == HandleKind::TopRight || handle == HandleKind::Right ||
                       handle == HandleKind::BottomRight;
    const bool top = handle == HandleKind::TopLeft || handle == HandleKind::Top ||
                     handle == HandleKind::TopRight;
    const bool bottom = handle == HandleKind::BottomLeft || handle == HandleKind::Bottom ||
                        handle == HandleKind::BottomRight;
    const bool image = std::holds_alternative<document::Image>(element.payload);
    const bool keepAspect =
        isCorner(handle) && (image != options.constrain) && size.x > 0.0F && size.y > 0.0F;
    // The fixed corner or edge, and the dragged one.
    const DVec2 fixed{left ? max.x : min.x, top ? max.y : min.y};
    DVec2 moved{left || right ? world.x : max.x, top || bottom ? world.y : max.y};
    if (keepAspect) {
        const double w = std::abs(moved.x - fixed.x);
        const double h = std::abs(moved.y - fixed.y);
        const double scale =
            std::max(w / static_cast<double>(size.x), h / static_cast<double>(size.y));
        moved = {fixed.x + std::copysign(static_cast<double>(size.x) * scale, moved.x - fixed.x),
                 fixed.y + std::copysign(static_cast<double>(size.y) * scale, moved.y - fixed.y)};
    }
    if (left || right) {
        min.x = std::min(fixed.x, moved.x);
        max.x = std::max(fixed.x, moved.x);
    }
    if (top || bottom) {
        min.y = std::min(fixed.y, moved.y);
        max.y = std::max(fixed.y, moved.y);
    }
    const double width = std::max(max.x - min.x, options.minSize);
    double height = std::max(max.y - min.y, options.minSize);
    if (auto* text = std::get_if<document::TextBox>(&result.payload)) {
        const auto w = static_cast<float>(width);
        height = options.textHeight ? static_cast<double>(options.textHeight(text->text, w))
                                    : static_cast<double>(size.y);
        min.y = element.transform.position.y; // text boxes grow downwards
    }
    result.transform.position = min;
    const core::Vec2 newSize{static_cast<float>(width), static_cast<float>(height)};
    std::visit(
        [&](auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, document::Shape> ||
                          std::is_same_v<T, document::Image> ||
                          std::is_same_v<T, document::TextBox>) {
                payload.size = newSize;
            }
        },
        result.payload);
    return result;
}

} // namespace studyapp::canvas
