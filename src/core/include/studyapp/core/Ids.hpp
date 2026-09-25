#pragma once

#include <studyapp/core/Id.hpp>

namespace studyapp::core {

// Identifier types of all persistent entities. They live in `core` so that modules can
// refer to each other's entities by id without depending on each other (e.g. a study
// task links to pages without `study` depending on `document`). The entities themselves
// are introduced by later phases; see docs/DATA_MODEL.md.

using WorkspaceId = Id<struct WorkspaceTag>;
using NotebookId = Id<struct NotebookTag>;
using SectionId = Id<struct SectionTag>;
using PageId = Id<struct PageTag>;
using LayerId = Id<struct LayerTag>;
using ElementId = Id<struct ElementTag>;
using AssetId = Id<struct AssetTag>;
using TagId = Id<struct TagTag>;
using CourseId = Id<struct CourseTag>;
using ProjectId = Id<struct ProjectTag>;
using TaskId = Id<struct TaskTag>;

} // namespace studyapp::core
