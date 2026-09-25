#pragma once

#include <filesystem>
#include <string>

namespace studyapp::persistence::detail {

/// UTF-8 form of a path, as SQLite and log messages expect (std::filesystem uses UTF-16
/// on Windows).
[[nodiscard]] inline std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace studyapp::persistence::detail
