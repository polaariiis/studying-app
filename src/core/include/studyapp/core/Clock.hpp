#pragma once

#include <chrono>

namespace studyapp::core {

/// A point in time, UTC, millisecond precision (the precision stored in the database).
using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

/// Source of the current time. Injected wherever time matters so that tests can control it.
class Clock {
public:
    Clock() = default;
    virtual ~Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;

    [[nodiscard]] virtual Timestamp now() const = 0;
};

/// Wall-clock time from `std::chrono::system_clock`.
class SystemClock final : public Clock {
public:
    [[nodiscard]] Timestamp now() const override;
};

} // namespace studyapp::core
