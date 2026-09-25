#pragma once

#include <studyapp/core/Error.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace studyapp::core {

/// 8-bit-per-channel sRGB colour with straight (non-premultiplied) alpha.
struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    [[nodiscard]] static constexpr Color fromRgba(std::uint8_t red, std::uint8_t green,
                                                  std::uint8_t blue,
                                                  std::uint8_t alpha = 255) noexcept {
        return {red, green, blue, alpha};
    }

    /// From packed `0xAARRGGBB` (the representation used in the database).
    [[nodiscard]] static constexpr Color fromArgb32(std::uint32_t argb) noexcept {
        return {static_cast<std::uint8_t>((argb >> 16U) & 0xFFU),
                static_cast<std::uint8_t>((argb >> 8U) & 0xFFU),
                static_cast<std::uint8_t>(argb & 0xFFU),
                static_cast<std::uint8_t>((argb >> 24U) & 0xFFU)};
    }

    /// To packed `0xAARRGGBB`.
    [[nodiscard]] constexpr std::uint32_t toArgb32() const noexcept {
        return (std::uint32_t{a} << 24U) | (std::uint32_t{r} << 16U) | (std::uint32_t{g} << 8U) |
               std::uint32_t{b};
    }

    /// Parses `#RGB`, `#RRGGBB` or `#RRGGBBAA` (case-insensitive, leading `#` required).
    [[nodiscard]] static Result<Color> fromHex(std::string_view text);

    /// `#RRGGBB` for opaque colours, `#RRGGBBAA` otherwise (upper-case hex digits).
    [[nodiscard]] std::string toHex() const;

    [[nodiscard]] constexpr Color withAlpha(std::uint8_t alpha) const noexcept {
        return {r, g, b, alpha};
    }
    [[nodiscard]] constexpr bool isOpaque() const noexcept { return a == 255; }

    [[nodiscard]] static constexpr Color black() noexcept { return {0, 0, 0, 255}; }
    [[nodiscard]] static constexpr Color white() noexcept { return {255, 255, 255, 255}; }
    [[nodiscard]] static constexpr Color transparent() noexcept { return {0, 0, 0, 0}; }

    [[nodiscard]] friend constexpr bool operator==(const Color&, const Color&) noexcept = default;
};

} // namespace studyapp::core
