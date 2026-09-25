#include <studyapp/persistence/PageStore.hpp>

#include "RowDecoder.hpp"
#include "StoreSupport.hpp"

#include <studyapp/persistence/StrokeCodec.hpp>

#include <cassert>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;
using detail::forward;
using detail::millis;
using detail::RowDecoder;
using document::Connector;
using document::Element;
using document::ElementKind;
using document::Image;
using document::Shape;
using document::Stroke;
using document::TextBox;

namespace {

/// Version of the `text_box.content` JSON written by this build: `{"v":1,"text":"…"}`
/// (plain text; the rich-text model of DATA_MODEL.md §4.3 will be version 2).
constexpr std::int64_t kTextContentVersion = 1;

std::int64_t argb(const core::Color& color) {
    return static_cast<std::int64_t>(color.toArgb32());
}

Statement& bindOptionalColor(Statement& statement, int index,
                             const std::optional<core::Color>& color) {
    return color ? statement.bindInt(index, argb(*color)) : statement.bindNull(index);
}

bool samePoints(const Stroke& a, const Stroke& b) {
    if (a.points == b.points) {
        return true; // shared array: the common case after a move or restyle
    }
    return a.points && b.points && *a.points == *b.points;
}

// ---------------------------------------------------------------------------- kind rows

Result<void> writeKindRow(Database& database, core::ElementId id, const Stroke& stroke, bool insert,
                          bool writePoints) {
    const auto& points = *stroke.points;
    const std::string_view sql =
        insert ? "INSERT INTO stroke (element_id, brush, color, base_width, point_count, "
                 "point_format, points) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
        : writePoints
            ? "UPDATE stroke SET brush = ?2, color = ?3, base_width = ?4, point_count = ?5, "
              "point_format = ?6, points = ?7 WHERE element_id = ?1"
            : "UPDATE stroke SET brush = ?2, color = ?3, base_width = ?4 WHERE element_id = ?1";
    auto statement = database.cached(sql);
    if (!statement) {
        return forward(statement);
    }
    Statement& s = **statement;
    s.bindId(1, id)
        .bindInt(2, static_cast<std::int64_t>(stroke.brush))
        .bindInt(3, argb(stroke.color))
        .bindReal(4, static_cast<double>(stroke.baseWidth));
    if (insert || writePoints) {
        const std::vector<std::uint8_t> blob = encodeStrokePoints(points);
        s.bindInt(5, static_cast<std::int64_t>(points.size()))
            .bindInt(6, kStrokePointFormat)
            .bindBlob(7, blob);
        return detail::runOnOneRow(database, s, "stroke", id.toString());
    }
    return detail::runOnOneRow(database, s, "stroke", id.toString());
}

Result<void> writeKindRow(Database& database, core::ElementId id, const TextBox& text, bool insert,
                          bool /*writePoints*/) {
    // sizing 2 = fixed box: the Phase 2 model stores an explicit size.
    auto statement = database.cached(
        insert ? "INSERT INTO text_box (element_id, width, height, sizing, content, plain_text) "
                 "VALUES (?1, ?2, ?3, 2, json_object('v', ?4, 'text', ?5), ?5)"
               : "UPDATE text_box SET width = ?2, height = ?3, "
                 "content = json_object('v', ?4, 'text', ?5), plain_text = ?5 "
                 "WHERE element_id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)
        ->bindId(1, id)
        .bindReal(2, static_cast<double>(text.size.x))
        .bindReal(3, static_cast<double>(text.size.y))
        .bindInt(4, kTextContentVersion)
        .bindText(5, text.text);
    return detail::runOnOneRow(database, **statement, "text box", id.toString());
}

Result<void> writeKindRow(Database& database, core::ElementId id, const Shape& shape, bool insert,
                          bool /*writePoints*/) {
    auto statement = database.cached(
        insert ? "INSERT INTO shape (element_id, shape_kind, width, height, stroke_color, "
                 "stroke_width, fill_color) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
               : "UPDATE shape SET shape_kind = ?2, width = ?3, height = ?4, stroke_color = ?5, "
                 "stroke_width = ?6, fill_color = ?7 WHERE element_id = ?1");
    if (!statement) {
        return forward(statement);
    }
    Statement& s = **statement;
    s.bindId(1, id)
        .bindInt(2, static_cast<std::int64_t>(shape.kind))
        .bindReal(3, static_cast<double>(shape.size.x))
        .bindReal(4, static_cast<double>(shape.size.y));
    bindOptionalColor(s, 5, shape.strokeColor).bindReal(6, static_cast<double>(shape.strokeWidth));
    bindOptionalColor(s, 7, shape.fillColor);
    return detail::runOnOneRow(database, s, "shape", id.toString());
}

Result<void> writeKindRow(Database& database, core::ElementId id, const Image& image, bool insert,
                          bool /*writePoints*/) {
    auto statement = database.cached(
        insert ? "INSERT INTO image (element_id, asset_id, width, height) VALUES (?1, ?2, ?3, ?4)"
               : "UPDATE image SET asset_id = ?2, width = ?3, height = ?4 WHERE element_id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)
        ->bindId(1, id)
        .bindId(2, image.asset)
        .bindReal(3, static_cast<double>(image.size.x))
        .bindReal(4, static_cast<double>(image.size.y));
    return detail::runOnOneRow(database, **statement, "image", id.toString());
}

Result<void> writeKindRow(Database& database, core::ElementId id, const Connector& connector,
                          bool insert, bool /*writePoints*/) {
    auto statement = database.cached(
        insert ? "INSERT INTO connector (element_id, start_x, start_y, start_element_id, end_x, "
                 "end_y, end_element_id, color, width) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
               : "UPDATE connector SET start_x = ?2, start_y = ?3, start_element_id = ?4, "
                 "end_x = ?5, end_y = ?6, end_element_id = ?7, color = ?8, width = ?9 "
                 "WHERE element_id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)
        ->bindId(1, id)
        .bindReal(2, connector.start.position.x)
        .bindReal(3, connector.start.position.y)
        .bindId(4, connector.start.attachedTo)
        .bindReal(5, connector.end.position.x)
        .bindReal(6, connector.end.position.y)
        .bindId(7, connector.end.attachedTo)
        .bindInt(8, argb(connector.color))
        .bindReal(9, static_cast<double>(connector.width));
    return detail::runOnOneRow(database, **statement, "connector", id.toString());
}

std::string_view kindTable(ElementKind kind) {
    switch (kind) {
    case ElementKind::Stroke:
        return "stroke";
    case ElementKind::TextBox:
        return "text_box";
    case ElementKind::Shape:
        return "shape";
    case ElementKind::Image:
        return "image";
    case ElementKind::Connector:
        return "connector";
    }
    return "stroke";
}

Result<void> deleteKindRow(Database& database, core::ElementId id, ElementKind kind) {
    auto statement =
        database.cached("DELETE FROM " + std::string(kindTable(kind)) + " WHERE element_id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)->bindId(1, id);
    return detail::runOnOneRow(database, **statement, kindTable(kind), id.toString());
}

Result<void> writeHeader(Database& database, const Element& element, bool insert,
                         core::Timestamp now) {
    auto statement = database.cached(
        insert ? "INSERT INTO element (id, page_id, layer_id, kind, z_key, pos_x, pos_y, rotation, "
                 "scale_x, scale_y, min_x, min_y, max_x, max_y, locked, created_at, updated_at) "
                 "VALUES (?1, (SELECT page_id FROM layer WHERE id = ?2), ?2, ?3, ?4, ?5, ?6, ?7, "
                 "?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?15)"
               : "UPDATE element SET page_id = (SELECT page_id FROM layer WHERE id = ?2), "
                 "layer_id = ?2, kind = ?3, z_key = ?4, pos_x = ?5, pos_y = ?6, rotation = ?7, "
                 "scale_x = ?8, scale_y = ?9, min_x = ?10, min_y = ?11, max_x = ?12, "
                 "max_y = ?13, locked = ?14, updated_at = ?15 WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    const core::DRect bounds = document::worldBounds(element);
    const document::Transform& t = element.transform;
    (*statement)
        ->bindId(1, element.id)
        .bindId(2, element.layer)
        .bindInt(3, static_cast<std::int64_t>(element.kind()))
        .bindText(4, element.z.value())
        .bindReal(5, t.position.x)
        .bindReal(6, t.position.y)
        .bindReal(7, static_cast<double>(t.rotation))
        .bindReal(8, static_cast<double>(t.scale.x))
        .bindReal(9, static_cast<double>(t.scale.y))
        .bindReal(10, bounds.min.x)
        .bindReal(11, bounds.min.y)
        .bindReal(12, bounds.max.x)
        .bindReal(13, bounds.max.y)
        .bindInt(14, element.locked ? 1 : 0)
        .bindInt(15, millis(now));
    return detail::runOnOneRow(database, **statement, "element", element.id.toString());
}

// ---------------------------------------------------------------------------- loading

/// Loaded headers of one page, with payload slots filled from the kind tables.
struct ElementRows {
    std::vector<Element> elements;
    std::vector<bool> filled;
    std::unordered_map<core::ElementId, std::size_t> index;
};

/// Runs `sql` (one parameter: the page id) and hands each row to `decodeRow`, which
/// returns the payload for the element in column 0.
template <class Payload, class DecodeRow>
Result<void> loadKind(Database& database, core::PageId page, std::string_view sql,
                      std::string_view what, ElementRows& rows, DecodeRow decodeRow) {
    auto statement = database.cached(sql);
    if (!statement) {
        return forward(statement);
    }
    (*statement)->bindId(1, page);
    while (true) {
        auto row = (*statement)->step();
        if (!row) {
            return forward(row);
        }
        if (!*row) {
            return {};
        }
        RowDecoder decode(**statement, what);
        const auto id = decode.id<core::ElementId>(0);
        Payload payload = decodeRow(decode, **statement);
        if (auto status = decode.status(); !status) {
            return forward(status);
        }
        const auto it = rows.index.find(id);
        if (it == rows.index.end()) {
            return makeError(ErrorCode::ParseError, "corrupt " + std::string(what) +
                                                        " row: element " + id.toString() +
                                                        " has no header on this page");
        }
        Element& element = rows.elements[it->second];
        if (element.kind() != document::kindOf(document::ElementPayload{payload}) ||
            rows.filled[it->second]) {
            return makeError(ErrorCode::ParseError, "corrupt " + std::string(what) +
                                                        " row: element " + id.toString() +
                                                        " has a different or duplicate kind row");
        }
        element.payload = std::move(payload);
        rows.filled[it->second] = true;
    }
}

} // namespace

Result<PageData> PageStore::load(core::PageId page) {
    PageData data{.page = page, .layers = {}, .elements = {}};

    // ---- layers
    {
        auto statement = database_->cached(
            "SELECT id, name, sort_key, visible, locked, opacity FROM layer WHERE page_id = ?1");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, page);
        while (true) {
            auto row = (*statement)->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            RowDecoder decode(**statement, "layer");
            document::Layer layer{.id = decode.id<core::LayerId>(0),
                                  .page = page,
                                  .name = decode.text(1),
                                  .order = decode.key(2),
                                  .visible = decode.boolean(3),
                                  .locked = decode.boolean(4),
                                  .opacity = decode.real32(5)};
            if (auto status = decode.status(); !status) {
                return forward(status);
            }
            data.layers.push_back(std::move(layer));
        }
    }

    // ---- element headers (kind stored; payload filled from the kind tables below)
    ElementRows rows;
    {
        auto statement = database_->cached(
            "SELECT e.id, e.layer_id, e.kind, e.z_key, e.pos_x, e.pos_y, e.rotation, e.scale_x, "
            "e.scale_y, e.locked, l.page_id FROM element e LEFT JOIN layer l ON l.id = e.layer_id "
            "WHERE e.page_id = ?1");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, page);
        while (true) {
            auto row = (*statement)->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            RowDecoder decode(**statement, "element");
            Element element{.id = decode.id<core::ElementId>(0),
                            .layer = decode.id<core::LayerId>(1),
                            .z = decode.key(3),
                            .transform = {.position = {decode.real(4), decode.real(5)},
                                          .rotation = decode.real32(6),
                                          .scale = {decode.real32(7), decode.real32(8)}},
                            .locked = decode.boolean(9),
                            .payload = {}};
            const auto kind = decode.enumeration<ElementKind>(2, 1, 5);
            const auto layerPage = decode.optionalId<core::PageId>(10);
            if (decode.ok() && layerPage != page) {
                decode.failRow("element " + element.id.toString() +
                               " is not on the page of its layer");
            }
            if (auto status = decode.status(); !status) {
                return forward(status);
            }
            // Placeholder payload of the stored kind; replaced by the kind row.
            switch (kind) {
            case ElementKind::Stroke:
                element.payload = Stroke{};
                break;
            case ElementKind::TextBox:
                element.payload = TextBox{};
                break;
            case ElementKind::Shape:
                element.payload = Shape{};
                break;
            case ElementKind::Image:
                element.payload = Image{};
                break;
            case ElementKind::Connector:
                element.payload = Connector{};
                break;
            }
            rows.index.emplace(element.id, rows.elements.size());
            rows.elements.push_back(std::move(element));
            rows.filled.push_back(false);
        }
    }

    // ---- kind rows
    auto strokes = loadKind<Stroke>(
        *database_, page,
        "SELECT k.element_id, k.brush, k.color, k.base_width, k.point_count, k.point_format, "
        "k.points FROM stroke k JOIN element e ON e.id = k.element_id WHERE e.page_id = ?1",
        "stroke", rows, [](RowDecoder& decode, const Statement& s) {
            Stroke stroke{.brush = decode.enumeration<document::Brush>(1, 0, 3),
                          .color = decode.color(2),
                          .baseWidth = decode.real32(3),
                          .points = {}};
            const std::int64_t count = decode.integer(4);
            const std::int64_t format = decode.integer(5);
            if (!decode.ok()) {
                return stroke;
            }
            if (format != kStrokePointFormatV1) {
                decode.failRow("unsupported point format " + std::to_string(format));
                return stroke;
            }
            if (s.columnType(6) != ColumnType::Blob) {
                decode.failRow("points are not a BLOB");
                return stroke;
            }
            auto points = decodeStrokePoints(s.columnBlob(6));
            if (!points) {
                decode.failRow(points.error().message);
                return stroke;
            }
            if (static_cast<std::int64_t>(points->size()) != count) {
                decode.failRow("point_count does not match the point blob");
                return stroke;
            }
            stroke.points = document::makeStrokePoints(std::move(*points));
            return stroke;
        });
    if (!strokes) {
        return forward(strokes);
    }

    auto texts = loadKind<TextBox>(
        *database_, page,
        "SELECT k.element_id, k.width, k.height, json_extract(k.content, '$.v'), "
        "json_extract(k.content, '$.text') FROM text_box k JOIN element e ON e.id = k.element_id "
        "WHERE e.page_id = ?1",
        "text_box", rows, [](RowDecoder& decode, const Statement& /*s*/) {
            TextBox text{.size = {decode.real32(1), decode.real32(2)}, .text = {}};
            const std::int64_t version = decode.integer(3);
            if (decode.ok() && version != kTextContentVersion) {
                decode.failRow("unsupported content version " + std::to_string(version));
                return text;
            }
            text.text = decode.text(4);
            return text;
        });
    if (!texts) {
        return forward(texts);
    }

    auto shapes = loadKind<Shape>(
        *database_, page,
        "SELECT k.element_id, k.shape_kind, k.width, k.height, k.stroke_color, k.stroke_width, "
        "k.fill_color FROM shape k JOIN element e ON e.id = k.element_id WHERE e.page_id = ?1",
        "shape", rows, [](RowDecoder& decode, const Statement& /*s*/) {
            return Shape{.kind = decode.enumeration<document::ShapeKind>(1, 0, 2),
                         .size = {decode.real32(2), decode.real32(3)},
                         .strokeColor = decode.optionalColor(4),
                         .strokeWidth = decode.real32(5),
                         .fillColor = decode.optionalColor(6)};
        });
    if (!shapes) {
        return forward(shapes);
    }

    auto images = loadKind<Image>(*database_, page,
                                  "SELECT k.element_id, k.asset_id, k.width, k.height FROM image k "
                                  "JOIN element e ON e.id = k.element_id WHERE e.page_id = ?1",
                                  "image", rows, [](RowDecoder& decode, const Statement& /*s*/) {
                                      return Image{.asset = decode.id<core::AssetId>(1),
                                                   .size = {decode.real32(2), decode.real32(3)}};
                                  });
    if (!images) {
        return forward(images);
    }

    auto connectors = loadKind<Connector>(
        *database_, page,
        "SELECT k.element_id, k.start_x, k.start_y, k.start_element_id, k.end_x, k.end_y, "
        "k.end_element_id, k.color, k.width FROM connector k JOIN element e ON e.id = k.element_id "
        "WHERE e.page_id = ?1",
        "connector", rows, [](RowDecoder& decode, const Statement& /*s*/) {
            return Connector{.start = {.position = {decode.real(1), decode.real(2)},
                                       .attachedTo = decode.optionalId<core::ElementId>(3)},
                             .end = {.position = {decode.real(4), decode.real(5)},
                                     .attachedTo = decode.optionalId<core::ElementId>(6)},
                             .color = decode.color(7),
                             .width = decode.real32(8)};
        });
    if (!connectors) {
        return forward(connectors);
    }

    for (std::size_t i = 0; i < rows.elements.size(); ++i) {
        if (!rows.filled[i]) {
            return makeError(ErrorCode::ParseError,
                             "corrupt element row: element " + rows.elements[i].id.toString() +
                                 " has no " + std::string(kindTable(rows.elements[i].kind())) +
                                 " row");
        }
    }
    data.elements = std::move(rows.elements);
    return data;
}

// ---------------------------------------------------------------------------- apply

Result<void> PageStore::apply(Transaction& transaction, const document::LayerChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        auto statement = database_->cached("DELETE FROM layer WHERE id = ?1");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, change.before->id);
        return detail::runOnOneRow(*database_, **statement, "layer", change.before->id.toString());
    }
    const document::Layer& layer = *change.after;
    auto statement = database_->cached(
        change.isCreate()
            ? "INSERT INTO layer (id, page_id, name, sort_key, visible, locked, opacity) "
              "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
            : "UPDATE layer SET page_id = ?2, name = ?3, sort_key = ?4, visible = ?5, "
              "locked = ?6, opacity = ?7 WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)
        ->bindId(1, layer.id)
        .bindId(2, layer.page)
        .bindText(3, layer.name)
        .bindText(4, layer.order.value())
        .bindInt(5, layer.visible ? 1 : 0)
        .bindInt(6, layer.locked ? 1 : 0)
        .bindReal(7, static_cast<double>(layer.opacity));
    if (auto written = detail::runOnOneRow(*database_, **statement, "layer", layer.id.toString());
        !written) {
        return written;
    }
    if (change.isUpdate() && change.before->page != layer.page) {
        // Keep the denormalised element.page_id in step with the layer's new page.
        auto moved = database_->cached("UPDATE element SET page_id = ?2 WHERE layer_id = ?1");
        if (!moved) {
            return forward(moved);
        }
        return (*moved)->bindId(1, layer.id).bindId(2, layer.page).run();
    }
    return {};
}

Result<void> PageStore::apply(Transaction& transaction, const document::ElementChange& change,
                              core::Timestamp now) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        auto statement = database_->cached("DELETE FROM element WHERE id = ?1");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, change.before->id);
        return detail::runOnOneRow(*database_, **statement, "element",
                                   change.before->id.toString());
    }

    const Element& after = *change.after;
    const bool insert = change.isCreate();
    if (auto header = writeHeader(*database_, after, insert, now); !header) {
        return header;
    }

    if (insert) {
        return std::visit(
            [&](const auto& payload) {
                return writeKindRow(*database_, after.id, payload, true, true);
            },
            after.payload);
    }

    const Element& before = *change.before;
    if (before.kind() != after.kind()) {
        if (auto removed = deleteKindRow(*database_, before.id, before.kind()); !removed) {
            return removed;
        }
        return std::visit(
            [&](const auto& payload) {
                return writeKindRow(*database_, after.id, payload, true, true);
            },
            after.payload);
    }
    if (before.payload == after.payload) {
        return {}; // header-only change (move, restyle of the header, reorder)
    }
    return std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            bool writePoints = true;
            if constexpr (std::is_same_v<T, Stroke>) {
                writePoints = !samePoints(std::get<Stroke>(before.payload), payload);
            }
            return writeKindRow(*database_, after.id, payload, false, writePoints);
        },
        after.payload);
}

} // namespace studyapp::persistence
