// Phase 7 application layer: the Planner against a real WorkspaceSession (SQLite). Every
// operation is one undoable, persisted edit; tags by name reuse or create tags in the same
// edit; scope queries; the full workflow survives close and reopen. 2.0-PLAN-02: a task's
// estimate is set, changed and cleared through updateTask, and refused when read-only.
// 2.0-PLAN-03: a study plan is saved as one undoable, persisted edit after a final check.

#include <studyapp/application/Planner.hpp>

#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/study/Planning.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace studyapp::application {
namespace {

class NoLocks final : public WorkspaceLocker {
public:
    core::Result<LockStatus> inspect(const std::filesystem::path&) override { return LockStatus{}; }
    core::Result<std::unique_ptr<WorkspaceLock>> acquire(const std::filesystem::path&,
                                                         bool) override {
        return std::unique_ptr<WorkspaceLock>(std::make_unique<Held>());
    }

private:
    class Held final : public WorkspaceLock {};
};

struct PlannerTest : ::testing::Test {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds ids;
    NoLocks locker;
    std::unique_ptr<WorkspaceSession> session;

    SessionServices services() { return {clock, ids, locker}; }
    Planner planner() { return {*session, clock, ids}; }
    const document::Workspace& ws() const { return session->workspace(); }
    std::size_t steps() const { return session->history().undoCount(); }

    void SetUp() override {
        auto created = WorkspaceSession::create(dir / "ws", "Planner", services());
        ASSERT_OK(created);
        session = std::move(*created);
    }

    void reopen(OpenOptions options = {}) {
        ASSERT_OK(session->close());
        session.reset();
        auto opened = WorkspaceSession::open(dir / "ws", options, services());
        ASSERT_OK(opened);
        session = std::move(*opened);
    }

    std::vector<std::string> titles(const std::vector<const study::Task*>& tasks) const {
        std::vector<std::string> out;
        for (const study::Task* task : tasks) {
            out.push_back(task->title);
        }
        return out;
    }
};

TEST_F(PlannerTest, EveryOperationIsOneUndoablePersistedEdit) {
    Planner p = planner();
    auto course = p.createCourse();
    ASSERT_OK(course);
    EXPECT_EQ(ws().findCourse(*course)->title, "Course 1");
    auto project = p.createProject(*course);
    ASSERT_OK(project);
    EXPECT_EQ(ws().findProject(*project)->title, "Project 1");
    ASSERT_OK(p.renameCourse(*course, "  Biology  "));
    EXPECT_EQ(ws().findCourse(*course)->title, "Biology");
    EXPECT_FALSE(p.renameCourse(*course, "   ").has_value()); // blank: refused
    auto task = p.createTask({.title = " Read chapter 3 ", .course = *course});
    ASSERT_OK(task);
    EXPECT_EQ(ws().findTask(*task)->title, "Read chapter 3");
    const auto before = steps();
    ASSERT_OK(p.setTaskDone(*task, true));
    EXPECT_EQ(session->history().nextUndo()->label, "Complete task");
    ASSERT_OK(p.setTaskDone(*task, true)); // already done: nothing recorded
    EXPECT_EQ(steps(), before + 1);
    EXPECT_FALSE(p.createTask({.title = "  "}).has_value());
    EXPECT_EQ(steps(), before + 1);
    EXPECT_EQ(session->pendingWriteCount(), 0U);

    const document::Workspace saved = ws();
    reopen();
    EXPECT_TRUE(ws() == saved);
    ASSERT_OK(ws().validate());
}

TEST_F(PlannerTest, TagsByNameAreReusedOrCreatedInOneStep) {
    WorkspaceStructure structure(*session, clock, ids);
    auto notebook = structure.createNotebook();
    ASSERT_OK(notebook);
    Planner p = planner();
    EXPECT_EQ(Planner::parseTagNames(" exam, Reading,,EXAM , "),
              (std::vector<std::string>{"exam", "Reading"}));

    const auto before = steps();
    ASSERT_OK(p.setPageTagNames(notebook->page, Planner::parseTagNames("Exam, Reading")));
    EXPECT_EQ(steps(), before + 1); // two tags created and the page tagged: one step
    EXPECT_EQ(ws().tagCount(), 2U);
    EXPECT_EQ(p.tagNames(ws().findPage(notebook->page)->tags), "Exam, Reading");

    auto task = p.createTask({.title = "Revise"});
    ASSERT_OK(task);
    ASSERT_OK(p.setTaskTagNames(*task, Planner::parseTagNames("reading, Lab")));
    EXPECT_EQ(ws().tagCount(), 3U); // "reading" reused, "Lab" created
    EXPECT_EQ(p.tagNames(ws().findTask(*task)->tags), "Lab, Reading");

    ASSERT_OK(session->undo()); // the task's tags and the new tag go together
    EXPECT_EQ(ws().tagCount(), 2U);
    EXPECT_TRUE(ws().findTask(*task)->tags.empty());
    ASSERT_OK(session->undo());
    ASSERT_OK(session->undo());
    EXPECT_EQ(ws().tagCount(), 0U);
    EXPECT_TRUE(ws().findPage(notebook->page)->tags.empty());
    ASSERT_OK(ws().validate());
}

TEST_F(PlannerTest, ScopesListTasksOfACourseOrProjectInOrder) {
    Planner p = planner();
    const auto bio = *p.createCourse("Biology");
    const auto chem = *p.createCourse("Chemistry");
    const auto lab = *p.createProject(bio, "Lab report");
    const auto a = *p.createTask({.title = "a", .course = bio});
    const auto b = *p.createTask({.title = "b", .project = lab}); // via the course's project
    ASSERT_OK(p.createTask({.title = "c", .course = chem}));
    ASSERT_OK(p.createTask({.title = "a.1", .parent = a}));
    const auto both = *p.createTask({.title = "d", .course = bio, .project = lab});
    EXPECT_EQ(titles(p.tasksInScope(bio, std::nullopt)),
              (std::vector<std::string>{"a", "a.1", "b", "d"}));
    EXPECT_EQ(titles(p.tasksInScope(std::nullopt, lab)), (std::vector<std::string>{"b", "d"}));
    EXPECT_EQ(titles(p.tasksInScope(std::nullopt, std::nullopt)),
              (std::vector<std::string>{"a", "a.1", "b", "c", "d"}));
    ASSERT_OK(p.moveTaskBy(both, -1));
    ASSERT_OK(p.moveTaskBy(both, -1));
    EXPECT_EQ(titles(p.tasksInScope(bio, std::nullopt)),
              (std::vector<std::string>{"a", "a.1", "d", "b"}));
    const auto before = steps();
    ASSERT_OK(p.moveTaskBy(a, -1)); // already first: no-op
    EXPECT_EQ(steps(), before);
    (void)b;
}

TEST_F(PlannerTest, PageLinksAndDeletesAcrossTheShell) {
    WorkspaceStructure structure(*session, clock, ids);
    auto notebook = structure.createNotebook();
    ASSERT_OK(notebook);
    Planner p = planner();
    auto task = p.createTask({.title = "Summarise", .linkedPages = {notebook->page}});
    ASSERT_OK(task);
    EXPECT_EQ(ws().tasksLinkedTo(notebook->page).size(), 1U);
    ASSERT_OK(structure.remove(notebook->notebook)); // the notebook, its page, the link
    EXPECT_TRUE(ws().findTask(*task)->linkedPages.empty());
    ASSERT_OK(session->undo());
    EXPECT_EQ(ws().findTask(*task)->linkedPages.size(), 1U);
    ASSERT_OK(p.setTaskPageLink(*task, notebook->page, false));
    EXPECT_TRUE(ws().tasksLinkedTo(notebook->page).empty());
    const document::Workspace saved = ws();
    reopen();
    EXPECT_TRUE(ws() == saved);
}

// 2.0-PLAN-02: the task editor's Estimate field saves through updateTask. No estimate
// (nullopt) and 0 minutes are different values, and both are stored.
TEST_F(PlannerTest, EstimateIsSetChangedAndClearedInOneStepEach) {
    using std::chrono::minutes;
    auto task = planner().createTask({.title = "Revise"});
    ASSERT_OK(task);
    const auto estimate = [&] {
        return ws().findTask(*task)->estimate;
    };
    // A planner of the current session: reopen() replaces the session.
    const auto edit = [&](std::optional<minutes> value) {
        study::Task edited = *ws().findTask(*task);
        edited.estimate = value;
        return planner().updateTask(std::move(edited));
    };
    EXPECT_EQ(estimate(), std::nullopt);

    const auto before = steps();
    ASSERT_OK(edit(minutes{45})); // set
    EXPECT_EQ(steps(), before + 1);
    EXPECT_EQ(session->history().nextUndo()->label, "Edit task");
    ASSERT_OK(edit(minutes{90})); // changed
    EXPECT_EQ(steps(), before + 2);
    ASSERT_OK(edit(minutes{90})); // unchanged: nothing recorded
    EXPECT_EQ(steps(), before + 2);
    ASSERT_OK(edit(minutes{0}));
    EXPECT_EQ(steps(), before + 3);
    EXPECT_EQ(estimate(), minutes{0});
    ASSERT_OK(edit(std::nullopt)); // cleared
    EXPECT_EQ(steps(), before + 4);
    EXPECT_EQ(estimate(), std::nullopt);

    ASSERT_OK(session->undo());
    EXPECT_EQ(estimate(), minutes{0});
    ASSERT_OK(session->undo());
    EXPECT_EQ(estimate(), minutes{90});
    ASSERT_OK(session->redo());
    EXPECT_EQ(estimate(), minutes{0});
    EXPECT_EQ(session->pendingWriteCount(), 0U);

    reopen(); // 0 minutes is stored as 0, not as no estimate
    EXPECT_EQ(estimate(), minutes{0});
    ASSERT_OK(edit(std::nullopt));
    reopen();
    EXPECT_EQ(estimate(), std::nullopt);
    ASSERT_OK(ws().validate());
}

TEST_F(PlannerTest, AReadOnlyWorkspaceRefusesAnEstimate) {
    using std::chrono::minutes;
    auto task = planner().createTask({.title = "Revise"});
    ASSERT_OK(task);
    study::Task edited = *ws().findTask(*task);
    edited.estimate = minutes{30};
    ASSERT_OK(planner().updateTask(edited));

    reopen({.mode = AccessMode::ReadOnly});
    ASSERT_TRUE(session->isReadOnly());
    const auto before = steps();
    edited = *ws().findTask(*task);
    edited.estimate = minutes{60};
    auto refused = planner().updateTask(edited);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().message, "the workspace is open read-only");
    edited.estimate.reset();
    EXPECT_FALSE(planner().updateTask(edited).has_value()); // nor cleared
    EXPECT_EQ(steps(), before);
    EXPECT_EQ(ws().findTask(*task)->estimate, minutes{30});
}

// ---------------------------------------------------------------------- study plans

struct PlannerPlanTest : PlannerTest {
    PlanScope scope;
    core::PageId firstPage;

    void SetUp() override {
        PlannerTest::SetUp();
        WorkspaceStructure structure(*session, clock, ids);
        auto notebook = structure.createNotebook("Biology"); // "Section 1" with one page
        ASSERT_OK(notebook);
        firstPage = notebook->page;
        ASSERT_OK(structure.createPage(notebook->section));
        ASSERT_OK(structure.createPage(notebook->section));
        auto photosynthesis = structure.createSection(notebook->notebook, "Photosynthesis");
        ASSERT_OK(photosynthesis);
        ASSERT_OK(structure.createPage(photosynthesis->section));
        scope = {.notebooks = {notebook->notebook}};
    }

    static constexpr study::CalendarDate kStart{std::chrono::year{2026} / 10 / 19};

    PlanRequest request(int days, int minutesPerDay) const {
        return {.goal = "  Biology exam ",
                .start = kStart,
                .days = days,
                .minutesPerDay = std::chrono::minutes{minutesPerDay},
                .scope = scope};
    }

    PlanDraft draft(const PlanRequest& planRequest) const {
        auto built = draftByTreeOrder(ws(), planRequest);
        EXPECT_TRUE(built && built->draft) << (built ? "no draft" : built.error().message);
        return built && built->draft ? *built->draft : PlanDraft{};
    }

    std::vector<const study::Task*> planTasks(core::ProjectId project) {
        return planner().tasksInScope(std::nullopt, project);
    }
};

TEST_F(PlannerPlanTest, CreatePlanIsOneUndoableEdit) {
    using std::chrono::minutes;
    // 5 pages, e = 120 / 5 = 24 → 20: Section 1 (60) on day 1, Photosynthesis (40) on day 2.
    const PlanRequest planRequest = request(2, 60);
    const PlanDraft plan = draft(planRequest);
    ASSERT_EQ(plan.blocks.size(), 2U);
    const document::Workspace before = ws();
    const auto stepsBefore = steps();

    auto project = planner().createPlan(plan, planRequest);
    ASSERT_OK(project);
    EXPECT_EQ(steps(), stepsBefore + 1);
    EXPECT_EQ(session->history().nextUndo()->label, "Create study plan");
    const study::Project& saved = *ws().findProject(*project);
    EXPECT_EQ(saved.title, "Biology exam");
    EXPECT_EQ(saved.description, "Biology exam");
    EXPECT_EQ(saved.start, kStart);
    EXPECT_EQ(saved.due, std::chrono::year{2026} / 10 / 20);

    const auto tasks = planTasks(*project);
    ASSERT_EQ(tasks.size(), 2U);
    EXPECT_EQ(titles(tasks), (std::vector<std::string>{"Section 1", "Photosynthesis"}));
    EXPECT_EQ(tasks[0]->dueDate, kStart);
    EXPECT_EQ(tasks[1]->dueDate, std::chrono::year{2026} / 10 / 20);
    EXPECT_EQ(tasks[0]->estimate, minutes{60});
    EXPECT_EQ(tasks[1]->estimate, minutes{40});
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        std::vector<core::PageId> pages = plan.blocks[i].pages;
        std::sort(pages.begin(), pages.end());
        EXPECT_EQ(tasks[i]->linkedPages, pages); // every link is a page of the scope
        for (const core::PageId page : tasks[i]->linkedPages) {
            EXPECT_NE(ws().findPage(page), nullptr);
        }
    }

    const document::Workspace after = ws();
    ASSERT_OK(session->undo()); // the project and every task, in one step
    EXPECT_TRUE(ws() == before);
    ASSERT_OK(session->redo()); // the same ids and fields
    EXPECT_TRUE(ws() == after);
    EXPECT_EQ(session->pendingWriteCount(), 0U);
}

TEST_F(PlannerPlanTest, CreatePlanSurvivesReopening) {
    const PlanRequest planRequest = request(2, 60);
    auto project = planner().createPlan(draft(planRequest), planRequest);
    ASSERT_OK(project);
    const document::Workspace saved = ws();
    reopen();
    EXPECT_TRUE(ws() == saved);
    ASSERT_OK(ws().validate());
    EXPECT_EQ(planTasks(*project).size(), 2U);
}

TEST_F(PlannerPlanTest, UnscheduledBlocksHaveNoDueDate) {
    // e = 5: one page a day on days 1 … 5 of a 1-day plan; (c) keeps days 2 … 5 unscheduled.
    PlanPreview preview(request(1, 5), draft(request(1, 5)));
    ASSERT_OK(preview.keepOverflowUnscheduled());
    ASSERT_TRUE(preview.review(ws()).canAccept());
    auto project = planner().createPlan(preview.draft(), preview.request());
    ASSERT_OK(project);

    const auto check = [&] {
        const study::Project* saved = ws().findProject(*project);
        ASSERT_NE(saved, nullptr);
        EXPECT_EQ(saved->due, kStart); // start + D − 1, D = 1
        const auto tasks = planTasks(*project);
        ASSERT_EQ(tasks.size(), 5U);
        EXPECT_EQ(tasks[0]->dueDate, kStart);
        for (std::size_t i = 1; i < tasks.size(); ++i) {
            EXPECT_EQ(tasks[i]->dueDate, std::nullopt) << i;
        }
        // The agenda lists them under "No date".
        const study::Agenda agenda =
            study::buildAgenda(std::span<const study::Task* const>(tasks), clock.now(),
                               study::FixedOffsetZone{std::chrono::minutes{0}});
        EXPECT_EQ(agenda.unscheduled, (std::vector<std::size_t>{1, 2, 3, 4}));
    };
    check();
    ASSERT_OK(session->undo());
    EXPECT_EQ(ws().findProject(*project), nullptr);
    ASSERT_OK(session->redo());
    check();
    reopen();
    check();
}

TEST_F(PlannerPlanTest, SearchFindsPlanTasks) {
    const PlanRequest planRequest = request(2, 60);
    auto project = planner().createPlan(draft(planRequest), planRequest);
    ASSERT_OK(project);
    auto hits = session->search("Photosynthesis", 10);
    ASSERT_OK(hits);
    const auto tasks = planTasks(*project);
    ASSERT_EQ(tasks.size(), 2U);
    ASSERT_EQ(hits->size(), 1U); // sections are not indexed; the task is
    EXPECT_EQ((*hits)[0].kind, SearchHit::Kind::Task);
    EXPECT_EQ((*hits)[0].owner, tasks[1]->id.value());
}

TEST_F(PlannerPlanTest, AReadOnlyWorkspaceRefusesAPlan) {
    const PlanRequest planRequest = request(2, 60);
    reopen({.mode = AccessMode::ReadOnly});
    ASSERT_TRUE(session->isReadOnly());
    // The preview works read-only.
    PlanPreview preview(planRequest, draft(planRequest));
    EXPECT_TRUE(preview.review(ws()).canAccept());
    const document::Workspace before = ws();
    auto refused = planner().createPlan(preview.draft(), preview.request());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, core::ErrorCode::Unsupported);
    EXPECT_EQ(steps(), 0U);
    EXPECT_TRUE(ws() == before);
}

TEST_F(PlannerPlanTest, TheFinalCheckRefusesWhatCannotBeSaved) {
    const PlanRequest planRequest = request(2, 60);
    const PlanDraft plan = draft(planRequest);
    const document::Workspace before = ws();
    const auto stepsBefore = steps();
    const auto refusedCode = [&](const PlanDraft& candidate) {
        auto result = planner().createPlan(candidate, planRequest);
        return result ? core::ErrorCode::Internal : result.error().code;
    };

    PlanDraft empty = plan;
    empty.blocks.clear();
    EXPECT_EQ(refusedCode(empty), core::ErrorCode::InvalidArgument);
    // Overflow not resolved: e = 5, one page a day on days 1 … 5 of 1.
    EXPECT_EQ(refusedCode(draft(request(1, 5))), core::ErrorCode::InvalidArgument);
    EXPECT_EQ(steps(), stepsBefore);
    EXPECT_TRUE(ws() == before);

    // A linked page deleted after the draft was built: nothing is saved.
    WorkspaceStructure structure(*session, clock, ids);
    ASSERT_OK(structure.remove(firstPage));
    EXPECT_EQ(refusedCode(plan), core::ErrorCode::InvalidArgument);
    EXPECT_EQ(steps(), stepsBefore + 1); // only the user's own edit
    EXPECT_TRUE(ws().projects().empty());
    ASSERT_OK(session->undo());
    ASSERT_OK(planner().createPlan(plan, planRequest)); // valid again
}

} // namespace
} // namespace studyapp::application
