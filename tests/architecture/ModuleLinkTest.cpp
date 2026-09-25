// Links every Qt-free module into one plain C++ test executable. If any of them acquired a
// Qt, OpenGL or other forbidden dependency, this target would fail to build in the
// `core-only` configuration, which has no Qt at all.

#include <studyapp/application/ComponentVersions.hpp>
#include <studyapp/canvas/Module.hpp>
#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/document/Module.hpp>
#include <studyapp/persistence/SqliteLibrary.hpp>
#include <studyapp/render/Module.hpp>
#include <studyapp/study/Module.hpp>

#include <gtest/gtest.h>

namespace studyapp {
namespace {

TEST(ModuleLinkTest, QtFreeModulesLinkWithoutQt) {
    EXPECT_EQ(document::moduleName(), "document");
    EXPECT_EQ(study::moduleName(), "study");
    EXPECT_EQ(render::moduleName(), "render");
    EXPECT_EQ(canvas::moduleName(), "canvas");
    EXPECT_FALSE(persistence::sqliteLibraryInfo().version.empty());
    EXPECT_FALSE(application::componentVersions().empty());
    EXPECT_FALSE(core::build::kVersion.empty());
}

} // namespace
} // namespace studyapp
