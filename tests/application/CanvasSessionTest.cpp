// Phase 4 integration: the canvas engine driving a real WorkspaceSession (SQLite) through
// the same DocumentPort the application uses. Draw, move, delete, erase, undo/redo →
// continuous autosave → close → reopen → identical ink, ids and order.

#include <studyapp/application/StartPage.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/RecordingRenderer.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <vector>

namespace studyapp::application {
namespace {

class NoLocks final : public WorkspaceLocker {
public:
    core::Result<LockStatus> inspect(const std::filesystem::path&) override { return LockStatus{}; }
    core::Result<std::unique_ptr<WorkspaceLock>> acquire(const std::filesystem::path&,
                                                         bool) override {
        return std::unique_ptr<WorkspaceLock>(std::make_unique<Held>());
    }

private:
    class Held final : public WorkspaceLock {};
};

/// The application's DocumentPort shape (ui::SessionDocumentPort), without Qt.
class SessionPort final : public canvas::DocumentPort {
public:
    explicit SessionPort(WorkspaceSession& session) : session_(&session) {}
    const document::Workspace& workspace() const override { return session_->workspace(); }
    core::Result<void> execute(document::Command command) override {
        return session_->execute(std::move(command));
    }
    bool isReadOnly() const override { return session_->isReadOnly(); }

private:
    WorkspaceSession* session_;
};

struct CanvasSessionTest : ::testing::Test {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds ids;
    NoLocks locker;
    std::unique_ptr<WorkspaceSession> session;
    std::unique_ptr<SessionPort> port;
    std::unique_ptr<canvas::CanvasController> controller;
    core::PageId page;
    std::uint64_t timestampUs = 0;

    SessionServices services() { return {clock, ids, locker}; }

    void attach() {
        port = std::make_unique<SessionPort>(*session);
        controller = std::make_unique<canvas::CanvasController>(*port, ids);
        session->setPatchListener(
            [this](const document::Patch& patch) { controller->onDocumentChanged(patch); });
        controller->setViewport({1000, 800}, 1.0);
        controller->setPage(page);
    }

    void SetUp() override {
        auto created = WorkspaceSession::create(dir / "ws", "Canvas", services());
        ASSERT_OK(created);
        session = std::move(*created);
        auto start = ensureStartPage(*session, clock, ids);
        ASSERT_OK(start);
        page = *start;
        attach();
    }

    void reopen() {
        session->setPatchListener({});
        controller.reset();
        port.reset();
        ASSERT_OK(session->close());
        session.reset();
        auto opened = WorkspaceSession::open(dir / "ws", {}, services());
        ASSERT_OK(opened);
        session = std::move(*opened);
        attach();
    }

    void pointer(canvas::PointerPhase phase, core::DVec2 at) {
        timestampUs += 8'000;
        controller->onPointer({.phase = phase, .viewPos = at, .timestampUs = timestampUs});
    }
    void drag(core::DVec2 from, core::DVec2 to) {
        pointer(canvas::PointerPhase::Down, from);
        for (int i = 1; i <= 8; ++i) {
            pointer(canvas::PointerPhase::Move, from + (to - from) * (i / 8.0));
        }
        pointer(canvas::PointerPhase::Up, to);
    }

    std::vector<core::ElementId> inkIds() const {
        std::vector<core::ElementId> out;
        for (const core::LayerId layer : session->workspace().layersOf(page)) {
            const auto layerIds = session->workspace().elementsOf(layer);
            out.insert(out.end(), layerIds.begin(), layerIds.end());
        }
        return out;
    }
};

TEST_F(CanvasSessionTest, InkSurvivesCloseAndReopen) {
    // Draw five strokes (one wavy), with zoom and pan in between.
    drag({100, 100}, {400, 120});
    controller->onPointer(
        {.phase = canvas::PointerPhase::Down, .viewPos = {100, 300}, .timestampUs = 1});
    for (int i = 1; i < 40; ++i) {
        controller->onPointer({.phase = canvas::PointerPhase::Move,
                               .viewPos = {100.0 + i * 8, 300.0 + 30 * std::sin(i * 0.4)},
                               .pressure = 0.3F + 0.017F * static_cast<float>(i),
                               .timestampUs = 1 + static_cast<std::uint64_t>(i) * 8'000});
    }
    controller->onPointer(
        {.phase = canvas::PointerPhase::Up, .viewPos = {420, 300}, .timestampUs = 400'000});
    controller->onWheel({.viewPos = {500, 400}, .angleDelta = {0, 240}});
    drag({200, 500}, {600, 520});
    controller->panBy({-150, 40});
    drag({300, 600}, {700, 650});
    drag({100, 700}, {300, 700});
    ASSERT_EQ(inkIds().size(), 5U);

    // Move two strokes, delete one, erase one, undo the erase, redo nothing.
    controller->setTool(canvas::ToolKind::Select);
    controller->selectAll();
    const auto all = inkIds();
    controller->clearSelection();
    const core::DVec2 firstView = controller->camera().worldToView(
        document::worldBounds(*session->workspace().findElement(all[0])).center());
    drag(firstView, firstView + core::DVec2{50, 25}); // select + move stroke 1 as one gesture
    ASSERT_OK(controller->deleteSelection());         // then delete it
    ASSERT_EQ(inkIds().size(), 4U);
    ASSERT_OK(session->undo()); // it is back (moved)
    ASSERT_EQ(inkIds().size(), 5U);

    controller->setTool(canvas::ToolKind::Eraser);
    const core::DRect lastBounds = document::worldBounds(*session->workspace().findElement(all[4]));
    const core::DVec2 eraseAt = controller->camera().worldToView(lastBounds.center());
    drag(eraseAt - core::DVec2{0, 20}, eraseAt + core::DVec2{0, 20});
    ASSERT_EQ(inkIds().size(), 4U);

    // Autosave is continuous: nothing is pending, nothing failed.
    EXPECT_EQ(session->pendingWriteCount(), 0U);
    EXPECT_FALSE(session->lastWriteError().has_value());

    const document::Workspace before = session->workspace();
    const std::vector<core::ElementId> orderBefore = inkIds();
    reopen();
    EXPECT_TRUE(session->workspace() == before) << "reopened ink differs";
    EXPECT_EQ(inkIds(), orderBefore);                             // same ids, same order
    EXPECT_EQ(session->workspace().findElement(all[4]), nullptr); // erased stays erased
    for (const core::ElementId id : orderBefore) {
        const auto& a = std::get<document::Stroke>(before.findElement(id)->payload);
        const auto& b = std::get<document::Stroke>(session->workspace().findElement(id)->payload);
        EXPECT_EQ(*a.points, *b.points); // bit-exact point data
    }

    // The reopened canvas shows all of it.
    testing::RecordingRenderer renderer;
    ASSERT_OK(renderer.initialize());
    controller->resetView();
    controller->zoomBy(0.25);
    const render::RenderFrame frame = controller->buildFrame(renderer);
    EXPECT_EQ(frame.content.size(), orderBefore.size());
}

TEST_F(CanvasSessionTest, StartPageIsCreatedOnceAsOneUndoStep) {
    EXPECT_EQ(session->history().undoCount(), 1U);
    EXPECT_EQ(session->history().nextUndo()->label, "Create start page");
    const document::PageInfo& info = *session->workspace().findPage(page);
    EXPECT_EQ(info.background.pattern, document::BackgroundPattern::Dots);
    auto again = ensureStartPage(*session, clock, ids);
    ASSERT_OK(again);
    EXPECT_EQ(*again, page);
    EXPECT_EQ(session->workspace().pageCount(), 1U);
}

TEST_F(CanvasSessionTest, ListenerSeesUndoAndRedo) {
    std::vector<std::size_t> sizes;
    session->setPatchListener([&](const document::Patch& patch) {
        controller->onDocumentChanged(patch);
        sizes.push_back(patch.size());
    });
    drag({10, 10}, {100, 10});
    ASSERT_OK(session->undo());
    ASSERT_OK(session->redo());
    EXPECT_EQ(sizes, (std::vector<std::size_t>{1, 1, 1}));
    EXPECT_EQ(controller->scene().elementCount(), 1U);
}

} // namespace
} // namespace studyapp::application
