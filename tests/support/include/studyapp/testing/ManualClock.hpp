#pragma once

#include <studyapp/core/Clock.hpp>

#include <chrono>
#include <cstdint>

namespace studyapp::testing {

/// Clock that only moves when told to. Makes timestamps in tests deterministic.
class ManualClock final : public core::Clock {
public:
    explicit ManualClock(std::int64_t unixMillis = kDefaultStart)
        : now_(std::chrono::milliseconds(unixMillis)) {}

    [[nodiscard]] core::Timestamp now() const override { return now_; }

    void set(std::int64_t unixMillis) {
        now_ = core::Timestamp(std::chrono::milliseconds(unixMillis));
    }
    void advance(std::chrono::milliseconds delta) { now_ += delta; }

    static constexpr std::int64_t kDefaultStart = 1'790'000'000'000; // fixed, arbitrary

private:
    core::Timestamp now_;
};

} // namespace studyapp::testing
