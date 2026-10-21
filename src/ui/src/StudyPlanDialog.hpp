#pragma once

#include <studyapp/application/Planner.hpp>
#include <studyapp/application/StudyPlan.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/study/Records.hpp>

#include <QDialog>

#include <functional>
#include <optional>

class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace studyapp::ui {

class ShellDialogs;

/// "New Study Plan" (2.0-PLAN-03; docs/VERSION_2_0.md §5): a goal, a start date, days,
/// minutes per day and the notebooks and sections to cover; Build spreads the pages over
/// the days (application::draftByTreeOrder) and shows the preview; Accept saves it as one
/// undoable edit through application::Planner, Cancel saves nothing.
///
/// Modeless, so the user can keep working in the window. The owner deletes it before the
/// workspace it was opened for closes: it keeps a reference to that workspace's Planner.
/// Holds ids only (the preview), never pointers into the workspace. Accept checks the
/// preview against the workspace as it is right before saving, and saves only if that
/// check allows it.
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

    // For tests.
    [[nodiscard]] const application::PlanPreview* preview() const noexcept;
    [[nodiscard]] bool isStale() const noexcept { return stale_; }

private:
    void buildLayout();
    void fillScope();
    [[nodiscard]] application::PlanRequest request() const;
    void build();
    void save();
    void markStale();
    void updateStartRange();
    /// Shows the preview (or why there is none), the warnings and why Accept is disabled.
    void render();

    application::Planner* planner_;
    ShellDialogs* dialogs_;
    bool readOnly_;
    std::optional<application::PlanPreview> preview_;
    std::optional<application::PlanBuild> refused_; ///< a build without a draft
    QString buildError_;                            ///< a request out of range
    bool stale_ = false;                            ///< days, minutes or scope changed since Build
    std::function<void(core::ProjectId)> saved_;

    QLineEdit* goal_ = nullptr;
    QDateEdit* start_ = nullptr;
    QSpinBox* days_ = nullptr;
    QSpinBox* minutes_ = nullptr;
    QTreeWidget* scope_ = nullptr;
    QPushButton* build_ = nullptr;
    QLabel* summary_ = nullptr;
    QTreeWidget* table_ = nullptr; ///< the preview
    QLabel* messages_ = nullptr;
    QPushButton* accept_ = nullptr;
    QPushButton* cancel_ = nullptr;
};

} // namespace studyapp::ui
