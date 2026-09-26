#include "NavigationPanel.hpp"

#include "WorkspaceTreeModel.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace studyapp::ui {

NavigationPanel::NavigationPanel(WorkspaceTreeModel& model, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("navigationPanel"));

    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("workspaceTitle"));
    title_->setTextInteractionFlags(Qt::NoTextInteraction);
    title_->setMinimumWidth(0);

    newMenu_ = new QMenu(this);
    newMenu_->setObjectName(QStringLiteral("menuNewItem"));
    newButton_ = new QToolButton(this);
    newButton_->setObjectName(QStringLiteral("newItemButton"));
    newButton_->setText(tr("New"));
    newButton_->setToolTip(tr("New page, section or notebook"));
    newButton_->setPopupMode(QToolButton::InstantPopup);
    newButton_->setMenu(newMenu_);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(10, 8, 6, 6);
    header->setSpacing(4);
    header->addWidget(title_, 1);
    header->addWidget(newButton_);

    tree_ = new QTreeView(this);
    tree_->setObjectName(QStringLiteral("workspaceTree"));
    tree_->setModel(&model);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed); // F2; also Rename in menus
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setIndentation(14);
    tree_->setExpandsOnDoubleClick(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(tree_, 1);
}

void NavigationPanel::setWorkspaceName(const QString& name) {
    title_->setText(name);
    title_->setToolTip(name);
}

} // namespace studyapp::ui
