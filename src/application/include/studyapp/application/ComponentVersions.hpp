#pragma once

#include <string>
#include <vector>

namespace studyapp::application {

struct ComponentVersion {
    std::string name;
    std::string version;
};

/// Versions of StudyBoard and of the libraries linked below the UI layer (currently SQLite),
/// for the About dialog and diagnostics. The UI adds the Qt version itself, because Qt is
/// not visible to this layer.
[[nodiscard]] std::vector<ComponentVersion> componentVersions();

} // namespace studyapp::application
