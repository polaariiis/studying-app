#pragma once

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/StudyCommands.hpp>
#include <studyapp/study/Records.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::application {

/// Study & planning use cases of the shell (Phase 7; docs/ARCHITECTURE.md §3, D42).
///
/// Like WorkspaceStructure, every operation builds document commands and executes them
/// through the WorkspaceSession: one undoable, persisted edit each, no other mutation
/// path. Tagging by name creates the tags that do not exist yet in the same edit.
/// Queries read the session's Workspace and never write.
class Planner {
public:
    Planner(WorkspaceSession& session, const core::Clock& clock, core::IdGenerator& ids) noexcept;

    // ---- courses and projects: an empty title gets the next free default ("Course 2").
    [[nodiscard]] core::Result<core::CourseId> createCourse(std::string title = {});
    [[nodiscard]] core::Result<void> updateCourse(study::Course course);
    [[nodiscard]] core::Result<void> renameCourse(core::CourseId course, std::string title);
    [[nodiscard]] core::Result<void> deleteCourse(core::CourseId course);
    [[nodiscard]] core::Result<core::ProjectId> createProject(std::optional<core::CourseId> course,
                                                              std::string title = {});
    [[nodiscard]] core::Result<void> updateProject(study::Project project);
    [[nodiscard]] core::Result<void> renameProject(core::ProjectId project, std::string title);
    [[nodiscard]] core::Result<void> deleteProject(core::ProjectId project);

    // ---- tasks
    [[nodiscard]] core::Result<core::TaskId> createTask(document::commands::NewTask task);
    [[nodiscard]] core::Result<void> updateTask(study::Task task);
    [[nodiscard]] core::Result<void> setTaskDone(core::TaskId task, bool done);
    [[nodiscard]] core::Result<void> deleteTask(core::TaskId task);
    /// One position up (-1) or down (+1) among its siblings; no-op at either end.
    [[nodiscard]] core::Result<void> moveTaskBy(core::TaskId task, int delta);
    [[nodiscard]] core::Result<void> setTaskPageLink(core::TaskId task, core::PageId page,
                                                     bool linked);

    // ---- tags: names are matched ignoring ASCII case; blank names are ignored.
    [[nodiscard]] core::Result<void> setPageTagNames(core::PageId page,
                                                     std::span<const std::string> names);
    [[nodiscard]] core::Result<void> setTaskTagNames(core::TaskId task,
                                                     std::span<const std::string> names);

    // ---- queries
    [[nodiscard]] const document::Workspace& workspace() const noexcept;
    /// Every task (top-level tasks, each followed by its subtasks). O(tasks).
    [[nodiscard]] std::vector<const study::Task*> allTasks() const;
    /// Tasks of a course (and its projects' tasks) or of a project, in list order; all top-
    /// level tasks when both are empty. Subtasks follow their task. O(tasks in scope).
    [[nodiscard]] std::vector<const study::Task*>
    tasksInScope(std::optional<core::CourseId> course,
                 std::optional<core::ProjectId> project) const;
    /// "Exam, Reading" for the given tags, in tag order.
    [[nodiscard]] std::string tagNames(std::span<const core::TagId> tags) const;

    /// Splits "a, b ,A,, c" into {"a", "b", "c"}: trimmed, blanks dropped, repeated names
    /// (ignoring case) kept once.
    [[nodiscard]] static std::vector<std::string> parseTagNames(std::string_view text);

private:
    [[nodiscard]] core::Result<void> run(core::Result<document::Command> command);
    /// Tag ids for `names`, appending the creation of missing tags to `changes`.
    [[nodiscard]] core::Result<std::vector<core::TagId>>
    tagIds(std::span<const std::string> names, std::vector<document::AnyChange>& changes);

    WorkspaceSession* session_;
    const core::Clock* clock_;
    core::IdGenerator* ids_;
};

} // namespace studyapp::application
