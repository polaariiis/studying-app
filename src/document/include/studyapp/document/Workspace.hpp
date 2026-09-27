#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Records.hpp>
#include <studyapp/study/Records.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace studyapp::document {

/// Root of the in-memory document model and sole owner of all its records:
///
///   Workspace → Notebook → Section → Page → Layer → Element
///
/// and, since Phase 7, the study records: courses, projects, tasks (with one level of
/// subtasks) and tags (docs/DATA_MODEL.md §5, docs/ARCHITECTURE.md D42).
///
/// Records are stored in id-keyed tables and reference their parent by id; ordered child
/// lists are maintained as indexes (docs/DATA_MODEL.md §3–4). All mutation goes through
/// apply(), which applies a Patch atomically: either every change succeeds, or the
/// workspace is left exactly as it was.
///
/// Queries return pointers/spans into the workspace. They stay valid until the next
/// successful apply() and must not be stored beyond that; store ids instead.
///
/// Invariants (checked by apply() and by validate()):
///   * ids are non-null and unique per record type;
///   * every record's parent exists (notebooks belong to the workspace);
///   * a record cannot be removed while it still has children;
///   * every page has at least one layer (checked at the end of each patch);
///   * notebook, section and layer names are not blank; numeric fields are finite and in
///     range; stroke points are present;
///   * connector ends attach only to existing, non-connector elements on the same page,
///     and an element cannot be removed while a connector is attached to it;
///   * siblings are ordered by (order key, id), so ordering is total and deterministic;
///     tags by (name ignoring ASCII case, id);
///   * study records reference only existing records: a project its course; a task its
///     course, project, parent (a top-level task; subtasks have no subtasks), linked
///     pages and tags; a page its tags. A referenced record cannot be removed (commands
///     clear the references in the same patch). Tag names are unique ignoring ASCII case.
///
/// Not thread-safe. Single-threaded use on the GUI thread (docs/ARCHITECTURE.md §10).
class Workspace {
public:
    explicit Workspace(WorkspaceInfo info);

    [[nodiscard]] const WorkspaceInfo& info() const noexcept { return info_; }

    // ---- hierarchy queries (children are ordered) -------------------------------------
    [[nodiscard]] std::span<const core::NotebookId> notebooks() const noexcept;
    [[nodiscard]] std::span<const core::SectionId> sectionsOf(core::NotebookId notebook) const;
    [[nodiscard]] std::span<const core::PageId> pagesOf(core::SectionId section) const;
    [[nodiscard]] std::span<const core::LayerId> layersOf(core::PageId page) const;
    [[nodiscard]] std::span<const core::ElementId> elementsOf(core::LayerId layer) const;
    /// Connectors with an end attached to `element` (a connector attached by both ends is
    /// listed twice); empty for none. O(1) lookup: moves and deletes touch only these.
    [[nodiscard]] std::span<const core::ElementId>
    connectorsAttachedTo(core::ElementId element) const;

    // ---- study queries (Phase 7) --------------------------------------------------------
    // Ordered lists: courses, projects, top-level tasks, subtasks of a task, tags (by name).
    [[nodiscard]] std::span<const core::CourseId> courses() const noexcept { return courseOrder_; }
    [[nodiscard]] std::span<const core::ProjectId> projects() const noexcept {
        return projectOrder_;
    }
    [[nodiscard]] std::span<const core::TaskId> topLevelTasks() const;
    [[nodiscard]] std::span<const core::TaskId> subtasksOf(core::TaskId task) const;
    [[nodiscard]] std::span<const core::TagId> tags() const noexcept { return tagOrder_; }
    // Reverse references, sorted by id (deterministic for commands and views). O(k log k)
    // for k references; kept in hash sets so that bulk edits (deleting a course with many
    // tasks) update them in O(1) per reference.
    /// Tasks linking to `page` (its backlinks).
    [[nodiscard]] std::vector<core::TaskId> tasksLinkedTo(core::PageId page) const;
    [[nodiscard]] std::vector<core::TaskId> tasksOfProject(core::ProjectId project) const;
    [[nodiscard]] std::vector<core::TaskId> tasksOfCourse(core::CourseId course) const;
    [[nodiscard]] std::vector<core::ProjectId> projectsOfCourse(core::CourseId course) const;
    [[nodiscard]] std::vector<core::PageId> pagesTagged(core::TagId tag) const;
    [[nodiscard]] std::vector<core::TaskId> tasksTagged(core::TagId tag) const;

    // ---- lookup (nullptr if the id is unknown) ----------------------------------------
    [[nodiscard]] const NotebookInfo* findNotebook(core::NotebookId id) const;
    [[nodiscard]] const SectionInfo* findSection(core::SectionId id) const;
    [[nodiscard]] const PageInfo* findPage(core::PageId id) const;
    [[nodiscard]] const Layer* findLayer(core::LayerId id) const;
    [[nodiscard]] const Element* findElement(core::ElementId id) const;
    [[nodiscard]] const study::Course* findCourse(core::CourseId id) const;
    [[nodiscard]] const study::Project* findProject(core::ProjectId id) const;
    [[nodiscard]] const study::Task* findTask(core::TaskId id) const;
    [[nodiscard]] const study::Tag* findTag(core::TagId id) const;
    /// The tag named `name` ignoring ASCII case, or nullptr.
    [[nodiscard]] const study::Tag* findTagByName(std::string_view name) const;

    /// Page that contains the element, if the element exists.
    [[nodiscard]] std::optional<core::PageId> pageOf(core::ElementId element) const;

    [[nodiscard]] std::size_t notebookCount() const noexcept { return notebooks_.size(); }
    [[nodiscard]] std::size_t sectionCount() const noexcept { return sections_.size(); }
    [[nodiscard]] std::size_t pageCount() const noexcept { return pages_.size(); }
    [[nodiscard]] std::size_t layerCount() const noexcept { return layers_.size(); }
    [[nodiscard]] std::size_t elementCount() const noexcept { return elements_.size(); }
    [[nodiscard]] std::size_t courseCount() const noexcept { return courses_.size(); }
    [[nodiscard]] std::size_t projectCount() const noexcept { return projects_.size(); }
    [[nodiscard]] std::size_t taskCount() const noexcept { return tasks_.size(); }
    [[nodiscard]] std::size_t tagCount() const noexcept { return tagsById_.size(); }

    // ---- mutation -----------------------------------------------------------------------
    /// Applies all changes of `patch` in order, atomically.
    ///
    /// Each change's `before` must equal the current record (Conflict otherwise), creates
    /// must use unused ids (AlreadyExists), updates/removes must target existing records
    /// (NotFound), and the result must satisfy all invariants (InvalidArgument). On error
    /// the workspace is unchanged and the error names the offending change.
    [[nodiscard]] core::Result<void> apply(const Patch& patch);

    /// Full consistency check of all invariants and indexes. O(size); intended for tests
    /// and debug builds.
    [[nodiscard]] core::Result<void> validate() const;

    /// Logical equality: same metadata and same records (indexes are derived).
    friend bool operator==(const Workspace& lhs, const Workspace& rhs);

private:
    template <class Record>
    core::Result<void> applyChange(const Change<Record>& change);
    core::Result<void> applyAny(const AnyChange& change);
    core::Result<void> checkPagesHaveLayers(const std::vector<core::PageId>& pages) const;

    // Per-record-type hooks used by applyChange (overloaded on the record type).
    core::Result<void> checkRecord(const NotebookInfo& record) const;
    core::Result<void> checkRecord(const SectionInfo& record) const;
    core::Result<void> checkRecord(const PageInfo& record) const;
    core::Result<void> checkRecord(const Layer& record) const;
    core::Result<void> checkRecord(const Element& record) const;
    core::Result<void> checkRecord(const study::Course& record) const;
    core::Result<void> checkRecord(const study::Project& record) const;
    core::Result<void> checkRecord(const study::Task& record) const;
    core::Result<void> checkRecord(const study::Tag& record) const;
    core::Result<void> checkTagIds(const std::vector<core::TagId>& tags) const;
    core::Result<void> checkAttachments(core::ElementId connectorId, const Connector& connector,
                                        core::PageId page) const;

    [[nodiscard]] bool hasDependents(const NotebookInfo& record) const;
    [[nodiscard]] bool hasDependents(const SectionInfo& record) const;
    [[nodiscard]] bool hasDependents(const PageInfo& record) const;
    [[nodiscard]] bool hasDependents(const Layer& record) const;
    [[nodiscard]] bool hasDependents(const Element& record) const;
    [[nodiscard]] bool hasDependents(const study::Course& record) const;
    [[nodiscard]] bool hasDependents(const study::Project& record) const;
    [[nodiscard]] bool hasDependents(const study::Task& record) const;
    [[nodiscard]] bool hasDependents(const study::Tag& record) const;

    auto& table(const NotebookInfo*) { return notebooks_; }
    auto& table(const SectionInfo*) { return sections_; }
    auto& table(const PageInfo*) { return pages_; }
    auto& table(const Layer*) { return layers_; }
    auto& table(const Element*) { return elements_; }
    auto& table(const study::Course*) { return courses_; }
    auto& table(const study::Project*) { return projects_; }
    auto& table(const study::Task*) { return tasks_; }
    auto& table(const study::Tag*) { return tagsById_; }

    std::vector<core::NotebookId>& siblingsOf(const NotebookInfo& record);
    std::vector<core::SectionId>& siblingsOf(const SectionInfo& record);
    std::vector<core::PageId>& siblingsOf(const PageInfo& record);
    std::vector<core::LayerId>& siblingsOf(const Layer& record);
    std::vector<core::ElementId>& siblingsOf(const Element& record);
    std::vector<core::CourseId>& siblingsOf(const study::Course& record);
    std::vector<core::ProjectId>& siblingsOf(const study::Project& record);
    std::vector<core::TaskId>& siblingsOf(const study::Task& record);
    std::vector<core::TagId>& siblingsOf(const study::Tag& record);

    template <class Record>
    void insertSorted(const Record& record);
    template <class Record>
    void eraseFromSiblings(const Record& record);

    // Derived reference indexes, updated for every added (+1) or removed (-1) record.
    void track(const NotebookInfo& /*record*/, int /*delta*/) noexcept {}
    void track(const SectionInfo& /*record*/, int /*delta*/) noexcept {}
    void track(const PageInfo& record, int delta);
    void track(const Layer& /*record*/, int /*delta*/) noexcept {}
    void track(const Element& record, int delta);
    void track(const study::Course& /*record*/, int /*delta*/) noexcept {}
    void track(const study::Project& record, int delta);
    void track(const study::Task& record, int delta);
    void track(const study::Tag& record, int delta);

    WorkspaceInfo info_;

    // Record tables: the single owner of every record.
    std::unordered_map<core::NotebookId, NotebookInfo> notebooks_;
    std::unordered_map<core::SectionId, SectionInfo> sections_;
    std::unordered_map<core::PageId, PageInfo> pages_;
    std::unordered_map<core::LayerId, Layer> layers_;
    std::unordered_map<core::ElementId, Element> elements_;
    std::unordered_map<core::CourseId, study::Course> courses_;
    std::unordered_map<core::ProjectId, study::Project> projects_;
    std::unordered_map<core::TaskId, study::Task> tasks_;
    std::unordered_map<core::TagId, study::Tag> tagsById_;

    // Derived indexes: ordered children per parent, connectors per attached element.
    std::vector<core::NotebookId> notebookOrder_;
    std::unordered_map<core::NotebookId, std::vector<core::SectionId>> sectionsByNotebook_;
    std::unordered_map<core::SectionId, std::vector<core::PageId>> pagesBySection_;
    std::unordered_map<core::PageId, std::vector<core::LayerId>> layersByPage_;
    std::unordered_map<core::LayerId, std::vector<core::ElementId>> elementsByLayer_;
    std::unordered_map<core::ElementId, std::vector<core::ElementId>> connectorsByAttachment_;
    std::vector<core::CourseId> courseOrder_;
    std::vector<core::ProjectId> projectOrder_;
    /// Tasks per parent; top-level tasks are listed under the null id.
    std::unordered_map<core::TaskId, std::vector<core::TaskId>> tasksByParent_;
    std::vector<core::TagId> tagOrder_;
    std::unordered_map<std::string, core::TagId> tagsByName_; ///< folded name -> tag
    std::unordered_map<core::PageId, std::unordered_set<core::TaskId>> tasksByPage_;
    std::unordered_map<core::ProjectId, std::unordered_set<core::TaskId>> tasksByProject_;
    std::unordered_map<core::CourseId, std::unordered_set<core::TaskId>> tasksByCourse_;
    std::unordered_map<core::CourseId, std::unordered_set<core::ProjectId>> projectsByCourse_;
    std::unordered_map<core::TagId, std::unordered_set<core::PageId>> pagesByTag_;
    std::unordered_map<core::TagId, std::unordered_set<core::TaskId>> tasksByTag_;
};

bool operator==(const Workspace& lhs, const Workspace& rhs);

} // namespace studyapp::document
