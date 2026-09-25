// Links every Qt-free module into one plain C++ test executable. If any of them acquired a
// Qt, OpenGL or other forbidden dependency, this target would fail to build in the
// `core-only` configuration, which has no Qt at all.

#include <studyapp/application/ComponentVersions.hpp>
#include <studyapp/canvas/Camera.hpp>
#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/document/Workspace.hpp>
#include <studyapp/persistence/SqliteLibrary.hpp>
#include <studyapp/render/Tessellation.hpp>
#include <studyapp/study/Module.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace studyapp {
namespace {

TEST(ModuleLinkTest, QtFreeModulesLinkWithoutQt) {
    const document::Workspace workspace(document::WorkspaceInfo{.name = "link test"});
    EXPECT_EQ(workspace.notebookCount(), 0U);
    EXPECT_EQ(study::moduleName(), "study");
    const std::vector<render::WidthPoint> dot{{.position = {0.0F, 0.0F}, .radius = 1.0F}};
    EXPECT_FALSE(render::tessellatePolyline(dot).empty());
    canvas::Camera camera;
    EXPECT_EQ(camera.viewToWorld(camera.worldToView({3.0, 4.0})), (core::DVec2{3.0, 4.0}));
    EXPECT_FALSE(persistence::sqliteLibraryInfo().version.empty());
    EXPECT_FALSE(application::componentVersions().empty());
    EXPECT_FALSE(core::build::kVersion.empty());
}

} // namespace
} // namespace studyapp
