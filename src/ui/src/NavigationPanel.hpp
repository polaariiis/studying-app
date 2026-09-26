#pragma once

#include <QWidget>

class QLabel;
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
    void setWorkspaceName(const QString& name);
    /// The menu behind the header's "New" button (the owner fills it).
    [[nodiscard]] QMenu* newMenu() const noexcept { return newMenu_; }

private:
    QLabel* title_ = nullptr;
    QToolButton* newButton_ = nullptr;
    QMenu* newMenu_ = nullptr;
    QTreeView* tree_ = nullptr;
};

} // namespace studyapp::ui
