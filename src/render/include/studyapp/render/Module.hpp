#pragma once

#include <string_view>

namespace studyapp::render {

/// Module anchor.
///
/// This module will contain backend-neutral renderer API, render primitives and tessellation. That
/// work belongs to Phase 4 (see docs/ROADMAP.md). Until then this function is the module's only
/// symbol: it keeps the `studyapp_render` target compiling and linking with its final dependencies,
/// so module boundaries are enforced from Phase 1 on. It is removed once real code exists.
[[nodiscard]] std::string_view moduleName() noexcept;

} // namespace studyapp::render
