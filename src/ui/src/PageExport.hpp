#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/core/Rect.hpp>
#include <studyapp/document/Workspace.hpp>

#include <QAbstractTextDocumentLayout>
#include <QFont>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

class QPainter;
class QPrinter;

namespace studyapp::ui {

// Export and printing of pages (Phase 8, docs/ARCHITECTURE.md D45). One painter routine
// draws a page with QPainter from the document — paper, pattern, the PDF page behind it,
// then every visible layer's elements with the canvas's own geometry (strokes, shapes and
// connectors from canvas::buildElementMeshes, text laid out like platform::QtTextLayout,
// images from their assets) — onto a PDF writer, an image, an SVG generator or a printer.
// Exporting only reads: the workspace, its assets and imported PDFs are never written, and
// the target file is written through QSaveFile (complete or not at all).
//
// Threads (1.2-CMD-03): exportPages may run on a thread other than the GUI thread when it is
// given plain data only — a Workspace nobody changes meanwhile (a copy: the session's
// Workspace is GUI-thread only), ExportSources whose functions touch no GUI-thread object,
// and ExportSources::text. One call paints on one thread; every Qt object it creates
// (documents, images, the writer) lives and dies inside the call.

enum class ExportFormat {
    Pdf, ///< one PDF page per page, vector content; PDF pages behind it as images
    Png, ///< one page as an image
    Svg  ///< one page as vector graphics
};

/// What painting a text box takes from the application object: the application font, and
/// the application palette every paint context starts from.
struct ExportTextStyle {
    QFont font;                                        ///< each box sets its own pixel size
    QAbstractTextDocumentLayout::PaintContext context; ///< black text
};

/// The style text boxes are painted with, as on the canvas. GUI thread only: it reads
/// QGuiApplication::font() and the application palette (QPalette's default constructor
/// copies it without a lock).
[[nodiscard]] ExportTextStyle exportTextStyle();

struct ExportSources {
    /// The file of an asset (images, imported PDFs); nullopt: missing.
    std::function<std::optional<std::filesystem::path>(core::AssetId)> assetPath;
    /// Called before each page with (pages done, pages in all); returning false cancels
    /// (the target file is then left as it was). Optional.
    std::function<bool(std::size_t, std::size_t)> progress;
    /// exportTextStyle() taken on the GUI thread. Required when the export runs on another
    /// thread; unset, it is taken when the first text box is painted (GUI thread only).
    std::optional<ExportTextStyle> text = std::nullopt;
};

/// Pixels per world unit of PNG exports (192 dpi).
inline constexpr double kPngPixelsPerUnit = 2.0;
/// Largest PNG side and pixel count (256 MB of RGBA); larger pages are exported at a lower
/// resolution.
inline constexpr int kMaxPngSide = 16384;
inline constexpr double kMaxPngPixels = 64.0 * 1024.0 * 1024.0;
/// Resolution of imported PDF pages drawn into PDF/SVG exports and prints.
inline constexpr double kDocumentExportDpi = 200.0;

/// The part of the page exported, world units: a bounded page's rectangle; for an infinite
/// page the content's bounds plus a margin (an A4 page at the origin if it is empty).
[[nodiscard]] core::DRect exportArea(const document::Workspace& workspace, core::PageId page);

/// The assets `pages` show — the imported PDFs behind them and the images on them — each
/// once, in page order: what ExportSources::assetPath is asked for when they are exported,
/// so that a caller can look the files up before handing the export to another thread.
[[nodiscard]] std::vector<core::AssetId> exportAssets(const document::Workspace& workspace,
                                                      std::span<const core::PageId> pages);

/// Writes `pages` to `target`: a PDF of all of them, or (PNG, SVG) exactly one page. Each
/// imported PDF is opened once per call. Errors: InvalidArgument (no page, a missing page,
/// several pages for PNG/SVG), IoError, Conflict (cancelled through `sources.progress`).
[[nodiscard]] core::Result<void> exportPages(const document::Workspace& workspace,
                                             std::span<const core::PageId> pages,
                                             const std::filesystem::path& target,
                                             ExportFormat format, const ExportSources& sources);

/// Prints `pages` on `printer` (already set up), one sheet each, scaled to fit the printable
/// area. Errors: InvalidArgument (no page), IoError (the printer could not start),
/// Conflict (cancelled; the printer job is aborted).
[[nodiscard]] core::Result<void> printPages(QPrinter& printer, const document::Workspace& workspace,
                                            std::span<const core::PageId> pages,
                                            const ExportSources& sources);

} // namespace studyapp::ui
