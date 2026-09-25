#pragma once

#include <functional>
#include <string_view>

namespace studyapp::core {

/// Minimal logging facade for Qt-free code.
///
/// Messages go to a single process-wide sink. The default sink writes to stderr; the
/// application installs a Qt-backed sink at start-up (`platform::installQtLogSink()`).
/// This sink is the only sanctioned piece of global mutable state: it is expected to be
/// set once during start-up, and access is synchronised so logging is safe from any thread.

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

using LogSink =
    std::function<void(LogLevel level, std::string_view category, std::string_view message)>;

/// Replaces the sink. An empty function restores the default stderr sink.
void setLogSink(LogSink sink);

/// Messages below this level are discarded before reaching the sink (default: Debug).
void setMinimumLogLevel(LogLevel level) noexcept;
[[nodiscard]] LogLevel minimumLogLevel() noexcept;

void log(LogLevel level, std::string_view category, std::string_view message);

inline void logDebug(std::string_view category, std::string_view message) {
    log(LogLevel::Debug, category, message);
}
inline void logInfo(std::string_view category, std::string_view message) {
    log(LogLevel::Info, category, message);
}
inline void logWarning(std::string_view category, std::string_view message) {
    log(LogLevel::Warning, category, message);
}
inline void logError(std::string_view category, std::string_view message) {
    log(LogLevel::Error, category, message);
}

} // namespace studyapp::core
