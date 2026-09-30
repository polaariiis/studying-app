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
    } else if constexpr (std::is_same_v<Record, Element>) {
        return "element";
    } else if constexpr (std::is_same_v<Record, study::Course>) {
        return "course";
    } else if constexpr (std::is_same_v<Record, study::Project>) {
        return "project";
    } else if constexpr (std::is_same_v<Record, study::Task>) {
        return "task";
    } else {
        static_assert(std::is_same_v<Record, study::Tag>);
        return "tag";
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
const core::FractionalIndex& orderKey(const study::Course& r) noexcept {
    return r.order;
}
const core::FractionalIndex& orderKey(const study::Project& r) noexcept {
    return r.order;
}
const core::FractionalIndex& orderKey(const study::Task& r) noexcept {
    return r.order;
}

/// Sibling order: (order key, id); tags by (name ignoring ASCII case, id).
template <class Record>
bool siblingLess(const Record& a, const Record& b) {
    const auto& ka = orderKey(a);
    const auto& kb = orderKey(b);
    return ka != kb ? ka < kb : a.id < b.id;
}
bool siblingLess(const study::Tag& a, const study::Tag& b) {
    const std::string fa = study::foldTagName(a.name);
    const std::string fb = study::foldTagName(b.name);
    return fa != fb ? fa < fb : a.id < b.id;
}

/// Adds `id` to the list of `key` in a reverse-reference index.
template <class Map, class Id>
void addReference(Map& index, const typename Map::key_type& key, const Id& id) {
    index[key].push_back(id);
}

/// Removes one `id` from the references of `key`; drops the entry when it becomes empty.
/// Vectors (connectors of an element: few) by linear search; sets in O(1).
template <class Map, class Id>
void removeReference(Map& index, const typename Map::key_type& key, const Id& id) {
    const auto it = index.find(key);
    if (it == index.end()) {
        return;
    }
    auto& ids = it->second;
    if constexpr (requires { ids.find(id); }) {
        ids.erase(id);
    } else if (const auto at = std::find(ids.begin(), ids.end(), id); at != ids.end()) {
        ids.erase(at);
    }
    if (ids.empty()) {
        index.erase(it);
    }
}

template <class Map, class Id>
void reference(Map& index, const typename Map::key_type& key, const Id& id, int delta) {
    if (delta > 0) {
        if constexpr (requires { index[key].insert(id); }) {
            index[key].insert(id);
        } else {
            addReference(index, key, id);
        }
    } else {
        removeReference(index, key, id);
    }
}

/// The references of `key` as a sorted vector (empty if none).
template <class Id, class Map>
std::vector<Id> sortedReferences(const Map& index, const typename Map::key_type& key) {
    const auto it = index.find(key);
    if (it == index.end()) {
        return {};
    }
    std::vector<Id> ids(it->second.begin(), it->second.end());
    std::sort(ids.begin(), ids.end());
    return ids;
}

/// Whether an update keeps the record at its place among its siblings (same parent, same
/// order key): its entry in the sibling index then stays as it is.
bool sameSlot(const NotebookInfo& a, const NotebookInfo& b) noexcept {
    return a.order == b.order;
}
bool sameSlot(const SectionInfo& a, const SectionInfo& b) noexcept {
    return a.notebook == b.notebook && a.order == b.order;
}
bool sameSlot(const PageInfo& a, const PageInfo& b) noexcept {
    return a.section == b.section && a.order == b.order;
}
bool sameSlot(const Layer& a, const Layer& b) noexcept {
    return a.page == b.page && a.order == b.order;
}
bool sameSlot(const Element& a, const Element& b) noexcept {
    return a.layer == b.layer && a.z == b.z;
}
bool sameSlot(const study::Course& a, const study::Course& b) noexcept {
    return a.order == b.order;
}
bool sameSlot(const study::Project& a, const study::Project& b) noexcept {
    return a.order == b.order;
}
bool sameSlot(const study::Task& a, const study::Task& b) noexcept {
    return a.parent == b.parent && a.order == b.order;
}
bool sameSlot(const study::Tag& a, const study::Tag& b) {
    return study::foldTagName(a.name) == study::foldTagName(b.name);
}

template <class Id>
bool isSortedUnique(const std::vector<Id>& ids) {
    return std::adjacent_find(ids.begin(), ids.end(),
                              [](const Id& a, const Id& b) { return !(a < b); }) == ids.end();
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
    if (!isNonNegative(text.size)) {
        return invalid("text box size must be finite and non-negative");
    }
    return isValidTextFontSize(text.fontSize)
               ? Result<void>{}
               : invalid("text font size must be a whole number in [6, 144]");
}

Result<void> checkPayload(const Shape& shape) {
    if (!isKnownShapeKind(shape.kind)) {
        return invalid("unknown shape kind");
    }
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

/// Sorts ids in sibling order using the records in `table`.
template <class Id, class Table>
void sortSiblings(std::vector<Id>& ids, const Table& table) {
    std::sort(ids.begin(), ids.end(),
              [&](const Id& a, const Id& b) { return siblingLess(table.at(a), table.at(b)); });
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

const study::Course* Workspace::findCourse(core::CourseId id) const {
    return findIn(courses_, id);
}
const study::Project* Workspace::findProject(core::ProjectId id) const {
    return findIn(projects_, id);
}
const study::Task* Workspace::findTask(core::TaskId id) const {
    return findIn(tasks_, id);
}
const study::Tag* Workspace::findTag(core::TagId id) const {
    return findIn(tagsById_, id);
}
const study::Tag* Workspace::findTagByName(std::string_view name) const {
    const auto it = tagsByName_.find(study::foldTagName(name));
    return it == tagsByName_.end() ? nullptr : findTag(it->second);
}

std::span<const core::TaskId> Workspace::topLevelTasks() const {
    return childrenIn<core::TaskId>(tasksByParent_, core::TaskId{});
}
std::span<const core::TaskId> Workspace::subtasksOf(core::TaskId task) const {
    return task.isNull() ? std::span<const core::TaskId>{}
                         : childrenIn<core::TaskId>(tasksByParent_, task);
}
std::vector<core::TaskId> Workspace::tasksLinkedTo(core::PageId page) const {
    return sortedReferences<core::TaskId>(tasksByPage_, page);
}
std::vector<core::TaskId> Workspace::tasksOfProject(core::ProjectId project) const {
    return sortedReferences<core::TaskId>(tasksByProject_, project);
}
std::vector<core::TaskId> Workspace::tasksOfCourse(core::CourseId course) const {
    return sortedReferences<core::TaskId>(tasksByCourse_, course);
}
std::vector<core::ProjectId> Workspace::projectsOfCourse(core::CourseId course) const {
    return sortedReferences<core::ProjectId>(projectsByCourse_, course);
}
std::vector<core::PageId> Workspace::pagesTagged(core::TagId tag) const {
    return sortedReferences<core::PageId>(pagesByTag_, tag);
}
std::vector<core::TaskId> Workspace::tasksTagged(core::TagId tag) const {
    return sortedReferences<core::TaskId>(tasksByTag_, tag);
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
    if (!isSortedUnique(record.tags)) {
        return invalid("page tags must be sorted and unique");
    }
    if (record.document) {
        if (record.document->asset.isNull() || record.document->index < 0) {
            return invalid("a document page needs an asset and a page index >= 0");
        }
        if (record.extent != PageExtent::Bounded) {
            return invalid("a document page must be bounded (the size of the document page)");
        }
    }
    return checkTagIds(record.tags);
}

Result<void> Workspace::checkTagIds(const std::vector<core::TagId>& tags) const {
    for (const core::TagId tag : tags) {
        if (findTag(tag) == nullptr) {
            return makeError(ErrorCode::NotFound, "tag " + tag.toString() + " does not exist");
        }
    }
    return {};
}

Result<void> Workspace::checkRecord(const study::Course& record) const {
    return study::checkValues(record);
}

Result<void> Workspace::checkRecord(const study::Project& record) const {
    if (auto values = study::checkValues(record); !values) {
        return values;
    }
    if (record.course && findCourse(*record.course) == nullptr) {
        return makeError(ErrorCode::NotFound,
                         "course " + record.course->toString() + " does not exist");
    }
    return {};
}

Result<void> Workspace::checkRecord(const study::Task& record) const {
    if (auto values = study::checkValues(record); !values) {
        return values;
    }
    if (record.course && findCourse(*record.course) == nullptr) {
        return makeError(ErrorCode::NotFound,
                         "course " + record.course->toString() + " does not exist");
    }
    if (record.project && findProject(*record.project) == nullptr) {
        return makeError(ErrorCode::NotFound,
                         "project " + record.project->toString() + " does not exist");
    }
    if (record.parent) {
        const study::Task* parent = findTask(*record.parent);
        if (parent == nullptr) {
            return makeError(ErrorCode::NotFound,
                             "parent task " + record.parent->toString() + " does not exist");
        }
        if (parent->parent) {
            return invalid("subtasks cannot have subtasks");
        }
        if (!subtasksOf(record.id).empty()) {
            return invalid("a task with subtasks cannot become a subtask");
        }
    }
    for (const core::PageId page : record.linkedPages) {
        if (findPage(page) == nullptr) {
            return makeError(ErrorCode::NotFound, "page " + page.toString() + " does not exist");
        }
    }
    return checkTagIds(record.tags);
}

Result<void> Workspace::checkRecord(const study::Tag& record) const {
    if (auto values = study::checkValues(record); !values) {
        return values;
    }
    if (const auto it = tagsByName_.find(study::foldTagName(record.name));
        it != tagsByName_.end() && it->second != record.id) {
        return makeError(ErrorCode::AlreadyExists, "a tag named \"" + record.name + "\" exists");
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
    return !layersOf(record.id).empty() || tasksByPage_.contains(record.id);
}
bool Workspace::hasDependents(const Layer& record) const {
    return !elementsOf(record.id).empty();
}
std::span<const core::ElementId> Workspace::connectorsAttachedTo(core::ElementId element) const {
    const auto it = connectorsByAttachment_.find(element);
    return it == connectorsByAttachment_.end() ? std::span<const core::ElementId>{}
                                               : std::span<const core::ElementId>{it->second};
}

bool Workspace::hasDependents(const Element& record) const {
    return connectorsByAttachment_.contains(record.id);
}
bool Workspace::hasDependents(const study::Course& record) const {
    return projectsByCourse_.contains(record.id) || tasksByCourse_.contains(record.id);
}
bool Workspace::hasDependents(const study::Project& record) const {
    return tasksByProject_.contains(record.id);
}
bool Workspace::hasDependents(const study::Task& record) const {
    return tasksByParent_.contains(record.id);
}
bool Workspace::hasDependents(const study::Tag& record) const {
    return pagesByTag_.contains(record.id) || tasksByTag_.contains(record.id);
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
std::vector<core::CourseId>& Workspace::siblingsOf(const study::Course& /*record*/) {
    return courseOrder_;
}
std::vector<core::ProjectId>& Workspace::siblingsOf(const study::Project& /*record*/) {
    return projectOrder_;
}
std::vector<core::TaskId>& Workspace::siblingsOf(const study::Task& record) {
    return tasksByParent_[record.parent.value_or(core::TaskId{})];
}
std::vector<core::TagId>& Workspace::siblingsOf(const study::Tag& /*record*/) {
    return tagOrder_;
}

template <class Record>
void Workspace::insertSorted(const Record& record) {
    auto& siblings = siblingsOf(record);
    const auto& records = table(static_cast<const Record*>(nullptr));
    const auto pos = std::lower_bound(siblings.begin(), siblings.end(), record.id,
                                      [&](const auto& sibling, const auto& /*id*/) {
                                          return siblingLess(records.at(sibling), record);
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
    } else if constexpr (std::is_same_v<Record, study::Task>) {
        if (siblings.empty()) {
            tasksByParent_.erase(record.parent.value_or(core::TaskId{}));
        }
    }
}

void Workspace::track(const Element& element, int delta) {
    const auto* connector = std::get_if<Connector>(&element.payload);
    if (connector == nullptr) {
        return;
    }
    for (const ConnectorEnd* end : {&connector->start, &connector->end}) {
        if (end->attachedTo) {
            reference(connectorsByAttachment_, *end->attachedTo, element.id, delta);
        }
    }
}

void Workspace::track(const PageInfo& page, int delta) {
    for (const core::TagId tag : page.tags) {
        reference(pagesByTag_, tag, page.id, delta);
    }
}

void Workspace::track(const study::Project& project, int delta) {
    if (project.course) {
        reference(projectsByCourse_, *project.course, project.id, delta);
    }
}

void Workspace::track(const study::Task& task, int delta) {
    if (task.course) {
        reference(tasksByCourse_, *task.course, task.id, delta);
    }
    if (task.project) {
        reference(tasksByProject_, *task.project, task.id, delta);
    }
    for (const core::PageId page : task.linkedPages) {
        reference(tasksByPage_, page, task.id, delta);
    }
    for (const core::TagId tag : task.tags) {
        reference(tasksByTag_, tag, task.id, delta);
    }
}

void Workspace::track(const study::Tag& tag, int delta) {
    if (delta > 0) {
        tagsByName_.emplace(study::foldTagName(tag.name), tag.id);
    } else {
        tagsByName_.erase(study::foldTagName(tag.name));
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
        track(inserted->second, +1);
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
            constexpr bool referenced = std::is_same_v<Record, study::Course> ||
                                        std::is_same_v<Record, study::Project> ||
                                        std::is_same_v<Record, study::Tag>;
            return invalid(std::string(what) + " " + id.toString() +
                           (std::is_same_v<Record, Element> ? " still has connectors attached"
                            : referenced ? " is still referenced; clear the references first"
                                         : " still has children or links; remove them first"));
        }
        eraseFromSiblings(it->second);
        track(it->second, -1);
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
    }
    // An update in place (the common case: a moved stroke, an edited task) keeps its sibling
    // index entry: O(1) instead of O(siblings), so a patch updating k siblings is O(k).
    const bool inPlace = sameSlot(it->second, *change.after);
    track(it->second, -1);
    if (!inPlace) {
        eraseFromSiblings(it->second);
    }
    it->second = *change.after;
    if (!inPlace) {
        insertSorted(it->second);
    }
    track(it->second, +1);
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
                       checkTable(layers_), checkTable(elements_), checkTable(courses_),
                       checkTable(projects_), checkTable(tasks_), checkTable(tagsById_)}) {
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
        rebuild(elements_, [](const Element& r) { return r.layer; }) != elementsByLayer_ ||
        rebuild(tasks_, [](const study::Task& r) { return r.parent.value_or(core::TaskId{}); }) !=
            tasksByParent_) {
        return makeError(ErrorCode::Internal, "child index is inconsistent with the records");
    }
    const auto ordered = [](const auto& records) {
        using Id = typename std::decay_t<decltype(records)>::key_type;
        std::vector<Id> ids;
        ids.reserve(records.size());
        for (const auto& [id, record] : records) {
            ids.push_back(id);
        }
        sortSiblings(ids, records);
        return ids;
    };
    if (ordered(courses_) != courseOrder_ || ordered(projects_) != projectOrder_ ||
        ordered(tagsById_) != tagOrder_) {
        return makeError(ErrorCode::Internal, "study order index is inconsistent");
    }

    // 3. Every page has at least one layer.
    for (const auto& [id, page] : pages_) {
        if (layersOf(id).empty()) {
            return invalid("page " + id.toString() + " has no layers");
        }
    }

    // 4. The connector attachment index matches the connectors.
    std::unordered_map<core::ElementId, std::vector<core::ElementId>> expectedAttachments;
    for (const auto& [id, element] : elements_) {
        if (const auto* connector = std::get_if<Connector>(&element.payload)) {
            for (const ConnectorEnd* end : {&connector->start, &connector->end}) {
                if (end->attachedTo) {
                    expectedAttachments[*end->attachedTo].push_back(id);
                }
            }
        }
    }
    if (expectedAttachments.size() != connectorsByAttachment_.size()) {
        return makeError(ErrorCode::Internal, "connector attachment index is inconsistent");
    }
    for (auto& [element, expected] : expectedAttachments) {
        const auto it = connectorsByAttachment_.find(element);
        if (it == connectorsByAttachment_.end()) {
            return makeError(ErrorCode::Internal, "connector attachment index is inconsistent");
        }
        std::vector<core::ElementId> actual = it->second;
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        if (actual != expected) {
            return makeError(ErrorCode::Internal, "connector attachment index is inconsistent");
        }
    }

    // 5. The study reference indexes match the records (compared as sorted lists).
    Workspace rebuilt(info_);
    for (const auto& [id, page] : pages_) {
        rebuilt.track(page, +1);
    }
    for (const auto& [id, project] : projects_) {
        rebuilt.track(project, +1);
    }
    for (const auto& [id, task] : tasks_) {
        rebuilt.track(task, +1);
    }
    for (const auto& [id, tag] : tagsById_) {
        rebuilt.track(tag, +1);
    }
    if (rebuilt.tasksByPage_ != tasksByPage_ || rebuilt.tasksByProject_ != tasksByProject_ ||
        rebuilt.tasksByCourse_ != tasksByCourse_ ||
        rebuilt.projectsByCourse_ != projectsByCourse_ || rebuilt.pagesByTag_ != pagesByTag_ ||
        rebuilt.tasksByTag_ != tasksByTag_ || rebuilt.tagsByName_ != tagsByName_) {
        return makeError(ErrorCode::Internal, "study reference index is inconsistent");
    }
    return {};
}

bool operator==(const Workspace& lhs, const Workspace& rhs) {
    return lhs.info_ == rhs.info_ && lhs.notebooks_ == rhs.notebooks_ &&
           lhs.sections_ == rhs.sections_ && lhs.pages_ == rhs.pages_ &&
           lhs.layers_ == rhs.layers_ && lhs.elements_ == rhs.elements_ &&
           lhs.courses_ == rhs.courses_ && lhs.projects_ == rhs.projects_ &&
           lhs.tasks_ == rhs.tasks_ && lhs.tagsById_ == rhs.tagsById_;
}

} // namespace studyapp::document
