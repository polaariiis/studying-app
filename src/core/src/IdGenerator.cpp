#include <studyapp/core/IdGenerator.hpp>

#include <algorithm>
#include <array>
#include <functional>

namespace studyapp::core {

namespace {

constexpr std::uint16_t kCounterMax = 0x0FFF;       // 12 bits
constexpr std::uint16_t kCounterSeedLimit = 0x07FF; // leave >= 2048 increments per ms
constexpr std::uint64_t kTimestampMask = (1ULL << 48U) - 1;

std::mt19937_64 seededFromRandomDevice() {
    std::random_device device;
    std::array<std::uint32_t, 8> seedData{};
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges algorithms are incomplete in older libc++
    std::generate(seedData.begin(), seedData.end(), std::ref(device));
    std::seed_seq seq(seedData.begin(), seedData.end());
    return std::mt19937_64(seq);
}

std::uint64_t currentUnixMillis(const Clock& clock) {
    const auto millis = clock.now().time_since_epoch().count();
    return millis > 0 ? static_cast<std::uint64_t>(millis) : 0;
}

} // namespace

UuidV7Generator::UuidV7Generator(const Clock& clock)
    : clock_(&clock), random_(seededFromRandomDevice()) {}

UuidV7Generator::UuidV7Generator(const Clock& clock, std::uint64_t seed)
    : clock_(&clock), random_(seed) {}

Uuid UuidV7Generator::next() {
    std::uint64_t millis = currentUnixMillis(*clock_);

    if (hasLast_ && millis <= lastMillis_) {
        // Same (or earlier) millisecond: keep the timestamp monotonic, bump the counter.
        millis = lastMillis_;
        if (counter_ >= kCounterMax) {
            ++millis;
            counter_ = static_cast<std::uint16_t>(random_() % (kCounterSeedLimit + 1U));
        } else {
            ++counter_;
        }
    } else {
        counter_ = static_cast<std::uint16_t>(random_() % (kCounterSeedLimit + 1U));
    }
    lastMillis_ = millis;
    hasLast_ = true;

    const std::uint64_t timestamp = millis & kTimestampMask;
    const std::uint64_t randomBits = random_();

    Uuid::Bytes bytes{};
    for (std::size_t i = 0; i < 6; ++i) {
        bytes[i] = static_cast<std::uint8_t>((timestamp >> (8U * (5U - i))) & 0xFFU);
    }
    bytes[6] = static_cast<std::uint8_t>(0x70U | ((counter_ >> 8U) & 0x0FU)); // version 7
    bytes[7] = static_cast<std::uint8_t>(counter_ & 0xFFU);
    bytes[8] = static_cast<std::uint8_t>(0x80U | ((randomBits >> 56U) & 0x3FU)); // variant 10
    for (std::size_t i = 9; i < 16; ++i) {
        bytes[i] = static_cast<std::uint8_t>((randomBits >> (8U * (15U - i))) & 0xFFU);
    }
    return Uuid{bytes};
}

} // namespace studyapp::core
