#include <studyapp/core/Uuid.hpp>

#include "Hex.hpp"

namespace studyapp::core {

namespace {

constexpr std::size_t kCanonicalLength = 36;

constexpr bool isHyphenPosition(std::size_t index) noexcept {
    return index == 8 || index == 13 || index == 18 || index == 23;
}

} // namespace

Result<Uuid> Uuid::parse(std::string_view text) {
    if (text.size() != kCanonicalLength) {
        return makeError(ErrorCode::ParseError,
                         "invalid UUID '" + std::string(text) + "': expected 36 characters");
    }

    Bytes bytes{};
    std::size_t byteIndex = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (isHyphenPosition(i)) {
            if (text[i] != '-') {
                return makeError(ErrorCode::ParseError,
                                 "invalid UUID '" + std::string(text) + "': misplaced hyphen");
            }
            ++i;
            continue;
        }
        const auto high = detail::hexDigitValue(text[i]);
        const auto low = detail::hexDigitValue(text[i + 1]);
        if (!high || !low || isHyphenPosition(i + 1)) {
            return makeError(ErrorCode::ParseError,
                             "invalid UUID '" + std::string(text) + "': non-hex character");
        }
        bytes[byteIndex++] = static_cast<std::uint8_t>((*high << 4U) | *low);
        i += 2;
    }
    return Uuid{bytes};
}

std::string Uuid::toString() const {
    std::string out;
    out.reserve(kCanonicalLength);
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        detail::appendHexByte(out, bytes_[i], detail::HexCase::Lower);
    }
    return out;
}

std::optional<std::uint64_t> Uuid::unixMillis() const noexcept {
    if (version() != 7) {
        return std::nullopt;
    }
    std::uint64_t millis = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        millis = (millis << 8U) | bytes_[i];
    }
    return millis;
}

} // namespace studyapp::core
