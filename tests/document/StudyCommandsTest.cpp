// Phase 7: study records in the Workspace — commands, invariants, reference indexes and
// undo/redo through the same patches as the notes hierarchy (docs/ARCHITECTURE.md D42).

#include <studyapp/document/StudyCommands.hpp>

#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <utility>
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

// 2.0-PLAN-03: a study plan is one project and its tasks in one patch.

commands::NewPlan plan(std::vector<commands::NewPlanTask> tasks) {
    using std::chrono::year;
    return {.title = "Biology exam",
            .description = "Biology exam in November",
            .start = year{2026} / 10 / 19,
            .due = year{2026} / 10 / 25,
            .tasks = std::move(tasks)};
}

TEST(StudyCommandsTest, CreatePlanIsOneProjectAndItsTasksInOnePatch) {
    using std::chrono::minutes;
    using std::chrono::year;
    StudyWorkspace t;
    const auto section = t.addSection(t.addNotebook("Biology"), "Cells");
    const auto page = t.addPage(section, "Membrane");
    const auto other = t.addPage(section, "Nucleus");
    const auto old = t.addProject("Old project");
    const auto earlier = t.addTask({.title = "Earlier"});
    const Workspace before = t.workspace;

    auto created = commands::createPlan(
        t.workspace,
        plan({{.title = "Cells",
               .estimate = minutes{30},
               .dueDate = year{2026} / 10 / 19,
               .linkedPages = {other, page, other}},
              {.title = "Genes", .notes = "Read twice", .estimate = minutes{45}}}),
        t.clock, t.ids);
    ASSERT_OK(created);
    EXPECT_EQ(created->command.label, "Create study plan");
    EXPECT_EQ(created->command.patch.size(), 3U); // the project, then its two tasks
    ASSERT_OK(t.editor.execute(created->command));
    t.ok();

    const study::Project& project = *t.workspace.findProject(created->id);
    EXPECT_EQ(project.title, "Biology exam");
    EXPECT_EQ(project.description, "Biology exam in November");
    EXPECT_EQ(project.start, year{2026} / 10 / 19);
    EXPECT_EQ(project.due, year{2026} / 10 / 25);
    EXPECT_EQ(project.status, study::ProjectStatus::Active);
    EXPECT_EQ(project.course, std::nullopt);
    EXPECT_EQ(project.color, std::nullopt);
    EXPECT_EQ(std::vector(t.workspace.projects().begin(), t.workspace.projects().end()),
              (std::vector{old, created->id}));

    const std::vector top(t.workspace.topLevelTasks().begin(), t.workspace.topLevelTasks().end());
    ASSERT_EQ(top.size(), 3U);
    EXPECT_EQ(top[0], earlier);
    const study::Task& cells = t.task(top[1]);
    const study::Task& genes = t.task(top[2]);
    EXPECT_EQ(cells.title, "Cells");
    EXPECT_EQ(cells.project, created->id);
    EXPECT_EQ(cells.estimate, minutes{30});
    EXPECT_EQ(cells.dueDate, year{2026} / 10 / 19);
    EXPECT_EQ(cells.linkedPages, sorted(std::vector{page, other})); // sorted, once each
    EXPECT_EQ(genes.title, "Genes");
    EXPECT_EQ(genes.notes, "Read twice");
    EXPECT_EQ(genes.dueDate, std::nullopt); // unscheduled
    for (const study::Task* task : {&cells, &genes}) {
        EXPECT_EQ(task->status, study::TaskStatus::Todo);
        EXPECT_EQ(task->priority, study::Priority::None);
        EXPECT_EQ(task->course, std::nullopt);
        EXPECT_EQ(task->parent, std::nullopt);
        EXPECT_EQ(task->dueTime, std::nullopt);
        EXPECT_EQ(task->scheduled, std::nullopt);
        EXPECT_TRUE(task->tags.empty());
    }
    EXPECT_EQ(sorted(t.workspace.tasksOfProject(created->id)), sorted(std::vector{top[1], top[2]}));

    const Workspace after = t.workspace;
    ASSERT_OK(t.editor.undo()); // one step removes the project and every task
    EXPECT_TRUE(t.workspace == before);
    ASSERT_OK(t.editor.redo());
    EXPECT_TRUE(t.workspace == after);
}

TEST(StudyCommandsTest, CreatePlanOrderKeysStrictlyIncrease) {
    StudyWorkspace t;
    const auto earlier = t.addTask({.title = "Earlier"});
    std::vector<commands::NewPlanTask> tasks;
    for (int i = 1; i <= 100; ++i) {
        tasks.push_back({.title = "Block " + std::to_string(i)});
    }
    auto created = commands::createPlan(t.workspace, plan(std::move(tasks)), t.clock, t.ids);
    ASSERT_OK(created);
    ASSERT_OK(t.editor.execute(std::move(created->command)));
    t.ok();
    const auto top = t.workspace.topLevelTasks();
    ASSERT_EQ(top.size(), 101U);
    EXPECT_EQ(top.front(), earlier);
    for (std::size_t i = 1; i < top.size(); ++i) {
        EXPECT_LT(t.task(top[i - 1]).order, t.task(top[i]).order) << i;
        EXPECT_EQ(t.task(top[i]).title, "Block " + std::to_string(i));
    }

    // In an empty workspace the first task starts the list.
    StudyWorkspace empty;
    auto first = commands::createPlan(empty.workspace, plan({{.title = "A"}, {.title = "B"}}),
                                      empty.clock, empty.ids);
    ASSERT_OK(first);
    ASSERT_OK(empty.editor.execute(std::move(first->command)));
    const auto two = empty.workspace.topLevelTasks();
    ASSERT_EQ(two.size(), 2U);
    EXPECT_EQ(empty.task(two[0]).title, "A");
    EXPECT_LT(empty.task(two[0]).order, empty.task(two[1]).order);
}

TEST(StudyCommandsTest, CreatePlanIsDeterministic) {
    const auto make = [] {
        StudyWorkspace t;
        t.addTask({.title = "Earlier"});
        auto created = commands::createPlan(
            t.workspace, plan({{.title = "Cells"}, {.title = "Genes"}}), t.clock, t.ids);
        EXPECT_TRUE(created.has_value());
        return created ? created->command : Command{};
    };
    const Command first = make();
    EXPECT_FALSE(first.patch.empty());
    EXPECT_TRUE(first == make());
}

TEST(StudyCommandsTest, CreatePlanRejectsInvalidValuesAndUnknownPages) {
    using std::chrono::year;
    StudyWorkspace t;
    commands::NewPlan blank = plan({{.title = "Cells"}});
    blank.title = " ";
    EXPECT_FALSE(commands::createPlan(t.workspace, blank, t.clock, t.ids).has_value());
    EXPECT_FALSE(
        commands::createPlan(t.workspace, plan({{.title = ""}}), t.clock, t.ids).has_value());
    commands::NewPlan late = plan({{.title = "Cells"}});
    late.due = year{10000} / 1 / 1;
    EXPECT_FALSE(commands::createPlan(t.workspace, late, t.clock, t.ids).has_value());
    EXPECT_FALSE(commands::createPlan(t.workspace,
                                      plan({{.title = "Cells", .dueDate = year{0} / 1 / 1}}),
                                      t.clock, t.ids)
                     .has_value());

    // A page that does not exist: the Workspace refuses the whole patch.
    const Workspace before = t.workspace;
    auto ghost = commands::createPlan(
        t.workspace,
        plan({{.title = "Cells"}, {.title = "Genes", .linkedPages = {core::PageId{t.ids.next()}}}}),
        t.clock, t.ids);
    ASSERT_OK(ghost);
    EXPECT_FALSE(t.editor.execute(std::move(ghost->command)).has_value());
    EXPECT_TRUE(t.workspace == before);
    EXPECT_EQ(t.editor.history().undoCount(), 0U);
}

} // namespace
} // namespace studyapp::document
