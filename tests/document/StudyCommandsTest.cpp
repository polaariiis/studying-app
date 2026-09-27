// Phase 7: study records in the Workspace — commands, invariants, reference indexes and
// undo/redo through the same patches as the notes hierarchy (docs/ARCHITECTURE.md D42).

#include <studyapp/document/StudyCommands.hpp>

#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>

namespace studyapp::document {
namespace {

using core::ErrorCode;
using test::millis;
using test::TestWorkspace;

template <class Id>
std::vector<Id> sorted(std::vector<Id> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

struct StudyWorkspace : TestWorkspace {
    core::CourseId addCourse(std::string title) {
        return run(commands::createCourse(workspace, std::move(title), clock, ids));
    }
    core::ProjectId addProject(std::string title, std::optional<core::CourseId> course = {}) {
        return run(commands::createProject(workspace, std::move(title), course, clock, ids));
    }
    core::TaskId addTask(commands::NewTask task) {
        return run(commands::createTask(workspace, std::move(task), clock, ids));
    }
    core::TagId addTag(std::string name) {
        return run(commands::createTag(workspace, std::move(name), clock, ids));
    }
    const study::Task& task(core::TaskId id) const { return *workspace.findTask(id); }
    void ok() const { ASSERT_OK(workspace.validate()); }
};

TEST(StudyCommandsTest, CoursesProjectsAndTasksAreCreatedEditedAndUndone) {
    StudyWorkspace t;
    const Workspace empty = t.workspace;
    const auto bio = t.addCourse("Biology");
    const auto chem = t.addCourse("Chemistry");
    EXPECT_EQ(std::vector(t.workspace.courses().begin(), t.workspace.courses().end()),
              (std::vector{bio, chem}));
    const auto lab = t.addProject("Lab report", bio);
    const auto read = t.addTask({.title = "Read chapter 3", .course = bio});
    const auto write = t.addTask({.title = "Write intro", .course = bio, .project = lab});
    t.ok();
    EXPECT_EQ(sorted(t.workspace.tasksOfCourse(bio)), sorted(std::vector{read, write}));
    EXPECT_EQ(sorted(t.workspace.tasksOfProject(lab)), (std::vector{write}));
    EXPECT_EQ(sorted(t.workspace.projectsOfCourse(bio)), (std::vector{lab}));
    EXPECT_EQ(std::vector(t.workspace.topLevelTasks().begin(), t.workspace.topLevelTasks().end()),
              (std::vector{read, write}));
    EXPECT_EQ(t.editor.history().nextUndo()->label, "Create task");

    // Editing keeps id, order and creation time; unchanged edits record nothing.
    study::Course course = *t.workspace.findCourse(bio);
    course.code = "BIO 101";
    course.order = core::FractionalIndex::first(); // ignored: order changes only by moving
    t.clock.advance(std::chrono::minutes{1});
    t.run(commands::updateCourse(t.workspace, course, t.clock));
    EXPECT_EQ(t.workspace.findCourse(bio)->code, "BIO 101");
    EXPECT_EQ(millis(t.workspace.findCourse(bio)->modified), millis(t.clock.now()));
    const auto steps = t.editor.history().undoCount();
    auto same = commands::updateCourse(t.workspace, *t.workspace.findCourse(bio), t.clock);
    ASSERT_OK(same);
    EXPECT_TRUE(same->patch.empty());

    study::Task edited = t.task(read);
    edited.priority = study::Priority::High;
    edited.dueDate =
        study::CalendarDate{std::chrono::year{2026}, std::chrono::October, std::chrono::day{2}};
    edited.dueTime = std::chrono::hours{9};
    edited.notes = "pages 40–62";
    t.run(commands::updateTask(t.workspace, edited, t.clock));
    EXPECT_EQ(t.task(read).notes, "pages 40–62");
    EXPECT_EQ(t.editor.history().undoCount(), steps + 1);

    // Completing records when; reopening clears it; the labels say which.
    study::Task done = t.task(read);
    done.status = study::TaskStatus::Done;
    t.run(commands::updateTask(t.workspace, done, t.clock));
    EXPECT_EQ(t.editor.history().nextUndo()->label, "Complete task");
    ASSERT_TRUE(t.task(read).completed.has_value());
    EXPECT_EQ(millis(*t.task(read).completed), millis(t.clock.now()));
    study::Task reopened = t.task(read);
    reopened.status = study::TaskStatus::Todo;
    t.run(commands::updateTask(t.workspace, reopened, t.clock));
    EXPECT_EQ(t.editor.history().nextUndo()->label, "Reopen task");
    EXPECT_FALSE(t.task(read).completed.has_value());

    // Reordering: one record changes; undo restores the order.
    t.run(commands::moveTask(t.workspace, write, 0, t.clock));
    EXPECT_EQ(t.workspace.topLevelTasks()[0], write);
    t.run(commands::moveCourse(t.workspace, chem, 0, t.clock));
    EXPECT_EQ(t.workspace.courses()[0], chem);
    t.ok();

    // Undo everything: exactly the empty workspace; redo everything back.
    const Workspace full = t.workspace;
    while (t.editor.history().undoCount() > 0) {
        ASSERT_OK(t.editor.undo());
        t.ok();
    }
    EXPECT_TRUE(t.workspace == empty);
    while (t.editor.history().redoCount() > 0) {
        ASSERT_OK(t.editor.redo());
    }
    EXPECT_TRUE(t.workspace == full);
    t.ok();
}

TEST(StudyCommandsTest, DeletingClearsReferencesInOnePatch) {
    StudyWorkspace t;
    const auto course = t.addCourse("History");
    const auto project = t.addProject("Essay", course);
    const auto task = t.addTask({.title = "Outline", .course = course, .project = project});
    const Workspace before = t.workspace;

    t.run(commands::deleteCourse(t.workspace, course));
    EXPECT_EQ(t.workspace.findCourse(course), nullptr);
    EXPECT_FALSE(t.workspace.findProject(project)->course.has_value());
    EXPECT_FALSE(t.task(task).course.has_value());
    EXPECT_EQ(t.task(task).project, project);
    t.ok();
    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.workspace == before);

    t.run(commands::deleteProject(t.workspace, project));
    EXPECT_FALSE(t.task(task).project.has_value());
    EXPECT_TRUE(t.workspace.tasksOfProject(project).empty());
    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.workspace == before);

    // A referenced record cannot be removed by a raw patch.
    const Patch raw({removed(*t.workspace.findCourse(course))});
    const auto refused = t.workspace.apply(raw);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(t.workspace == before);
}

TEST(StudyCommandsTest, SubtasksAreOneLevelAndGoWithTheirTask) {
    StudyWorkspace t;
    const auto parent = t.addTask({.title = "Revise for the exam"});
    const auto a = t.addTask({.title = "Flash cards", .parent = parent});
    const auto b = t.addTask({.title = "Past paper", .parent = parent});
    EXPECT_EQ(t.editor.history().nextUndo()->label, "Create subtask");
    EXPECT_EQ(
        std::vector(t.workspace.subtasksOf(parent).begin(), t.workspace.subtasksOf(parent).end()),
        (std::vector{a, b}));
    EXPECT_EQ(t.workspace.topLevelTasks().size(), 1U);

    // No subtask of a subtask; a task with subtasks cannot become one.
    const auto deeper =
        commands::createTask(t.workspace, {.title = "x", .parent = a}, t.clock, t.ids);
    ASSERT_OK(deeper);
    EXPECT_FALSE(t.editor.execute(deeper->command).has_value());
    const auto other = t.addTask({.title = "Other"});
    study::Task moved = t.task(parent);
    moved.parent = other;
    auto refused = commands::updateTask(t.workspace, moved, t.clock);
    ASSERT_OK(refused);
    EXPECT_FALSE(t.editor.execute(std::move(*refused)).has_value());

    // Moving a subtask to another parent appends it there.
    study::Task reparented = t.task(b);
    reparented.parent = other;
    t.run(commands::updateTask(t.workspace, reparented, t.clock));
    EXPECT_EQ(t.workspace.subtasksOf(other).size(), 1U);
    EXPECT_EQ(t.workspace.subtasksOf(parent).size(), 1U);
    t.ok();

    const Workspace before = t.workspace;
    t.run(commands::deleteTask(t.workspace, parent));
    EXPECT_EQ(t.workspace.findTask(parent), nullptr);
    EXPECT_EQ(t.workspace.findTask(a), nullptr); // its subtask went with it
    EXPECT_NE(t.workspace.findTask(b), nullptr); // no longer its subtask
    t.ok();
    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.workspace == before);
    t.ok();
}

TEST(StudyCommandsTest, TagsAreUniqueIgnoringCaseAndFollowPagesAndTasks) {
    StudyWorkspace t;
    const auto layer = t.addPath();
    const auto page = t.workspace.findLayer(layer)->page;
    const auto exam = t.addTag("Exam");
    const auto reading = t.addTag("reading");
    EXPECT_EQ(t.workspace.findTagByName("EXAM")->id, exam);
    EXPECT_EQ(commands::createTag(t.workspace, "exam", t.clock, t.ids).error().code,
              ErrorCode::AlreadyExists);
    EXPECT_FALSE(commands::createTag(t.workspace, "  ", t.clock, t.ids).has_value());
    // Renaming onto another tag's name is refused by the workspace.
    auto clash = commands::renameTag(t.workspace, reading, "EXAM");
    ASSERT_OK(clash);
    EXPECT_FALSE(t.editor.execute(std::move(*clash)).has_value());
    t.run(commands::renameTag(t.workspace, exam, "exam")); // own name, other case
    EXPECT_EQ(std::vector(t.workspace.tags().begin(), t.workspace.tags().end()),
              (std::vector{exam, reading})); // by name

    t.run(commands::setPageTags(t.workspace, page, {reading, exam, reading}, t.clock));
    EXPECT_EQ(t.workspace.findPage(page)->tags.size(), 2U); // sorted, unique
    EXPECT_EQ(t.workspace.pagesTagged(exam).size(), 1U);
    const auto task = t.addTask({.title = "Revise"});
    study::Task tagged = t.task(task);
    tagged.tags = {exam};
    t.run(commands::updateTask(t.workspace, tagged, t.clock));
    EXPECT_EQ(sorted(t.workspace.tasksTagged(exam)), (std::vector{task}));
    t.ok();

    const Workspace before = t.workspace;
    t.run(commands::deleteTag(t.workspace, exam));
    EXPECT_EQ(t.workspace.findPage(page)->tags, (std::vector{reading}));
    EXPECT_TRUE(t.task(task).tags.empty());
    EXPECT_EQ(t.workspace.findTagByName("exam"), nullptr);
    t.ok();
    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.workspace == before);
    t.ok();
}

TEST(StudyCommandsTest, TaskPageLinksAreBacklinksAndDeletedPagesAreUnlinked) {
    StudyWorkspace t;
    const auto notebook = t.addNotebook("Notes");
    const auto section = t.addSection(notebook, "Week 1");
    const auto p1 = t.addPage(section, "Cells");
    const auto p2 = t.addPage(section, "Membranes");
    const auto task = t.addTask({.title = "Summarise", .linkedPages = {p2, p1, p2}});
    EXPECT_EQ(t.task(task).linkedPages.size(), 2U);
    EXPECT_EQ(sorted(t.workspace.tasksLinkedTo(p1)), (std::vector{task}));
    const auto other = t.addTask({.title = "Quiz"});
    t.run(commands::setTaskPageLink(t.workspace, other, p1, true, t.clock));
    EXPECT_EQ(t.workspace.tasksLinkedTo(p1).size(), 2U);
    auto again = commands::setTaskPageLink(t.workspace, other, p1, true, t.clock);
    ASSERT_OK(again);
    EXPECT_TRUE(again->patch.empty());
    t.ok();

    // A page cannot be removed while tasks link to it; the delete commands unlink first.
    const Workspace before = t.workspace;
    t.run(commands::deletePage(t.workspace, p1));
    EXPECT_EQ(t.task(task).linkedPages, (std::vector{p2}));
    EXPECT_TRUE(t.task(other).linkedPages.empty());
    t.ok();
    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.workspace == before);

    t.run(commands::deleteSection(t.workspace, section)); // both pages at once: one update
    EXPECT_TRUE(t.task(task).linkedPages.empty());
    t.ok();
    ASSERT_OK(t.editor.undo());
    t.run(commands::deleteNotebook(t.workspace, notebook));
    EXPECT_TRUE(t.task(task).linkedPages.empty());
    t.ok();
    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.workspace == before);

    t.run(commands::setTaskPageLink(t.workspace, task, p2, false, t.clock));
    EXPECT_EQ(t.task(task).linkedPages, (std::vector{p1}));
    EXPECT_TRUE(t.workspace.tasksLinkedTo(p2).empty());
}

TEST(StudyCommandsTest, InvalidReferencesAndValuesAreRejected) {
    StudyWorkspace t;
    const core::CourseId ghost{t.ids.next()};
    EXPECT_EQ(commands::createProject(t.workspace, "x", ghost, t.clock, t.ids).error().code,
              ErrorCode::NotFound);
    EXPECT_FALSE(commands::createCourse(t.workspace, " ", t.clock, t.ids).has_value());
    EXPECT_FALSE(commands::createTask(t.workspace, {.title = ""}, t.clock, t.ids).has_value());
    const auto task = t.addTask({.title = "Task"});
    const auto steps = t.editor.history().undoCount();
    for (const auto& change : std::vector<std::function<void(study::Task&)>>{
             [&](study::Task& x) { x.course = ghost; },
             [&](study::Task& x) { x.project = core::ProjectId{t.ids.next()}; },
             [&](study::Task& x) { x.linkedPages = {core::PageId{t.ids.next()}}; },
             [&](study::Task& x) { x.tags = {core::TagId{t.ids.next()}}; },
             [&](study::Task& x) { x.dueTime = std::chrono::minutes{30}; }, // no date
         }) {
        study::Task bad = t.task(task);
        change(bad);
        auto command = commands::updateTask(t.workspace, bad, t.clock);
        if (command) {
            EXPECT_FALSE(t.editor.execute(std::move(*command)).has_value());
        }
    }
    EXPECT_EQ(t.editor.history().undoCount(), steps); // failures record nothing
    EXPECT_EQ(commands::updateTask(
                  t.workspace, study::Task{.id = core::TaskId{t.ids.next()}, .title = "x"}, t.clock)
                  .error()
                  .code,
              ErrorCode::NotFound);
    t.ok();
}

} // namespace
} // namespace studyapp::document
