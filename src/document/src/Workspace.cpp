#include <studyapp/document/Workspace.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace studyapp::document {

using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

template <class Record>
constexpr std::string_view recordName() {
    if constexpr (std::is_same_v<Record, NotebookInfo>) {
        return "notebook";
    } else if constexpr (std::is_same_v<Record, SectionInfo>) {
        return "section";
    } else if constexpr (std::is_same_v<Record, PageInfo>) {
        return "page";
    } else if constexpr (std::is_same_v<Record, Layer>) {
        return "layer";
    } else {
        static_assert(std::is_same_v<Record, Element>);
        return "element";
    }
}

const core::FractionalIndex& orderKey(const NotebookInfo& r) noexcept {
    return r.order;
}
const core::FractionalIndex& orderKey(const SectionInfo& r) noexcept {
    return r.order;
}
const core::FractionalIndex& orderKey(const PageInfo& r) noexcept {
    return r.order;
}
const core::FractionalIndex& orderKey(const Layer& r) noexcept {
    return r.order;
}
const core::FractionalIndex& orderKey(const Element& r) noexcept {
    return r.z;
}

bool isFinite(const core::Vec2& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y);
}
bool isFinite(const core::DVec2& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y);
}
bool isNonNegative(const core::Vec2& v) noexcept {
    return isFinite(v) && v.x >= 0.0F && v.y >= 0.0F;
}

Result<void> invalid(std::string message) {
    return makeError(ErrorCode::InvalidArgument, std::move(message));
}

// ---- payload value checks (no workspace context needed) ----

Result<void> checkPayload(const Stroke& stroke) {
    if (!stroke.points || stroke.points->empty()) {
        return invalid("stroke must have at least one point");
    }
    if (!(std::isfinite(stroke.baseWidth) && stroke.baseWidth > 0.0F)) {
        return invalid("stroke width must be positive and finite");
    }
    const bool pointsValid =
        std::all_of(stroke.points->begin(), stroke.points->end(), [](const StrokePoint& p) {
            return std::isfinite(p.x) && std::isfinite(p.y) && p.pressure >= 0.0F &&
                   p.pressure <= 1.0F;
        });
    return pointsValid ? Result<void>{}
                       : invalid("stroke points must be finite with pressure in [0, 1]");
}

Result<void> checkPayload(const TextBox& text) {
    return isNonNegative(text.size) ? Result<void>{}
                                    : invalid("text box size must be finite and non-negative");
}

Result<void> checkPayload(const Shape& shape) {
    if (!isNonNegative(shape.size)) {
        return invalid("shape size must be finite and non-negative");
    }
    return std::isfinite(shape.strokeWidth) && shape.strokeWidth >= 0.0F
               ? Result<void>{}
               : invalid("shape stroke width must be finite and non-negative");
}

Result<void> checkPayload(const Image& image) {
    if (image.asset.isNull()) {
        return invalid("image must reference an asset");
    }
    return isNonNegative(image.size) ? Result<void>{}
                                     : invalid("image size must be finite and non-negative");
}

/// Value checks only; attachment rules need the workspace (Workspace::checkAttachments).
Result<void> checkPayload(const Connector& connector) {
    if (!(std::isfinite(connector.width) && connector.width > 0.0F)) {
        return invalid("connector width must be positive and finite");
    }
    return isFinite(connector.start.position) && isFinite(connector.end.position)
               ? Result<void>{}
               : invalid("connector end positions must be finite");
}

template <class Id, class Map>
std::span<const Id> childrenIn(const Map& index, const typename Map::key_type& parent) {
    const auto it = index.find(parent);
    return it == index.end() ? std::span<const Id>{} : std::span<const Id>(it->second);
}

template <class Map>
auto* findIn(const Map& table, const typename Map::key_type& id) {
    const auto it = table.find(id);
    return it == table.end() ? nullptr : &it->second;
}

/// Sorts ids by (order key, id) using the records in `table`.
template <class Id, class Table>
void sortSiblings(std::vector<Id>& ids, const Table& table) {
    std::sort(ids.begin(), ids.end(), [&](const Id& a, const Id& b) {
        const auto& ka = orderKey(table.at(a));
        const auto& kb = orderKey(table.at(b));
        return ka != kb ? ka < kb : a < b;
    });
}

} // namespace

namespace {

template <class T>
struct RecordOf;
template <class R>
struct RecordOf<Change<R>> {
    using type = R;
};

std::string describeChange(const AnyChange& change, std::size_t index) {
    return std::visit(
        [index](const auto& c) {
            using Record = typename RecordOf<std::decay_t<decltype(c)>>::type;
            std::string_view verb = "update";
            if (c.isCreate()) {
                verb = "create";
            } else if (c.isRemove()) {
                verb = "remove";
            }
            return "change " + std::to_string(index) + " (" + std::string(verb) + " " +
                   std::string(recordName<Record>()) + ")";
        },
        change);
}

} // namespace

Workspace::Workspace(WorkspaceInfo info) : info_(std::move(info)) {}

// ---------------------------------------------------------------------------- queries

std::span<const core::NotebookId> Workspace::notebooks() const noexcept {
    return notebookOrder_;
}
std::span<const core::SectionId> Workspace::sectionsOf(core::NotebookId notebook) const {
    return childrenIn<core::SectionId>(sectionsByNotebook_, notebook);
}
std::span<const core::PageId> Workspace::pagesOf(core::SectionId section) const {
    return childrenIn<core::PageId>(pagesBySection_, section);
}
std::span<const core::LayerId> Workspace::layersOf(core::PageId page) const {
    return childrenIn<core::LayerId>(layersByPage_, page);
}
std::span<const core::ElementId> Workspace::elementsOf(core::LayerId layer) const {
    return childrenIn<core::ElementId>(elementsByLayer_, layer);
}

const NotebookInfo* Workspace::findNotebook(core::NotebookId id) const {
    return findIn(notebooks_, id);
}
const SectionInfo* Workspace::findSection(core::SectionId id) const {
    return findIn(sections_, id);
}
const PageInfo* Workspace::findPage(core::PageId id) const {
    return findIn(pages_, id);
}
const Layer* Workspace::findLayer(core::LayerId id) const {
    return findIn(layers_, id);
}
const Element* Workspace::findElement(core::ElementId id) const {
    return findIn(elements_, id);
}

std::optional<core::PageId> Workspace::pageOf(core::ElementId element) const {
    const Element* e = findElement(element);
    const Layer* layer = e != nullptr ? findLayer(e->layer) : nullptr;
    return layer != nullptr ? std::optional(layer->page) : std::nullopt;
}

// ---------------------------------------------------------------------------- record checks

Result<void> Workspace::checkRecord(const NotebookInfo& record) const {
    return validateName(record.title, "notebook title");
}

Result<void> Workspace::checkRecord(const SectionInfo& record) const {
    if (auto name = validateName(record.title, "section title"); !name) {
        return name;
    }
    if (findNotebook(record.notebook) == nullptr) {
        return makeError(ErrorCode::NotFound,
                         "parent notebook " + record.notebook.toString() + " does not exist");
    }
    return {};
}

Result<void> Workspace::checkRecord(const PageInfo& record) const {
    if (findSection(record.section) == nullptr) {
        return makeError(ErrorCode::NotFound,
                         "parent section " + record.section.toString() + " does not exist");
    }
    if (record.extent == PageExtent::Bounded &&
        !(isFinite(record.size) && record.size.x > 0.0 && record.size.y > 0.0)) {
        return invalid("bounded page must have a positive, finite size");
    }
    if (!(std::isfinite(record.background.spacing) && record.background.spacing > 0.0F)) {
        return invalid("page background spacing must be positive and finite");
    }
    return {};
}

Result<void> Workspace::checkRecord(const Layer& record) const {
    if (auto name = validateName(record.name, "layer name"); !name) {
        return name;
    }
    if (findPage(record.page) == nullptr) {
        return makeError(ErrorCode::NotFound,
                         "parent page " + record.page.toString() + " does not exist");
    }
    if (!(std::isfinite(record.opacity) && record.opacity >= 0.0F && record.opacity <= 1.0F)) {
        return invalid("layer opacity must be within [0, 1]");
    }
    return {};
}

Result<void> Workspace::checkRecord(const Element& record) const {
    const Layer* layer = findLayer(record.layer);
    if (layer == nullptr) {
        return makeError(ErrorCode::NotFound,
                         "parent layer " + record.layer.toString() + " does not exist");
    }
    const Transform& t = record.transform;
    if (!isFinite(t.position) || !std::isfinite(t.rotation) || !isFinite(t.scale) ||
        t.scale.x == 0.0F || t.scale.y == 0.0F) {
        return invalid("element transform must be finite with non-zero scale");
    }
    if (auto payload = std::visit([](const auto& p) { return checkPayload(p); }, record.payload);
        !payload) {
        return payload;
    }
    if (const auto* connector = std::get_if<Connector>(&record.payload)) {
        return checkAttachments(record.id, *connector, layer->page);
    }
    return {};
}

Result<void> Workspace::checkAttachments(core::ElementId connectorId, const Connector& connector,
                                         core::PageId page) const {
    for (const ConnectorEnd* end : {&connector.start, &connector.end}) {
        if (!end->attachedTo) {
            continue;
        }
        if (*end->attachedTo == connectorId) {
            return invalid("connector cannot attach to itself");
        }
        const Element* target = findElement(*end->attachedTo);
        if (target == nullptr) {
            return makeError(ErrorCode::NotFound,
                             "connector target " + end->attachedTo->toString() + " does not exist");
        }
        if (target->kind() == ElementKind::Connector) {
            return invalid("connector cannot attach to another connector");
        }
        if (pageOf(target->id) != page) {
            return invalid("connector target must be on the same page");
        }
    }
    return {};
}

bool Workspace::hasDependents(const NotebookInfo& record) const {
    return !sectionsOf(record.id).empty();
}
bool Workspace::hasDependents(const SectionInfo& record) const {
    return !pagesOf(record.id).empty();
}
bool Workspace::hasDependents(const PageInfo& record) const {
    return !layersOf(record.id).empty();
}
bool Workspace::hasDependents(const Layer& record) const {
    return !elementsOf(record.id).empty();
}
bool Workspace::hasDependents(const Element& record) const {
    return connectorAttachments_.contains(record.id);
}

// ---------------------------------------------------------------------------- indexes

std::vector<core::NotebookId>& Workspace::siblingsOf(const NotebookInfo& /*record*/) {
    return notebookOrder_;
}
std::vector<core::SectionId>& Workspace::siblingsOf(const SectionInfo& record) {
    return sectionsByNotebook_[record.notebook];
}
std::vector<core::PageId>& Workspace::siblingsOf(const PageInfo& record) {
    return pagesBySection_[record.section];
}
std::vector<core::LayerId>& Workspace::siblingsOf(const Layer& record) {
    return layersByPage_[record.page];
}
std::vector<core::ElementId>& Workspace::siblingsOf(const Element& record) {
    return elementsByLayer_[record.layer];
}

template <class Record>
void Workspace::insertSorted(const Record& record) {
    auto& siblings = siblingsOf(record);
    const auto& records = table(static_cast<const Record*>(nullptr));
    const auto& key = orderKey(record);
    const auto pos = std::lower_bound(
        siblings.begin(), siblings.end(), record.id, [&](const auto& sibling, const auto& id) {
            const auto& siblingKey = orderKey(records.at(sibling));
            return siblingKey != key ? siblingKey < key : sibling < id;
        });
    siblings.insert(pos, record.id);
}

template <class Record>
void Workspace::eraseFromSiblings(const Record& record) {
    auto& siblings = siblingsOf(record);
    siblings.erase(std::remove(siblings.begin(), siblings.end(), record.id), siblings.end());
    // Drop empty index entries so that indexes only describe existing parents.
    if constexpr (std::is_same_v<Record, SectionInfo>) {
        if (siblings.empty()) {
            sectionsByNotebook_.erase(record.notebook);
        }
    } else if constexpr (std::is_same_v<Record, PageInfo>) {
        if (siblings.empty()) {
            pagesBySection_.erase(record.section);
        }
    } else if constexpr (std::is_same_v<Record, Layer>) {
        if (siblings.empty()) {
            layersByPage_.erase(record.page);
        }
    } else if constexpr (std::is_same_v<Record, Element>) {
        if (siblings.empty()) {
            elementsByLayer_.erase(record.layer);
        }
    }
}

void Workspace::trackConnector(const Element& element, int delta) {
    const auto* connector = std::get_if<Connector>(&element.payload);
    if (connector == nullptr) {
        return;
    }
    for (const ConnectorEnd* end : {&connector->start, &connector->end}) {
        if (!end->attachedTo) {
            continue;
        }
        int& count = connectorAttachments_[*end->attachedTo];
        count += delta;
        if (count <= 0) {
            connectorAttachments_.erase(*end->attachedTo);
        }
    }
}

// ---------------------------------------------------------------------------- apply

template <class Record>
Result<void> Workspace::applyChange(const Change<Record>& change) {
    auto& records = table(static_cast<const Record*>(nullptr));
    constexpr std::string_view what = recordName<Record>();

    if (!change.before && !change.after) {
        return invalid("empty change");
    }
    if (change.before && change.after && change.before->id != change.after->id) {
        return invalid(std::string(what) + " id differs between before and after");
    }
    const auto id = change.after ? change.after->id : change.before->id;
    if (id.isNull()) {
        return invalid(std::string(what) + " id must not be null");
    }

    const auto it = records.find(id);

    if (change.isCreate()) {
        if (it != records.end()) {
            return makeError(ErrorCode::AlreadyExists,
                             std::string(what) + " " + id.toString() + " already exists");
        }
        if (auto check = checkRecord(*change.after); !check) {
            return check;
        }
        const auto [inserted, ok] = records.emplace(id, *change.after);
        assert(ok);
        insertSorted(inserted->second);
        if constexpr (std::is_same_v<Record, Element>) {
            trackConnector(inserted->second, +1);
        }
        return {};
    }

    if (it == records.end()) {
        return makeError(ErrorCode::NotFound,
                         std::string(what) + " " + id.toString() + " does not exist");
    }
    if (!(it->second == *change.before)) {
        return makeError(ErrorCode::Conflict, std::string(what) + " " + id.toString() +
                                                  " does not match the patch's expected state");
    }

    if (change.isRemove()) {
        if (hasDependents(it->second)) {
            return invalid(std::string(what) + " " + id.toString() +
                           (std::is_same_v<Record, Element>
                                ? " still has connectors attached"
                                : " still has children; remove them first"));
        }
        eraseFromSiblings(it->second);
        if constexpr (std::is_same_v<Record, Element>) {
            trackConnector(it->second, -1);
        }
        records.erase(it);
        return {};
    }

    // Update.
    if (auto check = checkRecord(*change.after); !check) {
        return check;
    }
    if constexpr (std::is_same_v<Record, Element>) {
        if (hasDependents(it->second) && pageOf(id) != [&] {
                const Layer* target = findLayer(change.after->layer);
                return target != nullptr ? std::optional(target->page) : std::nullopt;
            }()) {
            return invalid("element with attached connectors cannot move to another page");
        }
        trackConnector(it->second, -1);
    }
    eraseFromSiblings(it->second);
    it->second = *change.after;
    insertSorted(it->second);
    if constexpr (std::is_same_v<Record, Element>) {
        trackConnector(it->second, +1);
    }
    return {};
}

Result<void> Workspace::applyAny(const AnyChange& change) {
    return std::visit([this](const auto& c) { return applyChange(c); }, change);
}

Result<void> Workspace::checkPagesHaveLayers(const std::vector<core::PageId>& pages) const {
    for (const core::PageId page : pages) {
        if (findPage(page) != nullptr && layersOf(page).empty()) {
            return invalid("page " + page.toString() + " must have at least one layer");
        }
    }
    return {};
}

Result<void> Workspace::apply(const Patch& patch) {
    const auto& changes = patch.changes();
    std::vector<core::PageId> touchedPages;

    const auto rollback = [&](std::size_t appliedCount) {
        for (std::size_t i = appliedCount; i-- > 0;) {
            const auto undone =
                std::visit([this](const auto& c) { return applyChange(c.inverted()); }, changes[i]);
            // Inverting a change that was just applied cannot fail.
            assert(undone.has_value());
            (void)undone;
        }
    };

    for (std::size_t i = 0; i < changes.size(); ++i) {
        if (auto result = applyAny(changes[i]); !result) {
            rollback(i);
            return makeError(result.error().code,
                             describeChange(changes[i], i) + ": " + result.error().message);
        }
        if (const auto* layerChange = std::get_if<LayerChange>(&changes[i])) {
            if (layerChange->before) {
                touchedPages.push_back(layerChange->before->page);
            }
            if (layerChange->after) {
                touchedPages.push_back(layerChange->after->page);
            }
        } else if (const auto* pageChange = std::get_if<PageChange>(&changes[i])) {
            if (pageChange->after) {
                touchedPages.push_back(pageChange->after->id);
            }
        }
    }

    if (auto check = checkPagesHaveLayers(touchedPages); !check) {
        rollback(changes.size());
        return check;
    }
    return {};
}

// ---------------------------------------------------------------------------- validation

Result<void> Workspace::validate() const {
    // 1. Every record is keyed by its own id and passes its record checks
    //    (which include parent existence and connector attachment rules).
    const auto checkTable = [&](const auto& records) -> Result<void> {
        for (const auto& [id, record] : records) {
            if (id.isNull() || record.id != id) {
                return invalid("record keyed by the wrong id");
            }
            if (auto check = checkRecord(record); !check) {
                return makeError(check.error().code,
                                 std::string("invalid ") +
                                     std::string(recordName<std::decay_t<decltype(record)>>()) +
                                     " " + id.toString() + ": " + check.error().message);
            }
        }
        return Result<void>{};
    };
    for (auto check : {checkTable(notebooks_), checkTable(sections_), checkTable(pages_),
                       checkTable(layers_), checkTable(elements_)}) {
        if (!check) {
            return check;
        }
    }

    // 2. Indexes equal indexes rebuilt from the tables.
    const auto rebuild = [](const auto& records, auto parentOf) {
        using Id = typename std::decay_t<decltype(records)>::key_type;
        using ParentId = std::decay_t<decltype(parentOf(records.begin()->second))>;
        std::unordered_map<ParentId, std::vector<Id>> index;
        for (const auto& [id, record] : records) {
            index[parentOf(record)].push_back(id);
        }
        for (auto& [parent, ids] : index) {
            sortSiblings(ids, records);
        }
        return index;
    };
    std::vector<core::NotebookId> expectedNotebooks;
    expectedNotebooks.reserve(notebooks_.size());
    for (const auto& [id, record] : notebooks_) {
        expectedNotebooks.push_back(id);
    }
    sortSiblings(expectedNotebooks, notebooks_);
    if (expectedNotebooks != notebookOrder_) {
        return makeError(ErrorCode::Internal, "notebook order index is inconsistent");
    }
    if (rebuild(sections_, [](const SectionInfo& r) { return r.notebook; }) !=
            sectionsByNotebook_ ||
        rebuild(pages_, [](const PageInfo& r) { return r.section; }) != pagesBySection_ ||
        rebuild(layers_, [](const Layer& r) { return r.page; }) != layersByPage_ ||
        rebuild(elements_, [](const Element& r) { return r.layer; }) != elementsByLayer_) {
        return makeError(ErrorCode::Internal, "child index is inconsistent with the records");
    }

    // 3. Every page has at least one layer.
    for (const auto& [id, page] : pages_) {
        if (layersOf(id).empty()) {
            return invalid("page " + id.toString() + " has no layers");
        }
    }

    // 4. Connector attachment counts match the connectors.
    std::unordered_map<core::ElementId, int> expectedAttachments;
    for (const auto& [id, element] : elements_) {
        if (const auto* connector = std::get_if<Connector>(&element.payload)) {
            for (const ConnectorEnd* end : {&connector->start, &connector->end}) {
                if (end->attachedTo) {
                    ++expectedAttachments[*end->attachedTo];
                }
            }
        }
    }
    if (expectedAttachments != connectorAttachments_) {
        return makeError(ErrorCode::Internal, "connector attachment index is inconsistent");
    }
    return {};
}

bool operator==(const Workspace& lhs, const Workspace& rhs) {
    return lhs.info_ == rhs.info_ && lhs.notebooks_ == rhs.notebooks_ &&
           lhs.sections_ == rhs.sections_ && lhs.pages_ == rhs.pages_ &&
           lhs.layers_ == rhs.layers_ && lhs.elements_ == rhs.elements_;
}

} // namespace studyapp::document
