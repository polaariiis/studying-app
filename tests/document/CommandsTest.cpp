// Commands: successful and failing execution, determinism, cascading deletes, atomicity.

#include <studyapp/document/Commands.hpp>

#include <studyapp/document/StudyCommands.hpp>
#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace studyapp::document::test {
namespace {

using core::ErrorCode;
using namespace std::chrono_literals;

TEST(CommandsTest, CommandsDoNotMutateTheWorkspace) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const Workspace before = t.workspace;
    ASSERT_TRUE(commands::createSection(t.workspace, notebook, "S", t.clock, t.ids).has_value());
    ASSERT_TRUE(commands::renameNotebook(t.workspace, notebook, "M", t.clock).has_value());
    ASSERT_TRUE(commands::deleteNotebook(t.workspace, notebook).has_value());
    EXPECT_EQ(t.workspace, before);
}

TEST(CommandsTest, CreateNotebookSetsMetadata) {
    TestWorkspace t;
    const auto created = commands::createNotebook(t.workspace, "Chemistry", t.clock, t.ids);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->command.label, "Create notebook");
    ASSERT_OK(t.editor.execute(created->command));

    const NotebookInfo* notebook = t.workspace.findNotebook(created->id);
    ASSERT_NE(notebook, nullptr);
    EXPECT_EQ(notebook->title, "Chemistry");
    EXPECT_EQ(millis(notebook->created), millis(t.clock.now()));
    EXPECT_EQ(millis(notebook->modified), millis(t.clock.now()));
    EXPECT_EQ(notebook->order, core::FractionalIndex::first());
}

TEST(CommandsTest, RenameUpdatesTitleAndModifiedTime) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("Old");
    const auto section = t.addSection(notebook, "Old");
    const auto page = t.addPage(section, "Old");
    const auto layer = t.firstLayer(page);
    const auto createdAt = t.clock.now();
    t.clock.advance(5min);

    t.run(commands::renameNotebook(t.workspace, notebook, "New notebook", t.clock));
    t.run(commands::renameSection(t.workspace, section, "New section", t.clock));
    t.run(commands::renamePage(t.workspace, page, "New page", t.clock));
    t.run(commands::renameLayer(t.workspace, layer, "New layer"));

    EXPECT_EQ(t.workspace.findNotebook(notebook)->title, "New notebook");
    EXPECT_EQ(t.workspace.findSection(section)->title, "New section");
    EXPECT_EQ(t.workspace.findPage(page)->title, "New page");
    EXPECT_EQ(t.workspace.findLayer(layer)->name, "New layer");
    EXPECT_EQ(millis(t.workspace.findNotebook(notebook)->created), millis(createdAt));
    EXPECT_EQ(millis(t.workspace.findNotebook(notebook)->modified), millis(t.clock.now()));
    EXPECT_EQ(millis(t.workspace.findPage(page)->modified), millis(t.clock.now()));
}

TEST(CommandsTest, PageTitleMayBeEmptyButOtherNamesMayNot) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const auto section = t.addSection(notebook, "S");
    const auto page = t.addPage(section, "");
    EXPECT_EQ(t.workspace.findPage(page)->title, "");

    EXPECT_EQ(commands::createNotebook(t.workspace, " ", t.clock, t.ids).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(commands::renameSection(t.workspace, section, "", t.clock).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(commands::createLayer(t.workspace, page, "\t", t.ids).error().code,
              ErrorCode::InvalidArgument);
}

TEST(CommandsTest, DocumentSectionHasOneBoundedPagePerDocumentPage) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const core::AssetId asset{t.ids.next()};
    const std::vector<core::DVec2> sizes{{816.0, 1056.0}, {1056.0, 816.0}, {400.0, 300.0}};
    const auto section = t.run(commands::createDocumentSection(t.workspace, notebook, "Lecture 3",
                                                               asset, sizes, t.clock, t.ids));
    ASSERT_NE(t.workspace.findSection(section), nullptr);
    EXPECT_EQ(t.workspace.findSection(section)->title, "Lecture 3");
    const auto pages = t.workspace.pagesOf(section);
    ASSERT_EQ(pages.size(), 3U);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const PageInfo& page = *t.workspace.findPage(pages[i]);
        EXPECT_EQ(page.title, "Lecture 3 \xC2\xB7 p. " + std::to_string(i + 1));
        EXPECT_EQ(page.extent, PageExtent::Bounded);
        EXPECT_EQ(page.size, sizes[i]);
        ASSERT_TRUE(page.document.has_value());
        EXPECT_EQ(page.document->asset, asset);
        EXPECT_EQ(page.document->index, static_cast<std::int32_t>(i));
        EXPECT_EQ(t.workspace.layersOf(pages[i]).size(), 1U);
    }
    // One undo step removes the whole import.
    EXPECT_EQ(t.editor.history().nextUndo()->label, "Import PDF");
    ASSERT_TRUE(t.editor.undo().has_value());
    EXPECT_EQ(t.workspace.findSection(section), nullptr);
    EXPECT_EQ(t.workspace.pageCount(), 0U);
}

TEST(CommandsTest, DocumentSectionRejectsInvalidInput) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const core::AssetId asset{t.ids.next()};
    const std::vector<core::DVec2> one{{100.0, 100.0}};
    const auto code = [&](const auto& result) {
        return result.error().code;
    };
    EXPECT_EQ(code(commands::createDocumentSection(t.workspace, core::NotebookId{t.ids.next()}, "D",
                                                   asset, one, t.clock, t.ids)),
              ErrorCode::NotFound);
    EXPECT_EQ(code(commands::createDocumentSection(t.workspace, notebook, "  ", asset, one, t.clock,
                                                   t.ids)),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(code(commands::createDocumentSection(t.workspace, notebook, "D", asset, {}, t.clock,
                                                   t.ids)),
              ErrorCode::InvalidArgument);
    for (const core::DVec2 bad : {core::DVec2{0.0, 10.0}, core::DVec2{10.0, -1.0},
                                  core::DVec2{std::numeric_limits<double>::infinity(), 10.0},
                                  core::DVec2{std::numeric_limits<double>::quiet_NaN(), 10.0}}) {
        const std::vector<core::DVec2> sizes{{100.0, 100.0}, bad};
        EXPECT_EQ(code(commands::createDocumentSection(t.workspace, notebook, "D", asset, sizes,
                                                       t.clock, t.ids)),
                  ErrorCode::InvalidArgument);
    }
    // A null asset is rejected by the workspace invariants (atomically: nothing is applied).
    auto created = commands::createDocumentSection(t.workspace, notebook, "D", core::AssetId{}, one,
                                                   t.clock, t.ids);
    ASSERT_TRUE(created.has_value());
    EXPECT_FALSE(t.editor.execute(std::move(created->command)).has_value());
    EXPECT_EQ(t.workspace.pageCount(), 0U);
    EXPECT_NE(t.editor.history().nextUndo()->label, "Import PDF"); // no step added
}

/// A notebook with a tagged document page, an image, and a connector between two shapes.
struct ImportSource {
    TestWorkspace t;
    core::NotebookId notebook;
    core::PageId page;
    core::AssetId pdf{core::Uuid{}};
    core::AssetId photo{core::Uuid{}};
    core::ElementId left;
    core::ElementId right;
    core::ElementId connector;
    core::TagId tag;

    ImportSource() {
        notebook = t.addNotebook("Biology");
        pdf = core::AssetId{t.ids.next()};
        photo = core::AssetId{t.ids.next()};
        const std::vector<core::DVec2> sizes{{400.0, 300.0}};
        const auto section = t.run(commands::createDocumentSection(t.workspace, notebook, "Slides",
                                                                   pdf, sizes, t.clock, t.ids));
        page = t.workspace.pagesOf(section).front();
        tag = t.run(commands::createTag(t.workspace, "exam", t.clock, t.ids));
        t.run(commands::setPageTags(t.workspace, page, {tag}, t.clock));
        const auto layer = t.firstLayer(page);
        left = t.addElement(layer, Shape{.kind = ShapeKind::Rectangle, .size = {40, 40}},
                            {.position = {10, 10}});
        right = t.addElement(layer, Shape{.kind = ShapeKind::Rectangle, .size = {40, 40}},
                             {.position = {200, 10}});
        t.addElement(layer, Image{.asset = photo, .size = {50, 50}}, {.position = {10, 100}});
        Connector link;
        link.start = {.position = {50, 30}, .attachedTo = left};
        link.end = {.position = {200, 30}, .attachedTo = right};
        connector = t.addElement(layer, link);
    }
};

TEST(CommandsTest, ImportingNotebooksKeepsIdsWhenTheyAreFree) {
    ImportSource source;
    TestWorkspace target;
    for (int i = 0; i < 1000; ++i) {
        (void)target.ids.next(); // ids of its own, apart from the source's
    }
    const auto existing = target.addNotebook("Existing");
    const core::AssetId pdf{target.ids.next()};
    const core::AssetId photo{target.ids.next()};
    const std::unordered_map<core::AssetId, core::AssetId> assets{{source.pdf, pdf},
                                                                  {source.photo, photo}};
    const std::vector<core::NotebookId> which{source.notebook};
    auto imported =
        commands::importNotebooks(target.workspace, source.t.workspace, which, assets, target.ids);
    ASSERT_TRUE(imported.has_value()) << imported.error().message;
    EXPECT_EQ(imported->id, which); // no conflict: the same ids
    EXPECT_EQ(imported->command.label, "Import notebook");
    ASSERT_TRUE(target.editor.execute(std::move(imported->command)).has_value());
    ASSERT_EQ(target.workspace.notebooks().size(), 2U);
    EXPECT_EQ(target.workspace.notebooks()[0], existing); // appended
    EXPECT_EQ(target.workspace.notebooks()[1], source.notebook);
    const PageInfo& page = *target.workspace.findPage(source.page);
    EXPECT_EQ(page.document->asset, pdf); // assets follow the map
    ASSERT_EQ(page.tags.size(), 1U);
    EXPECT_EQ(target.workspace.findTag(page.tags[0])->name, "exam");
    EXPECT_EQ(target.workspace.elementCount(), source.t.workspace.elementCount());
    const auto& link = std::get<Connector>(target.workspace.findElement(source.connector)->payload);
    EXPECT_EQ(link.start.attachedTo, source.left);
    EXPECT_TRUE(target.workspace.validate().has_value());
    // One undo step removes the whole import (and the tag it created).
    ASSERT_TRUE(target.editor.undo().has_value());
    EXPECT_EQ(target.workspace.notebookCount(), 1U);
    EXPECT_EQ(target.workspace.tagCount(), 0U);
}

TEST(CommandsTest, ImportingANotebookTwiceRemapsEveryIdAndReusesTagsByName) {
    ImportSource source;
    const Workspace before = source.t.workspace;
    const std::unordered_map<core::AssetId, core::AssetId> assets{{source.pdf, source.pdf},
                                                                  {source.photo, source.photo}};
    const std::vector<core::NotebookId> which{source.notebook};
    // Into the workspace it comes from: every id is taken.
    auto imported =
        commands::importNotebooks(source.t.workspace, before, which, assets, source.t.ids);
    ASSERT_TRUE(imported.has_value());
    ASSERT_TRUE(source.t.editor.execute(std::move(imported->command)).has_value());
    const Workspace& ws = source.t.workspace;
    ASSERT_EQ(ws.notebookCount(), 2U);
    const core::NotebookId copy = imported->id.front();
    EXPECT_NE(copy, source.notebook);
    EXPECT_EQ(ws.notebooks().back(), copy);
    EXPECT_EQ(ws.findNotebook(copy)->title, "Biology");
    // The original is untouched; the copy has its own ids throughout.
    EXPECT_TRUE(*ws.findPage(source.page) == *before.findPage(source.page));
    const auto section = ws.sectionsOf(copy).front();
    const auto page = ws.pagesOf(section).front();
    EXPECT_NE(page, source.page);
    EXPECT_EQ(ws.tagCount(), 1U); // "exam" is reused
    EXPECT_EQ(ws.findPage(page)->tags, ws.findPage(source.page)->tags);
    const auto layer = ws.layersOf(page).front();
    const auto elements = ws.elementsOf(layer);
    ASSERT_EQ(elements.size(), 4U);
    for (const core::ElementId id : elements) {
        EXPECT_EQ(before.findElement(id), nullptr);
    }
    // The connector attaches to the copies of its shapes.
    const auto& link = std::get<Connector>(ws.findElement(elements[3])->payload);
    EXPECT_EQ(link.start.attachedTo, elements[0]);
    EXPECT_EQ(link.end.attachedTo, elements[1]);
    EXPECT_EQ(ws.connectorsAttachedTo(elements[0]).size(), 1U);
    EXPECT_TRUE(ws.validate().has_value());
}

TEST(CommandsTest, ImportingNotebooksRejectsInvalidRequests) {
    ImportSource source;
    TestWorkspace target;
    const std::vector<core::NotebookId> which{source.notebook};
    const std::unordered_map<core::AssetId, core::AssetId> noAssets;
    const auto code = [&](const auto& result) {
        return result.error().code;
    };
    EXPECT_EQ(code(commands::importNotebooks(target.workspace, source.t.workspace, which, noAssets,
                                             target.ids)),
              ErrorCode::NotFound); // the PDF and the image have no asset here
    EXPECT_EQ(code(commands::importNotebooks(target.workspace, source.t.workspace, {}, noAssets,
                                             target.ids)),
              ErrorCode::InvalidArgument);
    const std::vector<core::NotebookId> twice{source.notebook, source.notebook};
    EXPECT_EQ(code(commands::importNotebooks(target.workspace, source.t.workspace, twice, noAssets,
                                             target.ids)),
              ErrorCode::InvalidArgument);
    const std::vector<core::NotebookId> unknown{core::NotebookId{target.ids.next()}};
    EXPECT_EQ(code(commands::importNotebooks(target.workspace, source.t.workspace, unknown,
                                             noAssets, target.ids)),
              ErrorCode::NotFound);
    EXPECT_EQ(target.workspace.notebookCount(), 0U);
}

TEST(CommandsTest, CreationPatchReproducesTheWorkspace) {
    ImportSource source; // notebooks, a PDF section, tags, shapes, an image, a connector
    TestWorkspace& t = source.t;
    const auto course = t.run(commands::createCourse(t.workspace, "Biology", t.clock, t.ids));
    const auto project =
        t.run(commands::createProject(t.workspace, "Lab report", course, t.clock, t.ids));
    const auto task = t.run(commands::createTask(
        t.workspace, {.title = "Write up", .course = course, .project = project}, t.clock, t.ids));
    t.run(commands::createTask(t.workspace, {.title = "Figures", .parent = task}, t.clock, t.ids));
    Workspace copy(t.workspace.info());
    ASSERT_TRUE(copy.apply(commands::creationPatch(t.workspace)).has_value());
    EXPECT_TRUE(copy == t.workspace);
    EXPECT_TRUE(copy.validate().has_value());
}

TEST(CommandsTest, UnknownIdsFailWithNotFound) {
    TestWorkspace t;
    const core::NotebookId notebook{t.ids.next()};
    const core::SectionId section{t.ids.next()};
    const core::PageId page{t.ids.next()};
    const core::LayerId layer{t.ids.next()};
    const core::ElementId element{t.ids.next()};
    EXPECT_EQ(commands::renameNotebook(t.workspace, notebook, "x", t.clock).error().code,
              ErrorCode::NotFound);
    EXPECT_EQ(commands::deleteNotebook(t.workspace, notebook).error().code, ErrorCode::NotFound);
    EXPECT_EQ(commands::createSection(t.workspace, notebook, "x", t.clock, t.ids).error().code,
              ErrorCode::NotFound);
    EXPECT_EQ(commands::deleteSection(t.workspace, section).error().code, ErrorCode::NotFound);
    EXPECT_EQ(commands::createPage(t.workspace, section, "x", {}, t.clock, t.ids).error().code,
              ErrorCode::NotFound);
    EXPECT_EQ(commands::deletePage(t.workspace, page).error().code, ErrorCode::NotFound);
    EXPECT_EQ(commands::createLayer(t.workspace, page, "x", t.ids).error().code,
              ErrorCode::NotFound);
    EXPECT_EQ(commands::deleteLayer(t.workspace, layer).error().code, ErrorCode::NotFound);
    EXPECT_EQ(
        commands::createElement(t.workspace, layer, {.payload = makeText("x")}, t.ids).error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(commands::deleteElement(t.workspace, element).error().code, ErrorCode::NotFound);
}

TEST(CommandsTest, CreatePageAddsItsFirstLayerInOnePatch) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const auto section = t.addSection(notebook, "S");
    const auto created = commands::createPage(
        t.workspace, section, "Bounded",
        {.extent = PageExtent::Bounded, .size = kA4PortraitSize, .firstLayerName = "Ink"}, t.clock,
        t.ids);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->command.patch.size(), 2U);
    ASSERT_OK(t.editor.execute(created->command));

    const PageInfo* page = t.workspace.findPage(created->id);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->extent, PageExtent::Bounded);
    EXPECT_EQ(page->size, kA4PortraitSize);
    ASSERT_EQ(t.workspace.layersOf(created->id).size(), 1U);
    EXPECT_EQ(t.workspace.findLayer(t.workspace.layersOf(created->id)[0])->name, "Ink");
}

TEST(CommandsTest, InvalidPageOptionsFailWithoutSideEffects) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const auto section = t.addSection(notebook, "S");
    const Workspace before = t.workspace;
    const auto created =
        commands::createPage(t.workspace, section, "Bad",
                             {.extent = PageExtent::Bounded, .size = {0, 0}}, t.clock, t.ids);
    ASSERT_TRUE(created.has_value());
    EXPECT_FALSE(t.editor.execute(created->command).has_value());
    EXPECT_EQ(t.workspace, before);
    EXPECT_FALSE(t.editor.canUndo() && t.editor.history().undoLabel() == "Create page");
}

TEST(CommandsTest, DeleteNotebookRemovesWholeSubtree) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("Doomed");
    const auto keep = t.addNotebook("Kept");
    for (int s = 0; s < 2; ++s) {
        const auto section = t.addSection(notebook, "S" + std::to_string(s));
        for (int p = 0; p < 2; ++p) {
            const auto page = t.addPage(section, "P");
            const auto layer = t.firstLayer(page);
            const auto a = t.addElement(layer, makeText("a"));
            const auto b = t.addElement(layer, makeStroke());
            t.addElement(layer, makeConnector(a, b));
        }
    }
    const auto keptSection = t.addSection(keep, "Kept section");
    t.addPage(keptSection, "Kept page");

    t.run(commands::deleteNotebook(t.workspace, notebook));
    EXPECT_EQ(t.workspace.findNotebook(notebook), nullptr);
    EXPECT_EQ(t.workspace.notebookCount(), 1U);
    EXPECT_EQ(t.workspace.sectionCount(), 1U);
    EXPECT_EQ(t.workspace.pageCount(), 1U);
    EXPECT_EQ(t.workspace.layerCount(), 1U);
    EXPECT_EQ(t.workspace.elementCount(), 0U);
    EXPECT_OK(t.workspace.validate());
}

TEST(CommandsTest, DeleteElementDetachesConnectors) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto a = t.addElement(layer, makeText("a"));
    const auto b = t.addElement(layer, makeText("b"));
    const auto link = t.addElement(layer, makeConnector(a, b));

    t.run(commands::deleteElement(t.workspace, a));
    EXPECT_EQ(t.workspace.findElement(a), nullptr);
    const auto& connector = std::get<Connector>(t.workspace.findElement(link)->payload);
    EXPECT_FALSE(connector.start.attachedTo.has_value());
    EXPECT_EQ(connector.end.attachedTo, b);
    EXPECT_OK(t.workspace.validate());
}

TEST(CommandsTest, DeleteLayerDetachesConnectorsOnOtherLayers) {
    TestWorkspace t;
    const auto base = t.addPath();
    const auto page = t.workspace.findLayer(base)->page;
    const auto top = t.addLayer(page, "Top");
    const auto shape = t.addElement(top, Shape{.size = {10, 10}});
    const auto link = t.addElement(base, makeConnector(shape, std::nullopt));

    t.run(commands::deleteLayer(t.workspace, top));
    EXPECT_EQ(t.workspace.findLayer(top), nullptr);
    EXPECT_EQ(t.workspace.findElement(shape), nullptr);
    EXPECT_FALSE(std::get<Connector>(t.workspace.findElement(link)->payload).start.attachedTo);
    EXPECT_OK(t.workspace.validate());
}

TEST(CommandsTest, CannotDeleteLastLayer) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto result = commands::deleteLayer(t.workspace, layer);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(CommandsTest, ElementsAreAppendedInZOrder) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto first = t.addElement(layer, makeText("1"));
    const auto second = t.addElement(layer, makeStroke());
    const auto third =
        t.addElement(layer, Image{.asset = core::AssetId{t.ids.next()}, .size = {4, 3}});
    const auto order = t.workspace.elementsOf(layer);
    ASSERT_EQ(order.size(), 3U);
    EXPECT_EQ(order[0], first);
    EXPECT_EQ(order[1], second);
    EXPECT_EQ(order[2], third);
    EXPECT_LT(t.workspace.findElement(first)->z, t.workspace.findElement(third)->z);
}

TEST(CommandsTest, AreDeterministic) {
    // Same starting state, clock and id sequence -> identical commands and workspaces.
    const auto build = [] {
        auto t = std::make_unique<TestWorkspace>();
        const auto layer = t->addPath();
        t->addElement(layer, makeStroke());
        t->run(commands::renameNotebook(t->workspace, t->workspace.notebooks()[0], "R", t->clock));
        return t;
    };
    const auto a = build();
    const auto b = build();
    EXPECT_EQ(a->workspace, b->workspace);
    EXPECT_EQ(*a->editor.history().nextUndo(), *b->editor.history().nextUndo());
}

// ---------------------------------------------------------------------------- Phase 4

TEST(CommandsTest, DeleteElementsRemovesASetInOnePatchAndDetachesConnectors) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto a = t.addElement(layer, makeStroke());
    const auto b = t.addElement(layer, makeStroke());
    const auto keep = t.addElement(layer, makeStroke());
    const auto link = t.addElement(layer, makeConnector(a, keep));
    const auto undoBefore = t.editor.history().undoCount();

    const std::vector<core::ElementId> doomed{b, a, a}; // unsorted, duplicated
    t.run(commands::deleteElements(t.workspace, doomed));
    EXPECT_EQ(t.editor.history().undoCount(), undoBefore + 1);
    EXPECT_EQ(t.workspace.findElement(a), nullptr);
    EXPECT_EQ(t.workspace.findElement(b), nullptr);
    const auto& connector = std::get<Connector>(t.workspace.findElement(link)->payload);
    EXPECT_FALSE(connector.start.attachedTo.has_value()); // detached, not dangling
    EXPECT_EQ(connector.end.attachedTo, keep);
    ASSERT_OK(t.workspace.validate());

    ASSERT_OK(t.editor.undo());
    EXPECT_NE(t.workspace.findElement(a), nullptr);
    EXPECT_EQ(std::get<Connector>(t.workspace.findElement(link)->payload).start.attachedTo, a);

    EXPECT_EQ(commands::deleteElements(t.workspace, {}).error().code, ErrorCode::InvalidArgument);
    const std::vector<core::ElementId> unknown{core::ElementId{t.ids.next()}};
    EXPECT_EQ(commands::deleteElements(t.workspace, unknown).error().code, ErrorCode::NotFound);
}

TEST(CommandsTest, SplitStrokesKeepStyleAndDrawOrderInOneUndoableEdit) {
    TestWorkspace t;
    const auto layer = t.addPath();
    Stroke styled = makeStroke({{0, 0, 1}, {50, 0, 1}, {100, 0, 0.5F}});
    styled.brush = Brush::Highlighter;
    styled.color = core::Color::fromRgba(0xF2, 0xC9, 0x4C, 0x73);
    styled.baseWidth = 14.0F;
    const auto before = t.addElement(layer, makeStroke());
    const auto cut = t.addElement(layer, styled, {.position = {5, 6}});
    const auto after = t.addElement(layer, makeStroke());
    const auto gone = t.addElement(layer, makeStroke());
    const auto link = t.addElement(layer, makeConnector(cut, gone));
    const Workspace original = t.workspace;
    const auto undoBefore = t.editor.history().undoCount();

    const std::vector<commands::StrokePieces> splits{
        {.stroke = cut,
         .pieces = {makeStrokePoints({{0, 0, 1}, {20, 0, 1}}),
                    makeStrokePoints({{60, 0, 1}, {80, 0, 0.6F}}),
                    makeStrokePoints({{90, 0, 0.55F}, {100, 0, 0.5F}})}},
        {.stroke = gone, .pieces = {}},
    };
    auto command = commands::splitStrokes(t.workspace, splits, t.ids);
    ASSERT_OK(command);
    EXPECT_EQ(command->label, "Erase");
    t.run(std::move(command));
    EXPECT_EQ(t.editor.history().undoCount(), undoBefore + 1);
    ASSERT_OK(t.workspace.validate());

    // Draw order: before, cut (first piece, same id), two new pieces, after, link.
    const auto order = t.workspace.elementsOf(layer);
    ASSERT_EQ(order.size(), 6U);
    EXPECT_EQ(order[0], before);
    EXPECT_EQ(order[1], cut);
    EXPECT_EQ(order[4], after);
    EXPECT_EQ(order[5], link);
    for (std::size_t i = 1; i <= 3; ++i) {
        const Element& piece = *t.workspace.findElement(order[i]);
        const auto& stroke = std::get<Stroke>(piece.payload);
        EXPECT_EQ(stroke.brush, Brush::Highlighter);
        EXPECT_EQ(stroke.color, styled.color);
        EXPECT_FLOAT_EQ(stroke.baseWidth, 14.0F);
        EXPECT_EQ(piece.transform.position, (core::DVec2{5, 6}));
        EXPECT_EQ(*stroke.points, *splits[0].pieces[i - 1]);
    }
    EXPECT_EQ(t.workspace.findElement(gone), nullptr);
    // The kept id stays attached; the erased one is detached.
    const auto& connector = std::get<Connector>(t.workspace.findElement(link)->payload);
    EXPECT_EQ(connector.start.attachedTo, cut);
    EXPECT_FALSE(connector.end.attachedTo.has_value());

    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.workspace == original);
    ASSERT_OK(t.editor.redo());
    EXPECT_EQ(t.workspace.elementsOf(layer).size(), 6U);

    // Errors change nothing.
    EXPECT_EQ(commands::splitStrokes(t.workspace, {}, t.ids).error().code,
              ErrorCode::InvalidArgument);
    const std::vector<commands::StrokePieces> twice{{.stroke = cut}, {.stroke = cut}};
    EXPECT_EQ(commands::splitStrokes(t.workspace, twice, t.ids).error().code,
              ErrorCode::InvalidArgument);
    const std::vector<commands::StrokePieces> notStroke{{.stroke = link}};
    EXPECT_EQ(commands::splitStrokes(t.workspace, notStroke, t.ids).error().code,
              ErrorCode::InvalidArgument);
    const std::vector<commands::StrokePieces> emptyPiece{
        {.stroke = cut, .pieces = {makeStrokePoints({})}}};
    EXPECT_EQ(commands::splitStrokes(t.workspace, emptyPiece, t.ids).error().code,
              ErrorCode::InvalidArgument);
    const std::vector<commands::StrokePieces> unknown{{.stroke = core::ElementId{t.ids.next()}}};
    EXPECT_EQ(commands::splitStrokes(t.workspace, unknown, t.ids).error().code,
              ErrorCode::NotFound);
}

TEST(CommandsTest, ShapeKindsAreValidated) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto arrow =
        t.addElement(layer, Shape{.kind = ShapeKind::Arrow, .size = {50, 0}}, {.rotation = 0.5F});
    EXPECT_EQ(std::get<Shape>(t.workspace.findElement(arrow)->payload).kind, ShapeKind::Arrow);
    // Reserved values (triangle, polygon, polyline) are not shapes the model knows yet.
    auto reserved = commands::createElement(
        t.workspace, layer, {.payload = Shape{.kind = static_cast<ShapeKind>(4), .size = {10, 10}}},
        t.ids);
    ASSERT_OK(reserved);
    EXPECT_EQ(t.editor.execute(std::move(reserved->command)).error().code,
              ErrorCode::InvalidArgument);
}

TEST(CommandsTest, EditTextReplacesTextAndSizeInOneUndoableEdit) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto box = t.addElement(layer, TextBox{.size = {100, 20}, .text = "old"});
    const auto undoBefore = t.editor.history().undoCount();
    t.run(commands::editText(t.workspace, box, "new\ntext", {100, 40}));
    EXPECT_EQ(t.editor.history().undoCount(), undoBefore + 1);
    const auto& edited = std::get<TextBox>(t.workspace.findElement(box)->payload);
    EXPECT_EQ(edited.text, "new\ntext");
    EXPECT_EQ(edited.size, (core::Vec2{100, 40}));
    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(std::get<TextBox>(t.workspace.findElement(box)->payload).text, "old");
    auto same = commands::editText(t.workspace, box, "old", {100, 20});
    ASSERT_OK(same);
    EXPECT_TRUE(same->patch.empty());
    const auto stroke = t.addElement(layer, makeStroke());
    EXPECT_EQ(commands::editText(t.workspace, stroke, "x", {1, 1}).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(
        commands::editText(t.workspace, core::ElementId{t.ids.next()}, "x", {1, 1}).error().code,
        ErrorCode::NotFound);
}

TEST(CommandsTest, TheConnectorIndexAndSetConnectorEnds) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto a = t.addElement(layer, makeStroke());
    const auto b = t.addElement(layer, makeStroke());
    const auto link = t.addElement(layer, makeConnector(a, b));
    ASSERT_EQ(t.workspace.connectorsAttachedTo(a).size(), 1U);
    EXPECT_EQ(t.workspace.connectorsAttachedTo(a)[0], link);
    EXPECT_TRUE(t.workspace.connectorsAttachedTo(link).empty());
    // Re-attach the end to nothing: the index follows; undo restores it.
    t.run(commands::setConnectorEnds(t.workspace, link, {.position = {0, 0}, .attachedTo = a},
                                     {.position = {50, 50}, .attachedTo = std::nullopt}));
    EXPECT_TRUE(t.workspace.connectorsAttachedTo(b).empty());
    ASSERT_OK(t.workspace.validate());
    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(t.workspace.connectorsAttachedTo(b).size(), 1U);
    ASSERT_OK(t.workspace.validate());
    // A connector may not attach to a connector; unchanged ends are a no-op; errors.
    auto self = commands::setConnectorEnds(t.workspace, link, {.position = {}, .attachedTo = link},
                                           {.position = {}, .attachedTo = b});
    ASSERT_OK(self);
    EXPECT_FALSE(t.editor.execute(std::move(*self)).has_value());
    const auto& current = std::get<Connector>(t.workspace.findElement(link)->payload);
    auto same = commands::setConnectorEnds(t.workspace, link, current.start, current.end);
    ASSERT_OK(same);
    EXPECT_TRUE(same->patch.empty());
    EXPECT_EQ(commands::setConnectorEnds(t.workspace, a, {}, {}).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(commands::setConnectorEnds(t.workspace, link,
                                         {.position = {std::nan(""), 0}, .attachedTo = {}}, {})
                  .error()
                  .code,
              ErrorCode::InvalidArgument);
    // Deleting a attached element removes it from the index.
    t.run(commands::deleteElement(t.workspace, b));
    EXPECT_TRUE(t.workspace.connectorsAttachedTo(b).empty());
    ASSERT_OK(t.workspace.validate());
}

TEST(CommandsTest, PasteElementsCopiesWithNewIdsAndReattachesConnectors) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto rect = t.addElement(layer, Shape{.size = {100, 50}, .strokeColor = std::nullopt},
                                   {.position = {0, 0}});
    const core::AssetId asset{t.ids.next()};
    const auto image =
        t.addElement(layer, Image{.asset = asset, .size = {4, 3}}, {.position = {200, 0}});
    const auto outside = t.addElement(layer, makeStroke());
    const auto inner =
        t.addElement(layer, Connector{.start = {.position = {100, 25}, .attachedTo = rect},
                                      .end = {.position = {200, 1}, .attachedTo = image}});
    const auto dangling =
        t.addElement(layer, Connector{.start = {.position = {100, 25}, .attachedTo = rect},
                                      .end = {.position = {300, 25}, .attachedTo = outside}});
    std::vector<Element> source;
    for (const core::ElementId id : {rect, image, inner, dangling}) {
        source.push_back(*t.workspace.findElement(id));
    }
    const Workspace original = t.workspace;

    // Onto another page: one undo step, new ids, the given order, the offset applied.
    const auto target = t.addPath();
    const auto undoBefore = t.editor.history().undoCount();
    const std::vector<core::ElementId> copies =
        t.run(commands::pasteElements(t.workspace, target, source, {10, 20}, t.ids));
    ASSERT_EQ(copies.size(), 4U);
    EXPECT_EQ(t.editor.history().undoCount(), undoBefore + 1);
    EXPECT_EQ(t.editor.history().nextUndo()->label, "Paste");
    const auto order = t.workspace.elementsOf(target);
    EXPECT_TRUE(std::equal(order.begin(), order.end(), copies.begin(), copies.end()));
    for (std::size_t i = 0; i < copies.size(); ++i) {
        EXPECT_NE(copies[i], source[i].id);
        EXPECT_EQ(t.workspace.findElement(copies[i])->layer, target);
    }
    EXPECT_EQ(t.workspace.findElement(copies[0])->transform.position, (core::DVec2{10, 20}));
    EXPECT_EQ(std::get<Shape>(t.workspace.findElement(copies[0])->payload),
              std::get<Shape>(source[0].payload));
    EXPECT_EQ(std::get<Image>(t.workspace.findElement(copies[1])->payload).asset, asset); // shared
    // A connector between copied elements joins their copies; an end on anything else is
    // detached where it is (offset with the copy).
    const auto& joined = std::get<Connector>(t.workspace.findElement(copies[2])->payload);
    EXPECT_EQ(joined.start.attachedTo, copies[0]);
    EXPECT_EQ(joined.end.attachedTo, copies[1]);
    EXPECT_EQ(joined.start.position, (core::DVec2{110, 45}));
    const auto& loose = std::get<Connector>(t.workspace.findElement(copies[3])->payload);
    EXPECT_EQ(loose.start.attachedTo, copies[0]);
    EXPECT_FALSE(loose.end.attachedTo.has_value());
    EXPECT_EQ(loose.end.position, (core::DVec2{310, 45}));
    EXPECT_EQ(t.workspace.connectorsAttachedTo(copies[0]).size(), 2U);
    EXPECT_EQ(t.workspace.connectorsAttachedTo(rect).size(), 2U); // the originals are untouched
    ASSERT_OK(t.workspace.validate());

    // Undo removes every copy; redo brings back the same ids.
    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.workspace.elementsOf(target).empty());
    ASSERT_OK(t.editor.redo());
    const auto redone = t.workspace.elementsOf(target);
    EXPECT_TRUE(std::equal(redone.begin(), redone.end(), copies.begin(), copies.end()));

    // Onto the source layer: above everything that is there.
    const std::vector<core::ElementId> again =
        t.run(commands::pasteElements(t.workspace, layer, source, {}, t.ids));
    const auto layerOrder = t.workspace.elementsOf(layer);
    ASSERT_EQ(layerOrder.size(), 9U);
    EXPECT_TRUE(std::equal(layerOrder.begin() + 5, layerOrder.end(), again.begin(), again.end()));
    EXPECT_TRUE(std::equal(original.elementsOf(layer).begin(), original.elementsOf(layer).end(),
                           layerOrder.begin(), layerOrder.begin() + 5));

    EXPECT_EQ(commands::pasteElements(t.workspace, layer, {}, {}, t.ids).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(commands::pasteElements(t.workspace, core::LayerId{t.ids.next()}, source, {}, t.ids)
                  .error()
                  .code,
              ErrorCode::NotFound);
    const std::vector<Element> twice{source[0], source[0]};
    EXPECT_EQ(commands::pasteElements(t.workspace, layer, twice, {}, t.ids).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(commands::pasteElements(t.workspace, layer, source,
                                      {std::numeric_limits<double>::quiet_NaN(), 0}, t.ids)
                  .error()
                  .code,
              ErrorCode::InvalidArgument);
}

TEST(CommandsTest, PasteElementsAttachesAConnectorDrawnBelowItsTarget) {
    // A connector re-attached to an element drawn after it lies below that element; its
    // copy still joins the element's copy (the patch creates connectors last).
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto link =
        t.addElement(layer, Connector{.start = {.position = {0, 0}}, .end = {.position = {50, 0}}});
    const auto rect = t.addElement(layer, Shape{.size = {20, 20}}, {.position = {50, -10}});
    t.run(commands::setConnectorEnds(t.workspace, link, {.position = {0, 0}},
                                     {.position = {50, 0}, .attachedTo = rect}));
    const std::vector<Element> source{*t.workspace.findElement(link),
                                      *t.workspace.findElement(rect)};

    const std::vector<core::ElementId> copies =
        t.run(commands::pasteElements(t.workspace, layer, source, {5, 5}, t.ids));
    ASSERT_EQ(copies.size(), 2U);
    const auto order = t.workspace.elementsOf(layer);
    ASSERT_EQ(order.size(), 4U);
    EXPECT_EQ(order[2], copies[0]); // the painter order is kept
    EXPECT_EQ(order[3], copies[1]);
    EXPECT_EQ(std::get<Connector>(t.workspace.findElement(copies[0])->payload).end.attachedTo,
              copies[1]);
    ASSERT_OK(t.workspace.validate());
    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(t.workspace.elementsOf(layer).size(), 2U);
    ASSERT_OK(t.editor.redo());
    EXPECT_EQ(t.workspace.connectorsAttachedTo(copies[1]).size(), 1U);
    ASSERT_OK(t.workspace.validate());
}

TEST(CommandsTest, ResizeElementMapsAttachedConnectorEnds) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto rect = t.addElement(layer, Shape{.size = {100, 50}, .strokeColor = std::nullopt},
                                   {.position = {0, 0}});
    const auto other = t.addElement(layer, makeStroke());
    const auto link =
        t.addElement(layer, Connector{.start = {.position = {100, 25}, .attachedTo = rect},
                                      .end = {.position = {300, 25}, .attachedTo = other}});
    const auto undoBefore = t.editor.history().undoCount();
    t.run(commands::resizeElement(t.workspace, rect, {.position = {0, 0}}, {200, 100}));
    EXPECT_EQ(t.editor.history().undoCount(), undoBefore + 1);
    EXPECT_EQ(std::get<Shape>(t.workspace.findElement(rect)->payload).size, (core::Vec2{200, 100}));
    const auto& connector = std::get<Connector>(t.workspace.findElement(link)->payload);
    EXPECT_EQ(connector.start.position, (core::DVec2{200, 50})); // still at the right edge's middle
    EXPECT_EQ(connector.end.position, (core::DVec2{300, 25}));   // the other end untouched
    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(std::get<Connector>(t.workspace.findElement(link)->payload).start.position,
              (core::DVec2{100, 25}));
    auto same = commands::resizeElement(t.workspace, rect, {.position = {0, 0}}, {100, 50});
    ASSERT_OK(same);
    EXPECT_TRUE(same->patch.empty());
    EXPECT_EQ(commands::resizeElement(t.workspace, other, {}, {1, 1}).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(commands::resizeElement(t.workspace, rect, {}, {-1, 1}).error().code,
              ErrorCode::InvalidArgument);
}

TEST(CommandsTest, MoveElementsTranslatesAndConnectorEndsFollow) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto moved = t.addElement(layer, makeStroke(), {.position = {10, 20}});
    const auto still = t.addElement(layer, makeStroke(), {.position = {500, 500}});
    const auto attached = t.addElement(layer, makeConnector(moved, still));    // not moved itself
    const auto free = t.addElement(layer, makeConnector(std::nullopt, still)); // moved

    const std::vector<core::ElementId> ids{moved, free};
    t.run(commands::moveElements(t.workspace, ids, {5, -3}));
    EXPECT_EQ(t.workspace.findElement(moved)->transform.position, (core::DVec2{15, 17}));
    EXPECT_EQ(t.workspace.findElement(still)->transform.position, (core::DVec2{500, 500}));
    const auto& a = std::get<Connector>(t.workspace.findElement(attached)->payload);
    EXPECT_EQ(a.start.position, (core::DVec2{5, -3})); // follows the moved element
    EXPECT_EQ(a.end.position, (core::DVec2{100, 0}));  // attached to an unmoved element
    const auto& f = std::get<Connector>(t.workspace.findElement(free)->payload);
    EXPECT_EQ(f.start.position, (core::DVec2{5, -3})); // free end of a moved connector
    EXPECT_EQ(f.end.position, (core::DVec2{100, 0}));  // stays on its unmoved target
    ASSERT_OK(t.workspace.validate());

    const Workspace afterMove = t.workspace;
    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(t.workspace.findElement(moved)->transform.position, (core::DVec2{10, 20}));
    ASSERT_OK(t.editor.redo());
    EXPECT_EQ(t.workspace, afterMove);

    EXPECT_TRUE(commands::moveElements(t.workspace, ids, {0, 0})->patch.empty());
    EXPECT_EQ(commands::moveElements(t.workspace, ids, {std::nan(""), 0}).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(commands::moveElements(t.workspace, {}, {1, 1}).error().code,
              ErrorCode::InvalidArgument);
}

TEST(CommandsTest, SetPageFormatChangesExtentAndBackground) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const auto section = t.addSection(notebook, "S");
    const auto page = t.addPage(section, "P");
    t.clock.advance(1min);
    const commands::PageFormat format{.extent = PageExtent::Bounded,
                                      .size = kA4PortraitSize,
                                      .background = {.color = core::Color::white(),
                                                     .pattern = BackgroundPattern::Dots,
                                                     .spacing = 24}};
    t.run(commands::setPageFormat(t.workspace, page, format, t.clock));
    const PageInfo& info = *t.workspace.findPage(page);
    EXPECT_EQ(info.extent, PageExtent::Bounded);
    EXPECT_EQ(info.background.pattern, BackgroundPattern::Dots);
    EXPECT_EQ(millis(info.modified), millis(t.clock.now()));
    EXPECT_TRUE(commands::setPageFormat(t.workspace, page, format, t.clock)->patch.empty());

    const commands::PageFormat invalid{.extent = PageExtent::Bounded, .size = {0, 0}};
    auto rejected = commands::setPageFormat(t.workspace, page, invalid, t.clock);
    ASSERT_OK(rejected);
    EXPECT_FALSE(t.editor.execute(std::move(*rejected)).has_value()); // validated by apply
}

TEST(CommandsTest, RenamingToTheSameTitleIsANoOp) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const auto section = t.addSection(notebook, "S");
    const auto page = t.addPage(section, "");
    t.clock.advance(1min);
    EXPECT_TRUE(commands::renameNotebook(t.workspace, notebook, "N", t.clock)->patch.empty());
    EXPECT_TRUE(commands::renameSection(t.workspace, section, "S", t.clock)->patch.empty());
    EXPECT_TRUE(commands::renamePage(t.workspace, page, "", t.clock)->patch.empty());
    EXPECT_FALSE(commands::renamePage(t.workspace, page, "P", t.clock)->patch.empty());
}

TEST(CommandsTest, MoveNotebookReordersWithoutTouchingSiblings) {
    TestWorkspace t;
    const auto a = t.addNotebook("A");
    const auto b = t.addNotebook("B");
    const auto c = t.addNotebook("C");
    const auto order = [&] {
        return std::vector<core::NotebookId>(t.workspace.notebooks().begin(),
                                             t.workspace.notebooks().end());
    };
    const NotebookInfo beforeA = *t.workspace.findNotebook(a);
    const NotebookInfo beforeB = *t.workspace.findNotebook(b);

    auto toFront = commands::moveNotebook(t.workspace, c, 0, t.clock);
    ASSERT_OK(toFront);
    EXPECT_EQ(toFront->patch.size(), 1U); // only the moved record changes
    t.run(std::move(toFront));
    EXPECT_EQ(order(), (std::vector{c, a, b}));
    EXPECT_EQ(*t.workspace.findNotebook(a), beforeA);
    EXPECT_EQ(*t.workspace.findNotebook(b), beforeB);

    t.run(commands::moveNotebook(t.workspace, c, 1, t.clock)); // between a and b
    EXPECT_EQ(order(), (std::vector{a, c, b}));
    t.run(commands::moveNotebook(t.workspace, a, 99, t.clock)); // past the end appends
    EXPECT_EQ(order(), (std::vector{c, b, a}));
    // Already there: an empty command.
    EXPECT_TRUE(commands::moveNotebook(t.workspace, a, 2, t.clock)->patch.empty());
    EXPECT_TRUE(commands::moveNotebook(t.workspace, a, 7, t.clock)->patch.empty());

    ASSERT_OK(t.editor.undo());
    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(order(), (std::vector{c, a, b}));
    ASSERT_OK(t.workspace.validate());
    EXPECT_EQ(commands::moveNotebook(t.workspace, core::NotebookId{}, 0, t.clock).error().code,
              ErrorCode::NotFound);
}

TEST(CommandsTest, MoveSectionAndPageAcrossParentsTakeTheirSubtree) {
    TestWorkspace t;
    const auto n1 = t.addNotebook("N1");
    const auto n2 = t.addNotebook("N2");
    const auto s1 = t.addSection(n1, "S1");
    const auto s2 = t.addSection(n2, "S2");
    const auto p1 = t.addPage(s1, "P1");
    const auto p2 = t.addPage(s1, "P2");
    const auto layer = t.firstLayer(p1);
    t.addElement(layer, makeStroke());

    // The section goes to the other notebook (before S2), with its pages and content.
    t.run(commands::moveSection(t.workspace, s1, n2, 0, t.clock));
    EXPECT_TRUE(t.workspace.sectionsOf(n1).empty());
    EXPECT_EQ(std::vector(t.workspace.sectionsOf(n2).begin(), t.workspace.sectionsOf(n2).end()),
              (std::vector{s1, s2}));
    EXPECT_EQ(t.workspace.findSection(s1)->notebook, n2);
    EXPECT_EQ(t.workspace.pagesOf(s1).size(), 2U);
    EXPECT_EQ(t.workspace.elementsOf(layer).size(), 1U);

    // A page goes to the other section.
    t.run(commands::movePage(t.workspace, p1, s2, 0, t.clock));
    EXPECT_EQ(t.workspace.findPage(p1)->section, s2);
    EXPECT_EQ(std::vector(t.workspace.pagesOf(s1).begin(), t.workspace.pagesOf(s1).end()),
              (std::vector{p2}));
    EXPECT_EQ(t.workspace.layersOf(p1).size(), 1U);
    ASSERT_OK(t.workspace.validate());

    // Within one section: reorder.
    const auto p3 = t.addPage(s2, "P3");
    t.run(commands::movePage(t.workspace, p3, s2, 0, t.clock));
    EXPECT_EQ(std::vector(t.workspace.pagesOf(s2).begin(), t.workspace.pagesOf(s2).end()),
              (std::vector{p3, p1}));

    // Undo restores parents and order exactly.
    ASSERT_OK(t.editor.undo()); // reorder
    ASSERT_OK(t.editor.undo()); // create P3
    ASSERT_OK(t.editor.undo()); // page move
    ASSERT_OK(t.editor.undo()); // section move
    EXPECT_EQ(t.workspace.findSection(s1)->notebook, n1);
    EXPECT_EQ(std::vector(t.workspace.pagesOf(s1).begin(), t.workspace.pagesOf(s1).end()),
              (std::vector{p1, p2}));
    EXPECT_EQ(commands::movePage(t.workspace, p1, core::SectionId{}, 0, t.clock).error().code,
              ErrorCode::NotFound);
    EXPECT_EQ(commands::moveSection(t.workspace, s1, core::NotebookId{}, 0, t.clock).error().code,
              ErrorCode::NotFound);
}

TEST(CommandsTest, RepeatedMovesKeepATotalOrder) {
    // Moving the last page to the front over and over keeps producing keys strictly
    // between neighbours; the order stays exactly as intended.
    TestWorkspace t;
    const auto section = t.addSection(t.addNotebook("N"), "S");
    std::vector<core::PageId> expected;
    for (int i = 0; i < 6; ++i) {
        expected.push_back(t.addPage(section, "P" + std::to_string(i)));
    }
    for (int round = 0; round < 60; ++round) {
        const core::PageId last = expected.back();
        const std::size_t target = static_cast<std::size_t>(round % 3);
        t.run(commands::movePage(t.workspace, last, section, target, t.clock));
        expected.pop_back();
        expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(target), last);
        ASSERT_EQ(
            std::vector(t.workspace.pagesOf(section).begin(), t.workspace.pagesOf(section).end()),
            expected)
            << "round " << round;
    }
    ASSERT_OK(t.workspace.validate());
}

} // namespace
} // namespace studyapp::document::test
