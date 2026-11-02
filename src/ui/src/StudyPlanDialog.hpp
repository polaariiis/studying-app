#pragma once

#include <studyapp/application/Planner.hpp>
#include <studyapp/application/StudyPlan.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/study/Records.hpp>

#include <QDialog>

#include <cstddef>
#include <functional>
#include <optional>

class QDateEdit;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

namespace studyapp::ui {

class ShellDialogs;

/// "New Study Plan" (2.0-PLAN-03; docs/VERSION_2_0.md §5): a goal, a start date, days,
/// minutes per day and the notebooks and sections to cover; Build spreads the pages over
/// the days (application::draftByTreeOrder) and shows the preview, which the user edits
/// (rule 6) and resolves (overflow, rule 4; changed and deleted pages, rule 7); Accept
/// saves it as one undoable edit through application::Planner, Cancel saves nothing.
///
/// Modeless, so the user can keep working in the window; the owner reports every applied
/// patch (onPatch) and the preview is checked again against the workspace as it is. The
/// owner deletes the dialog before the workspace it was opened for closes: it keeps a
/// reference to that workspace's Planner. Holds ids only (the preview), never pointers
/// into the workspace. Accept checks the preview against the workspace right before
/// saving, and saves only if that check allows it.
class StudyPlanDialog final : public QDialog {
    Q_OBJECT

public:
    StudyPlanDialog(application::Planner& planner, ShellDialogs& dialogs, study::CalendarDate today,
                    bool readOnly, QWidget* parent);
    ~StudyPlanDialog() override;

    StudyPlanDialog(const StudyPlanDialog&) = delete;
    StudyPlanDialog& operator=(const StudyPlanDialog&) = delete;
    StudyPlanDialog(StudyPlanDialog&&) = delete;
    StudyPlanDialog& operator=(StudyPlanDialog&&) = delete;

    /// Called with the new project after Accept saved the plan, before the dialog closes.
    void setSavedHandler(std::function<void(core::ProjectId)> handler);
    /// Called for every applied patch; refreshes (deferred, once per event-loop turn) when
    /// it changes notebooks, sections or pages.
    void onPatch(const document::Patch& patch);

    // For tests.
    [[nodiscard]] const application::PlanPreview* preview() const noexcept;
    [[nodiscard]] bool isStale() const noexcept { return stale_; }
    [[nodiscard]] bool isEdited() const noexcept { return edited_; }
    /// Runs a pending refresh now.
    void refreshNow();

private:
    void buildLayout();
    void connectControls();
    /// The scope tree from the workspace, keeping the checked notebooks and sections.
    void fillScope();
    [[nodiscard]] application::PlanRequest request() const;
    void build();
    void save();
    void markStale();
    void updateStartRange();
    /// Applies a preview edit's result: refused edits leave their message. `select`: the
    /// block to show in the block editor afterwards (indices change when blocks move).
    void apply(core::Result<void> result, std::optional<std::size_t> select = std::nullopt);
    [[nodiscard]] std::optional<std::size_t> selectedBlock() const;
    [[nodiscard]] QString pageTitle(core::PageId page) const;
    /// Shows the preview (or why there is none), the warnings and why Accept is disabled.
    void render();
    void renderExcluded(const application::PlanReview& review);
    void renderReferences(const application::PlanReview& review);
    /// The block editor, for the selected block.
    void loadBlock();
    /// The action buttons, for the selected rows.
    void updateControls();

    application::Planner* planner_;
    ShellDialogs* dialogs_;
    bool readOnly_;
    std::optional<application::PlanPreview> preview_;
    std::optional<application::PlanBuild> refused_; ///< a build without a draft
    QString buildError_;                            ///< a request out of range
    QString notice_;                                ///< why the last edit was refused
    bool stale_ = false;                            ///< days, minutes or scope changed since Build
    bool edited_ = false;                           ///< the preview was edited since Build
    bool refreshPending_ = false;
    bool rendering_ = false;
    std::optional<std::size_t> select_; ///< the block render() selects, once
    std::function<void(core::ProjectId)> saved_;

    QLineEdit* goal_ = nullptr;
    QDateEdit* start_ = nullptr;
    QSpinBox* days_ = nullptr;
    QSpinBox* minutes_ = nullptr;
    QTreeWidget* scope_ = nullptr;
    QPushButton* build_ = nullptr;
    QLabel* summary_ = nullptr;
    QTreeWidget* table_ = nullptr; ///< the preview
    // The selected block.
    QGroupBox* blockBox_ = nullptr;
    QLineEdit* blockTitle_ = nullptr;
    QSpinBox* blockMinutes_ = nullptr;
    QSpinBox* blockDay_ = nullptr;
    QPushButton* moveBlock_ = nullptr;
    QPushButton* removeBlock_ = nullptr;
    QListWidget* blockPages_ = nullptr;
    QPushButton* excludePage_ = nullptr;
    // Overflow (rule 4).
    QGroupBox* overflowBox_ = nullptr;
    QPushButton* extend_ = nullptr;
    QPushButton* keepUnscheduled_ = nullptr;
    QPushButton* removeOverflow_ = nullptr;
    // "Excluded pages" (rule 6).
    QGroupBox* excludedBox_ = nullptr;
    QTreeWidget* excluded_ = nullptr;
    QPushButton* restore_ = nullptr;
    QPushButton* addPage_ = nullptr;
    // Deleted, moved and new pages (rule 7).
    QGroupBox* referencesBox_ = nullptr;
    QTreeWidget* references_ = nullptr;
    QPushButton* excludeReference_ = nullptr;
    QPushButton* keepReference_ = nullptr;

    QLabel* messages_ = nullptr;
    QPushButton* accept_ = nullptr;
    QPushButton* cancel_ = nullptr;
};

} // namespace studyapp::ui
