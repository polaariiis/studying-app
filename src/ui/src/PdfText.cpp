#include "PdfText.hpp"

#include "SessionDocumentRasterizer.hpp"

#include <QPdfDocument>
#include <QPdfSelection>
#include <QString>

#include <algorithm>
#include <string>

namespace studyapp::ui {

namespace {

using core::ErrorCode;
using core::makeError;

QString toQString(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// Line breaks and tabs stay; other control characters (and NUL) become spaces.
void sanitize(QString& text) {
    for (QChar& c : text) {
        const char16_t u = c.unicode();
        if ((u < 0x20U && u != u'\n' && u != u'\r' && u != u'\t') || u == 0x7FU) {
            c = u' ';
        }
    }
}

/// `text` cut to at most `limit` bytes at a UTF-8 character boundary.
bool cutTo(std::string& text, std::size_t limit) {
    if (text.size() <= limit) {
        return false;
    }
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
        --cut;
    }
    text.resize(cut);
    return true;
}

} // namespace

ipc::pdf::TextReply extractPdfTextLocally(const std::filesystem::path& file) {
    using ipc::pdf::InspectStatus;
    ipc::pdf::TextReply reply;
    const auto fail = [&](InspectStatus status, std::uint32_t detail = 0) {
        reply.status = status;
        reply.detail = detail;
        reply.pages.clear();
        return reply;
    };
    QPdfDocument pdf;
    switch (pdf.load(toQString(file))) {
    case QPdfDocument::Error::None:
        break;
    case QPdfDocument::Error::IncorrectPassword:
        return fail(InspectStatus::Protected, 0);
    case QPdfDocument::Error::UnsupportedSecurityScheme:
        return fail(InspectStatus::Protected, 1);
    case QPdfDocument::Error::FileNotFound:
        return fail(InspectStatus::Unreadable, 1);
    default:
        return fail(InspectStatus::Unreadable, 0);
    }
    const int count = pdf.pageCount();
    if (count <= 0) {
        return fail(InspectStatus::NoPages);
    }
    if (count > kMaxPdfPages) {
        return fail(InspectStatus::TooManyPages, static_cast<std::uint32_t>(count));
    }
    reply.pages.reserve(static_cast<std::size_t>(count));
    std::size_t budget = ipc::pdf::kMaxTextBytes;
    bool truncated = false;
    for (int i = 0; i < count; ++i) {
        if (budget == 0) {
            reply.pages.emplace_back(); // the budget is spent: later pages carry no text
            truncated = true;
            continue;
        }
        QString text = pdf.getAllText(i).text();
        sanitize(text);
        const QByteArray utf8 = text.toUtf8();
        std::string page(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        if (!ipc::pdf::validUtf8(page)) {
            page.clear(); // cannot happen with QString::toUtf8; never send what the reply forbids
        }
        truncated =
            cutTo(page, std::min<std::size_t>(ipc::pdf::kMaxPageTextBytes, budget)) || truncated;
        budget -= page.size();
        reply.pages.push_back(std::move(page));
    }
    reply.status = InspectStatus::Ok;
    reply.detail = truncated ? 1 : 0;
    return reply;
}

core::Result<application::PdfDocumentText> pdfTextFrom(const ipc::pdf::TextReply& reply) {
    using ipc::pdf::InspectStatus;
    switch (reply.status) {
    case InspectStatus::Ok:
        break;
    case InspectStatus::Protected:
        return reply.detail == 0
                   ? makeError(ErrorCode::Unsupported, "password-protected PDFs are not supported")
                   : makeError(ErrorCode::Unsupported,
                               "the PDF's security scheme is not supported");
    case InspectStatus::Unreadable:
        return reply.detail == 1
                   ? makeError(ErrorCode::IoError, "the file could not be opened")
                   : makeError(ErrorCode::IoError, "the file is not a readable PDF document");
    case InspectStatus::NoPages:
        return makeError(ErrorCode::InvalidArgument, "the PDF has no pages");
    case InspectStatus::TooManyPages:
        return makeError(ErrorCode::InvalidArgument,
                         "the PDF has " + std::to_string(reply.detail) + " pages; at most " +
                             std::to_string(kMaxPdfPages) + " are supported");
    default:
        return makeError(ErrorCode::IoError, "the PDF could not be read");
    }
    // Checked again here (the codec already did): the reply is input, not truth.
    if (reply.pages.empty() || reply.pages.size() > ipc::pdf::kMaxPages) {
        return makeError(ErrorCode::IoError, "the PDF could not be read");
    }
    std::size_t total = 0;
    for (const std::string& page : reply.pages) {
        total += page.size();
        if (page.size() > ipc::pdf::kMaxPageTextBytes || !ipc::pdf::validUtf8(page)) {
            return makeError(ErrorCode::IoError, "the PDF could not be read");
        }
    }
    if (total > ipc::pdf::kMaxTextBytes) {
        return makeError(ErrorCode::IoError, "the PDF could not be read");
    }
    return application::PdfDocumentText{.pages = reply.pages, .truncated = reply.detail == 1};
}

} // namespace studyapp::ui
