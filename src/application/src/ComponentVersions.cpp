#include <studyapp/application/ComponentVersions.hpp>

#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/persistence/SqliteLibrary.hpp>

namespace studyapp::application {

std::vector<ComponentVersion> componentVersions() {
    const auto sqlite = persistence::sqliteLibraryInfo();
    return {
        {std::string(core::build::kProductName), std::string(core::build::kVersion)},
        {"SQLite", std::string(sqlite.version)},
    };
}

} // namespace studyapp::application
