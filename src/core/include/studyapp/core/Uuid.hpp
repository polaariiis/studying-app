#pragma once

#include <studyapp/core/Error.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace studyapp::core {

/// 128-bit universally unique identifier (RFC 9562).
///
/// Application-generated ids are UUIDv7 (time-ordered; see `UuidV7Generator`). A UUID
/// gives an entity a globally unique identity; it does not by itself make importing or
/// merging data conflict-free (see docs/ARCHITECTURE.md §11).
class Uuid {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    /// The nil UUID (all zero bits).
    constexpr Uuid() noexcept = default;
    constexpr explicit Uuid(const Bytes& bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] static constexpr Uuid nil() noexcept { return Uuid{}; }

    /// Parses the canonical 36-character form `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`
    /// (hex digits in either case).
    [[nodiscard]] static Result<Uuid> parse(std::string_view text);

    /// Canonical lower-case 36-character form.
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }

    [[nodiscard]] constexpr bool isNil() const noexcept { return bytes_ == Bytes{}; }

    /// Version field (4 bits): 7 for UUIDv7, 4 for random UUIDs, 0 for nil.
    [[nodiscard]] constexpr int version() const noexcept { return bytes_[6] >> 4U; }

    /// True if the variant bits are `10` (RFC 9562 / RFC 4122 layout).
    [[nodiscard]] constexpr bool hasRfcVariant() const noexcept {
        return (bytes_[8] & 0xC0U) == 0x80U;
    }

    /// Unix timestamp in milliseconds for version-7 UUIDs; `nullopt` for other versions.
    [[nodiscard]] std::optional<std::uint64_t> unixMillis() const noexcept;

    [[nodiscard]] friend constexpr bool operator==(const Uuid&, const Uuid&) noexcept = default;

    /// Bytewise (big-endian) ordering, so UUIDv7 values sort by creation time. Written out
    /// instead of defaulted because older libc++ versions lack `operator<=>` for std::array.
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const Uuid& lhs,
                                                                    const Uuid& rhs) noexcept {
        for (std::size_t i = 0; i < lhs.bytes_.size(); ++i) {
            if (lhs.bytes_[i] != rhs.bytes_[i]) {
                return lhs.bytes_[i] < rhs.bytes_[i] ? std::strong_ordering::less
                                                     : std::strong_ordering::greater;
            }
        }
        return std::strong_ordering::equal;
    }

private:
    Bytes bytes_{};
};

} // namespace studyapp::core

template <>
struct std::hash<studyapp::core::Uuid> {
    [[nodiscard]] std::size_t operator()(const studyapp::core::Uuid& uuid) const noexcept {
        // UUIDv7 puts the timestamp in the high bytes and random bits in the low bytes;
        // folding both halves keeps the hash well distributed for any version.
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        const auto& bytes = uuid.bytes();
        for (std::size_t i = 0; i < 8; ++i) {
            high = (high << 8U) | bytes[i];
            low = (low << 8U) | bytes[i + 8];
        }
        return static_cast<std::size_t>(high ^ (low * 0x9E3779B97F4A7C15ULL));
    }
};
