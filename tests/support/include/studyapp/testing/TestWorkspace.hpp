#pragma once

// Document-model test fixture shared by the document, persistence and application tests.

#include <studyapp/document/Commands.hpp>
#include <studyapp/document/Editor.hpp>
#include <studyapp/document/Workspace.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace studyapp::document::test {

/// A workspace with an editor, a manual clock and sequential ids. Helper methods run
/// commands through the editor (so they are recorded in the undo history) and fail the
/// test if a command fails.
struct TestWorkspace {
    testing::ManualClock clock;
    testing::SequentialIds ids;
    Workspace workspace{WorkspaceInfo{
        .id = core::WorkspaceId::generate(ids), .name = "Test workspace", .created = clock.now()}};
    Editor editor{workspace};

    template <class Id>
    Id run(core::Result<commands::Created<Id>> created) {
        EXPECT_TRUE(created.has_value()) << (created ? "" : created.error().message);
        if (!created) {
            return Id{};
        }
        const auto executed = editor.execute(std::move(created->command));
        EXPECT_TRUE(executed.has_value()) << (executed ? "" : executed.error().message);
        return created->id;
    }

    void run(core::Result<Command> command) {
        ASSERT_TRUE(command.has_value()) << command.error().message;
        const auto executed = editor.execute(std::move(*command));
        ASSERT_TRUE(executed.has_value()) << executed.error().message;
    }

    core::NotebookId addNotebook(std::string title) {
        return run(commands::createNotebook(workspace, std::move(title), clock, ids));
    }
    core::SectionId addSection(core::NotebookId notebook, std::string title) {
        return run(commands::createSection(workspace, notebook, std::move(title), clock, ids));
    }
    core::PageId addPage(core::SectionId section, std::string title,
                         const commands::PageOptions& options = {}) {
        return run(commands::createPage(workspace, section, std::move(title), options, clock, ids));
    }
    core::LayerId addLayer(core::PageId page, std::string name) {
        return run(commands::createLayer(workspace, page, std::move(name), ids));
    }
    core::ElementId addElement(core::LayerId layer, ElementPayload payload,
                               Transform transform = {}) {
        return run(commands::createElement(
            workspace, layer,
            commands::NewElement{.transform = transform, .payload = std::move(payload)}, ids));
    }

    core::LayerId firstLayer(core::PageId page) const {
        const auto layers = workspace.layersOf(page);
        EXPECT_FALSE(layers.empty());
        return layers.empty() ? core::LayerId{} : layers.front();
    }

    /// Builds notebook → section → page (with its first layer) and returns the layer.
    core::LayerId addPath() {
        const auto notebook = addNotebook("Notebook");
        const auto section = addSection(notebook, "Section");
        return firstLayer(addPage(section, "Page"));
    }
};

/// Milliseconds since the epoch. Tests compare timestamps through this instead of comparing
/// time points directly: printing a std::chrono time point (GoogleTest does so on failure)
/// needs floating-point std::to_chars, which Apple's libc++ only provides from macOS 13.3,
/// while the deployment target is 13.0 (docs/ARCHITECTURE.md §17).
inline auto millis(core::Timestamp t) {
    return t.time_since_epoch().count();
}

inline Stroke makeStroke(std::vector<StrokePoint> points = {{0, 0, 1}, {10, 5, 0.5F}}) {
    return Stroke{.points = makeStrokePoints(std::move(points))};
}

inline TextBox makeText(std::string text) {
    return TextBox{.size = {120.0F, 40.0F}, .text = std::move(text)};
}

inline Connector makeConnector(std::optional<core::ElementId> from,
                               std::optional<core::ElementId> to) {
    return Connector{.start = {.position = {0, 0}, .attachedTo = from},
                     .end = {.position = {100, 0}, .attachedTo = to}};
}

} // namespace studyapp::document::test
