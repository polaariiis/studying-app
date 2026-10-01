#include <studyapp/ipc/PdfInspection.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace studyapp::ipc::pdf {

namespace {

using core::ErrorCode;

constexpr std::size_t kMagicAt = 0;
constexpr std::size_t kVersionAt = 4;
constexpr std::size_t kKindAt = 6; ///< the status in a reply
constexpr std::size_t kJobAt = 8;
constexpr std::size_t kWordAt = 24;     ///< request: path length; reply: detail
constexpr std::size_t kLastWordAt = 28; ///< request: reserved; reply: page count

void put16(std::vector<std::byte>& out, std::size_t at, std::uint16_t value) {
    out[at] = static_cast<std::byte>(value & 0xFFU);
    out[at + 1] = static_cast<std::byte>(value >> 8U);
}

void put32(std::vector<std::byte>& out, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        out[at + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

void put64(std::vector<std::byte>& out, std::size_t at, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[at + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

std::uint16_t get16(std::span<const std::byte> in, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(in[at]) |
                                      (std::to_integer<std::uint16_t>(in[at + 1]) << 8U));
}

std::uint32_t get32(std::span<const std::byte> in, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t>(in[at + i]) << (8U * i);
    }
    return value;
}

std::uint64_t get64(std::span<const std::byte> in, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= std::to_integer<std::uint64_t>(in[at + i]) << (8U * i);
    }
    return value;
}

void putHeader(std::vector<std::byte>& out, const std::array<char, 4>& magic, std::uint16_t kind,
               const core::JobId& job) {
    for (std::size_t i = 0; i < magic.size(); ++i) {
        out[kMagicAt + i] = static_cast<std::byte>(magic[i]);
    }
    put16(out, kVersionAt, kVersion);
    put16(out, kKindAt, kind);
    const auto& bytes = job.value().bytes();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[kJobAt + i] = static_cast<std::byte>(bytes[i]);
    }
}

bool hasMagic(std::span<const std::byte> in, const std::array<char, 4>& magic) {
    return std::equal(magic.begin(), magic.end(), in.begin(),
                      [](char m, std::byte b) { return static_cast<std::byte>(m) == b; });
}

core::JobId jobAt(std::span<const std::byte> in) {
    core::Uuid::Bytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = std::to_integer<std::uint8_t>(in[kJobAt + i]);
    }
    return core::JobId{core::Uuid(bytes)};
}

core::Error malformed(const std::string& what) {
    return core::Error{ErrorCode::ParseError, "PDF worker message: " + what};
}

/// Well-formed UTF-8 (no overlong forms, no surrogates, at most U+10FFFF) without NUL.
bool validUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead == 0) {
            return false;
        }
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code = lead & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        const std::uint32_t smallest = length == 2 ? 0x80U : length == 3 ? 0x800U : 0x10000U;
        if (code < smallest || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
            return false;
        }
        i += length;
    }
    return true;
}

bool knownStatus(std::uint16_t value) {
    return value <= static_cast<std::uint16_t>(InspectStatus::Internal);
}

/// The detail values each status allows (§8.2).
bool validDetail(InspectStatus status, std::uint32_t detail) {
    switch (status) {
    case InspectStatus::Unreadable:
    case InspectStatus::Protected:
        return detail <= 1;
    case InspectStatus::TooManyPages:
        return detail > kMaxPages;
    case InspectStatus::BadPageSize:
        return detail >= 1 && detail <= kMaxPages;
    case InspectStatus::Ok:
    case InspectStatus::InvalidRequest:
    case InspectStatus::UnsupportedRequest:
    case InspectStatus::NoPages:
    case InspectStatus::Internal:
        return detail == 0;
    }
    return false;
}

} // namespace

bool validPageSize(double widthPt, double heightPt) noexcept {
    return std::isfinite(widthPt) && std::isfinite(heightPt) && widthPt > 0.0 && heightPt > 0.0 &&
           widthPt <= kMaxPagePoints && heightPt <= kMaxPagePoints;
}

std::vector<std::byte> encode(const InspectRequest& request) {
    std::vector<std::byte> out(kHeaderSize + request.path.size());
    putHeader(out, kRequestMagic, kKindInspect, request.job);
    put32(out, kWordAt, static_cast<std::uint32_t>(request.path.size()));
    put32(out, kLastWordAt, 0);
    std::memcpy(out.data() + kHeaderSize, request.path.data(), request.path.size());
    return out;
}

std::vector<std::byte> encode(const InspectReply& reply) {
    std::vector<std::byte> out(kHeaderSize + kPageSize * reply.pages.size());
    putHeader(out, kReplyMagic, static_cast<std::uint16_t>(reply.status), reply.job);
    put32(out, kWordAt, reply.detail);
    put32(out, kLastWordAt, static_cast<std::uint32_t>(reply.pages.size()));
    std::size_t at = kHeaderSize;
    for (const PageSizePt& page : reply.pages) {
        put64(out, at, std::bit_cast<std::uint64_t>(page.width));
        put64(out, at + 8, std::bit_cast<std::uint64_t>(page.height));
        at += kPageSize;
    }
    return out;
}

core::JobId requestJob(std::span<const std::byte> message) noexcept {
    if (message.size() < kHeaderSize || !hasMagic(message, kRequestMagic)) {
        return {};
    }
    return jobAt(message);
}

core::Result<InspectRequest> decodeRequest(std::span<const std::byte> message) {
    if (message.size() < kHeaderSize) {
        return tl::unexpected(malformed("request shorter than its header"));
    }
    if (!hasMagic(message, kRequestMagic)) {
        return tl::unexpected(malformed("not a request (magic)"));
    }
    if (get16(message, kVersionAt) != kVersion || get16(message, kKindAt) != kKindInspect) {
        return core::makeError(ErrorCode::Unsupported,
                               "PDF worker message: unsupported request version or kind");
    }
    InspectRequest request{.job = jobAt(message), .path = {}};
    if (request.job.isNull()) {
        return tl::unexpected(malformed("request without a job id"));
    }
    const std::uint32_t length = get32(message, kWordAt);
    if (length == 0 || length > kMaxPathBytes || get32(message, kLastWordAt) != 0 ||
        message.size() != kHeaderSize + length) {
        return tl::unexpected(malformed("request path length or size"));
    }
    request.path.assign(reinterpret_cast<const char*>(message.data() + kHeaderSize), length);
    if (!validUtf8(request.path)) {
        return tl::unexpected(malformed("request path is not UTF-8"));
    }
    const std::u8string utf8(request.path.begin(), request.path.end());
    if (!std::filesystem::path(utf8).is_absolute()) {
        return tl::unexpected(malformed("request path is not absolute"));
    }
    return request;
}

core::Result<InspectReply> decodeReply(std::span<const std::byte> message) {
    if (message.size() < kHeaderSize) {
        return tl::unexpected(malformed("reply shorter than its header"));
    }
    if (!hasMagic(message, kReplyMagic)) {
        return tl::unexpected(malformed("not a reply (magic)"));
    }
    if (get16(message, kVersionAt) != kVersion) {
        return tl::unexpected(malformed("unsupported reply version"));
    }
    const std::uint16_t status = get16(message, kKindAt);
    if (!knownStatus(status)) {
        return tl::unexpected(malformed("unknown reply status"));
    }
    InspectReply reply{.job = jobAt(message),
                       .status = static_cast<InspectStatus>(status),
                       .detail = get32(message, kWordAt),
                       .pages = {}};
    if (reply.job.isNull()) {
        return tl::unexpected(malformed("reply without a job id"));
    }
    if (!validDetail(reply.status, reply.detail)) {
        return tl::unexpected(malformed("reply detail does not fit its status"));
    }
    const std::uint32_t count = get32(message, kLastWordAt);
    const bool ok = reply.status == InspectStatus::Ok;
    if ((ok && (count == 0 || count > kMaxPages)) || (!ok && count != 0) ||
        message.size() != kHeaderSize + kPageSize * count) {
        return tl::unexpected(malformed("reply page count or size"));
    }
    reply.pages.reserve(count);
    for (std::size_t i = 0, at = kHeaderSize; i < count; ++i, at += kPageSize) {
        const PageSizePt page{.width = std::bit_cast<double>(get64(message, at)),
                              .height = std::bit_cast<double>(get64(message, at + 8))};
        if (!validPageSize(page.width, page.height)) {
            return tl::unexpected(
                malformed("reply page " + std::to_string(i + 1) + " has no supported size"));
        }
        reply.pages.push_back(page);
    }
    return reply;
}

} // namespace studyapp::ipc::pdf
