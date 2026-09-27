#include <studyapp/application/Planner.hpp>

#include <algorithm>
#include <utility>

namespace studyapp::application {

namespace commands = document::commands;

using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

/// "<base> <n>" with the smallest n >= count + 1 not used by a sibling.
template <class Id, class TitleOf>
std::string nextDefaultTitle(std::string_view base, std::span<const Id> siblings, TitleOf titleOf) {
    for (std::size_t n = siblings.size() + 1;; ++n) {
        std::string candidate = std::string(base) + " " + std::to_string(n);
        if (std::none_of(siblings.begin(), siblings.end(),
                         [&](const Id& id) { return titleOf(id) == candidate; })) {
            return candidate;
        }
    }
}

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

template <class T>
Result<T> failed(const core::Error& error) {
    return tl::unexpected(error);
}

} // namespace

Planner::Planner(WorkspaceSession& session, const core::Clock& clock,
                 core::IdGenerator& ids) noexcept
    : session_(&session), clock_(&clock), ids_(&ids) {}

const document::Workspace& Planner::workspace() const noexcept {
    return session_->workspace();
}

Result<void> Planner::run(Result<document::Command> command) {
    if (!command) {
        return tl::unexpected(command.error());
    }
    if (command->patch.empty()) {
        return {}; // nothing changed: nothing to record
    }
    return session_->execute(std::move(*command));
}

// ---------------------------------------------------------------------------- courses

Result<core::CourseId> Planner::createCourse(std::string title) {
    const document::Workspace& ws = workspace();
    if (trim(title).empty()) {
        title = nextDefaultTitle("Course", ws.courses(),
                                 [&](core::CourseId id) { return ws.findCourse(id)->title; });
    }
    auto created = commands::createCourse(ws, std::move(title), *clock_, *ids_);
    if (!created) {
        return failed<core::CourseId>(created.error());
    }
    if (auto executed = session_->execute(std::move(created->command)); !executed) {
        return failed<core::CourseId>(executed.error());
    }
    return created->id;
}

Result<void> Planner::updateCourse(study::Course course) {
    return run(commands::updateCourse(workspace(), std::move(course), *clock_));
}

Result<void> Planner::renameCourse(core::CourseId course, std::string title) {
    const study::Course* current = workspace().findCourse(course);
    if (current == nullptr) {
        return makeError(ErrorCode::NotFound, "course " + course.toString() + " does not exist");
    }
    study::Course renamed = *current;
    renamed.title = std::string(trim(title));
    return run(commands::updateCourse(workspace(), std::move(renamed), *clock_));
}

Result<void> Planner::deleteCourse(core::CourseId course) {
    return run(commands::deleteCourse(workspace(), course));
}

Result<core::ProjectId> Planner::createProject(std::optional<core::CourseId> course,
                                               std::string title) {
    const document::Workspace& ws = workspace();
    if (trim(title).empty()) {
        title = nextDefaultTitle("Project", ws.projects(),
                                 [&](core::ProjectId id) { return ws.findProject(id)->title; });
    }
    auto created = commands::createProject(ws, std::move(title), course, *clock_, *ids_);
    if (!created) {
        return failed<core::ProjectId>(created.error());
    }
    if (auto executed = session_->execute(std::move(created->command)); !executed) {
        return failed<core::ProjectId>(executed.error());
    }
    return created->id;
}

Result<void> Planner::updateProject(study::Project project) {
    return run(commands::updateProject(workspace(), std::move(project), *clock_));
}

Result<void> Planner::renameProject(core::ProjectId project, std::string title) {
    const study::Project* current = workspace().findProject(project);
    if (current == nullptr) {
        return makeError(ErrorCode::NotFound, "project " + project.toString() + " does not exist");
    }
    study::Project renamed = *current;
    renamed.title = std::string(trim(title));
    return run(commands::updateProject(workspace(), std::move(renamed), *clock_));
}

Result<void> Planner::deleteProject(core::ProjectId project) {
    return run(commands::deleteProject(workspace(), project));
}

// ---------------------------------------------------------------------------- tasks

Result<core::TaskId> Planner::createTask(commands::NewTask task) {
    task.title = std::string(trim(task.title));
    auto created = commands::createTask(workspace(), std::move(task), *clock_, *ids_);
    if (!created) {
        return failed<core::TaskId>(created.error());
    }
    if (auto executed = session_->execute(std::move(created->command)); !executed) {
        return failed<core::TaskId>(executed.error());
    }
    return created->id;
}

Result<void> Planner::updateTask(study::Task task) {
    return run(commands::updateTask(workspace(), std::move(task), *clock_));
}

Result<void> Planner::setTaskDone(core::TaskId task, bool done) {
    const study::Task* current = workspace().findTask(task);
    if (current == nullptr) {
        return makeError(ErrorCode::NotFound, "task " + task.toString() + " does not exist");
    }
    study::Task after = *current;
    after.status = done ? study::TaskStatus::Done : study::TaskStatus::Todo;
    return run(commands::updateTask(workspace(), std::move(after), *clock_));
}

Result<void> Planner::deleteTask(core::TaskId task) {
    return run(commands::deleteTask(workspace(), task));
}

Result<void> Planner::moveTaskBy(core::TaskId task, int delta) {
    const document::Workspace& ws = workspace();
    const study::Task* current = ws.findTask(task);
    if (current == nullptr) {
        return makeError(ErrorCode::NotFound, "task " + task.toString() + " does not exist");
    }
    const auto siblings = current->parent ? ws.subtasksOf(*current->parent) : ws.topLevelTasks();
    const auto at = static_cast<std::ptrdiff_t>(std::find(siblings.begin(), siblings.end(), task) -
                                                siblings.begin());
    const std::ptrdiff_t target = at + delta;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(siblings.size())) {
        return {};
    }
    return run(commands::moveTask(ws, task, static_cast<std::size_t>(target), *clock_));
}

Result<void> Planner::setTaskPageLink(core::TaskId task, core::PageId page, bool linked) {
    return run(commands::setTaskPageLink(workspace(), task, page, linked, *clock_));
}

// ---------------------------------------------------------------------------- tags

std::vector<std::string> Planner::parseTagNames(std::string_view text) {
    std::vector<std::string> names;
    std::vector<std::string> folded;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view part = trim(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (part.empty()) {
            continue;
        }
        std::string key = study::foldTagName(part);
        if (std::find(folded.begin(), folded.end(), key) == folded.end()) {
            folded.push_back(std::move(key));
            names.emplace_back(part);
        }
    }
    return names;
}

Result<std::vector<core::TagId>> Planner::tagIds(std::span<const std::string> names,
                                                 std::vector<document::AnyChange>& changes) {
    const document::Workspace& ws = workspace();
    std::vector<core::TagId> ids;
    std::vector<std::string> newNames; // folded, created in this edit
    for (const std::string& raw : names) {
        const std::string name(trim(raw));
        if (name.empty()) {
            continue;
        }
        if (const study::Tag* existing = ws.findTagByName(name)) {
            ids.push_back(existing->id);
            continue;
        }
        const std::string folded = study::foldTagName(name);
        if (std::find(newNames.begin(), newNames.end(), folded) != newNames.end()) {
            continue;
        }
        auto created = commands::createTag(ws, name, *clock_, *ids_);
        if (!created) {
            return failed<std::vector<core::TagId>>(created.error());
        }
        newNames.push_back(folded);
        ids.push_back(created->id);
        for (const document::AnyChange& change : created->command.patch.changes()) {
            changes.push_back(change);
        }
    }
    return ids;
}

Result<void> Planner::setPageTagNames(core::PageId page, std::span<const std::string> names) {
    std::vector<document::AnyChange> changes;
    auto ids = tagIds(names, changes);
    if (!ids) {
        return tl::unexpected(ids.error());
    }
    auto command = commands::setPageTags(workspace(), page, std::move(*ids), *clock_);
    if (!command) {
        return tl::unexpected(command.error());
    }
    for (const document::AnyChange& change : command->patch.changes()) {
        changes.push_back(change);
    }
    return run(document::Command{command->label, document::Patch(std::move(changes))});
}

Result<void> Planner::setTaskTagNames(core::TaskId task, std::span<const std::string> names) {
    const study::Task* current = workspace().findTask(task);
    if (current == nullptr) {
        return makeError(ErrorCode::NotFound, "task " + task.toString() + " does not exist");
    }
    std::vector<document::AnyChange> changes;
    auto ids = tagIds(names, changes);
    if (!ids) {
        return tl::unexpected(ids.error());
    }
    study::Task after = *current;
    after.tags = std::move(*ids);
    auto command = commands::updateTask(workspace(), std::move(after), *clock_);
    if (!command) {
        return tl::unexpected(command.error());
    }
    for (const document::AnyChange& change : command->patch.changes()) {
        changes.push_back(change);
    }
    return run(document::Command{"Change task tags", document::Patch(std::move(changes))});
}

// ---------------------------------------------------------------------------- queries

std::vector<const study::Task*> Planner::allTasks() const {
    const document::Workspace& ws = workspace();
    std::vector<const study::Task*> tasks;
    tasks.reserve(ws.taskCount());
    for (const core::TaskId id : ws.topLevelTasks()) {
        tasks.push_back(ws.findTask(id));
        for (const core::TaskId sub : ws.subtasksOf(id)) {
            tasks.push_back(ws.findTask(sub));
        }
    }
    return tasks;
}

std::vector<const study::Task*>
Planner::tasksInScope(std::optional<core::CourseId> course,
                      std::optional<core::ProjectId> project) const {
    const document::Workspace& ws = workspace();
    if (!course && !project) {
        return allTasks();
    }
    // Top-level tasks of the scope in list order; subtasks follow their task.
    std::vector<const study::Task*> top;
    const auto addTop = [&](std::span<const core::TaskId> ids) {
        for (const core::TaskId id : ids) {
            const study::Task* task = ws.findTask(id);
            if (!task->parent) {
                top.push_back(task);
            }
        }
    };
    if (project) {
        addTop(ws.tasksOfProject(*project));
    } else {
        addTop(ws.tasksOfCourse(*course));
        for (const core::ProjectId p : ws.projectsOfCourse(*course)) {
            addTop(ws.tasksOfProject(p));
        }
    }
    std::sort(top.begin(), top.end(), [](const study::Task* a, const study::Task* b) {
        return a->order != b->order ? a->order < b->order : a->id < b->id;
    });
    top.erase(std::unique(top.begin(), top.end()), top.end());
    std::vector<const study::Task*> tasks;
    tasks.reserve(top.size());
    for (const study::Task* task : top) {
        tasks.push_back(task);
        for (const core::TaskId sub : ws.subtasksOf(task->id)) {
            tasks.push_back(ws.findTask(sub));
        }
    }
    return tasks;
}

std::string Planner::tagNames(std::span<const core::TagId> tags) const {
    std::vector<const study::Tag*> found;
    for (const core::TagId id : tags) {
        if (const study::Tag* tag = workspace().findTag(id)) {
            found.push_back(tag);
        }
    }
    std::sort(found.begin(), found.end(), [](const study::Tag* a, const study::Tag* b) {
        return study::foldTagName(a->name) < study::foldTagName(b->name);
    });
    std::string text;
    for (const study::Tag* tag : found) {
        text += (text.empty() ? "" : ", ") + tag->name;
    }
    return text;
}

} // namespace studyapp::application
