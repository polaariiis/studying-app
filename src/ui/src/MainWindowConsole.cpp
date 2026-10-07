// The command console of the main window (1.2-CMD-01, docs/COMMAND_CONSOLE.md): the dock,
// and the commands — each one a thin front for an operation the window already offers
// (menus, search, import, export, undo), so a console command and its menu equivalent
// change the document in the same way. Nothing here runs programs, opens a shell or touches
// the database: commands call the same application services and window functions as the
// menus, and `pdf search` reads PDF text through the PDF worker process (D53).

#include "CanvasWidget.hpp"
#include "CommandConsole.hpp"
#include "MainWindowWorkspace.hpp"
#include "PageExport.hpp"
#include "PdfInspectionClient.hpp"
#include "PlannerPanel.hpp"

#include <studyapp/application/CommandConsole.hpp>
#include <studyapp/application/ComponentVersions.hpp>
#include <studyapp/application/PdfTextSearch.hpp>
#include <studyapp/application/Search.hpp>
#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/ui/MainWindow.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QStatusBar>
#include <QSysInfo>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace studyapp::ui {

using application::ArgumentKind;
using application::CommandCall;
using application::ConsoleSink;
using application::WorkspaceStructure;

namespace {

/// Most lines a listing command writes (the rest is summarised).
constexpr std::size_t kMaxListed = 500;
/// Most PDF text matches one search shows in all (per document: kMaxPdfMatchesPerDocument).
constexpr std::size_t kMaxPdfMatches = 200;

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string toUtf8(const QString& text) {
    return text.toStdString();
}

std::string pathText(const std::filesystem::path& path) {
    return toUtf8(QDir::toNativeSeparators(QString::fromStdU16String(path.u16string())));
}

core::Result<void> fail(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message));
}

std::string inQuotes(std::string_view text) {
    return "\u201C" + std::string(text) + "\u201D";
}

std::string plural(std::size_t count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

std::string bytesText(std::size_t bytes) {
    constexpr double kMiB = 1024.0 * 1024.0;
    if (bytes >= 1024U * 1024U) {
        return QString::number(static_cast<double>(bytes) / kMiB, 'f', 1).toStdString() + " MiB";
    }
    if (bytes >= 1024U) {
        return std::to_string(bytes / 1024U) + " KiB";
    }
    return std::to_string(bytes) + " bytes";
}

} // namespace

// ---------------------------------------------------------------------------- dock

void MainWindow::createConsoleAction() {
    consoleAction_ = new QAction(tr("Command &Console"), this);
    consoleAction_->setObjectName(QStringLiteral("actionCommandConsole"));
    consoleAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    consoleAction_->setCheckable(true);
    addAction(consoleAction_);
    // The shortcut opens the console, focuses it if it is open elsewhere, and closes it when
    // it has the focus.
    connect(consoleAction_, &QAction::triggered, this, [this] {
        const QWidget* focus = QApplication::focusWidget();
        const bool focused = focus != nullptr && consoleDock_->isAncestorOf(focus);
        if (consoleDock_->isVisible() && focused) {
            consoleDock_->hide();
            if (canvasWidget_ != nullptr) {
                canvasWidget_->setFocus(Qt::ShortcutFocusReason);
            }
        } else {
            showCommandConsole();
        }
        consoleAction_->setChecked(consoleDock_->isVisible());
    });
}

void MainWindow::createConsoleDock() {
    consoleDock_ = new QDockWidget(tr("Command Console"), this);
    consoleDock_->setObjectName(QStringLiteral("commandConsoleDock"));
    consoleDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    consoleDock_->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock_);
    consoleDock_->hide();

    connect(consoleDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        consoleAction_->setChecked(!consoleDock_->isHidden());
        if (visible) {
            ensureConsole(); // also when the window state restored an open console
        }
    });
}

void MainWindow::showCommandConsole() {
    const bool first = console_ == nullptr;
    ensureConsole();
    consoleDock_->show();
    if (first && !consoleDock_->isFloating()) {
        // Compact: a quarter of the window, at least a few lines.
        resizeDocks({consoleDock_}, {std::max(160, height() / 4)}, Qt::Vertical);
    }
    consoleDock_->raise();
    console_->focusInput();
    consoleAction_->setChecked(true);
}

void MainWindow::ensureConsole() {
    if (console_ != nullptr) {
        return;
    }
    commands_ = std::make_unique<application::CommandRegistry>();
    registerConsoleCommands(*commands_);
    console_ = new CommandConsole(*commands_, consoleDock_);
    consoleDock_->setWidget(console_);
    console_->setTargetHandler(
        [this](const application::ConsoleTarget& target) { activateConsoleTarget(target); });
    console_->setCloseHandler([this] {
        consoleDock_->hide();
        if (canvasWidget_ != nullptr) {
            canvasWidget_->setFocus(Qt::ShortcutFocusReason);
        }
    });
}

void MainWindow::activateConsoleTarget(const application::ConsoleTarget& target) {
    if (!open_) {
        console_->laterSink()->error("No workspace is open.");
        return;
    }
    if (const auto* task = std::get_if<core::TaskId>(&target)) {
        if (open_->session->workspace().findTask(*task) == nullptr) {
            console_->laterSink()->error("That task no longer exists.");
            return;
        }
        plannerAction_->setChecked(true);
        planner_->showView(PlannerPanel::View::Tasks);
        planner_->selectTask(*task);
        return;
    }
    const core::PageId page = std::get<core::PageId>(target);
    if (open_->session->workspace().findPage(page) == nullptr) {
        console_->laterSink()->error("That page no longer exists.");
        return;
    }
    (void)openPage(page);
}

// ---------------------------------------------------------------------------- PDF search

core::Result<void> MainWindow::startPdfSearch(const std::string& query,
                                              std::shared_ptr<ConsoleSink> out) {
    const std::string text = application::normalizePdfQuery(query);
    if (text.empty()) {
        return fail("give the text to search for, e.g. pdf search \"Fourier transform\"");
    }
    const document::Workspace& ws = open_->session->workspace();
    std::vector<application::PdfSource> sources = application::pdfSourcesOf(ws);
    if (sources.empty()) {
        out->muted("No imported PDFs in this workspace (File \u203A Import PDF, or import pdf "
                   "\"<path>\").");
        return {};
    }
    // Per PDF (asset): where it is stored and a job id — resolved here, on the GUI thread
    // (the session and the id generator are not thread-safe).
    struct Document {
        core::AssetId asset;
        std::optional<std::filesystem::path> file;
        core::JobId job;
    };
    std::vector<Document> documents;
    for (const application::PdfSource& source : sources) {
        if (std::any_of(documents.begin(), documents.end(),
                        [&](const Document& d) { return d.asset == source.asset; })) {
            continue;
        }
        auto file = open_->session->assetPath(source.asset);
        documents.push_back({.asset = source.asset,
                             .file = file ? std::optional(std::move(*file)) : std::nullopt,
                             .job = core::JobId::generate(*open_->ids)});
    }
    // One search at a time: a new one stops the previous one (its results are not shown).
    if (open_->pdfSearchStop) {
        open_->pdfSearchStop->store(true);
    }
    auto stop = std::make_shared<std::atomic<bool>>(false);
    open_->pdfSearchStop = stop;
    out->muted("Searching the text of " + plural(documents.size(), "PDF", "PDFs") + " for " +
               inQuotes(text) + "\u2026 (cancel stops)");

    PdfWorkerOptions worker;
    worker.program = pdfWorkerProgram_;
    worker.arguments = pdfWorkerArguments_;
    worker.tempRoot = pdfWorkerTempRoot_;
    ++pdfSearches_;
    updateStatus();
    runInBackground([this, text, sources = std::move(sources), documents = std::move(documents),
                     stop, cache = open_->pdfText, worker = std::move(worker),
                     out]() -> std::function<void()> {
        // Pool thread: text from the cache, else from the PDF worker (one process per PDF,
        // one at a time); then matching. Never the GUI thread, never PDFium in this process.
        struct Outcome {
            std::shared_ptr<const application::PdfDocumentText> text;
            std::string error;
        };
        auto outcomes = std::make_shared<std::map<core::AssetId, Outcome>>();
        auto matches =
            std::make_shared<std::vector<std::vector<application::PdfTextMatch>>>(sources.size());
        bool cancelled = false;
        try {
            for (const Document& document : documents) {
                if (stop->load()) {
                    cancelled = true;
                    break;
                }
                Outcome& outcome = (*outcomes)[document.asset];
                outcome.text = cache->find(document.asset);
                if (outcome.text) {
                    continue;
                }
                std::error_code ec;
                if (!document.file || !std::filesystem::is_regular_file(*document.file, ec)) {
                    outcome.error = "the PDF file is missing from the workspace (File \u203A "
                                    "Check Workspace lists missing files)";
                    continue;
                }
                auto extracted =
                    extractPdfTextInWorker(*document.file, document.job, *stop, worker);
                if (!extracted) {
                    if (extracted.error().code == core::ErrorCode::Conflict) {
                        cancelled = true;
                        break;
                    }
                    outcome.error = extracted.error().message;
                    continue;
                }
                outcome.text =
                    std::make_shared<const application::PdfDocumentText>(std::move(*extracted));
                cache->insert(document.asset, outcome.text);
            }
            std::size_t total = 0;
            for (std::size_t i = 0; i < sources.size() && !cancelled; ++i) {
                const Outcome& outcome = (*outcomes)[sources[i].asset];
                if (!outcome.text || total >= kMaxPdfMatches) {
                    continue;
                }
                auto found = application::findInPdfText(
                    *outcome.text, text,
                    std::min(application::kMaxPdfMatchesPerDocument, kMaxPdfMatches - total) + 1);
                // Only pages the section still shows can be opened; others are not reported.
                std::erase_if(found, [&](const application::PdfTextMatch& match) {
                    return !sources[i].pageFor(match.page);
                });
                total += std::min(found.size(), kMaxPdfMatches - total);
                (*matches)[i] = std::move(found);
            }
        } catch (const std::exception& error) {
            return [this, out, what = std::string(error.what())] {
                --pdfSearches_;
                out->error("pdf search: " + what);
                updateStatus();
            };
        }
        return [this, out, text, sources, outcomes, matches, cancelled, stop] {
            --pdfSearches_;
            updateStatus();
            if (cancelled || stop->load()) {
                out->muted("PDF search for " + inQuotes(text) + " was cancelled.");
                return;
            }
            if (open_ && open_->pdfSearchStop == stop) {
                open_->pdfSearchStop.reset();
            }
            out->heading("PDF search: " + inQuotes(text));
            std::size_t shown = 0;
            bool more = false;
            std::size_t documentsWithMatches = 0;
            std::set<core::AssetId> reported;
            for (std::size_t i = 0; i < sources.size(); ++i) {
                const application::PdfSource& source = sources[i];
                const auto outcome = outcomes->find(source.asset);
                if (outcome == outcomes->end()) {
                    continue;
                }
                if (!outcome->second.error.empty()) {
                    if (reported.insert(source.asset).second) {
                        out->error(source.title + ": PDF search failed: " + outcome->second.error +
                                   ".");
                    }
                    continue;
                }
                const auto& list = (*matches)[i];
                if (list.empty()) {
                    continue;
                }
                ++documentsWithMatches;
                out->heading(source.title);
                const std::size_t perDocument =
                    std::min(list.size(), application::kMaxPdfMatchesPerDocument);
                std::size_t k = 0;
                for (; k < perDocument && shown < kMaxPdfMatches; ++k) {
                    const application::PdfTextMatch& match = list[k];
                    out->link("Page " + std::to_string(match.page + 1) + "  " + match.context,
                              *source.pageFor(match.page), 1);
                    ++shown;
                }
                if (k < list.size()) { // findInPdfText was asked for one more than is shown
                    more = true;
                    out->muted("More matches in this PDF are not shown; search for a longer "
                               "phrase.",
                               1);
                }
                if (outcome->second.text->truncated) {
                    out->muted("This PDF has more text than is searched (the first " +
                                   bytesText(ipc::pdf::kMaxTextBytes) + ", " +
                                   bytesText(ipc::pdf::kMaxPageTextBytes) + " per page).",
                               1);
                }
            }
            if (shown == 0) {
                out->text("No PDF text matches found for " + inQuotes(text) + ".");
                // PDFs without any text (scanned pages) cannot be searched: say so.
                for (const application::PdfSource& source : sources) {
                    const auto outcome = outcomes->find(source.asset);
                    if (outcome != outcomes->end() && outcome->second.text &&
                        std::all_of(outcome->second.text->pages.begin(),
                                    outcome->second.text->pages.end(),
                                    [](const std::string& page) { return page.empty(); })) {
                        out->muted(source.title + " has no text layer (a scanned PDF?).", 1);
                    }
                }
                return;
            }
            out->muted(plural(shown, "match", "matches") + (more ? " shown" : "") + " in " +
                       plural(documentsWithMatches, "PDF", "PDFs") +
                       ". Click a result or type go <n> to open its page.");
        };
    });
    return {};
}

bool MainWindow::cancelPdfSearch() {
    if (!open_ || !open_->pdfSearchStop || open_->pdfSearchStop->load()) {
        return false;
    }
    open_->pdfSearchStop->store(true);
    return true;
}

// ---------------------------------------------------------------------------- commands

void MainWindow::registerConsoleCommands(application::CommandRegistry& registry) {
    const auto add = [&registry](application::CommandSpec spec) {
        if (auto added = registry.add(std::move(spec)); !added) {
            core::logError("ui", "console command: " + added.error().message);
        }
    };
    // Every command that needs an open workspace says so the same way.
    const auto session = [this]() -> core::Result<application::WorkspaceSession*> {
        if (!open_) {
            return core::makeError(core::ErrorCode::Unsupported,
                                   "no workspace is open (File \u203A Open Workspace)");
        }
        return open_->session;
    };
    const auto writable = [session]() -> core::Result<application::WorkspaceSession*> {
        auto s = session();
        if (s && (*s)->isReadOnly()) {
            return core::makeError(core::ErrorCode::Unsupported,
                                   "this workspace is open read-only");
        }
        return s;
    };
    const auto title = [this](core::PageId page) {
        return WorkspaceStructure::displayTitle(open_->session->workspace(), page);
    };

    // ---- help
    add({.words = {"help"},
         .aliases = {{"?"}},
         .arguments = {{"command", ArgumentKind::OptionalText}},
         .summary = "List the commands, or explain one",
         .details = {},
         .examples = {"help", "help pdf search"},
         .handler = [&registry](const CommandCall& call, ConsoleSink& out) -> core::Result<void> {
             if (call.has(0)) {
                 return registry.writeHelp(call.arg(0), out);
             }
             registry.writeHelp(out);
             return {};
         }});

    // ---- navigation
    add({.words = {"list", "notebooks"},
         .aliases = {{"notebooks"}},
         .arguments = {},
         .summary = "List the notebooks (each opens its first page)",
         .details = {},
         .examples = {},
         .handler = [session](const CommandCall&, ConsoleSink& out) -> core::Result<void> {
             auto s = session();
             if (!s) {
                 return tl::unexpected(s.error());
             }
             const document::Workspace& ws = (*s)->workspace();
             if (ws.notebookCount() == 0) {
                 out.muted("The workspace has no notebooks yet.");
                 return {};
             }
             for (const core::NotebookId notebook : ws.notebooks()) {
                 std::size_t pages = 0;
                 std::optional<core::PageId> first;
                 for (const core::SectionId section : ws.sectionsOf(notebook)) {
                     const auto list = ws.pagesOf(section);
                     pages += list.size();
                     if (!first && !list.empty()) {
                         first = list.front();
                     }
                 }
                 const std::string line =
                     ws.findNotebook(notebook)->title + "  (" +
                     plural(ws.sectionsOf(notebook).size(), "section", "sections") + ", " +
                     plural(pages, "page", "pages") + ")";
                 if (first) {
                     out.link(line, *first);
                 } else {
                     out.text(line);
                 }
             }
             return {};
         }});
    add({.words = {"list", "sections"},
         .aliases = {{"sections"}},
         .arguments = {},
         .summary = "List the sections of every notebook (each opens its first page)",
         .details = {},
         .examples = {},
         .handler = [this, session](const CommandCall&, ConsoleSink& out) -> core::Result<void> {
             auto s = session();
             if (!s) {
                 return tl::unexpected(s.error());
             }
             const document::Workspace& ws = (*s)->workspace();
             const auto active = open_->navigator.activeSection();
             std::size_t listed = 0;
             for (const core::NotebookId notebook : ws.notebooks()) {
                 out.heading(ws.findNotebook(notebook)->title);
                 for (const core::SectionId section : ws.sectionsOf(notebook)) {
                     if (listed++ == kMaxListed) {
                         out.muted("\u2026 more sections are not listed.");
                         return {};
                     }
                     const auto pages = ws.pagesOf(section);
                     const std::string line = ws.findSection(section)->title + "  (" +
                                              plural(pages.size(), "page", "pages") + ")" +
                                              (active == section ? "  \u2190 current" : "");
                     if (pages.empty()) {
                         out.text(line, 1);
                     } else {
                         out.link(line, pages.front(), 1);
                     }
                 }
             }
             if (ws.notebookCount() == 0) {
                 out.muted("The workspace has no notebooks yet.");
             }
             return {};
         }});
    add({.words = {"list", "pages"},
         .aliases = {{"pages"}},
         .arguments = {{"all", ArgumentKind::Optional}},
         .summary = "List the pages of the current section (`all`: of the whole workspace)",
         .details = {},
         .examples = {"list pages", "list pages all"},
         .handler = [this, session, title](const CommandCall& call,
                                           ConsoleSink& out) -> core::Result<void> {
             auto s = session();
             if (!s) {
                 return tl::unexpected(s.error());
             }
             if (call.has(0) && call.arg(0) != "all") {
                 return fail("use `list pages` or `list pages all`");
             }
             const document::Workspace& ws = (*s)->workspace();
             const auto active = open_->navigator.activePage();
             std::size_t listed = 0;
             const auto listSection = [&](core::SectionId section, int indent) {
                 for (const core::PageId page : ws.pagesOf(section)) {
                     if (listed++ == kMaxListed) {
                         out.muted("\u2026 more pages are not listed.");
                         return false;
                     }
                     const document::PageInfo* info = ws.findPage(page);
                     std::string line = title(page);
                     if (info->document) {
                         line += "  (PDF page " + std::to_string(info->document->index + 1) + ")";
                     }
                     if (active == page) {
                         line += "  \u2190 current";
                     }
                     out.link(line, page, indent);
                 }
                 return true;
             };
             const auto section = open_->navigator.activeSection();
             if (!call.has(0) && section) {
                 out.heading(ws.findNotebook(ws.findSection(*section)->notebook)->title +
                             " \u203A " + ws.findSection(*section)->title);
                 (void)listSection(*section, 1);
                 return {};
             }
             for (const core::NotebookId notebook : ws.notebooks()) {
                 for (const core::SectionId each : ws.sectionsOf(notebook)) {
                     out.heading(ws.findNotebook(notebook)->title + " \u203A " +
                                 ws.findSection(each)->title);
                     if (!listSection(each, 1)) {
                         return {};
                     }
                 }
             }
             if (ws.pageCount() == 0) {
                 out.muted("The workspace has no pages yet.");
             }
             return {};
         }});
    add({.words = {"open", "page"},
         .aliases = {{"open"}},
         .arguments = {{"title", ArgumentKind::Text}},
         .summary = "Open the page with this title",
         .details = "Titles are compared without regard to case. A page of the current section "
                    "wins over the same title elsewhere;\nwithout an exact match, a page whose "
                    "title starts with (then contains) the text. Several matches are listed.",
         .examples = {"open page \"Week 3\"", "open page lecture"},
         .handler = [this, session, title](const CommandCall& call,
                                           ConsoleSink& out) -> core::Result<void> {
             auto s = session();
             if (!s) {
                 return tl::unexpected(s.error());
             }
             const document::Workspace& ws = (*s)->workspace();
             const QString wanted = toQString(call.arg(0)).trimmed();
             const auto section = open_->navigator.activeSection();
             // Every page in workspace order, the current section's first.
             std::vector<core::PageId> pages;
             if (section) {
                 const auto own = ws.pagesOf(*section);
                 pages.assign(own.begin(), own.end());
             }
             for (const core::NotebookId notebook : ws.notebooks()) {
                 for (const core::SectionId each : ws.sectionsOf(notebook)) {
                     if (each != section) {
                         const auto list = ws.pagesOf(each);
                         pages.insert(pages.end(), list.begin(), list.end());
                     }
                 }
             }
             const auto matching = [&](auto&& test) {
                 std::vector<core::PageId> found;
                 for (const core::PageId page : pages) {
                     if (test(toQString(title(page)))) {
                         found.push_back(page);
                     }
                 }
                 return found;
             };
             std::vector<core::PageId> found = matching([&](const QString& t) {
                 return QString::compare(t, wanted, Qt::CaseInsensitive) == 0;
             });
             if (found.size() > 1 && section && ws.findPage(found.front())->section == *section &&
                 ws.findPage(found[1])->section != *section) {
                 found.resize(1); // the current section's page wins
             }
             if (found.empty()) {
                 found = matching(
                     [&](const QString& t) { return t.startsWith(wanted, Qt::CaseInsensitive); });
             }
             if (found.empty()) {
                 found = matching(
                     [&](const QString& t) { return t.contains(wanted, Qt::CaseInsensitive); });
             }
             if (found.empty()) {
                 return core::makeError(core::ErrorCode::NotFound,
                                        "no page titled " + inQuotes(call.arg(0)) +
                                            " (list pages all shows every page)");
             }
             if (found.size() > 1) {
                 out.text(std::to_string(found.size()) + " pages match " + inQuotes(call.arg(0)) +
                          "; open one with go <n>:");
                 for (std::size_t i = 0; i < std::min(found.size(), kMaxListed); ++i) {
                     const document::PageInfo* info = ws.findPage(found[i]);
                     out.link(title(found[i]) + "  (" +
                                  ws.findNotebook(ws.findSection(info->section)->notebook)->title +
                                  " \u203A " + ws.findSection(info->section)->title + ")",
                              found[i], 1);
                 }
                 return {};
             }
             if (!openPage(found.front())) {
                 return core::makeError(core::ErrorCode::Internal, "the page could not be opened");
             }
             out.text("Opened " + inQuotes(title(found.front())) + ".");
             return {};
         }});
    add({.words = {"go"},
         .aliases = {{"open", "result"}},
         .arguments = {{"number", ArgumentKind::Required}},
         .summary = "Open result <number> of the last list (search, pdf search, list …)",
         .details = {},
         .examples = {"go 1"},
         .handler = [this](const CommandCall& call, ConsoleSink& out) -> core::Result<void> {
             bool ok = false;
             const qulonglong number = toQString(call.arg(0)).toULongLong(&ok);
             if (!ok || number == 0) {
                 return fail("give the number shown in [brackets], e.g. go 1");
             }
             if (!console_->activateNumbered(static_cast<std::size_t>(number))) {
                 return core::makeError(core::ErrorCode::NotFound, "there is no result [" +
                                                                       std::to_string(number) +
                                                                       "] in the last list");
             }
             if (const auto page = activePage()) {
                 out.text("Opened " +
                          inQuotes(WorkspaceStructure::displayTitle(open_->session->workspace(),
                                                                    *page)) +
                          ".");
             }
             return {};
         }});

    // ---- search
    add({.words = {"search"},
         .aliases = {{"find"}},
         .arguments = {{"text", ArgumentKind::Text}},
         .summary = "Search page titles, text boxes and tasks (like Edit \u203A Find)",
         .details = "This searches StudyBoard's own content. To search the text inside "
                    "imported PDFs, use pdf search.",
         .examples = {"search eigenvalue", "search \"exam dates\""},
         .handler = [session](const CommandCall& call, ConsoleSink& out) -> core::Result<void> {
             auto s = session();
             if (!s) {
                 return tl::unexpected(s.error());
             }
             constexpr std::size_t kLimit = 50;
             auto found = application::search(**s, call.arg(0), kLimit);
             if (!found) {
                 return tl::unexpected(found.error());
             }
             if (found->empty()) {
                 out.text((*s)->isSearchIndexed()
                              ? "No results for " + inQuotes(call.arg(0)) + "."
                              : std::string("Not indexed yet: open the workspace for editing once "
                                            "to search it."));
                 return {};
             }
             out.heading(plural(found->size(), "result", "results") + " for " +
                         inQuotes(call.arg(0)) + (found->size() == kLimit ? " (the best 50)" : ""));
             for (const application::SearchResult& result : *found) {
                 std::string line = result.title.empty() ? "Untitled" : result.title;
                 const bool snippetAdds = !result.snippet.empty() && result.snippet != result.title;
                 line += "  \u2014  " + (snippetAdds ? result.snippet : result.context);
                 if (result.kind == application::SearchResult::Kind::Task && result.task) {
                     out.link(line, *result.task, 1);
                 } else if (result.page) {
                     out.link(line, *result.page, 1);
                 }
             }
             return {};
         }});
    add({.words = {"pdf", "search"},
         .aliases = {{"pdf", "find"}},
         .arguments = {{"text", ArgumentKind::Text}},
         .summary = "Search the text inside the workspace's imported PDFs",
         .details = "Finds words and phrases in the PDFs' own text (not annotations or titles). "
                    "Case is ignored; line breaks\nin the PDF count as spaces. The text is read "
                    "by the PDF worker process and kept for later searches.\nResults link to "
                    "the page showing that PDF page; `cancel` stops a search.",
         .examples = {"pdf search eigenvalue", "pdf search \"Fourier transform\""},
         .handler = [this, session](const CommandCall& call, ConsoleSink&) -> core::Result<void> {
             if (auto s = session(); !s) {
                 return tl::unexpected(s.error());
             }
             return startPdfSearch(call.arg(0), console_->laterSink());
         }});
    add({.words = {"cancel"},
         .aliases = {},
         .arguments = {},
         .summary = "Stop a running PDF search",
         .details = {},
         .examples = {},
         .handler = [this](const CommandCall&, ConsoleSink& out) -> core::Result<void> {
             if (!cancelPdfSearch()) {
                 out.muted("No PDF search is running.");
             }
             return {};
         }});

    // ---- workspace
    add({.words = {"workspace", "info"},
         .aliases = {{"workspace"}},
         .arguments = {},
         .summary = "Show the open workspace: folder, contents, save state",
         .details = {},
         .examples = {},
         .handler = [session](const CommandCall&, ConsoleSink& out) -> core::Result<void> {
             auto s = session();
             if (!s) {
                 return tl::unexpected(s.error());
             }
             application::WorkspaceSession& open = **s;
             const document::Workspace& ws = open.workspace();
             std::set<core::AssetId> pdfs;
             for (const application::PdfSource& source : application::pdfSourcesOf(ws)) {
                 pdfs.insert(source.asset);
             }
             out.heading(ws.info().name);
             out.text("Folder:     " + pathText(open.root()), 1);
             out.text(std::string("Access:     ") +
                          (open.isReadOnly() ? "read-only" : "read and write"),
                      1);
             out.text("Contents:   " + plural(ws.notebookCount(), "notebook", "notebooks") + ", " +
                          plural(ws.sectionCount(), "section", "sections") + ", " +
                          plural(ws.pageCount(), "page", "pages") + ", " +
                          plural(ws.elementCount(), "element", "elements"),
                      1);
             out.text("Planner:    " + plural(ws.taskCount(), "task", "tasks") + ", " +
                          plural(ws.courseCount(), "course", "courses") + ", " +
                          plural(ws.projectCount(), "project", "projects"),
                      1);
             out.text("PDFs:       " + plural(pdfs.size(), "imported PDF", "imported PDFs"), 1);
             const auto& error = open.lastWriteError();
             out.text("Saved:      " +
                          (error ? "no \u2014 " +
                                       plural(open.pendingWriteCount(), "change", "changes") +
                                       " waiting (" + error->message + ")"
                                 : std::string("all changes saved")),
                      1);
             out.text(std::string("Search:     ") +
                          (open.isSearchIndexed() ? "indexed" : "not indexed yet"),
                      1);
             return {};
         }});

    // ---- import and export
    add({.words = {"import", "pdf"},
         .aliases = {},
         .arguments = {{"path", ArgumentKind::Required}},
         .summary = "Import a PDF file as a new section (like File \u203A Import PDF)",
         .details = "Give the full path; quote it if it contains spaces. The PDF is copied into "
                    "the workspace and checked\nby the PDF worker process. One undo step.",
         .examples = {"import pdf \"C:\\Users\\me\\Lecture 05.pdf\"",
                      "import pdf /home/me/notes.pdf"},
         .handler = [this, writable](const CommandCall& call, ConsoleSink&) -> core::Result<void> {
             if (auto s = writable(); !s) {
                 return tl::unexpected(s.error());
             }
             const QString given = toQString(call.arg(0));
             if (!QDir::isAbsolutePath(given)) {
                 return fail("give the full path of the PDF, e.g. import pdf \"" +
                             toUtf8(QDir::toNativeSeparators(QDir::homePath())) + "/notes.pdf\"");
             }
             const QFileInfo info(given);
             if (!info.exists()) {
                 return core::makeError(core::ErrorCode::NotFound,
                                        inQuotes(toUtf8(QDir::toNativeSeparators(given))) +
                                            " does not exist");
             }
             if (!info.isFile() || !info.isReadable()) {
                 return fail(inQuotes(toUtf8(QDir::toNativeSeparators(given))) +
                             " is not a readable file");
             }
             std::shared_ptr<ConsoleSink> later = console_->laterSink();
             later->muted("Importing " + inQuotes(toUtf8(info.fileName())) + "\u2026");
             importPdfFile(
                 std::filesystem::path(info.absoluteFilePath().toStdU16String()),
                 [later, name = toUtf8(info.completeBaseName())](
                     const QString& error, std::optional<core::PageId> first, std::size_t pages) {
                     if (!error.isEmpty() || !first) {
                         later->error("import pdf: the PDF could not be imported: " +
                                      toUtf8(error));
                         return;
                     }
                     later->link("Imported " + inQuotes(name) + " \u2014 " +
                                     plural(pages, "page", "pages") + " (undo removes it)",
                                 *first);
                 });
             return {};
         }});
    const auto exportCommand = [this, session](ExportFormat format, bool wholeSection) {
        return [this, session, format, wholeSection](const CommandCall& call,
                                                     ConsoleSink& out) -> core::Result<void> {
            if (auto s = session(); !s) {
                return tl::unexpected(s.error());
            }
            const std::vector<core::PageId> pages = pagesToExport(wholeSection);
            if (pages.empty()) {
                return fail("open a page first (open page <title>)");
            }
            const QString extension = format == ExportFormat::Png   ? QStringLiteral("png")
                                      : format == ExportFormat::Svg ? QStringLiteral("svg")
                                                                    : QStringLiteral("pdf");
            QString file;
            if (call.has(0)) {
                file = toQString(call.arg(0));
                if (!QDir::isAbsolutePath(file)) {
                    return fail("give the full path of the file to write");
                }
            } else {
                // No path: the same file dialog as File ▸ Export.
                const document::Workspace& ws = open_->session->workspace();
                const document::PageInfo& page = *ws.findPage(pages.front());
                const std::string& name =
                    wholeSection ? ws.findSection(page.section)->title : page.title;
                const auto chosen = dialogs_->chooseExportTarget(
                    this, toQString(name.empty() ? std::string("Page") : name),
                    format == ExportFormat::Pdf);
                if (!chosen) {
                    out.muted("Export cancelled.");
                    return {};
                }
                file = QString::fromStdU16String(chosen->u16string());
            }
            file = QDir::cleanPath(file);
            const QFileInfo given(file);
            if (given.suffix().isEmpty()) {
                file += QLatin1Char('.') + extension;
            } else if (given.suffix().compare(extension, Qt::CaseInsensitive) != 0) {
                if (call.has(0)) {
                    return fail("the file name ends in ." + toUtf8(given.suffix()) + "; give a ." +
                                toUtf8(extension) + " file name");
                }
                file = given.path() + QLatin1Char('/') + given.completeBaseName() +
                       QLatin1Char('.') + extension;
            }
            const QFileInfo target(file);
            // The dialog asked before replacing the file it offered, not one renamed here.
            const bool confirmed = !call.has(0) && file == QDir::cleanPath(given.filePath());
            if (!confirmed && target.exists()) {
                return core::makeError(core::ErrorCode::AlreadyExists,
                                       inQuotes(toUtf8(QDir::toNativeSeparators(file))) +
                                           " already exists; choose another name");
            }
            if (!target.dir().exists()) {
                return core::makeError(
                    core::ErrorCode::NotFound,
                    "the folder " + inQuotes(toUtf8(QDir::toNativeSeparators(target.path()))) +
                        " does not exist");
            }
            auto written =
                writePagesTo(pages, std::filesystem::path(file.toStdU16String()), format);
            if (!written) {
                if (written.error().code == core::ErrorCode::Conflict) {
                    out.muted("Export cancelled.");
                    return {};
                }
                return written;
            }
            out.text("Exported " + plural(pages.size(), "page", "pages") + " to " +
                     toUtf8(QDir::toNativeSeparators(file)));
            return {};
        };
    };
    const auto exportSpec = [&](std::vector<std::string> words, ExportFormat format,
                                bool wholeSection, std::string summary) {
        add({.words = std::move(words),
             .aliases = {},
             .arguments = {{"path", ArgumentKind::Optional}},
             .summary = std::move(summary),
             .details = "Without a path, the export dialog asks where to save. A path must be "
                        "absolute, outside the workspace,\nand not an existing file.",
             .examples = {},
             .handler = exportCommand(format, wholeSection)});
    };
    exportSpec({"export", "page", "pdf"}, ExportFormat::Pdf, false,
               "Export the current page as PDF");
    exportSpec({"export", "page", "png"}, ExportFormat::Png, false,
               "Export the current page as a PNG image");
    exportSpec({"export", "page", "svg"}, ExportFormat::Svg, false,
               "Export the current page as SVG");
    exportSpec({"export", "section", "pdf"}, ExportFormat::Pdf, true,
               "Export every page of the current section as one PDF");

    // ---- history of the document
    const auto undoRedo = [this, writable](bool isUndo) {
        return
            [this, writable, isUndo](const CommandCall&, ConsoleSink& out) -> core::Result<void> {
                auto s = writable();
                if (!s) {
                    return tl::unexpected(s.error());
                }
                const document::Command* next =
                    isUndo ? (*s)->history().nextUndo() : (*s)->history().nextRedo();
                if (next == nullptr) {
                    out.muted(isUndo ? "Nothing to undo." : "Nothing to redo.");
                    return {};
                }
                const std::string label = next->label;
                if (isUndo) {
                    undo(); // the same path as Edit ▸ Undo
                } else {
                    redo();
                }
                out.text((isUndo ? "Undone: " : "Redone: ") + label);
                return {};
            };
    };
    add({.words = {"undo"},
         .aliases = {},
         .arguments = {},
         .summary = "Undo the last change (like Edit \u203A Undo)",
         .details = {},
         .examples = {},
         .handler = undoRedo(true)});
    add({.words = {"redo"},
         .aliases = {},
         .arguments = {},
         .summary = "Redo the last undone change (like Edit \u203A Redo)",
         .details = {},
         .examples = {},
         .handler = undoRedo(false)});

    // ---- the console itself and diagnostics
    add({.words = {"diagnostics"},
         .aliases = {{"diag"}},
         .arguments = {},
         .summary = "Show versions, platform, workspace state and background work",
         .details = {},
         .examples = {},
         .handler = [this](const CommandCall&, ConsoleSink& out) -> core::Result<void> {
             out.heading(std::string(core::build::kProductName) + " " +
                         std::string(core::build::kVersion));
             std::string components;
             for (const auto& component : application::componentVersions()) {
                 components += component.name + " " + component.version + ", ";
             }
             components += "Qt " + std::string(qVersion()) + " (built with " QT_VERSION_STR ")";
             out.text("Components: " + components, 1);
             out.text("Platform:   " + toUtf8(QSysInfo::prettyProductName()) + ", " +
                          toUtf8(QSysInfo::currentCpuArchitecture()) + ", Qt platform " +
                          toUtf8(QApplication::platformName()),
                      1);
             if (open_) {
                 const application::WorkspaceSession& open = *open_->session;
                 out.text("Workspace:  " + pathText(open.root()) +
                              (open.isReadOnly() ? " (read-only)" : ""),
                          1);
                 out.text("Writes:     " +
                              (open.lastWriteError()
                                   ? plural(open.pendingWriteCount(), "change", "changes") +
                                         " not saved: " + open.lastWriteError()->message
                                   : std::string("all saved")),
                          1);
                 const application::PdfTextCache::Stats cache = open_->pdfText->stats();
                 out.text("PDF text:   " + plural(cache.entries, "PDF", "PDFs") + " cached, " +
                              bytesText(cache.bytes) + " of " + bytesText(cache.byteBudget) + " (" +
                              plural(cache.hits, "hit", "hits") + ", " +
                              plural(cache.misses, "miss", "misses") + ")",
                          1);
             } else {
                 out.text("Workspace:  none open", 1);
             }
             out.text(
                 "Background: " + plural(static_cast<std::size_t>(backgroundJobs_), "job", "jobs") +
                     " (" +
                     plural(static_cast<std::size_t>(pdfSearches_), "PDF search", "PDF searches") +
                     "), pool of " + std::to_string(jobs_->maxThreadCount()) + " threads",
                 1);
             out.text("PDF worker: " +
                          (pdfWorkerProgram_.isEmpty()
                               ? std::string("this application (--pdf-worker), one process per PDF")
                               : toUtf8(QDir::toNativeSeparators(pdfWorkerProgram_))),
                      1);
             out.text("Console:    " + std::to_string(console_->history().size()) + " of " +
                          std::to_string(console_->history().capacity()) + " history entries, " +
                          std::to_string(console_->lines().size()) + " of " +
                          std::to_string(CommandConsole::kMaxLines) + " output lines",
                      1);
             return {};
         }});
    add({.words = {"history"},
         .aliases = {},
         .arguments = {},
         .summary = "Show the commands entered in this session (\u2191/\u2193 recall them)",
         .details = {},
         .examples = {},
         .handler = [this](const CommandCall&, ConsoleSink& out) -> core::Result<void> {
             const auto& entries = console_->history().entries();
             for (std::size_t i = 0; i < entries.size(); ++i) {
                 out.muted(std::to_string(i + 1) + "  " + entries[i], 1);
             }
             return {};
         }});
    add({.words = {"clear"},
         .aliases = {{"cls"}},
         .arguments = {},
         .summary = "Clear the console output (Ctrl+L)",
         .details = {},
         .examples = {},
         .handler = [this](const CommandCall&, ConsoleSink&) -> core::Result<void> {
             console_->clearOutput();
             return {};
         }});
}

} // namespace studyapp::ui
