#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace studyapp::testing {

/// A small, valid PDF whose pages show the given lines of text in Helvetica (one of the
/// standard 14 fonts every PDF reader has built in, so no font is needed on the machine
/// running the test). Lines are printed top to bottom; text is ASCII (WinAnsi); parentheses
/// and backslashes are escaped. Deterministic: the same input gives the same bytes. Used to
/// test reading PDF text (1.2-CMD-01) with real PDFs instead of mocks.
inline std::string minimalPdf(const std::vector<std::vector<std::string>>& pages,
                              double widthPt = 612.0, double heightPt = 792.0) {
    std::vector<std::string> objects; // object i + 1
    const auto number = [](double value) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.2f", value);
        return std::string(buffer);
    };
    const std::size_t count = pages.size();
    // 1: catalog, 2: page tree, 3: font, then per page: page, content stream.
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (std::size_t i = 0; i < count; ++i) {
        kids += std::to_string(4 + 2 * i) + " 0 R ";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(count) +
                      " >>");
    objects.push_back(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    for (std::size_t i = 0; i < count; ++i) {
        std::string content = "BT /F1 12 Tf 14 TL 72 " + number(heightPt - 72.0) + " Td\n";
        for (const std::string& line : pages[i]) {
            std::string escaped;
            for (const char c : line) {
                if (c == '(' || c == ')' || c == '\\') {
                    escaped += '\\';
                }
                escaped += c;
            }
            content += "(" + escaped + ") Tj T*\n";
        }
        content += "ET\n";
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + number(widthPt) + " " +
                          number(heightPt) + "] /Resources << /Font << /F1 3 0 R >> >> /Contents " +
                          std::to_string(5 + 2 * i) + " 0 R >>");
        objects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
                          content + "endstream");
    }
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        char entry[24];
        std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", offset);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

} // namespace studyapp::testing
