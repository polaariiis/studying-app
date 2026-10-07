// Searching text extracted from PDFs (1.2-CMD-01): matching rules, context, ordering and
// limits; which sections show which PDF pages; the bounded text cache. Qt-free.

#include <studyapp/application/PdfTextSearch.hpp>

#include <studyapp/document/Commands.hpp>
#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace studyapp::application {
namespace {

PdfDocumentText document(std::vector<std::string> pages) {
    return {.pages = std::move(pages), .truncated = false};
}

std::vector<std::size_t> pagesOf(const std::vector<PdfTextMatch>& matches) {
    std::vector<std::size_t> pages;
    for (const PdfTextMatch& match : matches) {
        pages.push_back(match.page);
    }
    return pages;
}

const PdfDocumentText kLecture = document({
    "Chapter 1\r\nIntroduction to linear algebra.",
    "The eigenvalue of matrix A is \xCE\xBB.\r\nEvery Eigenvalue has an eigenvector.",
    "",
    "We calculate the eigen-\r\nvalues; see Fourier\r\ntransform (FFT) and f(x) = 2x.",
});

TEST(PdfTextSearchTest, FindsAWordOnTheRightPages) {
    const auto matches = findInPdfText(kLecture, "eigenvalue");
    EXPECT_EQ(pagesOf(matches), (std::vector<std::size_t>{1, 1}));
    EXPECT_EQ(matches[0].offset, 4U);
    EXPECT_LT(matches[0].offset, matches[1].offset); // position order within a page
}

TEST(PdfTextSearchTest, IgnoresCase) {
    EXPECT_EQ(findInPdfText(kLecture, "EIGENVALUE").size(), 2U);
    EXPECT_EQ(findInPdfText(kLecture, "eIgEnVeCtOr").size(), 1U);
    // Beyond ASCII: Latin-1, Latin Extended-A, Greek, Cyrillic.
    const auto text = document({"\xC3\x9C"
                                "BUNG \xC5\x81\xC3\x93"
                                "D\xC5\xB9 \xCE\x9B\xCE\xA3 "
                                "\xD0\x9C\xD0\x90\xD0\xA2\xD0\xA0\xD0\x98\xD0\xA6\xD0\x90"});
    EXPECT_EQ(findInPdfText(text, "\xC3\xBC"
                                  "bung")
                  .size(),
              1U); // übung
    EXPECT_EQ(findInPdfText(text, "\xC5\x82\xC3\xB3"
                                  "d\xC5\xBA")
                  .size(),
              1U);                                                 // łódź
    EXPECT_EQ(findInPdfText(text, "\xCE\xBB\xCF\x82").size(), 1U); // λς (final σ)
    EXPECT_EQ(
        findInPdfText(text, "\xD0\xBC\xD0\xB0\xD1\x82\xD1\x80\xD0\xB8\xD1\x86\xD0\xB0").size(),
        1U); // матрица
}

TEST(PdfTextSearchTest, FindsPhrasesAcrossLineBreaks) {
    const auto matches = findInPdfText(kLecture, "Fourier transform");
    ASSERT_EQ(matches.size(), 1U);
    EXPECT_EQ(matches[0].page, 3U);
    EXPECT_EQ(findInPdfText(kLecture, "  fourier \t transform ").size(), 1U);
    EXPECT_EQ(findInPdfText(kLecture, "linear algebra.").size(), 1U);
    EXPECT_TRUE(findInPdfText(kLecture, "fourier transformation").empty());
}

TEST(PdfTextSearchTest, KeepsPunctuationAsWritten) {
    EXPECT_EQ(findInPdfText(kLecture, "f(x) = 2x.").size(), 1U);
    EXPECT_EQ(findInPdfText(kLecture, "(FFT)").size(), 1U);
    EXPECT_EQ(findInPdfText(kLecture, "eigen-").size(), 1U);
    EXPECT_TRUE(findInPdfText(kLecture, "f[x]").empty());
    // Typographic quotes and dashes match what people type.
    const auto text = document({"don\xE2\x80\x99t \xE2\x80\x9Cquote\xE2\x80\x9D 1\xE2\x80\x93"
                                "2"});
    EXPECT_EQ(findInPdfText(text, "don't").size(), 1U);
    EXPECT_EQ(findInPdfText(text, "\"quote\"").size(), 1U);
    EXPECT_EQ(findInPdfText(text, "1-2").size(), 1U);
    // Ligatures are spelled out.
    EXPECT_EQ(findInPdfText(document({"de\xEF\xAC\x81nition"}), "definition").size(), 1U);
}

TEST(PdfTextSearchTest, NoMatchesAndEmptyQueries) {
    EXPECT_TRUE(findInPdfText(kLecture, "quaternion").empty());
    EXPECT_TRUE(findInPdfText(kLecture, "").empty());
    EXPECT_TRUE(findInPdfText(kLecture, " \t\n").empty());
    EXPECT_TRUE(findInPdfText(document({}), "x").empty());
    EXPECT_EQ(normalizePdfQuery("  a \t b\n"), "a b");
    EXPECT_EQ(normalizePdfQuery(" \t "), "");
}

TEST(PdfTextSearchTest, OrdersByPageThenPositionAndHonoursTheLimit) {
    std::vector<std::string> pages;
    for (int i = 0; i < 30; ++i) {
        pages.push_back("x y x y x");
    }
    const auto all = findInPdfText(document(pages), "x", 1000);
    ASSERT_EQ(all.size(), 90U);
    for (std::size_t i = 1; i < all.size(); ++i) {
        EXPECT_TRUE(all[i - 1].page < all[i].page ||
                    (all[i - 1].page == all[i].page && all[i - 1].offset < all[i].offset));
    }
    EXPECT_EQ(findInPdfText(document(pages), "x").size(), kMaxPdfMatchesPerDocument);
    EXPECT_EQ(findInPdfText(document(pages), "x", 7).size(), 7U);
    EXPECT_TRUE(findInPdfText(document(pages), "x", 0).empty());
    // Occurrences do not overlap.
    EXPECT_EQ(findInPdfText(document({"aaaa"}), "aa").size(), 2U);
}

TEST(PdfTextSearchTest, ContextIsOneLineAroundTheMatch) {
    const auto matches = findInPdfText(kLecture, "eigenvector", 10, 20);
    ASSERT_EQ(matches.size(), 1U);
    // About 20 bytes before the match, starting at a whole word; the page's end after it.
    EXPECT_EQ(matches[0].context, "…Eigenvalue has an eigenvector.");
    const auto spanning = findInPdfText(kLecture, "matrix", 10, 30);
    ASSERT_EQ(spanning.size(), 1U);
    EXPECT_EQ(spanning[0].context, "The eigenvalue of matrix A is \xCE\xBB. Every Eigenvalue…");
    const std::string longPage(5000, 'a');
    const auto far = findInPdfText(document({longPage + " needle " + longPage}), "needle", 1, 10);
    ASSERT_EQ(far.size(), 1U);
    EXPECT_EQ(far[0].context, "…aaaaaaaaa needle aaaaaaaaa…"); // long words are cut
    EXPECT_LT(far[0].context.size(), 40U);                     // never the whole page
}

TEST(PdfTextSearchTest, BrokenUtf8OnlyMatchesItself) {
    const auto text = document({std::string("ab\xFF"
                                            "cd ab cd")});
    EXPECT_EQ(findInPdfText(text, "ab cd").size(), 1U);
    EXPECT_EQ(findInPdfText(text, std::string("b\xFF"
                                              "c"))
                  .size(),
              1U);
}

// ---------------------------------------------------------------------------- sources

TEST(PdfSourcesTest, MapsPdfPagesToTheSectionsShowingThem) {
    document::test::TestWorkspace t;
    const auto notebook = t.addNotebook("Course");
    const auto plain = t.addSection(notebook, "Notes");
    t.addPage(plain, "Ink");
    const core::AssetId lecture = core::AssetId::generate(t.ids);
    const core::AssetId slides = core::AssetId::generate(t.ids);
    const std::vector<core::DVec2> sizes(3, core::DVec2{800, 600});
    auto first = document::commands::createDocumentSection(t.workspace, notebook, "Lecture",
                                                           lecture, sizes, t.clock, t.ids);
    ASSERT_TRUE(first);
    const core::SectionId lectureSection = first->id;
    ASSERT_TRUE(t.editor.execute(std::move(first->command)));
    auto again = document::commands::createDocumentSection(t.workspace, notebook, "Lecture copy",
                                                           lecture, sizes, t.clock, t.ids);
    ASSERT_TRUE(again);
    ASSERT_TRUE(t.editor.execute(std::move(again->command)));
    auto other = document::commands::createDocumentSection(t.workspace, notebook, "Slides", slides,
                                                           std::vector<core::DVec2>(2, {800, 600}),
                                                           t.clock, t.ids);
    ASSERT_TRUE(other);
    ASSERT_TRUE(t.editor.execute(std::move(other->command)));

    const auto sources = pdfSourcesOf(t.workspace);
    ASSERT_EQ(sources.size(), 3U); // the plain section has no PDF; the PDF imported twice: two
    EXPECT_EQ(sources[0].asset, lecture);
    EXPECT_EQ(sources[0].section, lectureSection);
    EXPECT_EQ(sources[0].title, "Course › Lecture");
    EXPECT_EQ(sources[1].asset, lecture);
    EXPECT_EQ(sources[2].asset, slides);
    ASSERT_EQ(sources[0].pages.size(), 3U);
    const auto pages = t.workspace.pagesOf(lectureSection);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(sources[0].pageFor(i), pages[i]);
        EXPECT_EQ(t.workspace.findPage(pages[i])->document->index, static_cast<std::int32_t>(i));
    }
    EXPECT_FALSE(sources[0].pageFor(3));

    // A deleted page is no longer a place to go.
    t.run(document::commands::deletePage(t.workspace, pages[1]));
    const auto after = pdfSourcesOf(t.workspace);
    EXPECT_FALSE(after[0].pageFor(1));
    EXPECT_EQ(after[0].pageFor(2), pages[2]);
    const document::test::TestWorkspace empty;
    EXPECT_TRUE(pdfSourcesOf(empty.workspace).empty());
}

// ---------------------------------------------------------------------------- cache

std::shared_ptr<const PdfDocumentText> textOf(std::size_t bytes) {
    return std::make_shared<const PdfDocumentText>(document({std::string(bytes, 'x')}));
}

core::AssetId asset(std::uint8_t n) {
    core::Uuid::Bytes bytes{};
    bytes[15] = n;
    bytes[6] = 0x70; // version 7
    return core::AssetId{core::Uuid(bytes)};
}

TEST(PdfTextCacheTest, KeepsAndFindsText) {
    PdfTextCache cache;
    EXPECT_EQ(cache.find(asset(1)), nullptr);
    const auto text = textOf(100);
    cache.insert(asset(1), text);
    EXPECT_EQ(cache.find(asset(1)), text);
    const auto stats = cache.stats();
    EXPECT_EQ(stats.entries, 1U);
    EXPECT_EQ(stats.bytes, text->byteSize());
    EXPECT_EQ(stats.hits, 1U);
    EXPECT_EQ(stats.misses, 1U);
    cache.clear();
    EXPECT_EQ(cache.find(asset(1)), nullptr);
    EXPECT_EQ(cache.stats().bytes, 0U);
}

TEST(PdfTextCacheTest, IsBoundedByBytesAndEntriesLeastRecentlyUsedFirst) {
    const std::size_t one = textOf(1000)->byteSize();
    PdfTextCache cache(3 * one, 10);
    cache.insert(asset(1), textOf(1000));
    cache.insert(asset(2), textOf(1000));
    cache.insert(asset(3), textOf(1000));
    (void)cache.find(asset(1)); // 2 is now the least recently used
    cache.insert(asset(4), textOf(1000));
    EXPECT_EQ(cache.find(asset(2)), nullptr);
    EXPECT_NE(cache.find(asset(1)), nullptr);
    EXPECT_NE(cache.find(asset(3)), nullptr);
    EXPECT_NE(cache.find(asset(4)), nullptr);
    EXPECT_LE(cache.stats().bytes, 3 * one);

    PdfTextCache few(1'000'000, 2);
    few.insert(asset(1), textOf(10));
    few.insert(asset(2), textOf(10));
    few.insert(asset(3), textOf(10));
    EXPECT_EQ(few.stats().entries, 2U);
    EXPECT_EQ(few.find(asset(1)), nullptr);

    // Larger than the whole budget: not kept, nothing else pushed out.
    cache.insert(asset(9), textOf(10 * 1000));
    EXPECT_EQ(cache.find(asset(9)), nullptr);
    EXPECT_NE(cache.find(asset(4)), nullptr);
    // Replacing an entry does not count it twice.
    cache.insert(asset(4), textOf(1000));
    EXPECT_LE(cache.stats().bytes, 3 * one);
    cache.insert(asset(5), nullptr); // ignored
    EXPECT_EQ(cache.find(asset(5)), nullptr);
}

TEST(PdfTextCacheTest, IsSafeAcrossThreads) {
    PdfTextCache cache(64 * textOf(100)->byteSize(), 16);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < 500; ++i) {
                const auto id = asset(static_cast<std::uint8_t>((t * 7 + i) % 40));
                if (!cache.find(id)) {
                    cache.insert(id, textOf(100));
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_LE(cache.stats().entries, 16U);
}

} // namespace
} // namespace studyapp::application
