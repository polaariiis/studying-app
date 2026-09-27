#pragma once

#include <studyapp/document/Commands.hpp>
#include <studyapp/study/Records.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace studyapp::document::commands {

// Study & planning edits (Phase 7; docs/DATA_MODEL.md §5, §6.2). Same contract as the
// commands in Commands.hpp: pure functions returning a Command, one undo step each.
//
// Updates take the edited record: the command keeps what is not the user's to change (id,
// order, creation time), sets the modification time, normalises lists (sorted, unique)
// and returns an empty command when nothing changed. The Workspace validates references.
// Deleting a referenced record clears the references in the same patch, as the database
// does (`ON DELETE SET NULL`): a course's projects and tasks lose their course, a
// project's tasks their project, a tag disappears from its pages and tasks, a deleted
// page from the tasks linking to it; a task's subtasks are deleted with it.

// ---- courses --------------------------------------------------------------------------
[[nodiscard]] core::Result<Created<core::CourseId>> createCourse(const Workspace& workspace,
                                                                 std::string title,
                                                                 const core::Clock& clock,
                                                                 core::IdGenerator& ids);
[[nodiscard]] core::Result<Command> updateCourse(const Workspace& workspace, study::Course course,
                                                 const core::Clock& clock);
[[nodiscard]] core::Result<Command> deleteCourse(const Workspace& workspace, core::CourseId course);
[[nodiscard]] core::Result<Command> moveCourse(const Workspace& workspace, core::CourseId course,
                                               std::size_t index, const core::Clock& clock);

// ---- projects -------------------------------------------------------------------------
[[nodiscard]] core::Result<Created<core::ProjectId>>
createProject(const Workspace& workspace, std::string title, std::optional<core::CourseId> course,
              const core::Clock& clock, core::IdGenerator& ids);
[[nodiscard]] core::Result<Command> updateProject(const Workspace& workspace,
                                                  study::Project project, const core::Clock& clock);
[[nodiscard]] core::Result<Command> deleteProject(const Workspace& workspace,
                                                  core::ProjectId project);
[[nodiscard]] core::Result<Command> moveProject(const Workspace& workspace, core::ProjectId project,
                                                std::size_t index, const core::Clock& clock);

// ---- tasks ----------------------------------------------------------------------------
struct NewTask {
    std::string title;
    std::optional<core::CourseId> course;
    std::optional<core::ProjectId> project;
    std::optional<core::TaskId> parent; ///< a subtask of this top-level task
    std::optional<study::CalendarDate> dueDate;
    std::vector<core::PageId> linkedPages;
};

/// Appends a task to its sibling list (top-level tasks, or the parent's subtasks).
[[nodiscard]] core::Result<Created<core::TaskId>> createTask(const Workspace& workspace,
                                                             NewTask task, const core::Clock& clock,
                                                             core::IdGenerator& ids);
/// Replaces the task's fields. Becoming done records the completion time; reopening
/// clears it. A new parent appends the task to that parent's subtasks.
[[nodiscard]] core::Result<Command> updateTask(const Workspace& workspace, study::Task task,
                                               const core::Clock& clock);
/// Removes the task and its subtasks.
[[nodiscard]] core::Result<Command> deleteTask(const Workspace& workspace, core::TaskId task);
/// Places the task at `index` among its siblings.
[[nodiscard]] core::Result<Command> moveTask(const Workspace& workspace, core::TaskId task,
                                             std::size_t index, const core::Clock& clock);
/// Adds (`linked`) or removes a link from the task to a page.
[[nodiscard]] core::Result<Command> setTaskPageLink(const Workspace& workspace, core::TaskId task,
                                                    core::PageId page, bool linked,
                                                    const core::Clock& clock);

// ---- tags -----------------------------------------------------------------------------
/// Errors: AlreadyExists if a tag of that name (ignoring ASCII case) exists.
[[nodiscard]] core::Result<Created<core::TagId>> createTag(const Workspace& workspace,
                                                           std::string name,
                                                           const core::Clock& clock,
                                                           core::IdGenerator& ids);
[[nodiscard]] core::Result<Command> renameTag(const Workspace& workspace, core::TagId tag,
                                              std::string name);
[[nodiscard]] core::Result<Command> deleteTag(const Workspace& workspace, core::TagId tag);
/// Sets the tags of a page (any order; stored sorted and unique).
[[nodiscard]] core::Result<Command> setPageTags(const Workspace& workspace, core::PageId page,
                                                std::vector<core::TagId> tags,
                                                const core::Clock& clock);

// ---- used by the page removals in Commands.cpp -----------------------------------------
/// Appends updates that remove `pages` from every task linking to them (one update per
/// task). O(links to those pages).
void appendTaskUnlinks(const Workspace& workspace, std::span<const core::PageId> pages,
                       std::vector<AnyChange>& out);

} // namespace studyapp::document::commands
