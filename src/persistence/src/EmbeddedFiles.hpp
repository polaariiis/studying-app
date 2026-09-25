#pragma once

#include <span>
#include <string_view>

namespace studyapp::persistence::detail {

/// A file embedded into the binary at build time (cmake/EmbedFiles.cmake).
struct EmbeddedFile {
    std::string_view name;
    std::string_view content;
};

/// src/persistence/migrations/*.sql, in version order.
[[nodiscard]] std::span<const EmbeddedFile> migrationFiles();

} // namespace studyapp::persistence::detail
