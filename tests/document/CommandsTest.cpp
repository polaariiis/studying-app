// Commands: successful and failing execution, determinism, cascading deletes, atomicity.

#include <studyapp/document/Commands.hpp>

#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
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
