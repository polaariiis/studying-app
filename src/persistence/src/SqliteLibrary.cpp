#include <studyapp/persistence/SqliteLibrary.hpp>

#include <sqlite3.h>

namespace studyapp::persistence {

SqliteLibraryInfo sqliteLibraryInfo() noexcept {
    return SqliteLibraryInfo{
        .version = sqlite3_libversion(),
        .versionNumber = sqlite3_libversion_number(),
        .hasFts5 = sqlite3_compileoption_used("ENABLE_FTS5") != 0,
        .hasRtree = sqlite3_compileoption_used("ENABLE_RTREE") != 0,
        .threadSafe = sqlite3_threadsafe() != 0,
    };
}

} // namespace studyapp::persistence
