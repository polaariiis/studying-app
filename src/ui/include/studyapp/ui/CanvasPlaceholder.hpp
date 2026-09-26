#pragma once

#include <QStringList>
#include <QWidget>

class QPushButton;
class QVBoxLayout;

namespace studyapp::ui {

class ThemeManager;

/// The central area while no workspace is open: the canvas look from the design tokens
/// (neutral paper-like background with a subtle dot grid), a short explanation, and the
/// ways to start — new workspace, open workspace, recently used workspaces. It only emits
/// requests; the MainWindow opens or creates the workspace.
class CanvasPlaceholder final : public QWidget {
    Q_OBJECT

public:
    explicit CanvasPlaceholder(const ThemeManager& themes, QWidget* parent = nullptr);

    /// Shows the start buttons (off when the window cannot open workspaces).
    void setActionsAvailable(bool available);
    /// Recently used workspace directories, most recent first (native separators).
    void setRecentWorkspaces(const QStringList& paths);

Q_SIGNALS:
    void newWorkspaceRequested();
    void openWorkspaceRequested();
    void openRecentRequested(const QString& path);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    const ThemeManager* themes_;
    QWidget* actions_ = nullptr;
    QWidget* recentBox_ = nullptr;
    QVBoxLayout* recentList_ = nullptr;
    bool actionsAvailable_ = true;
};

} // namespace studyapp::ui
