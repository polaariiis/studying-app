// Phase 7 application layer: the Planner against a real WorkspaceSession (SQLite). Every
// operation is one undoable, persisted edit; tags by name reuse or create tags in the same
// edit; scope queries; the full workflow survives close and reopen.

#include <studyapp/application/Planner.hpp>

#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <memory>
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

    void reopen() {
        ASSERT_OK(session->close());
        session.reset();
        auto opened = WorkspaceSession::open(dir / "ws", {}, services());
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

} // namespace
} // namespace studyapp::application
