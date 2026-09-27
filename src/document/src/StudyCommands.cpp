#include <studyapp/document/StudyCommands.hpp>

#include "CommandSupport.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace studyapp::document::commands {

using core::ErrorCode;
using core::makeError;
using core::Result;
using detail::appendKey;
using detail::keyAt;
using detail::makeCommand;
using detail::notFound;

namespace {

template <class Id>
void normalise(std::vector<Id>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

template <class Id>
bool erase(std::vector<Id>& ids, const Id& id) {
    const auto at = std::find(ids.begin(), ids.end(), id);
    if (at == ids.end()) {
        return false;
    }
    ids.erase(at);
    return true;
}

/// Working copies of records changed by one command: each record gets one update, from its
/// current state to its final state, however many steps change it.
template <class Record, class Id, class Find>
class Edits {
public:
    explicit Edits(Find find) : find_(std::move(find)) {}

    Record& operator[](const Id& id) {
        auto it = edited_.find(id);
        if (it == edited_.end()) {
            it = edited_.emplace(id, *find_(id)).first;
            order_.push_back(id);
        }
        return it->second;
    }

    void appendTo(std::vector<AnyChange>& out) const {
        for (const Id& id : order_) {
            const Record& before = *find_(id);
            const Record& after = edited_.at(id);
            if (!(before == after)) {
                out.push_back(updated(before, after));
            }
        }
    }

private:
    Find find_;
    std::unordered_map<Id, Record> edited_;
    std::vector<Id> order_; ///< first-edit order: deterministic patches
};

template <class Record, class Id, class Find>
Edits<Record, Id, Find> editsOf(Find find) {
    return Edits<Record, Id, Find>(std::move(find));
}

} // namespace

// ---------------------------------------------------------------------------- courses

Result<Created<core::CourseId>> createCourse(const Workspace& workspace, std::string title,
                                             const core::Clock& clock, core::IdGenerator& ids) {
    const auto now = clock.now();
    study::Course course{
        .id = core::CourseId::generate(ids),
        .title = std::move(title),
        .order = appendKey(workspace.courses(),
                           [&](core::CourseId id) { return workspace.findCourse(id)->order; }),
        .created = now,
        .modified = now,
    };
    if (auto check = study::checkValues(course); !check) {
        return tl::unexpected(check.error());
    }
    const auto id = course.id;
    return Created<core::CourseId>{id, makeCommand("Create course", {created(std::move(course))})};
}

Result<Command> updateCourse(const Workspace& workspace, study::Course course,
                             const core::Clock& clock) {
    const study::Course* current = workspace.findCourse(course.id);
    if (current == nullptr) {
        return tl::unexpected(notFound("course", course.id));
    }
    course.order = current->order;
    course.created = current->created;
    course.modified = current->modified;
    if (course == *current) {
        return Command{"Edit course", Patch{}};
    }
    course.modified = clock.now();
    return makeCommand("Edit course", {updated(*current, std::move(course))});
}

Result<Command> deleteCourse(const Workspace& workspace, core::CourseId course) {
    const study::Course* current = workspace.findCourse(course);
    if (current == nullptr) {
        return tl::unexpected(notFound("course", course));
    }
    std::vector<AnyChange> changes;
    auto projects = editsOf<study::Project, core::ProjectId>(
        [&](core::ProjectId id) { return workspace.findProject(id); });
    for (const core::ProjectId id : workspace.projectsOfCourse(course)) {
        projects[id].course.reset();
    }
    auto tasks =
        editsOf<study::Task, core::TaskId>([&](core::TaskId id) { return workspace.findTask(id); });
    for (const core::TaskId id : workspace.tasksOfCourse(course)) {
        tasks[id].course.reset();
    }
    projects.appendTo(changes);
    tasks.appendTo(changes);
    changes.push_back(removed(*current));
    return makeCommand("Delete course", std::move(changes));
}

Result<Command> moveCourse(const Workspace& workspace, core::CourseId course, std::size_t index,
                           const core::Clock& clock) {
    const study::Course* current = workspace.findCourse(course);
    if (current == nullptr) {
        return tl::unexpected(notFound("course", course));
    }
    auto key = keyAt(workspace.courses(), course, index,
                     [&](core::CourseId id) { return workspace.findCourse(id)->order; });
    if (!key) {
        return tl::unexpected(key.error());
    }
    if (!*key) {
        return Command{"Move course", Patch{}};
    }
    study::Course after = *current;
    after.order = std::move(**key);
    after.modified = clock.now();
    return makeCommand("Move course", {updated(*current, std::move(after))});
}

// ---------------------------------------------------------------------------- projects

Result<Created<core::ProjectId>> createProject(const Workspace& workspace, std::string title,
                                               std::optional<core::CourseId> course,
                                               const core::Clock& clock, core::IdGenerator& ids) {
    if (course && workspace.findCourse(*course) == nullptr) {
        return tl::unexpected(notFound("course", *course));
    }
    const auto now = clock.now();
    study::Project project{
        .id = core::ProjectId::generate(ids),
        .title = std::move(title),
        .course = course,
        .order = appendKey(workspace.projects(),
                           [&](core::ProjectId id) { return workspace.findProject(id)->order; }),
        .created = now,
        .modified = now,
    };
    if (auto check = study::checkValues(project); !check) {
        return tl::unexpected(check.error());
    }
    const auto id = project.id;
    return Created<core::ProjectId>{id,
                                    makeCommand("Create project", {created(std::move(project))})};
}

Result<Command> updateProject(const Workspace& workspace, study::Project project,
                              const core::Clock& clock) {
    const study::Project* current = workspace.findProject(project.id);
    if (current == nullptr) {
        return tl::unexpected(notFound("project", project.id));
    }
    project.order = current->order;
    project.created = current->created;
    project.modified = current->modified;
    const bool done = project.status == study::ProjectStatus::Done;
    const bool wasDone = current->status == study::ProjectStatus::Done;
    project.completed = done ? (wasDone ? current->completed : std::nullopt) : std::nullopt;
    if (project == *current) {
        return Command{"Edit project", Patch{}};
    }
    const auto now = clock.now();
    if (done && !wasDone) {
        project.completed = now;
    }
    project.modified = now;
    return makeCommand("Edit project", {updated(*current, std::move(project))});
}

Result<Command> deleteProject(const Workspace& workspace, core::ProjectId project) {
    const study::Project* current = workspace.findProject(project);
    if (current == nullptr) {
        return tl::unexpected(notFound("project", project));
    }
    std::vector<AnyChange> changes;
    auto tasks =
        editsOf<study::Task, core::TaskId>([&](core::TaskId id) { return workspace.findTask(id); });
    for (const core::TaskId id : workspace.tasksOfProject(project)) {
        tasks[id].project.reset();
    }
    tasks.appendTo(changes);
    changes.push_back(removed(*current));
    return makeCommand("Delete project", std::move(changes));
}

Result<Command> moveProject(const Workspace& workspace, core::ProjectId project, std::size_t index,
                            const core::Clock& clock) {
    const study::Project* current = workspace.findProject(project);
    if (current == nullptr) {
        return tl::unexpected(notFound("project", project));
    }
    auto key = keyAt(workspace.projects(), project, index,
                     [&](core::ProjectId id) { return workspace.findProject(id)->order; });
    if (!key) {
        return tl::unexpected(key.error());
    }
    if (!*key) {
        return Command{"Move project", Patch{}};
    }
    study::Project after = *current;
    after.order = std::move(**key);
    after.modified = clock.now();
    return makeCommand("Move project", {updated(*current, std::move(after))});
}

// ---------------------------------------------------------------------------- tasks

namespace {

std::span<const core::TaskId> siblingTasks(const Workspace& ws,
                                           const std::optional<core::TaskId>& parent) {
    return parent ? ws.subtasksOf(*parent) : ws.topLevelTasks();
}

} // namespace

Result<Created<core::TaskId>> createTask(const Workspace& workspace, NewTask task,
                                         const core::Clock& clock, core::IdGenerator& ids) {
    if (task.parent && workspace.findTask(*task.parent) == nullptr) {
        return tl::unexpected(notFound("task", *task.parent));
    }
    normalise(task.linkedPages);
    const auto now = clock.now();
    study::Task record{
        .id = core::TaskId::generate(ids),
        .title = std::move(task.title),
        .course = task.course,
        .project = task.project,
        .parent = task.parent,
        .dueDate = task.dueDate,
        .linkedPages = std::move(task.linkedPages),
        .order = appendKey(siblingTasks(workspace, task.parent),
                           [&](core::TaskId id) { return workspace.findTask(id)->order; }),
        .created = now,
        .modified = now,
    };
    if (auto check = study::checkValues(record); !check) {
        return tl::unexpected(check.error());
    }
    const auto id = record.id;
    return Created<core::TaskId>{id, makeCommand(record.parent ? "Create subtask" : "Create task",
                                                 {created(std::move(record))})};
}

Result<Command> updateTask(const Workspace& workspace, study::Task task, const core::Clock& clock) {
    const study::Task* current = workspace.findTask(task.id);
    if (current == nullptr) {
        return tl::unexpected(notFound("task", task.id));
    }
    normalise(task.linkedPages);
    normalise(task.tags);
    task.created = current->created;
    task.modified = current->modified;
    task.order = current->order;
    if (task.parent != current->parent) {
        if (task.parent && workspace.findTask(*task.parent) == nullptr) {
            return tl::unexpected(notFound("task", *task.parent));
        }
        task.order = appendKey(siblingTasks(workspace, task.parent),
                               [&](core::TaskId id) { return workspace.findTask(id)->order; });
    }
    const bool done = task.status == study::TaskStatus::Done;
    const bool wasDone = current->status == study::TaskStatus::Done;
    task.completed = done && wasDone ? current->completed : std::nullopt;
    if (task == *current) {
        return Command{"Edit task", Patch{}};
    }
    const auto now = clock.now();
    if (done && !wasDone) {
        task.completed = now;
    }
    task.modified = now;
    // Only the status changed: say what it did.
    study::Task statusOnly = *current;
    statusOnly.status = task.status;
    statusOnly.completed = task.completed;
    statusOnly.modified = task.modified;
    std::string label = "Edit task";
    if (statusOnly == task) {
        label = done ? "Complete task" : (wasDone ? "Reopen task" : "Change task status");
    }
    return makeCommand(std::move(label), {updated(*current, std::move(task))});
}

Result<Command> deleteTask(const Workspace& workspace, core::TaskId task) {
    const study::Task* current = workspace.findTask(task);
    if (current == nullptr) {
        return tl::unexpected(notFound("task", task));
    }
    std::vector<AnyChange> changes;
    for (const core::TaskId subtask : workspace.subtasksOf(task)) {
        changes.push_back(removed(*workspace.findTask(subtask)));
    }
    changes.push_back(removed(*current));
    return makeCommand("Delete task", std::move(changes));
}

Result<Command> moveTask(const Workspace& workspace, core::TaskId task, std::size_t index,
                         const core::Clock& clock) {
    const study::Task* current = workspace.findTask(task);
    if (current == nullptr) {
        return tl::unexpected(notFound("task", task));
    }
    auto key = keyAt(siblingTasks(workspace, current->parent), task, index,
                     [&](core::TaskId id) { return workspace.findTask(id)->order; });
    if (!key) {
        return tl::unexpected(key.error());
    }
    if (!*key) {
        return Command{"Move task", Patch{}};
    }
    study::Task after = *current;
    after.order = std::move(**key);
    after.modified = clock.now();
    return makeCommand("Move task", {updated(*current, std::move(after))});
}

Result<Command> setTaskPageLink(const Workspace& workspace, core::TaskId task, core::PageId page,
                                bool linked, const core::Clock& clock) {
    const study::Task* current = workspace.findTask(task);
    if (current == nullptr) {
        return tl::unexpected(notFound("task", task));
    }
    if (workspace.findPage(page) == nullptr) {
        return tl::unexpected(notFound("page", page));
    }
    study::Task after = *current;
    if (linked) {
        after.linkedPages.push_back(page);
        normalise(after.linkedPages);
    } else {
        erase(after.linkedPages, page);
    }
    const char* label = linked ? "Link task to page" : "Unlink task from page";
    if (after.linkedPages == current->linkedPages) {
        return Command{label, Patch{}};
    }
    after.modified = clock.now();
    return makeCommand(label, {updated(*current, std::move(after))});
}

void appendTaskUnlinks(const Workspace& workspace, std::span<const core::PageId> pages,
                       std::vector<AnyChange>& out) {
    auto tasks =
        editsOf<study::Task, core::TaskId>([&](core::TaskId id) { return workspace.findTask(id); });
    for (const core::PageId page : pages) {
        for (const core::TaskId task : workspace.tasksLinkedTo(page)) {
            erase(tasks[task].linkedPages, page);
        }
    }
    tasks.appendTo(out);
}

// ---------------------------------------------------------------------------- tags

Result<Created<core::TagId>> createTag(const Workspace& workspace, std::string name,
                                       const core::Clock& clock, core::IdGenerator& ids) {
    if (workspace.findTagByName(name) != nullptr) {
        return makeError(ErrorCode::AlreadyExists, "a tag named \"" + name + "\" exists");
    }
    study::Tag tag{
        .id = core::TagId::generate(ids), .name = std::move(name), .created = clock.now()};
    if (auto check = study::checkValues(tag); !check) {
        return tl::unexpected(check.error());
    }
    const auto id = tag.id;
    return Created<core::TagId>{id, makeCommand("Create tag", {created(std::move(tag))})};
}

Result<Command> renameTag(const Workspace& workspace, core::TagId tag, std::string name) {
    const study::Tag* current = workspace.findTag(tag);
    if (current == nullptr) {
        return tl::unexpected(notFound("tag", tag));
    }
    if (current->name == name) {
        return Command{"Rename tag", Patch{}};
    }
    study::Tag after = *current;
    after.name = std::move(name);
    return makeCommand("Rename tag", {updated(*current, std::move(after))});
}

Result<Command> deleteTag(const Workspace& workspace, core::TagId tag) {
    const study::Tag* current = workspace.findTag(tag);
    if (current == nullptr) {
        return tl::unexpected(notFound("tag", tag));
    }
    std::vector<AnyChange> changes;
    auto pages =
        editsOf<PageInfo, core::PageId>([&](core::PageId id) { return workspace.findPage(id); });
    for (const core::PageId page : workspace.pagesTagged(tag)) {
        erase(pages[page].tags, tag);
    }
    auto tasks =
        editsOf<study::Task, core::TaskId>([&](core::TaskId id) { return workspace.findTask(id); });
    for (const core::TaskId task : workspace.tasksTagged(tag)) {
        erase(tasks[task].tags, tag);
    }
    pages.appendTo(changes);
    tasks.appendTo(changes);
    changes.push_back(removed(*current));
    return makeCommand("Delete tag", std::move(changes));
}

Result<Command> setPageTags(const Workspace& workspace, core::PageId page,
                            std::vector<core::TagId> tags, const core::Clock& clock) {
    const PageInfo* current = workspace.findPage(page);
    if (current == nullptr) {
        return tl::unexpected(notFound("page", page));
    }
    normalise(tags);
    if (tags == current->tags) {
        return Command{"Change page tags", Patch{}};
    }
    PageInfo after = *current;
    after.tags = std::move(tags);
    after.modified = clock.now();
    return makeCommand("Change page tags", {updated(*current, std::move(after))});
}

} // namespace studyapp::document::commands
