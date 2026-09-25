#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Uuid.hpp>

#include <cstdint>
#include <random>

namespace studyapp::core {

/// Source of new unique ids. Injected so tests can use deterministic sequences.
class IdGenerator {
public:
    IdGenerator() = default;
    virtual ~IdGenerator() = default;
    IdGenerator(const IdGenerator&) = delete;
    IdGenerator& operator=(const IdGenerator&) = delete;
    IdGenerator(IdGenerator&&) = delete;
    IdGenerator& operator=(IdGenerator&&) = delete;

    [[nodiscard]] virtual Uuid next() = 0;
};

/// Generates RFC 9562 version-7 UUIDs:
///
///   48-bit Unix timestamp (ms) | ver=7 | 12-bit counter | var=10 | 62 random bits
///
/// The 12-bit field is a counter (RFC 9562 §6.2, method 1) that starts at a random value
/// below 2048 for each new millisecond and increments within the same millisecond, so ids
/// from one generator are strictly increasing even if the clock stalls or goes backwards
/// (the generator never moves its timestamp backwards; on counter overflow it advances
/// the timestamp by 1 ms).
///
/// Random bits come from a `std::mt19937_64` seeded from `std::random_device`. The goal is
/// uniqueness, not unguessability; ids are not secrets.
///
/// Not thread-safe: use one generator per thread (Phase 1 is single-threaded).
class UuidV7Generator final : public IdGenerator {
public:
    /// Seeds the random engine from `std::random_device`.
    explicit UuidV7Generator(const Clock& clock);
    /// Deterministic sequence for tests.
    UuidV7Generator(const Clock& clock, std::uint64_t seed);

    [[nodiscard]] Uuid next() override;

private:
    const Clock* clock_;
    std::mt19937_64 random_;
    std::uint64_t lastMillis_ = 0;
    std::uint16_t counter_ = 0;
    bool hasLast_ = false;
};

} // namespace studyapp::core
