#pragma once

#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Workspace.hpp>

#include <QAbstractItemModel>
#include <QIcon>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace studyapp::ui {

/// Item model of the navigation tree: Notebook → Section → Page, read directly from the
/// document (docs/ARCHITECTURE.md §3.5). It holds no copy of titles or records — only
/// the tree shape, as ids, to map rows to records — so there is no second hierarchy.
///
/// Updates are targeted: the owner reports every applied patch (onPatch). Renames and
/// other record updates emit dataChanged for their rows; structural changes (create,
/// delete, reorder, move to another parent) are mirrored with row insert/remove/move
/// signals, so views keep their expansion and selection. Canvas edits (element and layer
/// changes) cost nothing here.
///
/// Link marks (2.0-PLAN-01) are UI state, never stored: the owner sets the pages linked to
/// the selected task; the model marks those pages and the sections and notebooks that
/// contain them, and keeps the marks on the right rows after structural patches. Once the
/// markers are set, every unmarked row has a transparent decoration of the same size, so
/// titles never shift when marks appear or disappear.
///
/// The model never changes the document: renaming (inline editing) and drag-and-drop call
/// the handlers set by the owner, which run commands through the session.
class WorkspaceTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    using Item = application::HierarchyItem;
    /// Rename request from inline editing; true if the rename was applied.
    using RenameHandler = std::function<bool(const Item& item, const QString& title)>;
    /// Drag-and-drop request: move `item` into `parent` (nullopt: top level) at `index`
    /// (position among the parent's children once moved). true if the move was applied.
    using MoveHandler =
        std::function<bool(const Item& item, const std::optional<Item>& parent, std::size_t index)>;

    enum Roles {
        KindRole = Qt::UserRole + 1, ///< "notebook", "section" or "page"
        ActiveRole,                  ///< true for the active page
        LinkedRole,                  ///< true for a page in the linked set
        ContainsLinkedRole,          ///< true for a section or notebook containing one
    };

    static constexpr const char* kMimeType = "application/x-studyboard-hierarchy-item";

    explicit WorkspaceTreeModel(QObject* parent = nullptr);
    ~WorkspaceTreeModel() override;
    WorkspaceTreeModel(const WorkspaceTreeModel&) = delete;
    WorkspaceTreeModel& operator=(const WorkspaceTreeModel&) = delete;
    WorkspaceTreeModel(WorkspaceTreeModel&&) = delete;
    WorkspaceTreeModel& operator=(WorkspaceTreeModel&&) = delete;

    /// Shows `workspace` (nullptr: nothing). A model reset: only for opening/closing.
    void setWorkspace(const document::Workspace* workspace);
    /// Must be called after every patch applied to the shown workspace.
    void onPatch(const document::Patch& patch);

    void setReadOnly(bool readOnly);
    void setActivePage(std::optional<core::PageId> page);
    /// Marks `pages` as linked (empty: no marks). Pages not in the workspace are ignored
    /// until they exist (e.g. restored by undo).
    void setLinkedPages(const std::vector<core::PageId>& pages);
    /// The decorations of a linked page and of a section or notebook containing one (null
    /// icons: no decoration on any row).
    void setLinkMarkers(const QIcon& page, const QIcon& container);
    void setRenameHandler(RenameHandler handler) { rename_ = std::move(handler); }
    void setMoveHandler(MoveHandler handler) { move_ = std::move(handler); }

    [[nodiscard]] QModelIndex indexOf(const Item& item) const;
    [[nodiscard]] std::optional<Item> itemAt(const QModelIndex& index) const;

    // QAbstractItemModel
    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                                       int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;

private:
    struct Node;
    struct ItemHash {
        std::size_t operator()(const Item& item) const noexcept;
    };

    [[nodiscard]] Node* nodeOf(const QModelIndex& index) const;
    [[nodiscard]] QModelIndex indexOfNode(const Node* node) const;
    [[nodiscard]] std::vector<Item> childrenInDocument(const Node& node) const;
    [[nodiscard]] bool isChildOf(const Item& item, const Node& parent) const;
    void sync(Node& node);
    void unregister(Node& node);
    /// Derives the marked rows from the linked pages and emits dataChanged for the rows
    /// whose marks changed (nothing if none did).
    void updateLinkMarks();
    /// Emits dataChanged for the decoration of every row below `node`.
    void decorationsChanged(const Node& node);
    [[nodiscard]] static bool isStructural(const document::Patch& patch);
    [[nodiscard]] std::optional<Item> decode(const QMimeData* data) const;
    /// Where a drop of `item` at (row, parent) goes: parent item (nullopt: top level) and
    /// the index among the parent's children once moved; nullopt if not allowed.
    struct DropTarget {
        std::optional<Item> parent;
        std::size_t index = 0;
    };
    [[nodiscard]] std::optional<DropTarget> dropTarget(const Item& item, int row,
                                                       const QModelIndex& parent) const;

    const document::Workspace* workspace_ = nullptr;
    std::unique_ptr<Node> root_;
    std::unordered_map<Item, Node*, ItemHash> nodes_;
    std::optional<core::PageId> activePage_;
    std::unordered_set<core::PageId> linkedPages_;
    std::unordered_set<Item, ItemHash> marked_; ///< linked pages and their ancestors
    QIcon linkedMarker_;
    QIcon containerMarker_;
    QIcon blankMarker_; ///< transparent; keeps the titles of unmarked rows in place
    bool readOnly_ = false;
    RenameHandler rename_;
    MoveHandler move_;
};

} // namespace studyapp::ui
