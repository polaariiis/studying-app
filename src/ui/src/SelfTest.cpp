#include <studyapp/ui/SelfTest.hpp>

#include "PageExport.hpp"
#include "SessionDocumentRasterizer.hpp"

#include <studyapp/application/Search.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/canvas/DocumentRasterizer.hpp>
#include <studyapp/canvas/TextLayout.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/render_gl/OffscreenCheck.hpp>

#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QTemporaryDir>

#include <filesystem>
#include <optional>

namespace studyapp::ui {

namespace {

std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.toStdU16String());
}

/// Collects check results.
class Checks {
public:
    explicit Checks(const std::function<void(const std::string&)>& log) : log_(&log) {}

    bool check(bool passed, const std::string& what, const std::string& detail = {}) {
        if (passed) {
            (*log_)("ok: " + what);
        } else {
            const std::string line = what + (detail.empty() ? "" : " (" + detail + ")");
            (*log_)("FAILED: " + line);
            failures_.push_back(line);
        }
        return passed;
    }
    template <class T>
    bool check(const core::Result<T>& result, const std::string& what) {
        return check(result.has_value(), what, result ? std::string{} : result.error().message);
    }

    [[nodiscard]] std::vector<std::string> failures() && { return std::move(failures_); }

private:
    const std::function<void(const std::string&)>* log_;
    std::vector<std::string> failures_;
};

std::uintmax_t sizeOf(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    return ec ? 0 : size;
}

} // namespace

std::vector<std::string> runSelfTest(const ShellServices& services, canvas::TextLayout& textLayout,
                                     const std::function<void(const std::string&)>& log,
                                     bool requireOpenGl) {
    Checks checks(log);

    // Qt runtime: platform plugin and image formats (plugins deployed with the app).
    checks.check(!QGuiApplication::platformName().isEmpty(),
                 "Qt platform plugin: " + QGuiApplication::platformName().toStdString());
    const QList<QByteArray> formats = QImageReader::supportedImageFormats();
    for (const char* format : {"png", "jpg", "gif", "bmp"}) {
        checks.check(formats.contains(format), std::string("image format ") + format);
    }

    // OpenGL 3.3 core: the canvas renderer draws offscreen with the driver and the C++
    // runtime this process loaded (docs/BUILDING.md §8).
    {
        auto rendered = render_gl::checkOffscreenRendering();
        if (rendered || requireOpenGl) {
            checks.check(rendered.has_value(),
                         "OpenGL 3.3 core rendering" +
                             (rendered ? ": " + *rendered : std::string{}),
                         rendered ? std::string{} : rendered.error().message);
        } else {
            log("not required: OpenGL 3.3 core rendering (" + rendered.error().message + ")");
        }
    }

    // Fonts and text layout: a text box raster with ink in it.
    {
        const render::ImageData raster =
            textLayout.rasterize("Self-test Ag", {200.0F, 40.0F}, canvas::kTextSize, 1.0F);
        bool ink = false;
        for (std::size_t i = 3; i < raster.pixels.size() && !ink; i += 4) {
            ink = raster.pixels[i] != 0;
        }
        checks.check(!raster.empty() && ink, "fonts and text layout");
    }

    QTemporaryDir scratch;
    if (!checks.check(scratch.isValid(), "temporary directory")) {
        return std::move(checks).failures();
    }
    const auto dir = toPath(scratch.path());

    // Workspace: create, edit, search, close, reopen (SQLite, FTS5, WAL).
    const auto root = dir / "SelfTest.studyws";
    const application::SessionServices sessionServices{services.clock, services.ids,
                                                       services.locker};
    auto created = application::WorkspaceSession::create(root, "Self-test", sessionServices);
    if (!checks.check(created, "create a workspace")) {
        return std::move(checks).failures();
    }
    std::unique_ptr<application::WorkspaceSession> session = std::move(*created);
    application::WorkspaceStructure structure(*session, services.clock, services.ids);
    auto notebook = structure.createNotebook("Self-test");
    checks.check(notebook, "create a notebook");
    if (notebook) {
        const auto layer = session->workspace().layersOf(notebook->page).front();
        auto text = document::commands::createElement(
            session->workspace(), layer,
            {.payload = document::TextBox{.size = {200, 40}, .text = "Photosynthesis"}},
            services.ids);
        checks.check(text && session->execute(std::move(text->command)).has_value(),
                     "add a text box");
        auto found = application::search(*session, "photo", 10);
        checks.check(found && found->size() == 1, "full-text search");
    }
    checks.check(session->close(), "close the workspace");
    session.reset();
    auto reopened = application::WorkspaceSession::open(root, {}, sessionServices);
    if (!checks.check(reopened, "reopen the workspace")) {
        return std::move(checks).failures();
    }
    session = std::move(*reopened);
    checks.check(session->workspace().elementCount() == 1, "content survives reopening");

    // Qt PDF: write a PDF, inspect it, import it, render a tile of it.
    const auto pdf = dir / "self-test.pdf";
    {
        QPdfWriter writer(QString::fromStdU16String(pdf.u16string()));
        writer.setPageSize(QPageSize(QPageSize::A5));
        QPainter painter(&writer);
        painter.fillRect(QRect(0, 0, 400, 400), Qt::black);
        writer.newPage();
    }
    auto info = inspectPdf(pdf);
    checks.check(info.has_value() && info->pageSizes.size() == 2, "read a PDF (Qt PDF)",
                 info ? std::string{} : info.error().message);
    if (info) {
        auto asset = session->importAsset(pdf, "application/pdf");
        std::optional<application::WorkspaceStructure::ImportedDocument> imported;
        if (asset) {
            application::WorkspaceStructure pdfStructure(*session, services.clock, services.ids);
            if (auto made = pdfStructure.importDocument(std::nullopt, "Self-test PDF", *asset,
                                                        info->pageSizes)) {
                imported = *made;
            }
        }
        checks.check(imported.has_value(), "import a PDF");
        if (imported) {
            SessionDocumentRasterizer rasterizer(*session);
            const canvas::DocumentTileKey key{.asset = *asset, .page = 0, .level = -1};
            (void)rasterizer.tile(key);
            rasterizer.waitForTiles();
            const auto tile = rasterizer.tile(key);
            checks.check(tile && !tile->empty(), "render a PDF page (PDFium)");
        }
    }

    // Export and printing.
    if (session->workspace().pageCount() > 0) {
        const ExportSources sources{
            .assetPath = [&](core::AssetId asset) -> std::optional<std::filesystem::path> {
                auto path = session->assetPath(asset);
                return path ? std::optional(*path) : std::nullopt;
            },
            .progress = {}};
        const auto pages = session->workspace().pagesOf(
            session->workspace().sectionsOf(session->workspace().notebooks().front()).front());
        const std::vector<core::PageId> first{pages.front()};
        const std::pair<ExportFormat, const char*> exports[] = {
            {ExportFormat::Pdf, "pdf"}, {ExportFormat::Png, "png"}, {ExportFormat::Svg, "svg"}};
        for (const auto& [format, extension] : exports) {
            const auto target = dir / (std::string("export.") + extension);
            auto written = exportPages(session->workspace(), first, target, format, sources);
            checks.check(written.has_value() && sizeOf(target) > 0,
                         std::string("export ") + extension,
                         written ? std::string{} : written.error().message);
        }
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        const auto printed = dir / "print.pdf";
        printer.setOutputFileName(QString::fromStdU16String(printed.u16string()));
        auto done = printPages(printer, session->workspace(), first, sources);
        checks.check(done.has_value() && sizeOf(printed) > 0, "print (Qt Print Support)",
                     done ? std::string{} : done.error().message);
    }
    checks.check(session->close(), "close");
    return std::move(checks).failures();
}

} // namespace studyapp::ui
