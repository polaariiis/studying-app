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

#include <filesystem>
#include <fstream>
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

    controller->setToolSettings({.eraser = canvas::EraserMode::WholeStroke});
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

TEST_F(CanvasSessionTest, PenStylesArePersistedWithEachStroke) {
    // Phase 6, step 1: each stroke carries the tool settings it was drawn with.
    const core::Color blue = core::Color::fromRgba(0x2F, 0x5F, 0xA8);
    const core::Color red = core::Color::fromRgba(0xB3, 0x36, 0x2F, 200);
    controller->setToolSettings({.pen = {.color = blue, .width = 4.0F}});
    drag({100, 100}, {400, 120});
    controller->setToolSettings({.pen = {.color = red, .width = 1.2F}});
    drag({100, 200}, {400, 220});
    ASSERT_EQ(inkIds().size(), 2U);
    const auto strokeIds = inkIds();
    reopen();
    const auto& first =
        std::get<document::Stroke>(session->workspace().findElement(strokeIds[0])->payload);
    const auto& second =
        std::get<document::Stroke>(session->workspace().findElement(strokeIds[1])->payload);
    EXPECT_EQ(first.color, blue);
    EXPECT_FLOAT_EQ(first.baseWidth, 4.0F);
    EXPECT_EQ(second.color, red); // including alpha
    EXPECT_FLOAT_EQ(second.baseWidth, 1.2F);
    EXPECT_EQ(first.brush, document::Brush::Pen);
}

TEST_F(CanvasSessionTest, HighlighterStrokesSurviveReopenAsDrawn) {
    // Phase 6, step 2: a highlighter stroke is an ordinary stroke whose brush, translucent
    // colour and width are document data; reopening needs no tool state at all.
    const core::Color green = core::Color::fromRgba(0x8C, 0xCF, 0x7E, 0x73);
    controller->setToolSettings({.highlighter = {.color = green, .width = 22.0F}});
    controller->setTool(canvas::ToolKind::Highlighter);
    drag({100, 100}, {400, 130});
    controller->setTool(canvas::ToolKind::Pen);
    drag({100, 200}, {400, 200});
    controller->setTool(canvas::ToolKind::Highlighter);
    canvas::PenStyle narrow = canvas::kDefaultHighlighter; // the default yellow
    narrow.width = 8.0F;
    controller->setToolSettings({.highlighter = narrow});
    drag({100, 300}, {400, 280});
    ASSERT_EQ(inkIds().size(), 3U);
    const auto strokeIds = inkIds();
    const document::Workspace before = session->workspace();
    EXPECT_EQ(session->pendingWriteCount(), 0U);

    reopen(); // a fresh controller: default tool, default settings
    EXPECT_TRUE(session->workspace() == before) << "reopened ink differs";
    const auto strokeAt = [&](std::size_t i) -> const document::Stroke& {
        return std::get<document::Stroke>(session->workspace().findElement(strokeIds[i])->payload);
    };
    EXPECT_EQ(strokeAt(0).brush, document::Brush::Highlighter);
    EXPECT_EQ(strokeAt(0).color, green); // alpha included
    EXPECT_FLOAT_EQ(strokeAt(0).baseWidth, 22.0F);
    EXPECT_EQ(strokeAt(1).brush, document::Brush::Pen);
    EXPECT_EQ(strokeAt(1).color, core::Color::black());
    EXPECT_EQ(strokeAt(2).brush, document::Brush::Highlighter);
    EXPECT_EQ(strokeAt(2).color, canvas::kDefaultHighlighter.color);
    EXPECT_FLOAT_EQ(strokeAt(2).baseWidth, 8.0F);
    for (std::size_t i = 0; i < strokeIds.size(); ++i) {
        const auto& drawn = std::get<document::Stroke>(before.findElement(strokeIds[i])->payload);
        EXPECT_EQ(*drawn.points, *strokeAt(i).points); // bit-exact geometry and pressure
    }

    // The reopened canvas draws them in their stored colours.
    testing::RecordingRenderer renderer;
    ASSERT_OK(renderer.initialize());
    const render::RenderFrame frame = controller->buildFrame(renderer);
    ASSERT_EQ(frame.content.size(), 3U);
    EXPECT_EQ(frame.content[0].color, green);
    EXPECT_EQ(frame.content[1].color, core::Color::black());
    EXPECT_EQ(frame.content[2].color, canvas::kDefaultHighlighter.color);

    // Undo after reopen is not available (history is per session), but a new highlighter
    // stroke is one undo step again.
    const std::size_t steps = session->history().undoCount();
    controller->setTool(canvas::ToolKind::Highlighter);
    drag({100, 400}, {400, 400});
    EXPECT_EQ(session->history().undoCount(), steps + 1);
    ASSERT_OK(session->undo());
    EXPECT_EQ(inkIds().size(), 3U);
}

TEST_F(CanvasSessionTest, PartiallyErasedInkSurvivesReopen) {
    // Phase 6, step 3: the pieces a partial erase leaves are ordinary strokes.
    drag({100, 100}, {500, 100});
    controller->setTool(canvas::ToolKind::Highlighter);
    drag({100, 200}, {500, 200});
    controller->setTool(canvas::ToolKind::Eraser);
    drag({300, 60}, {300, 240}); // through both
    drag({150, 60}, {150, 240}); // and again
    ASSERT_EQ(inkIds().size(), 6U);
    EXPECT_EQ(session->pendingWriteCount(), 0U);
    const document::Workspace before = session->workspace();
    const auto order = inkIds();
    reopen();
    EXPECT_TRUE(session->workspace() == before) << "reopened ink differs";
    EXPECT_EQ(inkIds(), order);
}

TEST_F(CanvasSessionTest, ShapesSurviveReopen) {
    // Phase 6, step 4: every shape kind, including the new arrow, is stored as drawn.
    controller->setTool(canvas::ToolKind::Shape);
    int y = 100;
    for (const auto kind : {document::ShapeKind::Rectangle, document::ShapeKind::Ellipse,
                            document::ShapeKind::Line, document::ShapeKind::Arrow}) {
        controller->setToolSettings({.shape = {.kind = kind,
                                               .color = core::Color::fromRgba(0xB3, 0x36, 0x2F),
                                               .width = 4.0F,
                                               .fill = kind == document::ShapeKind::Ellipse}});
        drag({100, static_cast<double>(y)}, {300, static_cast<double>(y + 60)});
        y += 100;
    }
    ASSERT_EQ(inkIds().size(), 4U);
    const document::Workspace before = session->workspace();
    reopen();
    EXPECT_TRUE(session->workspace() == before) << "reopened shapes differ";
    const auto shapeIds = inkIds();
    EXPECT_EQ(
        std::get<document::Shape>(session->workspace().findElement(shapeIds[3])->payload).kind,
        document::ShapeKind::Arrow);
    EXPECT_TRUE(std::get<document::Shape>(session->workspace().findElement(shapeIds[1])->payload)
                    .fillColor.has_value());
}

TEST_F(CanvasSessionTest, TextBoxesSurviveReopen) {
    // Phase 6, step 5: text is plain UTF-8 in the document (no layout needed to reopen).
    controller->setTool(canvas::ToolKind::Text);
    pointer(canvas::PointerPhase::Down, {100, 100});
    pointer(canvas::PointerPhase::Up, {100, 100});
    const std::string text =
        "Line one\nzwei \xC3\xA4\xC3\xB6\xC3\xBC \xE2\x80\x94 \xF0\x9F\x93\x9A";
    ASSERT_OK(controller->finishTextEdit(text));
    ASSERT_EQ(inkIds().size(), 1U);
    EXPECT_EQ(session->pendingWriteCount(), 0U);
    const document::Workspace before = session->workspace();
    reopen();
    EXPECT_TRUE(session->workspace() == before);
    EXPECT_EQ(
        std::get<document::TextBox>(session->workspace().findElement(inkIds()[0])->payload).text,
        text);
}

TEST_F(CanvasSessionTest, ImagesReferenceOneStoredAssetAndSurviveReopen) {
    // Phase 6, step 6: the image's bytes live once in the content-addressed asset store;
    // elements reference the asset by id.
    const auto file = dir / "photo.png";
    {
        std::ofstream out(file, std::ios::binary);
        out << "\x89PNG pretend image bytes";
    }
    auto first = session->importAsset(file, "image/png");
    ASSERT_OK(first);
    auto again = session->importAsset(file, "image/png"); // identical content: same asset
    ASSERT_OK(again);
    EXPECT_EQ(*first, *again);
    ASSERT_OK(controller->insertImage(*first, {320, 240}));
    ASSERT_OK(controller->insertImage(*again, {64, 48}));
    ASSERT_EQ(inkIds().size(), 2U);
    const document::Workspace before = session->workspace();
    reopen();
    EXPECT_TRUE(session->workspace() == before);
    for (const core::ElementId id : inkIds()) {
        EXPECT_EQ(std::get<document::Image>(session->workspace().findElement(id)->payload).asset,
                  *first);
    }
    auto stored = session->assetPath(*first);
    ASSERT_OK(stored);
    EXPECT_TRUE(std::filesystem::exists(*stored));
}

TEST_F(CanvasSessionTest, ConnectorsKeepTheirAttachmentsAcrossReopen) {
    controller->setTool(canvas::ToolKind::Shape);
    drag({100, 100}, {200, 160});
    drag({400, 100}, {500, 160});
    controller->setTool(canvas::ToolKind::Connector);
    drag({150, 130}, {450, 130});
    ASSERT_EQ(inkIds().size(), 3U);
    const document::Workspace before = session->workspace();
    reopen();
    EXPECT_TRUE(session->workspace() == before);
    const auto ids3 = inkIds();
    const auto& link =
        std::get<document::Connector>(session->workspace().findElement(ids3[2])->payload);
    EXPECT_EQ(link.start.attachedTo, ids3[0]);
    EXPECT_EQ(link.end.attachedTo, ids3[1]);
    EXPECT_EQ(session->workspace().connectorsAttachedTo(ids3[0]).size(), 1U);
}

TEST_F(CanvasSessionTest, ResizedElementsSurviveReopen) {
    controller->setTool(canvas::ToolKind::Shape);
    drag({100, 100}, {200, 160});
    controller->setTool(canvas::ToolKind::Select);
    pointer(canvas::PointerPhase::Down, {130, 100});
    pointer(canvas::PointerPhase::Up, {130, 100});
    drag({200, 160}, {300, 260}); // the bottom-right handle
    ASSERT_EQ(inkIds().size(), 1U);
    EXPECT_EQ(
        std::get<document::Shape>(session->workspace().findElement(inkIds()[0])->payload).size,
        (core::Vec2{200, 160}));
    const document::Workspace before = session->workspace();
    reopen();
    EXPECT_TRUE(session->workspace() == before);
}

TEST_F(CanvasSessionTest, PastedMixedSelectionSurvivesReopen) {
    // Phase 6, step 9: stroke, shape, text, image and a connector between the shape and the
    // image, copied and pasted as one command; the copies reference the same stored asset.
    controller->setTool(canvas::ToolKind::Pen);
    drag({50, 400}, {250, 420});
    controller->setTool(canvas::ToolKind::Shape);
    drag({100, 100}, {200, 160});
    controller->setTool(canvas::ToolKind::Text);
    pointer(canvas::PointerPhase::Down, {100, 250});
    pointer(canvas::PointerPhase::Up, {100, 250});
    ASSERT_OK(controller->finishTextEdit("pasted text"));
    const auto file = dir / "picture.png";
    {
        std::ofstream out(file, std::ios::binary);
        out << "\x89PNG pretend image bytes";
    }
    auto asset = session->importAsset(file, "image/png");
    ASSERT_OK(asset);
    ASSERT_OK(controller->insertImage(*asset, {64, 48}));
    const core::DVec2 image = controller->camera().worldToView(
        document::worldBounds(*session->workspace().findElement(inkIds().back())).center());
    controller->setTool(canvas::ToolKind::Connector);
    drag({150, 130}, image);
    ASSERT_EQ(inkIds().size(), 5U);
    const std::vector<core::ElementId> originals = inkIds();
    ASSERT_TRUE(
        std::get<document::Connector>(session->workspace().findElement(originals[4])->payload)
            .end.attachedTo.has_value());

    controller->setTool(canvas::ToolKind::Select);
    controller->selectAll();
    ASSERT_TRUE(controller->copySelection());
    const std::size_t steps = session->history().undoCount();
    ASSERT_OK(controller->paste());
    EXPECT_EQ(session->history().undoCount(), steps + 1);
    ASSERT_EQ(inkIds().size(), 10U);
    EXPECT_EQ(session->pendingWriteCount(), 0U);
    const document::Workspace before = session->workspace();
    reopen();
    EXPECT_TRUE(session->workspace() == before) << "reopened copies differ";

    const auto ids10 = inkIds();
    const std::vector<core::ElementId> copies(ids10.begin() + 5, ids10.end());
    std::set<core::ElementId> distinct(ids10.begin(), ids10.end());
    EXPECT_EQ(distinct.size(), 10U); // new ids
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(session->workspace().findElement(copies[i])->kind(),
                  session->workspace().findElement(originals[i])->kind());
    }
    EXPECT_EQ(std::get<document::Image>(session->workspace().findElement(copies[3])->payload).asset,
              *asset); // the stored file is shared
    const auto& link =
        std::get<document::Connector>(session->workspace().findElement(copies[4])->payload);
    const auto& original =
        std::get<document::Connector>(session->workspace().findElement(originals[4])->payload);
    EXPECT_EQ(link.start.attachedTo, copies[1]);
    EXPECT_EQ(link.end.attachedTo, copies[3]);
    EXPECT_EQ(original.start.attachedTo, originals[1]);
    EXPECT_EQ(original.end.attachedTo, originals[3]);
}

TEST_F(CanvasSessionTest, MixedEditsUndoRedoAndReopenRestoreExactState) {
    // Every Phase 6 edit on one page, each one undo step: undoing walks back through each
    // exact earlier state and redoing forward again; the result survives reopening, a
    // further edit, and reopening again.
    std::vector<document::Workspace> states{session->workspace()};
    const std::size_t base = session->history().undoCount(); // the start page
    const auto step = [&](const char* what) {
        EXPECT_EQ(session->history().undoCount(), base + states.size()) << what;
        EXPECT_EQ(session->pendingWriteCount(), 0U) << what;
        states.push_back(session->workspace());
    };
    controller->setTool(canvas::ToolKind::Pen);
    drag({100, 600}, {500, 600});
    step("pen");
    controller->setTool(canvas::ToolKind::Highlighter);
    drag({100, 650}, {500, 650});
    step("highlighter");
    controller->setTool(canvas::ToolKind::Shape);
    drag({100, 100}, {200, 160});
    step("rectangle");
    controller->setToolSettings({.shape = {.kind = document::ShapeKind::Arrow}});
    drag({600, 100}, {700, 200});
    step("arrow");
    controller->setTool(canvas::ToolKind::Connector);
    drag({300, 300}, {400, 300}); // free at both ends
    step("connector");
    controller->setTool(canvas::ToolKind::Text);
    pointer(canvas::PointerPhase::Down, {100, 250});
    pointer(canvas::PointerPhase::Up, {100, 250});
    ASSERT_OK(controller->finishTextEdit("mixed \xE2\x9C\x93\nsecond line"));
    step("text");
    const auto file = dir / "mixed.png";
    {
        std::ofstream out(file, std::ios::binary);
        out << "\x89PNG pretend image bytes";
    }
    auto asset = session->importAsset(file, "image/png");
    ASSERT_OK(asset);
    ASSERT_OK(controller->insertImage(*asset, {64, 48}));
    step("image");
    const core::ElementId link = inkIds()[4];
    const core::ElementId image = inkIds()[6];
    controller->setTool(canvas::ToolKind::Connector);
    drag({400, 300}, controller->camera().worldToView(
                         document::worldBounds(*session->workspace().findElement(image)).center()));
    ASSERT_EQ(std::get<document::Connector>(session->workspace().findElement(link)->payload)
                  .end.attachedTo,
              image); // attached to an element above it
    step("re-attach");
    controller->setTool(canvas::ToolKind::Eraser);
    drag({300, 560}, {300, 690}); // cuts the ink and the highlighter
    ASSERT_EQ(inkIds().size(), 9U);
    step("erase");
    controller->setTool(canvas::ToolKind::Select);
    pointer(canvas::PointerPhase::Down, {130, 100});
    pointer(canvas::PointerPhase::Up, {130, 100});
    drag({200, 160}, {250, 200}); // the rectangle's bottom-right handle
    step("resize");
    drag({130, 100}, {160, 130}); // the rectangle's edge
    step("move");
    controller->selectAll();
    ASSERT_TRUE(controller->copySelection());
    ASSERT_OK(controller->paste());
    step("paste");
    const auto pasted = inkIds();
    ASSERT_EQ(pasted.size(), 18U);
    // pen, its piece, highlighter, its piece, rectangle, arrow, connector, text, image; then
    // their copies in the same order.
    const auto& copy =
        std::get<document::Connector>(session->workspace().findElement(pasted[15])->payload);
    EXPECT_EQ(copy.end.attachedTo, pasted[17]); // the connector's copy joins the image's copy
    ASSERT_OK(controller->cutSelection());
    step("cut");
    ASSERT_OK(controller->paste());
    step("paste again");

    for (std::size_t i = states.size() - 1; i > 0; --i) {
        ASSERT_OK(session->undo());
        EXPECT_TRUE(session->workspace() == states[i - 1]) << "undo to state " << i - 1;
    }
    for (std::size_t i = 1; i < states.size(); ++i) {
        ASSERT_OK(session->redo());
        EXPECT_TRUE(session->workspace() == states[i]) << "redo to state " << i;
    }
    ASSERT_OK(session->workspace().validate());
    const document::Workspace final = session->workspace();
    reopen();
    EXPECT_TRUE(session->workspace() == final) << "reopened state differs";

    // Edit after reopening (a text box and a move), then reopen again.
    controller->setTool(canvas::ToolKind::Text);
    pointer(canvas::PointerPhase::Down, {110, 255});
    pointer(canvas::PointerPhase::Up, {110, 255});
    ASSERT_TRUE(controller->textEdit().has_value());
    ASSERT_TRUE(controller->textEdit()->element.has_value());
    ASSERT_OK(controller->finishTextEdit("edited after reopen"));
    controller->setTool(canvas::ToolKind::Select);
    controller->selectAll();
    drag(controller->camera().worldToView(
             document::worldBounds(*session->workspace().findElement(image)).center()),
         {520, 460});
    EXPECT_EQ(session->history().undoCount(), 2U);
    const document::Workspace edited = session->workspace();
    EXPECT_FALSE(edited == final);
    reopen();
    EXPECT_TRUE(session->workspace() == edited) << "reopened edits differ";
    ASSERT_OK(session->workspace().validate());
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
