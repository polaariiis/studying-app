#include <studyapp/persistence/Sha256.hpp>

#include <bit>

namespace studyapp::persistence {

namespace {

constexpr std::array<std::uint32_t, 64> kRound{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr std::array<std::uint32_t, 8> kInitial{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                                0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

Sha256::Sha256() noexcept : state_(kInitial) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t{block[4 * i]} << 24U) | (std::uint32_t{block[4 * i + 1]} << 16U) |
               (std::uint32_t{block[4 * i + 2]} << 8U) | std::uint32_t{block[4 * i + 3]};
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 =
            std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + choose + kRound[i] + w[i];
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
    totalBytes_ += data.size();
    std::size_t offset = 0;
    if (buffered_ > 0) {
        while (buffered_ < buffer_.size() && offset < data.size()) {
            buffer_[buffered_++] = data[offset++];
        }
        if (buffered_ < buffer_.size()) {
            return;
        }
        compress(buffer_.data());
        buffered_ = 0;
    }
    for (; offset + 64 <= data.size(); offset += 64) {
        compress(data.data() + offset);
    }
    while (offset < data.size()) {
        buffer_[buffered_++] = data[offset++];
    }
}

Sha256Digest Sha256::finish() noexcept {
    const std::uint64_t bitLength = totalBytes_ * 8U;
    buffer_[buffered_++] = 0x80;
    if (buffered_ > 56) {
        while (buffered_ < 64) {
            buffer_[buffered_++] = 0;
        }
        compress(buffer_.data());
        buffered_ = 0;
    }
    while (buffered_ < 56) {
        buffer_[buffered_++] = 0;
    }
    for (int i = 7; i >= 0; --i) {
        buffer_[buffered_++] =
            static_cast<std::uint8_t>((bitLength >> (8U * static_cast<unsigned>(i))) & 0xFFU);
    }
    compress(buffer_.data());
    buffered_ = 0;

    Sha256Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[4 * i] = static_cast<std::uint8_t>(state_[i] >> 24U);
        digest[4 * i + 1] = static_cast<std::uint8_t>((state_[i] >> 16U) & 0xFFU);
        digest[4 * i + 2] = static_cast<std::uint8_t>((state_[i] >> 8U) & 0xFFU);
        digest[4 * i + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFU);
    }
    return digest;
}

Sha256Digest Sha256::of(std::span<const std::uint8_t> data) noexcept {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}

std::string toHex(const Sha256Digest& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const std::uint8_t byte : digest) {
        text.push_back(kDigits[byte >> 4U]);
        text.push_back(kDigits[byte & 0x0FU]);
    }
    return text;
}

core::Result<Sha256Digest> parseSha256(std::string_view hex) {
    if (hex.size() != 64) {
        return core::makeError(core::ErrorCode::ParseError, "SHA-256 must have 64 hex digits");
    }
    Sha256Digest digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = hexValue(hex[2 * i]);
        const int low = hexValue(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return core::makeError(core::ErrorCode::ParseError, "invalid hex digit in SHA-256");
        }
        digest[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return digest;
}

} // namespace studyapp::persistence
