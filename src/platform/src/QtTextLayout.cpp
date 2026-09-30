#include <studyapp/platform/QtTextLayout.hpp>

#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QTextDocument>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace studyapp::platform {

namespace {

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// The document text boxes are laid out with (plain text at the box's font size, no margin,
/// wrapped).
void prepare(QTextDocument& document, std::string_view text, float width, float fontSize) {
    QFont font = QGuiApplication::font();
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(fontSize))));
    document.setDefaultFont(font);
    document.setDocumentMargin(0.0);
    document.setPlainText(toQString(text));
    document.setTextWidth(std::max(1.0, static_cast<double>(width) - 2.0 * canvas::kTextPadding));
}

} // namespace

float QtTextLayout::heightFor(std::string_view text, float width, float fontSize) {
    QTextDocument document;
    prepare(document, text, width, fontSize);
    return static_cast<float>(document.size().height()) + 2.0F * canvas::kTextPadding;
}

render::ImageData QtTextLayout::rasterize(std::string_view text, const core::Vec2& size,
                                          float fontSize, float pixelsPerUnit) {
    render::ImageData image;
    if (text.empty() || !(pixelsPerUnit > 0.0F)) {
        return image;
    }
    image.width = std::max(1, static_cast<int>(std::ceil(size.x * pixelsPerUnit)));
    image.height = std::max(1, static_cast<int>(std::ceil(size.y * pixelsPerUnit)));
    QImage raster(image.width, image.height, QImage::Format_RGBA8888_Premultiplied);
    raster.fill(Qt::transparent);
    {
        QTextDocument document;
        prepare(document, text, size.x, fontSize);
        QPainter painter(&raster);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.scale(pixelsPerUnit, pixelsPerUnit);
        painter.translate(canvas::kTextPadding, canvas::kTextPadding);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, Qt::black);
        document.documentLayout()->draw(&painter, context);
    }
    image.pixels.resize(static_cast<std::size_t>(image.width) *
                        static_cast<std::size_t>(image.height) * 4U);
    const auto rowBytes = static_cast<std::size_t>(image.width) * 4U;
    for (int y = 0; y < image.height; ++y) {
        std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * rowBytes,
                    raster.constScanLine(y), rowBytes);
    }
    return image;
}

} // namespace studyapp::platform
