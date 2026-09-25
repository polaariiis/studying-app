#pragma once

#include <string_view>

namespace studyapp::document {

/// Module anchor.
///
/// This module will contain workspace catalog, page documents, elements, patches, commands,
/// undo/redo. That work belongs to Phase 2 (see docs/ROADMAP.md). Until then this function is the
/// module's only symbol: it keeps the `studyapp_document` target compiling and linking with its
/// final dependencies, so module boundaries are enforced from Phase 1 on. It is removed once real
/// code exists.
[[nodiscard]] std::string_view moduleName() noexcept;

} // namespace studyapp::document
