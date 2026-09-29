#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QStackedWidget;
class QTreeWidget;
class QMenu;
class QToolButton;
class QTreeView;

namespace studyapp::ui {

class WorkspaceTreeModel;

/// The navigation area of the shell: the workspace name and the Notebook → Section →
/// Page tree (WorkspaceTreeModel). Presentation only — which page opens, and every edit,
/// is decided by the MainWindow through the application layer.
class NavigationPanel final : public QWidget {
    Q_OBJECT

public:
    NavigationPanel(WorkspaceTreeModel& model, QWidget* parent = nullptr);

    [[nodiscard]] QTreeView* tree() const noexcept { return tree_; }
    /// The search field above the tree and the list that replaces the tree while a search
    /// is shown (Phase 8; filled by the MainWindow).
    [[nodiscard]] QLineEdit* searchField() const noexcept { return searchField_; }
    [[nodiscard]] QTreeWidget* searchResults() const noexcept { return results_; }
    /// Shows the search results (true) or the tree.
    void showSearchResults(bool show);
    [[nodiscard]] bool isShowingSearchResults() const noexcept;
    void setWorkspaceName(const QString& name);
    /// The menu behind the header's "New" button (the owner fills it).
    [[nodiscard]] QMenu* newMenu() const noexcept { return newMenu_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLabel* title_ = nullptr;
    QToolButton* newButton_ = nullptr;
    QMenu* newMenu_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLineEdit* searchField_ = nullptr;
    QTreeWidget* results_ = nullptr;
    QStackedWidget* views_ = nullptr;
};

} // namespace studyapp::ui
