#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Records.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace studyapp::document {

/// Root of the in-memory document model and sole owner of all its records:
///
///   Workspace → Notebook → Section → Page → Layer → Element
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
///   * siblings are ordered by (order key, id), so ordering is total and deterministic.
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

    // ---- lookup (nullptr if the id is unknown) ----------------------------------------
    [[nodiscard]] const NotebookInfo* findNotebook(core::NotebookId id) const;
    [[nodiscard]] const SectionInfo* findSection(core::SectionId id) const;
    [[nodiscard]] const PageInfo* findPage(core::PageId id) const;
    [[nodiscard]] const Layer* findLayer(core::LayerId id) const;
    [[nodiscard]] const Element* findElement(core::ElementId id) const;

    /// Page that contains the element, if the element exists.
    [[nodiscard]] std::optional<core::PageId> pageOf(core::ElementId element) const;

    [[nodiscard]] std::size_t notebookCount() const noexcept { return notebooks_.size(); }
    [[nodiscard]] std::size_t sectionCount() const noexcept { return sections_.size(); }
    [[nodiscard]] std::size_t pageCount() const noexcept { return pages_.size(); }
    [[nodiscard]] std::size_t layerCount() const noexcept { return layers_.size(); }
    [[nodiscard]] std::size_t elementCount() const noexcept { return elements_.size(); }

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
    core::Result<void> checkAttachments(core::ElementId connectorId, const Connector& connector,
                                        core::PageId page) const;

    [[nodiscard]] bool hasDependents(const NotebookInfo& record) const;
    [[nodiscard]] bool hasDependents(const SectionInfo& record) const;
    [[nodiscard]] bool hasDependents(const PageInfo& record) const;
    [[nodiscard]] bool hasDependents(const Layer& record) const;
    [[nodiscard]] bool hasDependents(const Element& record) const;

    auto& table(const NotebookInfo*) { return notebooks_; }
    auto& table(const SectionInfo*) { return sections_; }
    auto& table(const PageInfo*) { return pages_; }
    auto& table(const Layer*) { return layers_; }
    auto& table(const Element*) { return elements_; }

    std::vector<core::NotebookId>& siblingsOf(const NotebookInfo& record);
    std::vector<core::SectionId>& siblingsOf(const SectionInfo& record);
    std::vector<core::PageId>& siblingsOf(const PageInfo& record);
    std::vector<core::LayerId>& siblingsOf(const Layer& record);
    std::vector<core::ElementId>& siblingsOf(const Element& record);

    template <class Record>
    void insertSorted(const Record& record);
    template <class Record>
    void eraseFromSiblings(const Record& record);

    void trackConnector(const Element& element, int delta);

    WorkspaceInfo info_;

    // Record tables: the single owner of every record.
    std::unordered_map<core::NotebookId, NotebookInfo> notebooks_;
    std::unordered_map<core::SectionId, SectionInfo> sections_;
    std::unordered_map<core::PageId, PageInfo> pages_;
    std::unordered_map<core::LayerId, Layer> layers_;
    std::unordered_map<core::ElementId, Element> elements_;

    // Derived indexes: ordered children per parent, connector attachment counts.
    std::vector<core::NotebookId> notebookOrder_;
    std::unordered_map<core::NotebookId, std::vector<core::SectionId>> sectionsByNotebook_;
    std::unordered_map<core::SectionId, std::vector<core::PageId>> pagesBySection_;
    std::unordered_map<core::PageId, std::vector<core::LayerId>> layersByPage_;
    std::unordered_map<core::LayerId, std::vector<core::ElementId>> elementsByLayer_;
    std::unordered_map<core::ElementId, int> connectorAttachments_;
};

bool operator==(const Workspace& lhs, const Workspace& rhs);

} // namespace studyapp::document
