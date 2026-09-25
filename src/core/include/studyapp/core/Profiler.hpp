#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace studyapp::core {

/// Lightweight CPU timing for development (docs/RENDERING.md §10): named sections
/// accumulate call count, last, maximum and total duration. A Profiler is an ordinary
/// object owned by whoever reports the numbers (e.g. the canvas controller for the debug
/// HUD) — there is no global registry. Section names must be string literals (they are
/// stored as views). Not thread-safe.
class Profiler {
public:
    struct Section {
        std::string_view name;
        std::uint64_t count = 0;
        std::chrono::nanoseconds last{};
        std::chrono::nanoseconds max{};
        std::chrono::nanoseconds total{};

        [[nodiscard]] double lastMs() const noexcept {
            return std::chrono::duration<double, std::milli>(last).count();
        }
        [[nodiscard]] double averageMs() const noexcept {
            return count == 0 ? 0.0
                              : std::chrono::duration<double, std::milli>(total).count() /
                                    static_cast<double>(count);
        }
    };

    void record(std::string_view name, std::chrono::nanoseconds duration);
    [[nodiscard]] const std::vector<Section>& sections() const noexcept { return sections_; }
    [[nodiscard]] const Section* find(std::string_view name) const noexcept;
    void reset() noexcept { sections_.clear(); }

private:
    std::vector<Section> sections_; // few entries; linear search keeps order of first use
};

/// Records the lifetime of the scope into a Profiler (nullptr = disabled).
class ProfileScope {
public:
    ProfileScope(Profiler* profiler, std::string_view name) noexcept
        : profiler_(profiler), name_(name),
          start_(profiler != nullptr ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{}) {}
    ~ProfileScope() {
        if (profiler_ != nullptr) {
            profiler_->record(name_, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start_));
        }
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
    ProfileScope(ProfileScope&&) = delete;
    ProfileScope& operator=(ProfileScope&&) = delete;

private:
    Profiler* profiler_;
    std::string_view name_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace studyapp::core

#define STUDYAPP_PROFILE_CONCAT_INNER(a, b) a##b
#define STUDYAPP_PROFILE_CONCAT(a, b) STUDYAPP_PROFILE_CONCAT_INNER(a, b)

/// STUDYAPP_PROFILE_SCOPE(profilerPointer, "name") times the enclosing scope. Compiles to
/// nothing when the build disables profiling (CMake option STUDYAPP_ENABLE_PROFILING).
#if defined(STUDYAPP_ENABLE_PROFILING) && STUDYAPP_ENABLE_PROFILING
#define STUDYAPP_PROFILE_SCOPE(profiler, name)                                                     \
    const ::studyapp::core::ProfileScope STUDYAPP_PROFILE_CONCAT(studyappProfileScope_,            \
                                                                 __LINE__)((profiler), (name))
#else
#define STUDYAPP_PROFILE_SCOPE(profiler, name) static_cast<void>(profiler)
#endif
