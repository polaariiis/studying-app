#pragma once

// Private helpers shared by the Color and Uuid implementations.

#include <cstdint>
#include <optional>
#include <string>

namespace studyapp::core::detail {

enum class HexCase {
    Lower,
    Upper
};

[[nodiscard]] constexpr std::optional<std::uint8_t> hexDigitValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    return std::nullopt;
}

inline void appendHexByte(std::string& out, std::uint8_t byte, HexCase hexCase) {
    const char* digits = hexCase == HexCase::Upper ? "0123456789ABCDEF" : "0123456789abcdef";
    out.push_back(digits[(byte >> 4U) & 0x0FU]);
    out.push_back(digits[byte & 0x0FU]);
}

} // namespace studyapp::core::detail
