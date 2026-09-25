#include <studyapp/core/Color.hpp>

#include "Hex.hpp"

#include <array>
#include <optional>

namespace studyapp::core {

namespace {

std::optional<std::uint8_t> parseByte(char high, char low) noexcept {
    const auto hi = detail::hexDigitValue(high);
    const auto lo = detail::hexDigitValue(low);
    if (!hi || !lo) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>((*hi << 4U) | *lo);
}

Result<Color> invalidColor(std::string_view text) {
    return makeError(ErrorCode::ParseError, "invalid colour '" + std::string(text) +
                                                "' (expected #RGB, #RRGGBB or #RRGGBBAA)");
}

} // namespace

Result<Color> Color::fromHex(std::string_view text) {
    if (text.empty() || text.front() != '#') {
        return invalidColor(text);
    }
    const std::string_view digits = text.substr(1);

    if (digits.size() == 3) {
        std::array<std::uint8_t, 3> channels{};
        for (std::size_t i = 0; i < channels.size(); ++i) {
            const auto byte = parseByte(digits[i], digits[i]); // #abc == #aabbcc
            if (!byte) {
                return invalidColor(text);
            }
            channels[i] = *byte;
        }
        return Color{channels[0], channels[1], channels[2], 255};
    }

    if (digits.size() == 6 || digits.size() == 8) {
        std::array<std::uint8_t, 4> channels{0, 0, 0, 255};
        for (std::size_t i = 0; i < digits.size() / 2; ++i) {
            const auto byte = parseByte(digits[2 * i], digits[(2 * i) + 1]);
            if (!byte) {
                return invalidColor(text);
            }
            channels[i] = *byte;
        }
        return Color{channels[0], channels[1], channels[2], channels[3]};
    }

    return invalidColor(text);
}

std::string Color::toHex() const {
    std::string out = "#";
    detail::appendHexByte(out, r, detail::HexCase::Upper);
    detail::appendHexByte(out, g, detail::HexCase::Upper);
    detail::appendHexByte(out, b, detail::HexCase::Upper);
    if (!isOpaque()) {
        detail::appendHexByte(out, a, detail::HexCase::Upper);
    }
    return out;
}

} // namespace studyapp::core
