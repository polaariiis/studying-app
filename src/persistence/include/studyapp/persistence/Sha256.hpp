#pragma once

#include <studyapp/core/Error.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace studyapp::persistence {

/// SHA-256 digest (FIPS 180-4). Identifies asset content (docs/DATABASE_SCHEMA.md §3).
using Sha256Digest = std::array<std::uint8_t, 32>;

/// Incremental SHA-256, so files can be hashed while they are copied.
class Sha256 {
public:
    Sha256() noexcept;

    void update(std::span<const std::uint8_t> data) noexcept;
    /// Finishes the hash. The object must not be updated afterwards.
    [[nodiscard]] Sha256Digest finish() noexcept;

    [[nodiscard]] static Sha256Digest of(std::span<const std::uint8_t> data) noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t totalBytes_ = 0;
};

/// Lower-case hexadecimal form (64 characters).
[[nodiscard]] std::string toHex(const Sha256Digest& digest);
/// Parses 64 hexadecimal digits (either case).
[[nodiscard]] core::Result<Sha256Digest> parseSha256(std::string_view hex);

} // namespace studyapp::persistence
