#pragma once

#include <studyapp/application/Planner.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/study/Planning.hpp>

#include <QWidget>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QTabBar;
class QTimeEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace studyapp::ui {

class ShellDialogs;

/// The study planner beside the canvas (Phase 7; docs/ARCHITECTURE.md §3.5): Today (the
/// agenda), Tasks (by course or project), Week (a simple week calendar) and Page (the open
/// page's tags and the tasks that link to it), plus an editor for the selected task.
///
/// Presentation only: it reads the Workspace and edits exclusively through
/// application::Planner, so every change is one undoable, persisted command. It rebuilds
/// its lists only after patches that change study records or pages (never for canvas
/// edits), once per event-loop turn, and not while hidden.
class PlannerPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PlannerPanel(ShellDialogs& dialogs, QWidget* parent = nullptr);
    ~PlannerPanel() override;

    /// The open workspace's planner and clock (nullptr: no workspace).
    void setPlanner(application::Planner* planner, const core::Clock* clock);
    /// Local time for the agenda and the week (default: UTC).
    void setTimeZone(const study::TimeZone* zone);
    void setActivePage(std::optional<core::PageId> page);
    void setReadOnly(bool readOnly);
    /// Called for every applied patch; refreshes (deferred) when it concerns the planner.
    void onPatch(const document::Patch& patch);
    /// The page to open when a linked page is activated.
    void setOpenPageHandler(std::function<void(core::PageId)> handler);
    void setFailureHandler(std::function<void(const QString&, const QString&)> handler);
    /// Saves what is being typed in a planner field (before the workspace closes).
    void finishEditing();

    // For tests and the shell.
    enum class View {
        Today = 0,
        Tasks = 1,
        Week = 2,
        Page = 3
    };
    void showView(View view);
    [[nodiscard]] std::optional<core::TaskId> selectedTask() const noexcept { return selected_; }
    void selectTask(std::optional<core::TaskId> task);
    /// Runs a pending refresh now.
    void refreshNow();
    [[nodiscard]] int refreshCount() const noexcept { return refreshes_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void ensureBuilt();
    void applyAccessibleNames();
    void buildLayout();
    void buildEditor();
    void scheduleRefresh();
    void refreshAgenda();
    void refreshTasks();
    void refreshScopes();
    void refreshWeek();
    void refreshPage();
    void loadEditor();
    void updateEnabled();

    /// Fills `tree` with task rows; returns the item of each task by id text.
    QTreeWidgetItem* addTaskItem(QTreeWidget* tree, QTreeWidgetItem* parent,
                                 const study::Task& task, const QString& detail);
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onCurrentItemChanged(QTreeWidgetItem* item);
    void restoreSelection(QTreeWidget* tree);

    void createTaskFrom(QLineEdit* edit);
    void commitPageTags();
    void commit(core::Result<void> result, const QString& what);
    /// Applies `change` to a copy of the selected task and saves it (one command).
    void editSelected(const std::function<void(study::Task&)>& change);
    [[nodiscard]] const study::Task* selected() const;
    [[nodiscard]] std::optional<core::CourseId> scopeCourse() const;
    [[nodiscard]] std::optional<core::ProjectId> scopeProject() const;
    void newCourse();
    void newProject();
    void renameScope();
    void deleteScope();
    void setScopeProjectDone(bool done);
    [[nodiscard]] study::CalendarDate today() const;
    [[nodiscard]] QString dueText(const study::Task& task) const;

    ShellDialogs* dialogs_;
    application::Planner* planner_ = nullptr;
    const core::Clock* clock_ = nullptr;
    study::FixedOffsetZone utc_{std::chrono::minutes{0}};
    const study::TimeZone* zone_ = &utc_;
    std::optional<core::PageId> activePage_;
    std::optional<core::PageId> pageTagsFor_; ///< the page whose tags the field shows
    bool readOnly_ = false;
    std::optional<core::TaskId> selected_;
    bool built_ = false;
    bool refreshPending_ = false;
    bool dirty_ = true;
    bool updating_ = false;
    int refreshes_ = 0;
    study::CalendarDate weekStart_{};
    std::function<void(core::PageId)> openPage_;
    std::function<void(const QString&, const QString&)> failed_;

    QTabBar* tabs_ = nullptr;
    QStackedWidget* views_ = nullptr;
    // Today
    QTreeWidget* agenda_ = nullptr;
    QLineEdit* todayAdd_ = nullptr;
    // Tasks
    QComboBox* scope_ = nullptr;
    QToolButton* scopeNew_ = nullptr;
    QToolButton* scopeMore_ = nullptr;
    QLabel* progress_ = nullptr;
    QTreeWidget* tasks_ = nullptr;
    QLineEdit* taskAdd_ = nullptr;
    // Week
    QLabel* weekLabel_ = nullptr;
    QTreeWidget* week_ = nullptr;
    // Page
    QLabel* pageTitle_ = nullptr;
    QLineEdit* pageTags_ = nullptr;
    QTreeWidget* backlinks_ = nullptr;
    QLineEdit* pageAdd_ = nullptr;
    // Task editor
    QScrollArea* editorArea_ = nullptr;
    QWidget* editor_ = nullptr;
    QLineEdit* title_ = nullptr;
    QCheckBox* done_ = nullptr;
    QComboBox* priority_ = nullptr;
    QCheckBox* hasDue_ = nullptr;
    QDateEdit* dueDate_ = nullptr;
    QCheckBox* hasDueTime_ = nullptr;
    QTimeEdit* dueTime_ = nullptr;
    QCheckBox* scheduled_ = nullptr;
    QDateTimeEdit* blockStart_ = nullptr;
    QSpinBox* blockMinutes_ = nullptr;
    QComboBox* course_ = nullptr;
    QComboBox* project_ = nullptr;
    QLineEdit* tags_ = nullptr;
    QListWidget* links_ = nullptr;
    QPushButton* linkPage_ = nullptr;
    QPushButton* unlinkPage_ = nullptr;
    QPlainTextEdit* notes_ = nullptr;
    QPushButton* addSubtask_ = nullptr;
    QPushButton* deleteTask_ = nullptr;
};

} // namespace studyapp::ui
