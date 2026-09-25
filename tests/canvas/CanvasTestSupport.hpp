#pragma once

#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/canvas/DocumentPort.hpp>
#include <studyapp/render/Renderer.hpp>
#include <studyapp/testing/RecordingRenderer.hpp>
#include <studyapp/testing/TestWorkspace.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace studyapp::canvas::test {

using testing::RecordingRenderer;

/// DocumentPort over the Phase 2 Editor (no persistence). Every applied patch, including
/// undo/redo, is reported to the attached controller — as the application's session does.
class EditorPort final : public DocumentPort {
public:
    explicit EditorPort(document::test::TestWorkspace& doc) : doc_(&doc) {}

    void attach(CanvasController& controller) { controller_ = &controller; }

    const document::Workspace& workspace() const override { return doc_->workspace; }
    bool isReadOnly() const override { return readOnly; }
    core::Result<void> execute(document::Command command) override {
        ++executed;
        document::Patch patch = command.patch;
        if (auto done = doc_->editor.execute(std::move(command)); !done) {
            return done;
        }
        notify(patch);
        return {};
    }
    core::Result<void> undo() {
        const document::Command* entry = doc_->editor.history().nextUndo();
        const document::Patch inverse =
            entry != nullptr ? entry->patch.inverted() : document::Patch{};
        if (auto done = doc_->editor.undo(); !done) {
            return done;
        }
        notify(inverse);
        return {};
    }
    core::Result<void> redo() {
        const document::Command* entry = doc_->editor.history().nextRedo();
        const document::Patch patch = entry != nullptr ? entry->patch : document::Patch{};
        if (auto done = doc_->editor.redo(); !done) {
            return done;
        }
        notify(patch);
        return {};
    }

    bool readOnly = false;
    std::size_t executed = 0;

private:
    void notify(const document::Patch& patch) {
        if (controller_ != nullptr) {
            controller_->onDocumentChanged(patch);
        }
    }

    document::test::TestWorkspace* doc_;
    CanvasController* controller_ = nullptr;
};

/// A controller on a fresh page with an 800×600 viewport at DPR 1 and the camera origin
/// at the top-left (so view == world at zoom 1).
struct CanvasFixture {
    document::test::TestWorkspace doc;
    EditorPort port{doc};
    CanvasController controller{port, doc.ids};
    RecordingRenderer renderer;
    core::PageId page;
    core::LayerId layer;

    explicit CanvasFixture(const document::commands::PageOptions& options = {}) {
        const auto notebook = doc.addNotebook("Notebook");
        const auto section = doc.addSection(notebook, "Section");
        page = doc.addPage(section, "Page", options);
        layer = doc.firstLayer(page);
        port.attach(controller);
        controller.setViewport({800.0, 600.0}, 1.0);
        controller.setPage(page);
        (void)renderer.initialize();
    }

    void pointer(PointerPhase phase, core::DVec2 view,
                 PointerButton button = PointerButton::Primary, Modifiers modifiers = {},
                 float pressure = 1.0F, PointerDevice device = PointerDevice::Mouse) {
        timestampUs += 8'000; // 125 Hz
        controller.onPointer(PointerEvent{.phase = phase,
                                          .device = device,
                                          .button = button,
                                          .viewPos = view,
                                          .pressure = pressure,
                                          .modifiers = modifiers,
                                          .timestampUs = timestampUs});
    }

    /// Press at `from`, move in `steps` straight steps to `to`, release.
    void drag(core::DVec2 from, core::DVec2 to, int steps = 10,
              PointerButton button = PointerButton::Primary, Modifiers modifiers = {}) {
        pointer(PointerPhase::Down, from, button, modifiers);
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            pointer(PointerPhase::Move, from + (to - from) * t, button, modifiers);
        }
        pointer(PointerPhase::Up, to, button, modifiers);
    }

    void click(core::DVec2 at, Modifiers modifiers = {}) {
        pointer(PointerPhase::Down, at, PointerButton::Primary, modifiers);
        pointer(PointerPhase::Up, at, PointerButton::Primary, modifiers);
    }

    /// Elements of the page's first layer, in draw order.
    std::vector<core::ElementId> elements() const {
        const auto ids = doc.workspace.elementsOf(layer);
        return {ids.begin(), ids.end()};
    }

    const document::Element& element(core::ElementId id) const {
        return *doc.workspace.findElement(id);
    }

    render::RenderFrame frame() {
        const render::RenderFrame built = controller.buildFrame(renderer);
        renderer.render(built);
        return built;
    }

    std::uint64_t timestampUs = 1'000'000;
};

} // namespace studyapp::canvas::test
