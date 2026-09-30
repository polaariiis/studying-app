#include "PageExport.hpp"

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/canvas/TextLayout.hpp>
#include <studyapp/ui/DesignTokens.hpp>

#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QPdfWriter>
#include <QPrinter>
#include <QSaveFile>
#include <QSvgGenerator>
#include <QTextDocument>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace studyapp::ui {

namespace {

using core::ErrorCode;
using core::makeError;

/// Margin around the content of an infinite page, world units.
constexpr double kInfiniteMargin = 24.0;
/// Round caps and joins are subdivided as for this zoom (smooth at print resolutions).
constexpr float kExportPixelsPerUnit = 4.0F;
/// Largest side of an image or PDF page drawn into an export, pixels.
constexpr int kMaxEmbeddedSide = 8192;
/// Pattern lines and dots drawn at most per page.
constexpr double kMaxPatternLines = 20000.0;
constexpr double kMaxPatternDots = 250000.0;

QString toQString(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

QColor toQColor(const core::Color& c) {
    return QColor(c.r, c.g, c.b, c.a);
}

QTransform toQTransform(const core::Affine2& m) {
    return QTransform(m.a, m.b, m.c, m.d, m.tx, m.ty);
}

/// The triangles of a mesh as one path. Every triangle is made counter-clockwise, so with
/// the winding fill rule overlapping triangles unite (a translucent stroke covers each
/// point once, as on screen) and nothing cancels out.
QPainterPath meshPath(const render::MeshData& mesh) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const auto& v = mesh.vertices;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t ia = mesh.indices[i];
        const std::uint32_t ib = mesh.indices[i + 1];
        const std::uint32_t ic = mesh.indices[i + 2];
        if (ia >= v.size() || ib >= v.size() || ic >= v.size()) {
            continue;
        }
        QPointF a(v[ia].x, v[ia].y);
        QPointF b(v[ib].x, v[ib].y);
        const QPointF c(v[ic].x, v[ic].y);
        const double cross = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
        if (cross == 0.0 || !std::isfinite(cross)) {
            continue; // degenerate
        }
        if (cross < 0.0) {
            std::swap(a, b);
        }
        path.moveTo(a);
        path.lineTo(b);
        path.lineTo(c);
        path.closeSubpath();
    }
    return path;
}

/// Text boxes are laid out exactly like platform::QtTextLayout lays them out on the canvas
/// (application font at the box's font size in pixels, no margin, wrapped within the box
/// minus padding).
void paintText(QPainter& painter, const document::TextBox& box) {
    if (box.text.empty()) {
        return;
    }
    QTextDocument document;
    QFont font = QGuiApplication::font();
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(box.fontSize))));
    document.setDefaultFont(font);
    document.setDocumentMargin(0.0);
    document.setPlainText(
        QString::fromUtf8(box.text.data(), static_cast<qsizetype>(box.text.size())));
    document.setTextWidth(
        std::max(1.0, static_cast<double>(box.size.x) - 2.0 * canvas::kTextPadding));
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false); // the canvas raster's hints
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setClipRect(QRectF(0, 0, box.size.x, box.size.y), Qt::IntersectClip);
    painter.translate(canvas::kTextPadding, canvas::kTextPadding);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, Qt::black);
    document.documentLayout()->draw(&painter, context);
    painter.restore();
}

/// Documents and images read while painting one export (each file opened once).
class Resources {
public:
    explicit Resources(const ExportSources& sources) : sources_(&sources) {}

    QPdfDocument* pdf(core::AssetId asset) {
        auto [it, inserted] = pdfs_.try_emplace(asset);
        if (inserted) {
            if (const auto path = assetPath(asset)) {
                auto document = std::make_unique<QPdfDocument>();
                if (document->load(toQString(*path)) == QPdfDocument::Error::None) {
                    it->second = std::move(document);
                }
            }
        }
        return it->second.get();
    }

    /// The image of `asset`, at most `maxSide` pixels per side (null if unreadable).
    QImage image(core::AssetId asset, int maxSide) {
        const auto path = assetPath(asset);
        if (!path) {
            return {};
        }
        QImageReader reader(toQString(*path));
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid() && std::max(size.width(), size.height()) > maxSide) {
            reader.setScaledSize(size.scaled(maxSide, maxSide, Qt::KeepAspectRatio));
        }
        return reader.read();
    }

private:
    std::optional<std::filesystem::path> assetPath(core::AssetId asset) const {
        return sources_->assetPath ? sources_->assetPath(asset) : std::nullopt;
    }

    const ExportSources* sources_;
    std::map<core::AssetId, std::unique_ptr<QPdfDocument>> pdfs_;
};

void paintPattern(QPainter& painter, const document::PageBackground& background,
                  const core::DRect& area) {
    const double spacing = static_cast<double>(background.spacing);
    if (background.pattern == document::BackgroundPattern::None || !(spacing > 0.0)) {
        return;
    }
    // Like the canvas: lines one device pixel wide, dots of 1.25 units, anchored at the
    // world origin.
    const QColor color = toQColor(lightColorTokens().canvasGrid);
    const auto first = [&](double from) {
        return std::ceil(from / spacing) * spacing;
    };
    // Bounded work on huge infinite pages (the canvas fades such dense patterns out too).
    const double columns = area.width() / spacing;
    const double rows = area.height() / spacing;
    if (columns + rows > kMaxPatternLines ||
        (background.pattern == document::BackgroundPattern::Dots &&
         columns * rows > kMaxPatternDots)) {
        return;
    }
    painter.save();
    if (background.pattern == document::BackgroundPattern::Dots) {
        // One line per row, dashed into round dots (a dash of almost no length with round
        // caps): a row is one path in PDF and SVG instead of one ellipse per dot.
        constexpr double kDotWidth = 2.5; // world units: dots of radius 1.25
        constexpr double kDash = 1e-3;    // in pen widths
        QPen pen(color);
        pen.setWidthF(kDotWidth);
        pen.setCapStyle(Qt::RoundCap);
        pen.setDashPattern({kDash, spacing / kDotWidth - kDash});
        painter.setPen(pen);
        const double x0 = first(area.min.x);
        for (double y = first(area.min.y); y <= area.max.y; y += spacing) {
            painter.drawLine(QPointF(x0, y), QPointF(area.max.x + kDotWidth, y));
        }
    } else {
        QPen pen(color);
        pen.setCosmetic(true);
        pen.setWidthF(1.0);
        painter.setPen(pen);
        for (double y = first(area.min.y); y <= area.max.y; y += spacing) {
            painter.drawLine(QPointF(area.min.x, y), QPointF(area.max.x, y));
        }
        if (background.pattern == document::BackgroundPattern::Grid) {
            for (double x = first(area.min.x); x <= area.max.x; x += spacing) {
                painter.drawLine(QPointF(x, area.min.y), QPointF(x, area.max.y));
            }
        }
    }
    painter.restore();
}

void paintDocumentPage(QPainter& painter, const document::PageInfo& page, Resources& resources,
                       double pixelsPerUnit) {
    QPdfDocument* pdf = resources.pdf(page.document->asset);
    const int index = page.document->index;
    if (pdf == nullptr || index < 0 || index >= pdf->pageCount()) {
        return; // unreadable: the paper shows, as on the canvas
    }
    const double scale =
        std::min(pixelsPerUnit, kMaxEmbeddedSide / std::max(page.size.x, page.size.y));
    const QSize pixels(std::max(1, static_cast<int>(std::ceil(page.size.x * scale))),
                       std::max(1, static_cast<int>(std::ceil(page.size.y * scale))));
    const QImage image = pdf->render(index, pixels);
    if (!image.isNull()) {
        const QRectF rect(0, 0, page.size.x, page.size.y);
        painter.fillRect(rect, Qt::white); // the PDF's paper, as on the canvas
        painter.drawImage(rect, image);
    }
}

/// Ink as stroked polylines — a few path operators per point instead of the tessellated
/// triangles (PDF/SVG files several times smaller and faster to write and to render).
/// Consecutive points whose width varies by at most kWidthTolerance form one run, drawn with
/// round caps and joins at the run's mean width. Returns false (use the triangles) for
/// translucent ink whose width varies: its runs would overlap at their ends and show darker
/// there. Translucent ink of one width (highlighters) is a single path, so each point is
/// covered once, as on screen.
bool paintStrokeOutline(QPainter& painter, const document::Stroke& stroke) {
    constexpr float kWidthTolerance = 1.05F; // largest / smallest width within a run
    const auto& points = *stroke.points;
    std::vector<QPointF> run;
    run.reserve(points.size());
    bool varies = false;
    float first = -1.0F;
    for (const document::StrokePoint& point : points) {
        const float r = canvas::strokeRadius(stroke, point.pressure);
        if (!(std::isfinite(point.x) && std::isfinite(point.y) && r > 0.0F)) {
            continue;
        }
        if (first < 0.0F) {
            first = r;
        } else if (r != first) {
            varies = true;
        }
    }
    if (first < 0.0F) {
        return true; // nothing drawable (the tessellation would be empty too)
    }
    if (varies && stroke.color.a != 255) {
        return false;
    }
    const QColor color = toQColor(stroke.color);
    const auto drawRun = [&](float smallest, float largest) {
        const double width = static_cast<double>(smallest + largest); // 2 × mean radius
        if (run.size() == 1) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(run.front(), width / 2.0, width / 2.0);
            return;
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(run.data(), static_cast<int>(run.size()));
    };
    float smallest = 0.0F;
    float largest = 0.0F;
    for (const document::StrokePoint& point : points) {
        const float r = canvas::strokeRadius(stroke, point.pressure);
        if (!(std::isfinite(point.x) && std::isfinite(point.y) && r > 0.0F)) {
            continue;
        }
        const QPointF at(point.x, point.y);
        if (!run.empty() && at == run.back()) {
            continue; // consecutive duplicates add nothing
        }
        if (run.empty()) {
            run.push_back(at);
            smallest = largest = r;
            continue;
        }
        const float low = std::min(smallest, r);
        const float high = std::max(largest, r);
        if (high > low * kWidthTolerance) {
            drawRun(smallest, largest);
            const QPointF joint = run.back(); // the next run continues from here
            run.clear();
            run.push_back(joint);
            smallest = largest = r;
        } else {
            smallest = low;
            largest = high;
        }
        run.push_back(at);
    }
    drawRun(smallest, largest);
    return true;
}

void paintElement(QPainter& painter, const document::Element& element, Resources& resources,
                  double pixelsPerUnit) {
    painter.save();
    painter.setTransform(toQTransform(canvas::meshToWorld(element)), true);
    if (const auto* box = std::get_if<document::TextBox>(&element.payload)) {
        paintText(painter, *box);
        painter.restore();
        return;
    }
    if (const auto* image = std::get_if<document::Image>(&element.payload)) {
        const double longest = std::max(image->size.x, image->size.y);
        const int side =
            std::clamp(static_cast<int>(std::ceil(longest * pixelsPerUnit)), 1, kMaxEmbeddedSide);
        const QImage pixels = resources.image(image->asset, side);
        if (!pixels.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(QRectF(0, 0, image->size.x, image->size.y), pixels);
            painter.restore();
            return;
        }
        // Missing or unreadable: the neutral frame, as on the canvas.
    }
    if (const auto* stroke = std::get_if<document::Stroke>(&element.payload);
        stroke != nullptr && paintStrokeOutline(painter, *stroke)) {
        painter.restore();
        return;
    }
    painter.setPen(Qt::NoPen);
    for (const canvas::MeshPart& part : canvas::buildElementMeshes(element, kExportPixelsPerUnit)) {
        painter.setBrush(toQColor(part.color));
        painter.drawPath(meshPath(part.mesh));
    }
    painter.restore();
}

core::Result<const document::PageInfo*> findPage(const document::Workspace& workspace,
                                                 core::PageId page) {
    const document::PageInfo* info = workspace.findPage(page);
    if (info == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the page does not exist");
    }
    return info;
}

core::Result<void> checkPages(const document::Workspace& workspace,
                              std::span<const core::PageId> pages) {
    if (pages.empty()) {
        return makeError(ErrorCode::InvalidArgument, "there is no page to export");
    }
    for (const core::PageId page : pages) {
        if (auto found = findPage(workspace, page); !found) {
            return tl::unexpected(found.error());
        }
    }
    return {};
}

/// Maps `area` (world) onto `target` (device), keeping the aspect ratio, centred.
void fit(QPainter& painter, const core::DRect& area, const QRectF& target) {
    const double scale = std::min(target.width() / area.width(), target.height() / area.height());
    painter.translate(target.center());
    painter.scale(scale, scale);
    painter.translate(-(area.min.x + area.max.x) / 2.0, -(area.min.y + area.max.y) / 2.0);
}

QPageSize pageSizeOf(const core::DRect& area) {
    return QPageSize(QSizeF(area.width() * 72.0 / 96.0, area.height() * 72.0 / 96.0),
                     QPageSize::Point, QString(), QPageSize::ExactMatch);
}

core::Error cancelled() {
    return {ErrorCode::Conflict, "cancelled"};
}

bool proceed(const ExportSources& sources, std::size_t done, std::size_t total) {
    return !sources.progress || sources.progress(done, total);
}

/// Paints `page` with world units mapped by the painter's transform; `area` (exportArea) is
/// filled with the paper. `documentPixelsPerUnit`: resolution of a PDF page behind it.
void paintPage(QPainter& painter, const document::Workspace& workspace, core::PageId page,
               const core::DRect& area, Resources& resources, double documentPixelsPerUnit);

core::Result<void> commit(QSaveFile& file) {
    if (!file.commit()) {
        return makeError(ErrorCode::IoError,
                         "the file could not be written: " + file.errorString().toStdString());
    }
    return {};
}

} // namespace

core::DRect exportArea(const document::Workspace& workspace, core::PageId page) {
    const document::PageInfo* info = workspace.findPage(page);
    if (info == nullptr) {
        return core::DRect::fromOriginSize({0.0, 0.0}, document::kA4PortraitSize);
    }
    if (info->extent == document::PageExtent::Bounded) {
        return core::DRect::fromOriginSize({0.0, 0.0}, info->size);
    }
    core::DRect bounds = core::DRect::emptyBounds();
    for (const core::LayerId layer : workspace.layersOf(page)) {
        if (!workspace.findLayer(layer)->visible) {
            continue;
        }
        for (const core::ElementId id : workspace.elementsOf(layer)) {
            bounds = bounds.united(canvas::visualBounds(*workspace.findElement(id)));
        }
    }
    if (bounds.isEmpty() || !std::isfinite(bounds.width()) || !std::isfinite(bounds.height())) {
        return core::DRect::fromOriginSize({0.0, 0.0}, document::kA4PortraitSize);
    }
    return bounds.expanded(kInfiniteMargin);
}

namespace {

void paintPage(QPainter& painter, const document::Workspace& workspace, core::PageId page,
               const core::DRect& area, Resources& resources, double documentPixelsPerUnit) {
    const document::PageInfo* info = workspace.findPage(page);
    if (info == nullptr) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QRectF areaRect(area.min.x, area.min.y, area.width(), area.height());
    painter.setClipRect(areaRect, Qt::IntersectClip);
    painter.fillRect(areaRect, toQColor(info->background.color));
    paintPattern(painter, info->background, area);
    if (info->document) {
        paintDocumentPage(painter, *info, resources, documentPixelsPerUnit);
    }
    for (const core::LayerId layerId : workspace.layersOf(page)) {
        const document::Layer* layer = workspace.findLayer(layerId);
        if (!layer->visible || !(layer->opacity > 0.0F)) {
            continue;
        }
        painter.setOpacity(static_cast<double>(layer->opacity));
        for (const core::ElementId id : workspace.elementsOf(layerId)) {
            paintElement(painter, *workspace.findElement(id), resources, documentPixelsPerUnit);
        }
    }
    painter.restore();
}

} // namespace

core::Result<void> exportPages(const document::Workspace& workspace,
                               std::span<const core::PageId> pages,
                               const std::filesystem::path& target, ExportFormat format,
                               const ExportSources& sources) {
    if (auto checked = checkPages(workspace, pages); !checked) {
        return checked;
    }
    if (format != ExportFormat::Pdf && pages.size() != 1) {
        return makeError(ErrorCode::InvalidArgument, "PNG and SVG exports hold one page");
    }
    Resources resources(sources);
    QSaveFile file(toQString(target));
    if (!file.open(QIODevice::WriteOnly)) {
        return makeError(ErrorCode::IoError,
                         "the file could not be created: " + file.errorString().toStdString());
    }
    const double documentPixelsPerUnit = kDocumentExportDpi / 96.0;
    switch (format) {
    case ExportFormat::Pdf: {
        QPdfWriter writer(&file);
        writer.setCreator(QStringLiteral("StudyBoard"));
        writer.setResolution(96); // one device unit per world unit
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        const core::DRect firstArea = exportArea(workspace, pages.front());
        writer.setPageSize(pageSizeOf(firstArea));
        QPainter painter;
        if (!painter.begin(&writer)) {
            file.cancelWriting();
            return makeError(ErrorCode::IoError, "the PDF could not be started");
        }
        for (std::size_t i = 0; i < pages.size(); ++i) {
            if (!proceed(sources, i, pages.size())) {
                painter.end();
                file.cancelWriting();
                return tl::unexpected(cancelled());
            }
            const core::DRect area = i == 0 ? firstArea : exportArea(workspace, pages[i]);
            if (i > 0) {
                writer.setPageSize(pageSizeOf(area));
                writer.newPage();
            }
            painter.save();
            fit(painter, area, QRectF(0, 0, area.width(), area.height()));
            paintPage(painter, workspace, pages[i], area, resources, documentPixelsPerUnit);
            painter.restore();
        }
        painter.end();
        return commit(file);
    }
    case ExportFormat::Png: {
        const core::DRect area = exportArea(workspace, pages.front());
        const double scale =
            std::min({kPngPixelsPerUnit, kMaxPngSide / std::max(area.width(), area.height()),
                      std::sqrt(kMaxPngPixels / (area.width() * area.height()))});
        QImage image(std::max(1, static_cast<int>(std::ceil(area.width() * scale))),
                     std::max(1, static_cast<int>(std::ceil(area.height() * scale))),
                     QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) {
            file.cancelWriting();
            return makeError(ErrorCode::IoError, "not enough memory for the image");
        }
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.scale(scale, scale);
            painter.translate(-area.min.x, -area.min.y);
            paintPage(painter, workspace, pages.front(), area, resources, scale);
        }
        if (!image.save(&file, "PNG")) {
            file.cancelWriting();
            return makeError(ErrorCode::IoError, "the image could not be encoded");
        }
        return commit(file);
    }
    case ExportFormat::Svg: {
        const core::DRect area = exportArea(workspace, pages.front());
        QSvgGenerator generator;
        generator.setOutputDevice(&file);
        generator.setResolution(96);
        generator.setSize(QSize(static_cast<int>(std::ceil(area.width())),
                                static_cast<int>(std::ceil(area.height()))));
        generator.setViewBox(QRectF(0, 0, area.width(), area.height()));
        generator.setTitle(QString::fromStdString(workspace.findPage(pages.front())->title));
        QPainter painter;
        if (!painter.begin(&generator)) {
            file.cancelWriting();
            return makeError(ErrorCode::IoError, "the SVG could not be started");
        }
        painter.translate(-area.min.x, -area.min.y);
        paintPage(painter, workspace, pages.front(), area, resources, documentPixelsPerUnit);
        painter.end();
        return commit(file);
    }
    }
    file.cancelWriting();
    return makeError(ErrorCode::InvalidArgument, "unknown export format");
}

core::Result<void> printPages(QPrinter& printer, const document::Workspace& workspace,
                              std::span<const core::PageId> pages, const ExportSources& sources) {
    if (auto checked = checkPages(workspace, pages); !checked) {
        return checked;
    }
    Resources resources(sources);
    QPainter painter;
    if (!painter.begin(&printer)) {
        return makeError(ErrorCode::IoError, "the printer could not be started");
    }
    const double documentPixelsPerUnit = kDocumentExportDpi / 96.0;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (!proceed(sources, i, pages.size())) {
            printer.abort();
            painter.end();
            return tl::unexpected(cancelled());
        }
        if (i > 0 && !printer.newPage()) {
            painter.end();
            return makeError(ErrorCode::IoError, "the printer could not start a new sheet");
        }
        const core::DRect area = exportArea(workspace, pages[i]);
        painter.save();
        // The painter's origin is the printable area's top-left corner.
        const QRect printable = printer.pageLayout().paintRectPixels(printer.resolution());
        fit(painter, area, QRectF(0, 0, printable.width(), printable.height()));
        paintPage(painter, workspace, pages[i], area, resources, documentPixelsPerUnit);
        painter.restore();
    }
    painter.end();
    return {};
}

} // namespace studyapp::ui
