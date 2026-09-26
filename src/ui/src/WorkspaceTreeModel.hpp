#pragma once

#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Workspace.hpp>

#include <QAbstractItemModel>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
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
    [[nodiscard]] static bool isStructural(const document::Patch& patch);
    [[nodiscard]] std::optional<Item> decode(const QMimeData* data) const;
    /// Where a drop of `item` at (row, parent) goes: parent item (nullopt: top level) and
    /// the index among the parent's children once moved; nullopt if not allowed.
    struct DropTarget {
        std::optional<Item> parent;
        std::size_t index;
    };
    [[nodiscard]] std::optional<DropTarget> dropTarget(const Item& item, int row,
                                                       const QModelIndex& parent) const;

    const document::Workspace* workspace_ = nullptr;
    std::unique_ptr<Node> root_;
    std::unordered_map<Item, Node*, ItemHash> nodes_;
    std::optional<core::PageId> activePage_;
    bool readOnly_ = false;
    RenameHandler rename_;
    MoveHandler move_;
};

} // namespace studyapp::ui
