#include <studyapp/core/Log.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <utility>

namespace studyapp::core {

namespace {

void writeToStderr(LogLevel level, std::string_view category, std::string_view message) {
    const std::string_view levelName = toString(level);
    std::fprintf(stderr, "[%.*s] %.*s: %.*s\n", static_cast<int>(levelName.size()),
                 levelName.data(), static_cast<int>(category.size()), category.data(),
                 static_cast<int>(message.size()), message.data());
}

struct LogState {
    std::mutex mutex;
    std::shared_ptr<const LogSink> sink; // null = default sink
    std::atomic<LogLevel> minimumLevel{LogLevel::Debug};
};

LogState& state() {
    static LogState instance;
    return instance;
}

} // namespace

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "unknown";
}

void setLogSink(LogSink sink) {
    auto shared = sink ? std::make_shared<const LogSink>(std::move(sink)) : nullptr;
    const std::scoped_lock lock(state().mutex);
    state().sink = std::move(shared);
}

void setMinimumLogLevel(LogLevel level) noexcept {
    state().minimumLevel.store(level, std::memory_order_relaxed);
}

LogLevel minimumLogLevel() noexcept {
    return state().minimumLevel.load(std::memory_order_relaxed);
}

void log(LogLevel level, std::string_view category, std::string_view message) {
    if (level < minimumLogLevel()) {
        return;
    }
    std::shared_ptr<const LogSink> sink;
    {
        const std::scoped_lock lock(state().mutex);
        sink = state().sink;
    }
    // Called outside the lock so a sink may itself log or replace the sink.
    if (sink) {
        (*sink)(level, category, message);
    } else {
        writeToStderr(level, category, message);
    }
}

} // namespace studyapp::core
