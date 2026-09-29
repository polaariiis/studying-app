#include "NavigationPanel.hpp"

#include "WorkspaceTreeModel.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
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
    tree_->installEventFilter(this);

    searchField_ = new QLineEdit(this);
    searchField_->setObjectName(QStringLiteral("searchField"));
    searchField_->setPlaceholderText(tr("Search"));
    searchField_->setClearButtonEnabled(true);
    searchField_->setToolTip(tr("Search page titles, text and tasks (Ctrl+F)"));
    auto* searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(8, 0, 6, 6);
    searchRow->addWidget(searchField_);

    results_ = new QTreeWidget(this);
    results_->setObjectName(QStringLiteral("searchResults"));
    results_->setHeaderHidden(true);
    results_->setRootIsDecorated(false);
    results_->setFrameShape(QFrame::NoFrame);
    results_->setWordWrap(true);
    results_->setUniformRowHeights(false);
    results_->setTextElideMode(Qt::ElideRight);
    results_->installEventFilter(this);

    views_ = new QStackedWidget(this);
    views_->addWidget(tree_);
    views_->addWidget(results_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addLayout(searchRow);
    layout->addWidget(views_, 1);
}

void NavigationPanel::showSearchResults(bool show) {
    views_->setCurrentWidget(show ? static_cast<QWidget*>(results_) : tree_);
}

bool NavigationPanel::isShowingSearchResults() const noexcept {
    return views_->currentWidget() == results_;
}

bool NavigationPanel::eventFilter(QObject* watched, QEvent* event) {
    // Plain typing in the tree is its keyboard search: the window's single-key shortcuts
    // (tools: P, M, V, E, H, Z) must not take those keys while the tree has focus.
    if ((watched == tree_ || watched == results_) && event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers chord =
            key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if (chord == Qt::NoModifier && !key->text().isEmpty() && key->text().front().isPrint()) {
            event->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NavigationPanel::setWorkspaceName(const QString& name) {
    title_->setText(name);
    title_->setToolTip(name);
}

} // namespace studyapp::ui
