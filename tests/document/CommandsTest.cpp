// Commands: successful and failing execution, determinism, cascading deletes, atomicity.

#include <studyapp/document/Commands.hpp>

#include "TestWorkspace.hpp"

#include <gtest/gtest.h>

#include <chrono>

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
    EXPECT_EQ(notebook->created, t.clock.now());
    EXPECT_EQ(notebook->modified, t.clock.now());
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
    EXPECT_EQ(t.workspace.findNotebook(notebook)->created, createdAt);
    EXPECT_EQ(t.workspace.findNotebook(notebook)->modified, t.clock.now());
    EXPECT_EQ(t.workspace.findPage(page)->modified, t.clock.now());
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

} // namespace
} // namespace studyapp::document::test
