#include "WorkspaceTreeModel.hpp"

#include <QFont>
#include <QMimeData>

#include <algorithm>
#include <cassert>
#include <string>
#include <type_traits>

namespace studyapp::ui {

using application::WorkspaceStructure;

namespace {

QString toQString(const std::string& text) {
    return QString::fromStdString(text);
}

const char* kindOf(const application::HierarchyItem& item) {
    switch (item.index()) {
    case 0:
        return "notebook";
    case 1:
        return "section";
    default:
        return "page";
    }
}

} // namespace

struct WorkspaceTreeModel::Node {
    std::optional<Item> item; ///< nullopt for the (invisible) root
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    [[nodiscard]] int row() const {
        if (parent == nullptr) {
            return 0;
        }
        const auto it = std::find_if(parent->children.begin(), parent->children.end(),
                                     [this](const auto& child) { return child.get() == this; });
        return static_cast<int>(it - parent->children.begin());
    }
};

std::size_t WorkspaceTreeModel::ItemHash::operator()(const Item& item) const noexcept {
    const std::size_t value = std::visit(
        [](const auto& id) { return std::hash<std::decay_t<decltype(id)>>{}(id); }, item);
    return value ^ (item.index() * 0x9E3779B97F4A7C15ULL);
}

WorkspaceTreeModel::WorkspaceTreeModel(QObject* parent)
    : QAbstractItemModel(parent), root_(std::make_unique<Node>()) {}

WorkspaceTreeModel::~WorkspaceTreeModel() = default;

// ---------------------------------------------------------------------------- content

void WorkspaceTreeModel::setWorkspace(const document::Workspace* workspace) {
    beginResetModel();
    workspace_ = workspace;
    root_ = std::make_unique<Node>();
    nodes_.clear();
    activePage_.reset();
    endResetModel();
    if (workspace_ != nullptr) {
        sync(*root_); // fills the tree with insert signals (cheap on an empty model)
    }
}

void WorkspaceTreeModel::setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
}

void WorkspaceTreeModel::setActivePage(std::optional<core::PageId> page) {
    if (page == activePage_) {
        return;
    }
    const std::optional<core::PageId> previous = activePage_;
    activePage_ = page;
    for (const auto& changed : {previous, page}) {
        if (changed) {
            if (const QModelIndex index = indexOf(*changed); index.isValid()) {
                Q_EMIT dataChanged(index, index, {Qt::FontRole, ActiveRole});
            }
        }
    }
}

bool WorkspaceTreeModel::isStructural(const document::Patch& patch) {
    for (const document::AnyChange& change : patch.changes()) {
        const bool structural = std::visit(
            [](const auto& c) {
                using Record = std::decay_t<decltype(c.after ? *c.after : *c.before)>;
                if constexpr (std::is_same_v<Record, document::NotebookInfo>) {
                    return !c.isUpdate() || c.before->order != c.after->order;
                } else if constexpr (std::is_same_v<Record, document::SectionInfo>) {
                    return !c.isUpdate() || c.before->order != c.after->order ||
                           c.before->notebook != c.after->notebook;
                } else if constexpr (std::is_same_v<Record, document::PageInfo>) {
                    return !c.isUpdate() || c.before->order != c.after->order ||
                           c.before->section != c.after->section;
                } else {
                    return false; // layers and elements are not in the tree
                }
            },
            change);
        if (structural) {
            return true;
        }
    }
    return false;
}

void WorkspaceTreeModel::onPatch(const document::Patch& patch) {
    if (workspace_ == nullptr) {
        return;
    }
    if (isStructural(patch)) {
        sync(*root_);
    }
    // Titles (and anything else shown) of updated records.
    for (const document::AnyChange& change : patch.changes()) {
        std::visit(
            [this](const auto& c) {
                using Record = std::decay_t<decltype(c.after ? *c.after : *c.before)>;
                if constexpr (std::is_same_v<Record, document::NotebookInfo> ||
                              std::is_same_v<Record, document::SectionInfo> ||
                              std::is_same_v<Record, document::PageInfo>) {
                    if (c.isUpdate()) {
                        if (const QModelIndex index = indexOf(Item{c.after->id}); index.isValid()) {
                            Q_EMIT dataChanged(index, index);
                        }
                    }
                }
            },
            change);
    }
}

std::vector<WorkspaceTreeModel::Item>
WorkspaceTreeModel::childrenInDocument(const Node& node) const {
    std::vector<Item> items;
    if (!node.item) {
        for (const core::NotebookId id : workspace_->notebooks()) {
            items.emplace_back(id);
        }
    } else if (const auto* notebook = std::get_if<core::NotebookId>(&*node.item)) {
        for (const core::SectionId id : workspace_->sectionsOf(*notebook)) {
            items.emplace_back(id);
        }
    } else if (const auto* section = std::get_if<core::SectionId>(&*node.item)) {
        for (const core::PageId id : workspace_->pagesOf(*section)) {
            items.emplace_back(id);
        }
    }
    return items;
}

bool WorkspaceTreeModel::isChildOf(const Item& item, const Node& parent) const {
    if (const auto* section = std::get_if<core::SectionId>(&item)) {
        const document::SectionInfo* info = workspace_->findSection(*section);
        return info != nullptr && parent.item && *parent.item == Item{info->notebook};
    }
    if (const auto* page = std::get_if<core::PageId>(&item)) {
        const document::PageInfo* info = workspace_->findPage(*page);
        return info != nullptr && parent.item && *parent.item == Item{info->section};
    }
    return !parent.item; // notebooks belong to the top level
}

void WorkspaceTreeModel::unregister(Node& node) {
    for (auto& child : node.children) {
        unregister(*child);
    }
    if (node.item) {
        nodes_.erase(*node.item);
    }
}

void WorkspaceTreeModel::sync(Node& node) {
    const QModelIndex parentIndex = indexOfNode(&node);
    const std::vector<Item> desired = childrenInDocument(node);

    // 1. Rows whose record no longer exists. Records that moved to another parent stay
    //    until that parent claims them (a row move keeps their subtree and view state).
    for (int i = static_cast<int>(node.children.size()) - 1; i >= 0; --i) {
        const auto index = static_cast<std::size_t>(i);
        if (!WorkspaceStructure::exists(*workspace_, *node.children[index]->item)) {
            beginRemoveRows(parentIndex, i, i);
            unregister(*node.children[index]);
            node.children.erase(node.children.begin() + i);
            endRemoveRows();
        }
    }

    // 2. The document's children in order: already placed, moved here (from this or
    //    another parent) or new. Rows that are leaving for another parent are stepped
    //    over, so they cause no reordering here; their new parent moves them.
    const auto belongsHere = [&](const Node& child) {
        return isChildOf(*child.item, node);
    };
    std::size_t row = 0;
    for (const Item& wanted : desired) {
        while (row < node.children.size() && !belongsHere(*node.children[row])) {
            ++row;
        }
        if (row < node.children.size() && *node.children[row]->item == wanted) {
            ++row;
            continue;
        }
        const auto destination = static_cast<int>(row);
        if (const auto found = nodes_.find(wanted); found != nodes_.end()) {
            Node* moving = found->second;
            Node* from = moving->parent;
            const int fromRow = moving->row();
            const bool moved =
                beginMoveRows(indexOfNode(from), fromRow, fromRow, parentIndex, destination);
            assert(moved);
            (void)moved;
            std::unique_ptr<Node> owned =
                std::move(from->children[static_cast<std::size_t>(fromRow)]);
            from->children.erase(from->children.begin() + fromRow);
            owned->parent = &node;
            node.children.insert(node.children.begin() + destination, std::move(owned));
            endMoveRows();
        } else {
            auto created = std::make_unique<Node>();
            created->item = wanted;
            created->parent = &node;
            beginInsertRows(parentIndex, destination, destination);
            nodes_.emplace(wanted, created.get());
            node.children.insert(node.children.begin() + destination, std::move(created));
            endInsertRows();
        }
        ++row;
    }

    // 3. Grandchildren (new nodes are filled here, so their pages can also be claimed
    //    from elsewhere).
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        if (!std::holds_alternative<core::PageId>(*node.children[i]->item)) {
            sync(*node.children[i]);
        }
    }
}

QModelIndex WorkspaceTreeModel::indexOf(const Item& item) const {
    const auto found = nodes_.find(item);
    return found != nodes_.end() ? indexOfNode(found->second) : QModelIndex();
}

std::optional<WorkspaceTreeModel::Item> WorkspaceTreeModel::itemAt(const QModelIndex& index) const {
    const Node* node = index.isValid() ? nodeOf(index) : nullptr;
    return node != nullptr ? node->item : std::nullopt;
}

WorkspaceTreeModel::Node* WorkspaceTreeModel::nodeOf(const QModelIndex& index) const {
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : root_.get();
}

QModelIndex WorkspaceTreeModel::indexOfNode(const Node* node) const {
    if (node == nullptr || node == root_.get()) {
        return {};
    }
    return createIndex(node->row(), 0, const_cast<Node*>(node));
}

// ---------------------------------------------------------------------------- model API

QModelIndex WorkspaceTreeModel::index(int row, int column, const QModelIndex& parent) const {
    const Node* node = nodeOf(parent);
    if (column != 0 || row < 0 || node == nullptr ||
        static_cast<std::size_t>(row) >= node->children.size()) {
        return {};
    }
    return createIndex(row, 0, node->children[static_cast<std::size_t>(row)].get());
}

QModelIndex WorkspaceTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    return indexOfNode(nodeOf(child)->parent);
}

int WorkspaceTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    const Node* node = nodeOf(parent);
    return node != nullptr ? static_cast<int>(node->children.size()) : 0;
}

int WorkspaceTreeModel::columnCount(const QModelIndex& /*parent*/) const {
    return 1;
}

QVariant WorkspaceTreeModel::data(const QModelIndex& index, int role) const {
    const std::optional<Item> item = itemAt(index);
    if (!item || workspace_ == nullptr) {
        return {};
    }
    const bool active = activePage_ && *item == Item{*activePage_};
    switch (role) {
    case Qt::DisplayRole:
        return toQString(WorkspaceStructure::displayTitle(*workspace_, *item));
    case Qt::EditRole:
        return std::visit(
            [this](const auto& id) -> QVariant {
                using Id = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, core::NotebookId>) {
                    const auto* notebook = workspace_->findNotebook(id);
                    return notebook != nullptr ? toQString(notebook->title) : QVariant();
                } else if constexpr (std::is_same_v<Id, core::SectionId>) {
                    const auto* section = workspace_->findSection(id);
                    return section != nullptr ? toQString(section->title) : QVariant();
                } else {
                    const auto* page = workspace_->findPage(id);
                    return page != nullptr ? toQString(page->title) : QVariant();
                }
            },
            *item);
    case Qt::FontRole: {
        if (!active && !std::holds_alternative<core::NotebookId>(*item)) {
            return {};
        }
        QFont font;
        font.setWeight(active ? QFont::Bold : QFont::DemiBold);
        return font;
    }
    case Qt::AccessibleDescriptionRole:
        return active ? tr("%1, open").arg(QString::fromLatin1(kindOf(*item)))
                      : QString::fromLatin1(kindOf(*item));
    case KindRole:
        return QString::fromLatin1(kindOf(*item));
    case ActiveRole:
        return active;
    default:
        return {};
    }
}

Qt::ItemFlags WorkspaceTreeModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return readOnly_ ? Qt::NoItemFlags : Qt::ItemIsDropEnabled; // top level: notebooks
    }
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (readOnly_) {
        return flags;
    }
    flags |= Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
    const std::optional<Item> item = itemAt(index);
    if (item && !std::holds_alternative<core::PageId>(*item)) {
        flags |= Qt::ItemIsDropEnabled; // notebooks take sections, sections take pages
    }
    return flags;
}

bool WorkspaceTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const std::optional<Item> item = itemAt(index);
    if (role != Qt::EditRole || !item || readOnly_ || !rename_) {
        return false;
    }
    // The rename goes through the session; the resulting patch emits dataChanged.
    return rename_(*item, value.toString());
}

// ---------------------------------------------------------------------------- drag & drop

Qt::DropActions WorkspaceTreeModel::supportedDropActions() const {
    return Qt::MoveAction;
}

Qt::DropActions WorkspaceTreeModel::supportedDragActions() const {
    return Qt::MoveAction;
}

QStringList WorkspaceTreeModel::mimeTypes() const {
    return {QString::fromLatin1(kMimeType)};
}

QMimeData* WorkspaceTreeModel::mimeData(const QModelIndexList& indexes) const {
    if (indexes.isEmpty()) {
        return nullptr;
    }
    const std::optional<Item> item = itemAt(indexes.front());
    if (!item) {
        return nullptr;
    }
    const std::string id = std::visit([](const auto& value) { return value.toString(); }, *item);
    auto* data = new QMimeData;
    data->setData(QString::fromLatin1(kMimeType),
                  QByteArray(kindOf(*item)) + ':' + QByteArray::fromStdString(id));
    return data;
}

std::optional<WorkspaceTreeModel::Item> WorkspaceTreeModel::decode(const QMimeData* data) const {
    if (data == nullptr || !data->hasFormat(QString::fromLatin1(kMimeType))) {
        return std::nullopt;
    }
    const QByteArray payload = data->data(QString::fromLatin1(kMimeType));
    const auto colon = payload.indexOf(':');
    if (colon < 0) {
        return std::nullopt;
    }
    const QByteArray kind = payload.left(colon);
    const std::string id = payload.mid(colon + 1).toStdString();
    std::optional<Item> item;
    if (kind == "notebook") {
        if (auto parsed = core::NotebookId::parse(id)) {
            item = *parsed;
        }
    } else if (kind == "section") {
        if (auto parsed = core::SectionId::parse(id)) {
            item = *parsed;
        }
    } else if (kind == "page") {
        if (auto parsed = core::PageId::parse(id)) {
            item = *parsed;
        }
    }
    if (!item || nodes_.find(*item) == nodes_.end()) {
        return std::nullopt; // not from this workspace
    }
    return item;
}

std::optional<WorkspaceTreeModel::DropTarget>
WorkspaceTreeModel::dropTarget(const Item& item, int row, const QModelIndex& parent) const {
    const std::optional<Item> parentItem = itemAt(parent);
    // Notebooks go to the top level, sections into notebooks, pages into sections.
    const bool fits = (std::holds_alternative<core::NotebookId>(item) && !parentItem) ||
                      (std::holds_alternative<core::SectionId>(item) && parentItem &&
                       std::holds_alternative<core::NotebookId>(*parentItem)) ||
                      (std::holds_alternative<core::PageId>(item) && parentItem &&
                       std::holds_alternative<core::SectionId>(*parentItem));
    if (!fits) {
        return std::nullopt;
    }
    const Node* parentNode = nodeOf(parent);
    const Node* moving = nodes_.at(item);
    // `row` is the insertion row among the current children (the dragged row included);
    // -1 means "onto the parent", i.e. at the end.
    std::size_t index = row < 0 ? parentNode->children.size() : static_cast<std::size_t>(row);
    if (moving->parent == parentNode && static_cast<std::size_t>(moving->row()) < index) {
        --index; // the dragged row is removed before it is inserted again
    }
    return DropTarget{.parent = parentItem, .index = index};
}

bool WorkspaceTreeModel::canDropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                                         int /*column*/, const QModelIndex& parent) const {
    if (readOnly_ || action != Qt::MoveAction) {
        return false;
    }
    const std::optional<Item> item = decode(data);
    return item && dropTarget(*item, row, parent).has_value();
}

bool WorkspaceTreeModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                                      int column, const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent) || !move_) {
        return false;
    }
    const Item item = *decode(data);
    const DropTarget target = *dropTarget(item, row, parent);
    // The move runs as a command; the resulting patch moves the row (onPatch). The view
    // removes nothing afterwards: this model has no removeRows (every removal is a patch).
    return move_(item, target.parent, target.index);
}

} // namespace studyapp::ui
