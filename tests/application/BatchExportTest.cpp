// Batch export (1.2-CMD-03): file names from section titles, the plan of a notebook export
// (order, unique names, empty sections) and the sequential runner (failures, cancellation,
// progress). Qt-free.

#include <studyapp/application/BatchExport.hpp>

#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace studyapp::application {
namespace {

using State = BatchExportOutcome::State;

/// `count` copies of `text`.
std::string repeated(std::string_view text, std::size_t count) {
    std::string all;
    for (std::size_t i = 0; i < count; ++i) {
        all += text;
    }
    return all;
}

std::vector<std::string> fileNamesOf(const std::vector<BatchExportItem>& items) {
    std::vector<std::string> names;
    for (const BatchExportItem& item : items) {
        names.push_back(item.fileName);
    }
    return names;
}

std::vector<State> statesOf(const BatchExportResult& result) {
    std::vector<State> states;
    for (const BatchExportOutcome& outcome : result.outcomes) {
        states.push_back(outcome.state);
    }
    return states;
}

/// Items as a plan gives them: one page each, except the titles listed in `empty`.
std::vector<BatchExportItem> itemsOf(const std::vector<std::string>& titles,
                                     const std::vector<std::string>& empty = {}) {
    testing::SequentialIds ids;
    std::vector<BatchExportItem> items;
    for (const std::string& title : titles) {
        const bool skipped = std::find(empty.begin(), empty.end(), title) != empty.end();
        items.push_back({.section = core::SectionId::generate(ids),
                         .title = title,
                         .pages = skipped ? std::vector<core::PageId>{}
                                          : std::vector<core::PageId>{core::PageId::generate(ids)},
                         .fileName = skipped ? std::string() : title + ".pdf"});
    }
    return items;
}

const std::string kUmlaut = "\xC3\xA4";        // ä, 2 bytes
const std::string kKanji = "\xE6\x95\xB0";     // 数, 3 bytes
const std::string kEmoji = "\xF0\x9F\x93\x9D"; // U+1F4DD, 4 bytes

/// Whether `text` is well-formed UTF-8 (no cut, overlong or surrogate sequence).
bool isValidUtf8(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at]);
        const std::size_t length = lead < 0x80U              ? 1
                                   : (lead & 0xE0U) == 0xC0U ? 2
                                   : (lead & 0xF0U) == 0xE0U ? 3
                                   : (lead & 0xF8U) == 0xF0U ? 4
                                                             : 0;
        if (length == 0 || at + length > text.size()) {
            return false;
        }
        char32_t code = length == 1 ? lead : lead & (0xFFU >> (length + 1));
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[at + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        const char32_t smallest = length == 1   ? 0U
                                  : length == 2 ? 0x80U
                                  : length == 3 ? 0x800U
                                                : 0x10000U;
        if (code < smallest || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
            return false;
        }
        at += length;
    }
    return true;
}

/// Code points of well-formed UTF-8: the bytes that do not continue a character.
std::size_t codePointsOf(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
    }));
}

core::Result<void> failure(core::ErrorCode code, std::string message) {
    return core::makeError(code, std::move(message));
}

// ---------------------------------------------------------------------------- file names

TEST(ExportFileNameTest, KeepsOrdinaryAndNonAsciiTitles) {
    EXPECT_EQ(exportFileName("Lecture 3"), "Lecture 3");
    EXPECT_EQ(exportFileName("Week 1 - notes (draft)"), "Week 1 - notes (draft)");
    EXPECT_EQ(exportFileName("\xC3\x9C"
                             "bung 2"),
              "\xC3\x9C"
              "bung 2"); // Übung 2
    EXPECT_EQ(exportFileName("\xD0\x9B\xD0\xB5\xD0\xBA\xD1\x86\xD0\xB8\xD1\x8F"),
              "\xD0\x9B\xD0\xB5\xD0\xBA\xD1\x86\xD0\xB8\xD1\x8F");                     // Лекция
    EXPECT_EQ(exportFileName("\xE6\x95\xB0\xE5\xAD\xA6"), "\xE6\x95\xB0\xE5\xAD\xA6"); // 数学
    EXPECT_EQ(exportFileName("Notes \xF0\x9F\x93\x9D"), "Notes \xF0\x9F\x93\x9D");     // U+1F4DD
    EXPECT_EQ(exportFileName(".hidden"), ".hidden");
    EXPECT_EQ(exportFileName("v1.2 plan"), "v1.2 plan");
}

TEST(ExportFileNameTest, ReplacesForbiddenCharacters) {
    EXPECT_EQ(exportFileName("a\\b/c:d*e?f\"g<h>i|j"), "a_b_c_d_e_f_g_h_i_j");
    EXPECT_EQ(exportFileName("Q&A: part 1/2"), "Q&A_ part 1_2");
    EXPECT_EQ(exportFileName("???"), "___");
}

TEST(ExportFileNameTest, ReplacesControlCharacters) {
    EXPECT_EQ(exportFileName("a\tb\nc\rd"), "a_b_c_d");
    EXPECT_EQ(exportFileName(std::string("a\0b", 3)), "a_b");
    EXPECT_EQ(exportFileName("a\x01\x1F\x7F"
                             "b"),
              "a___b");
    EXPECT_EQ(exportFileName("a\xC2\x85"
                             "b"),
              "a_b"); // U+0085, a C1 control
}

TEST(ExportFileNameTest, ReplacesBytesThatAreNotUtf8) {
    EXPECT_EQ(exportFileName("a\xFF"
                             "b"),
              "a_b");
    EXPECT_EQ(exportFileName("a\xC3"), "a_");               // a sequence cut short
    EXPECT_EQ(exportFileName("a\xC0\xAF"), "a__");          // overlong "/"
    EXPECT_EQ(exportFileName("a\xED\xA0\x80"), "a___");     // a surrogate
    EXPECT_EQ(exportFileName("\x80\xC3\xA9"), "_\xC3\xA9"); // a stray continuation byte, then é
}

TEST(ExportFileNameTest, BlankTitlesBecomeTheFallback) {
    EXPECT_EQ(exportFileName(""), "Section");
    EXPECT_EQ(exportFileName("   "), "Section");
    EXPECT_EQ(exportFileName("."), "Section");
    EXPECT_EQ(exportFileName(" . .. "), "Section");
    EXPECT_EQ(exportFileName("", "Page"), "Page");
}

TEST(ExportFileNameTest, RemovesLeadingSpacesAndTrailingSpacesAndDots) {
    EXPECT_EQ(exportFileName("  Notes"), "Notes");
    EXPECT_EQ(exportFileName("Notes  "), "Notes");
    EXPECT_EQ(exportFileName("Notes..."), "Notes");
    EXPECT_EQ(exportFileName("Notes. . "), "Notes");
    EXPECT_EQ(exportFileName("  a  b.c.  "), "a  b.c"); // inner spaces and dots stay
    EXPECT_EQ(exportFileName("etc.?"), "etc._");        // the replacement is not a dot
}

TEST(ExportFileNameTest, WindowsDeviceNamesGetAnUnderscore) {
    for (const char* device : {"CON", "PRN", "AUX", "NUL"}) {
        EXPECT_EQ(exportFileName(device), std::string(device) + "_") << device;
    }
    for (char digit = '1'; digit <= '9'; ++digit) {
        const std::string com = std::string("COM") + digit;
        const std::string lpt = std::string("LPT") + digit;
        EXPECT_EQ(exportFileName(com), com + "_") << com;
        EXPECT_EQ(exportFileName(lpt), lpt + "_") << lpt;
    }
    // Any case, and with an extension (the device is the part before the first dot).
    EXPECT_EQ(exportFileName("con"), "con_");
    EXPECT_EQ(exportFileName("Nul"), "Nul_");
    EXPECT_EQ(exportFileName("lpt9"), "lpt9_");
    EXPECT_EQ(exportFileName("CON.txt"), "CON_.txt");
    EXPECT_EQ(exportFileName("aux.tar.gz"), "aux_.tar.gz");
    EXPECT_EQ(exportFileName("COM1.notes"), "COM1_.notes");
    EXPECT_EQ(exportFileName(" NUL. "), "NUL_"); // what is left after trimming
    // Not devices.
    EXPECT_EQ(exportFileName("COM0"), "COM0");
    EXPECT_EQ(exportFileName("COM10"), "COM10");
    EXPECT_EQ(exportFileName("LPT"), "LPT");
    EXPECT_EQ(exportFileName("CONSOLE"), "CONSOLE");
    EXPECT_EQ(exportFileName("My CON"), "My CON");
    EXPECT_EQ(exportFileName("notes.CON"), "notes.CON");
}

TEST(ExportFileNameTest, KeepsAtMostAHundredCodePoints) {
    EXPECT_EQ(exportFileName(std::string(100, 'a')), std::string(100, 'a'));
    EXPECT_EQ(exportFileName(std::string(101, 'a')), std::string(100, 'a'));
    EXPECT_EQ(exportFileName(std::string(500, 'a')), std::string(100, 'a'));
    EXPECT_EQ(exportFileName(std::string(150, 'a')), std::string(100, 'a'));
    // Two-byte characters are cut between characters: 100 of them, never half of one.
    EXPECT_EQ(exportFileName(repeated(kUmlaut, 130)), repeated(kUmlaut, 100));
    // One ASCII letter first, so a cut by bytes would land inside a character.
    EXPECT_EQ(exportFileName("x" + repeated(kUmlaut, 130)), "x" + repeated(kUmlaut, 99));
    // What the cut leaves is trimmed again; replaced characters count as one each.
    EXPECT_EQ(exportFileName(std::string(99, 'a') + " b"), std::string(99, 'a'));
    EXPECT_EQ(exportFileName(std::string(99, 'a') + ".b"), std::string(99, 'a'));
    EXPECT_EQ(exportFileName(std::string(150, '/')), std::string(100, '_'));
    // Leading spaces are not counted.
    EXPECT_EQ(exportFileName("   " + std::string(100, 'a') + "b"), std::string(100, 'a'));
}

TEST(ExportFileNameTest, KeepsAtMostTwoHundredBytes) {
    ASSERT_EQ(kMaxExportNameBytes, 200U);
    // Three- and four-byte characters reach 200 bytes before 100 characters: the cut is the
    // last whole character that fits.
    EXPECT_EQ(exportFileName(repeated(kKanji, 130)), repeated(kKanji, 66)); // 198 bytes
    EXPECT_EQ(exportFileName(repeated(kEmoji, 130)), repeated(kEmoji, 50)); // 200 bytes
    // One ASCII letter first, so a cut at byte 200 would land inside a character.
    EXPECT_EQ(exportFileName("x" + repeated(kKanji, 130)), "x" + repeated(kKanji, 66));   // 199
    EXPECT_EQ(exportFileName("x" + repeated(kEmoji, 130)), "x" + repeated(kEmoji, 49));   // 197
    EXPECT_EQ(exportFileName("xy" + repeated(kKanji, 130)), "xy" + repeated(kKanji, 66)); // 200
    for (const std::string& character : {kKanji, kEmoji}) {
        for (const char* prefix : {"", "x", "xy", "xyz"}) {
            const std::string name = exportFileName(prefix + repeated(character, 130));
            EXPECT_LE(name.size(), kMaxExportNameBytes) << prefix << character;
            EXPECT_GT(name.size(), kMaxExportNameBytes - character.size()) << prefix << character;
            EXPECT_LE(codePointsOf(name), kMaxExportNameLength) << prefix << character;
            EXPECT_TRUE(isValidUtf8(name)) << prefix << character;
        }
    }
    // What the byte cut leaves is trimmed again (the cut falls after " " or "."), and a
    // replaced character is one byte.
    EXPECT_EQ(exportFileName(repeated(kKanji, 65) + "ab " + kKanji), repeated(kKanji, 65) + "ab");
    EXPECT_EQ(exportFileName(repeated(kKanji, 65) + "ab." + kKanji), repeated(kKanji, 65) + "ab");
    EXPECT_EQ(exportFileName(repeated(kKanji, 66) + "??" + kKanji), repeated(kKanji, 66) + "__");
    EXPECT_EQ(exportFileName(repeated(kKanji, 66) + "\xFF\xFF\xFF"), repeated(kKanji, 66) + "__");
}

TEST(ExportFileNameTest, ADeviceNameStillGetsItsUnderscoreAfterTheCut) {
    // The cut applies to the title; the "_" comes on top of it.
    const std::string longAscii = "CON." + std::string(150, 'a');
    EXPECT_EQ(exportFileName(longAscii), "CON_." + std::string(96, 'a')); // 100 characters + "_"
    const std::string longKanji = "aux." + repeated(kKanji, 130);
    const std::string name = exportFileName(longKanji);
    EXPECT_EQ(name, "aux_." + repeated(kKanji, 65)); // 4 + 195 bytes of the title, + "_"
    EXPECT_EQ(name.size(), kMaxExportNameBytes);
    EXPECT_TRUE(isValidUtf8(name));
    // The cut can also be what leaves a device name: "NUL " then the rest is dropped.
    EXPECT_EQ(exportFileName("NUL" + std::string(97, ' ') + "notes"), "NUL_");
    EXPECT_EQ(exportFileName("LPT1" + std::string(96, '.') + "notes"), "LPT1_");
}

// ---------------------------------------------------------------------------- plan

TEST(BatchExportPlanTest, ListsSectionsAndPagesInNotebookOrder) {
    document::test::TestWorkspace t;
    const auto other = t.addNotebook("Other");
    t.addPage(t.addSection(other, "Elsewhere"), "Page");
    const auto notebook = t.addNotebook("Course");
    const auto first = t.addSection(notebook, "Zeta");
    const auto second = t.addSection(notebook, "Alpha");
    const auto third = t.addSection(notebook, "Mid");
    const auto a = t.addPage(first, "One");
    const auto b = t.addPage(first, "Two");
    const auto c = t.addPage(second, "Three");
    const auto d = t.addPage(third, "Four");
    const auto e = t.addPage(third, "Five");
    const auto f = t.addPage(third, "Six");

    const auto plan = planNotebookExport(t.workspace, notebook);
    ASSERT_OK(plan);
    ASSERT_EQ(plan->size(), 3U); // the other notebook's section is not part of it
    // Notebook order (the order the sections were added in), not alphabetical.
    EXPECT_EQ((*plan)[0].section, first);
    EXPECT_EQ((*plan)[1].section, second);
    EXPECT_EQ((*plan)[2].section, third);
    EXPECT_EQ((*plan)[0].title, "Zeta");
    EXPECT_EQ(fileNamesOf(*plan), (std::vector<std::string>{"Zeta.pdf", "Alpha.pdf", "Mid.pdf"}));
    EXPECT_EQ((*plan)[0].pages, (std::vector<core::PageId>{a, b}));
    EXPECT_EQ((*plan)[1].pages, (std::vector<core::PageId>{c}));
    EXPECT_EQ((*plan)[2].pages, (std::vector<core::PageId>{d, e, f}));
    for (const BatchExportItem& item : *plan) {
        EXPECT_FALSE(item.skipped());
    }
    EXPECT_EQ(fileNamesOf(*planNotebookExport(t.workspace, notebook, ".png")),
              (std::vector<std::string>{"Zeta.png", "Alpha.png", "Mid.png"}));
}

TEST(BatchExportPlanTest, DuplicateTitlesAreNumberedInNotebookOrder) {
    document::test::TestWorkspace t;
    const auto notebook = t.addNotebook("Course");
    for (const char* title : {"Notes", "Notes", "Other", "Notes"}) {
        t.addPage(t.addSection(notebook, title), "Page");
    }
    const auto plan = planNotebookExport(t.workspace, notebook);
    ASSERT_OK(plan);
    EXPECT_EQ(fileNamesOf(*plan), (std::vector<std::string>{"Notes.pdf", "Notes (2).pdf",
                                                            "Other.pdf", "Notes (3).pdf"}));
}

TEST(BatchExportPlanTest, NamesDifferingOnlyInAsciiCaseAreNumbered) {
    document::test::TestWorkspace t;
    const auto notebook = t.addNotebook("Course");
    // "a/b" and "a:b" are different titles with the same file name.
    for (const char* title : {"Notes", "NOTES", "notes", "a/b", "a:b", "A_B"}) {
        t.addPage(t.addSection(notebook, title), "Page");
    }
    const auto plan = planNotebookExport(t.workspace, notebook);
    ASSERT_OK(plan);
    EXPECT_EQ(fileNamesOf(*plan),
              (std::vector<std::string>{"Notes.pdf", "NOTES (2).pdf", "notes (3).pdf", "a_b.pdf",
                                        "a_b (2).pdf", "A_B (3).pdf"}));
}

TEST(BatchExportPlanTest, ANumberedNameNeverTakesAnotherSectionsName) {
    document::test::TestWorkspace t;
    const auto notebook = t.addNotebook("Course");
    for (const char* title : {"Notes", "Notes (2)", "Notes", "notes (2)"}) {
        t.addPage(t.addSection(notebook, title), "Page");
    }
    const auto plan = planNotebookExport(t.workspace, notebook);
    ASSERT_OK(plan);
    EXPECT_EQ(fileNamesOf(*plan), (std::vector<std::string>{"Notes.pdf", "Notes (2).pdf",
                                                            "Notes (3).pdf", "notes (2) (2).pdf"}));
}

TEST(BatchExportPlanTest, EmptySectionsAreSkippedAndTakeNoName) {
    document::test::TestWorkspace t;
    const auto notebook = t.addNotebook("Course");
    const auto empty = t.addSection(notebook, "Notes");
    const auto full = t.addSection(notebook, "Notes");
    const auto page = t.addPage(full, "Page");
    const auto last = t.addSection(notebook, "Later");

    const auto plan = planNotebookExport(t.workspace, notebook);
    ASSERT_OK(plan);
    ASSERT_EQ(plan->size(), 3U);
    EXPECT_EQ((*plan)[0].section, empty);
    EXPECT_EQ((*plan)[0].title, "Notes");
    EXPECT_TRUE((*plan)[0].skipped());
    EXPECT_TRUE((*plan)[0].pages.empty());
    EXPECT_FALSE((*plan)[1].skipped());
    EXPECT_EQ((*plan)[1].pages, (std::vector<core::PageId>{page}));
    EXPECT_EQ((*plan)[2].section, last);
    EXPECT_TRUE((*plan)[2].skipped());
    // The empty section does not use up "Notes.pdf".
    EXPECT_EQ(fileNamesOf(*plan), (std::vector<std::string>{"", "Notes.pdf", ""}));
}

TEST(BatchExportPlanTest, MaximumLengthNamesStayDistinctAndWithin255Bytes) {
    document::test::TestWorkspace t;
    const auto notebook = t.addNotebook("Course");
    // Twelve sections with the same title of 130 three-byte characters (390 bytes), one of
    // them a device name once cut, and a longer extension than ".pdf".
    const std::string title = repeated(kKanji, 130);
    for (int i = 0; i < 12; ++i) {
        t.addPage(t.addSection(notebook, title), "Page");
    }
    t.addPage(t.addSection(notebook, "con." + repeated(kEmoji, 130)), "Page");
    for (const char* extension : {".pdf", ".sbexport"}) {
        const auto plan = planNotebookExport(t.workspace, notebook, extension);
        ASSERT_OK(plan);
        ASSERT_EQ(plan->size(), 13U);
        std::set<std::string> names;
        for (const BatchExportItem& item : *plan) {
            EXPECT_LE(item.fileName.size(), 255U) << item.fileName;
            EXPECT_TRUE(isValidUtf8(item.fileName)) << item.fileName;
            EXPECT_TRUE(item.fileName.ends_with(extension)) << item.fileName;
            names.insert(item.fileName);
        }
        EXPECT_EQ(names.size(), 13U);
        const std::string kept = repeated(kKanji, 66); // 198 bytes
        EXPECT_EQ((*plan)[0].fileName, kept + extension);
        EXPECT_EQ((*plan)[1].fileName, kept + " (2)" + extension);
        EXPECT_EQ((*plan)[11].fileName, kept + " (12)" + extension);
        EXPECT_EQ((*plan)[12].fileName, "con_." + repeated(kEmoji, 49) + extension);
    }
}

TEST(BatchExportPlanTest, AnEmptyNotebookGivesAnEmptyPlan) {
    document::test::TestWorkspace t;
    const auto notebook = t.addNotebook("Course");
    const auto plan = planNotebookExport(t.workspace, notebook);
    ASSERT_OK(plan);
    EXPECT_TRUE(plan->empty());
}

TEST(BatchExportPlanTest, AnUnknownNotebookIsAnError) {
    document::test::TestWorkspace t;
    t.addPath();
    const auto plan = planNotebookExport(t.workspace, core::NotebookId::generate(t.ids));
    ASSERT_FALSE(plan);
    EXPECT_EQ(plan.error().code, core::ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------- runner

TEST(BatchExportRunnerTest, WritesEveryItemInOrder) {
    const auto items = itemsOf({"A", "B", "C"});
    const std::atomic<bool> stop{false};
    std::vector<std::string> written;
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem& item) -> core::Result<void> {
            written.push_back(item.fileName);
            return {};
        },
        stop);
    EXPECT_EQ(written, (std::vector<std::string>{"A.pdf", "B.pdf", "C.pdf"}));
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(statesOf(result), (std::vector<State>{State::Done, State::Done, State::Done}));
    EXPECT_EQ(result.count(State::Done), 3U);
    EXPECT_EQ(result.count(State::Failed), 0U);
    EXPECT_EQ(result.count(State::Skipped), 0U);
    EXPECT_EQ(result.count(State::NotStarted), 0U);
}

TEST(BatchExportRunnerTest, AFailedItemDoesNotStopTheBatch) {
    const auto items = itemsOf({"A", "B", "C"});
    const std::atomic<bool> stop{false};
    std::vector<std::string> called;
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem& item) -> core::Result<void> {
            called.push_back(item.title);
            if (item.title == "B") {
                return failure(core::ErrorCode::IoError, "the disk is full");
            }
            return {};
        },
        stop);
    EXPECT_EQ(called, (std::vector<std::string>{"A", "B", "C"}));
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(statesOf(result), (std::vector<State>{State::Done, State::Failed, State::Done}));
    EXPECT_EQ(result.outcomes[1].message, "the disk is full");
    EXPECT_TRUE(result.outcomes[0].message.empty());
    EXPECT_EQ(result.count(State::Done), 2U);
    EXPECT_EQ(result.count(State::Failed), 1U);
}

TEST(BatchExportRunnerTest, EveryErrorButConflictIsAFailure) {
    const auto items = itemsOf({"A", "B", "C", "D", "E", "F"});
    const std::atomic<bool> stop{false};
    const std::vector<core::ErrorCode> codes{
        core::ErrorCode::InvalidArgument, core::ErrorCode::NotFound,
        core::ErrorCode::AlreadyExists,   core::ErrorCode::IoError,
        core::ErrorCode::Unsupported,     core::ErrorCode::Internal};
    std::size_t next = 0;
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem&) -> core::Result<void> { return failure(codes[next++], "no"); },
        stop);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.count(State::Failed), 6U);
}

TEST(BatchExportRunnerTest, StopSetBeforeTheFirstItemWritesNothing) {
    const auto items = itemsOf({"A", "B"});
    const std::atomic<bool> stop{true};
    std::size_t calls = 0;
    std::size_t reports = 0;
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem&) -> core::Result<void> {
            ++calls;
            return {};
        },
        stop, [&](std::size_t, const BatchExportOutcome&) { ++reports; });
    EXPECT_EQ(calls, 0U);
    EXPECT_EQ(reports, 0U);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(statesOf(result), (std::vector<State>{State::NotStarted, State::NotStarted}));
}

TEST(BatchExportRunnerTest, StopSetBetweenItemsKeepsWhatWasWritten) {
    const auto items = itemsOf({"A", "B", "C"});
    std::atomic<bool> stop{false};
    std::vector<std::string> called;
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem& item) -> core::Result<void> {
            called.push_back(item.title);
            stop.store(true); // "cancel" arrives while the first item is being written
            return {};
        },
        stop);
    EXPECT_EQ(called, (std::vector<std::string>{"A"}));
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(statesOf(result),
              (std::vector<State>{State::Done, State::NotStarted, State::NotStarted}));
}

TEST(BatchExportRunnerTest, StopSetDuringTheLastItemIsNotACancellation) {
    const auto items = itemsOf({"A", "B"});
    std::atomic<bool> stop{false};
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem& item) -> core::Result<void> {
            stop.store(item.title == "B");
            return {};
        },
        stop);
    EXPECT_FALSE(result.cancelled); // everything was written
    EXPECT_EQ(statesOf(result), (std::vector<State>{State::Done, State::Done}));
}

TEST(BatchExportRunnerTest, AnItemReturningConflictCancelsTheBatch) {
    const auto items = itemsOf({"A", "B", "C"});
    const std::atomic<bool> stop{false};
    std::vector<std::string> called;
    std::vector<std::size_t> reported;
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem& item) -> core::Result<void> {
            called.push_back(item.title);
            if (item.title == "B") {
                return failure(core::ErrorCode::Conflict, "cancelled");
            }
            return {};
        },
        stop, [&](std::size_t index, const BatchExportOutcome&) { reported.push_back(index); });
    EXPECT_EQ(called, (std::vector<std::string>{"A", "B"}));
    EXPECT_TRUE(result.cancelled);
    // The cancelled item is not a failure and is not reported.
    EXPECT_EQ(statesOf(result),
              (std::vector<State>{State::Done, State::NotStarted, State::NotStarted}));
    EXPECT_TRUE(result.outcomes[1].message.empty());
    EXPECT_EQ(result.count(State::Failed), 0U);
    EXPECT_EQ(reported, (std::vector<std::size_t>{0}));
}

TEST(BatchExportRunnerTest, AnItemThatThrowsFailsAndTheRestContinue) {
    const auto items = itemsOf({"A", "B", "C", "D"});
    const std::atomic<bool> stop{false};
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem& item) -> core::Result<void> {
            if (item.title == "A") {
                throw std::runtime_error("out of paper");
            }
            if (item.title == "C") {
                throw 42; // not a std::exception, on purpose
            }
            return {};
        },
        stop);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(statesOf(result),
              (std::vector<State>{State::Failed, State::Done, State::Failed, State::Done}));
    EXPECT_EQ(result.outcomes[0].message, "out of paper");
    EXPECT_EQ(result.outcomes[2].message, "unknown error");
}

TEST(BatchExportRunnerTest, SkippedItemsAreReportedWithoutBeingWritten) {
    const auto items = itemsOf({"A", "Empty", "C", "Also empty"}, {"Empty", "Also empty"});
    const std::atomic<bool> stop{false};
    std::vector<std::string> called;
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem& item) -> core::Result<void> {
            called.push_back(item.title);
            return {};
        },
        stop);
    EXPECT_EQ(called, (std::vector<std::string>{"A", "C"}));
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(statesOf(result),
              (std::vector<State>{State::Done, State::Skipped, State::Done, State::Skipped}));
    EXPECT_EQ(result.count(State::Done), 2U);
    EXPECT_EQ(result.count(State::Skipped), 2U);
    EXPECT_EQ(result.count(State::Failed), 0U);
}

TEST(BatchExportRunnerTest, ProgressIsCalledOncePerItemInOrderAfterItsWrite) {
    const auto items = itemsOf({"A", "Empty", "C", "D"}, {"Empty"});
    const std::atomic<bool> stop{false};
    std::vector<std::string> events;
    std::vector<BatchExportOutcome> outcomes;
    const auto result = runBatchExport(
        items,
        [&](const BatchExportItem& item) -> core::Result<void> {
            events.push_back("write " + item.title);
            if (item.title == "C") {
                return failure(core::ErrorCode::IoError, "denied");
            }
            return {};
        },
        stop,
        [&](std::size_t index, const BatchExportOutcome& outcome) {
            events.push_back("progress " + std::to_string(index));
            outcomes.push_back(outcome);
        });
    EXPECT_EQ(events, (std::vector<std::string>{"write A", "progress 0", "progress 1", "write C",
                                                "progress 2", "write D", "progress 3"}));
    ASSERT_EQ(outcomes.size(), 4U);
    EXPECT_EQ(outcomes[0].state, State::Done);
    EXPECT_EQ(outcomes[1].state, State::Skipped);
    EXPECT_EQ(outcomes[2].state, State::Failed);
    EXPECT_EQ(outcomes[2].message, "denied");
    EXPECT_EQ(outcomes[3].state, State::Done);
    EXPECT_EQ(outcomes, result.outcomes); // what was reported is what is returned
}

TEST(BatchExportRunnerTest, NoItemsIsAnEmptyResult) {
    const std::atomic<bool> stop{false};
    std::size_t calls = 0;
    const auto result = runBatchExport(
        {},
        [&](const BatchExportItem&) -> core::Result<void> {
            ++calls;
            return {};
        },
        stop);
    EXPECT_EQ(calls, 0U);
    EXPECT_FALSE(result.cancelled);
    EXPECT_TRUE(result.outcomes.empty());
}

} // namespace
} // namespace studyapp::application
