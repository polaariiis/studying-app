#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::ipc::pdf {

// The messages between StudyBoard and its PDF inspection worker (docs/PDF_WORKER.md §8,
// D53): one request, one reply, fixed little-endian layouts. Pure functions, Qt-free; the
// transport is ipc::FileChannel. An inspection reply carries no free text; a text reply
// (1.2-CMD-01, docs/PDF_WORKER.md §23) carries the PDF's own page text as bounded,
// validated UTF-8 data, never as a message to show.
//
// The PDF limits live here because they are part of the contract: the worker applies them
// when it inspects, and StudyBoard applies them again to every reply (validPageSize).

/// Most pages an imported PDF may have.
inline constexpr std::uint32_t kMaxPages = 5000;
/// Largest page side accepted, in points (PDF's own limit: 14 400 pt = 200 in).
inline constexpr double kMaxPagePoints = 14400.0;
/// Longest staged-file path in a request, in UTF-8 bytes.
inline constexpr std::uint32_t kMaxPathBytes = 32768;

inline constexpr std::array<char, 4> kRequestMagic{'S', 'B', 'P', 'Q'};
inline constexpr std::array<char, 4> kReplyMagic{'S', 'B', 'P', 'R'};
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::array<char, 4> kTextReplyMagic{'S', 'B', 'P', 'T'};
inline constexpr std::uint16_t kKindInspect = 1;
inline constexpr std::uint16_t kKindExtractText = 2;
/// Most UTF-8 bytes of text a text reply carries for one page; longer page text is cut at a
/// character boundary and the reply marked truncated.
inline constexpr std::uint32_t kMaxPageTextBytes = 64U * 1024U;
/// Most UTF-8 bytes of text in one text reply (all pages); pages after the budget is spent
/// carry no text and the reply is marked truncated.
inline constexpr std::uint32_t kMaxTextBytes = 8U * 1024U * 1024U;
/// Both messages start with a 32-byte header.
inline constexpr std::size_t kHeaderSize = 32;
/// Bytes per page in a reply: width and height as IEEE-754 doubles.
inline constexpr std::size_t kPageSize = 16;

/// A page side pair that StudyBoard accepts: finite, positive, at most kMaxPagePoints.
[[nodiscard]] bool validPageSize(double widthPt, double heightPt) noexcept;

/// What a request asks the worker to do.
enum class RequestKind : std::uint16_t {
    Inspect = kKindInspect,         ///< page count and sizes → InspectReply
    ExtractText = kKindExtractText, ///< the text of every page → TextReply (1.2-CMD-01)
};

/// Inspect (or extract the text of) the PDF at `path` (UTF-8, absolute; the worker only
/// reads it): the staged copy of an import, or a stored asset.
struct InspectRequest {
    core::JobId job;
    std::string path;
    RequestKind kind = RequestKind::Inspect;

    [[nodiscard]] friend bool operator==(const InspectRequest&, const InspectRequest&) = default;
};

enum class InspectStatus : std::uint16_t {
    Ok = 0,
    InvalidRequest = 1,     ///< the request was malformed
    UnsupportedRequest = 2, ///< unknown version or kind
    Unreadable = 3,         ///< detail 0: not a readable PDF; 1: the file could not be opened
    Protected = 4,          ///< detail 0: password; 1: unsupported security scheme
    NoPages = 5,
    TooManyPages = 6, ///< detail: the page count (> kMaxPages)
    BadPageSize = 7,  ///< detail: the 1-based number of the first unsupported page
    Internal = 8,     ///< the worker failed otherwise
};

struct PageSizePt {
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] friend bool operator==(const PageSizePt&, const PageSizePt&) = default;
};

/// The worker's reply: `pages` only with InspectStatus::Ok (1..kMaxPages, each validPageSize).
struct InspectReply {
    core::JobId job;
    InspectStatus status = InspectStatus::Internal;
    std::uint32_t detail = 0;
    std::vector<PageSizePt> pages;

    [[nodiscard]] friend bool operator==(const InspectReply&, const InspectReply&) = default;
};

/// The worker's reply to an ExtractText request. With InspectStatus::Ok: one UTF-8 string
/// per page (1..kMaxPages pages; each at most kMaxPageTextBytes, all together at most
/// kMaxTextBytes; no NUL), `detail` 1 if text was cut at those limits, else 0. Other
/// statuses as for InspectReply (BadPageSize is not used), without pages.
struct TextReply {
    core::JobId job;
    InspectStatus status = InspectStatus::Internal;
    std::uint32_t detail = 0;
    std::vector<std::string> pages;

    [[nodiscard]] friend bool operator==(const TextReply&, const TextReply&) = default;
};

[[nodiscard]] std::vector<std::byte> encode(const InspectRequest& request);
[[nodiscard]] std::vector<std::byte> encode(const InspectReply& reply);
[[nodiscard]] std::vector<std::byte> encode(const TextReply& reply);

/// Well-formed UTF-8 (no overlong forms, no surrogates, at most U+10FFFF) without NUL.
[[nodiscard]] bool validUtf8(std::string_view text) noexcept;

/// Errors: ParseError (malformed: size, magic, nil job, path rules), Unsupported (another
/// version or kind — the header was well-formed).
[[nodiscard]] core::Result<InspectRequest> decodeRequest(std::span<const std::byte> message);

/// The JobId of a request whose header is readable (for an error reply to a malformed
/// request); nil otherwise.
[[nodiscard]] core::JobId requestJob(std::span<const std::byte> message) noexcept;

/// Errors: ParseError for anything that does not follow §8.2 exactly — size, magic,
/// version, unknown status, nil job, a detail or page count that does not fit the status,
/// or a page size that is not validPageSize.
[[nodiscard]] core::Result<InspectReply> decodeReply(std::span<const std::byte> message);

/// Errors: ParseError for anything that does not follow the text reply layout exactly
/// (docs/PDF_WORKER.md §23) — size, magic, version, unknown status, nil job, a detail or
/// page count that does not fit the status, a page or total text over its limit, or text
/// that is not validUtf8.
[[nodiscard]] core::Result<TextReply> decodeTextReply(std::span<const std::byte> message);

} // namespace studyapp::ipc::pdf
