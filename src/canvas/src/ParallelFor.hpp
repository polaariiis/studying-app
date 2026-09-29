#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace studyapp::canvas::detail {

/// Runs `body(begin, end)` over [0, count) split into contiguous chunks on up to
/// kMaxThreads threads (the calling thread takes the first chunk), and returns when all
/// chunks are done. Below 2 × `minPerThread` items everything runs on the calling thread.
///
/// For pure, independent per-item work only (docs/ARCHITECTURE.md D47): `body` must not
/// touch shared mutable state except distinct, pre-sized output slots, so the result is
/// the same whatever the number of threads. The threads live only for this call; the
/// first exception thrown by a chunk is rethrown on the calling thread.
template <class Body>
void parallelFor(std::size_t count, std::size_t minPerThread, Body&& body) {
    constexpr unsigned kMaxThreads = 8;
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t byWork = minPerThread > 0 ? count / minPerThread : count;
    const auto threads = static_cast<std::size_t>(
        std::min<std::size_t>({static_cast<std::size_t>(std::min(hardware, kMaxThreads)), byWork}));
    if (threads < 2) {
        body(std::size_t{0}, count);
        return;
    }
    std::exception_ptr failure;
    std::mutex failureMutex;
    const auto run = [&](std::size_t begin, std::size_t end) noexcept {
        try {
            body(begin, end);
        } catch (...) {
            const std::lock_guard lock(failureMutex);
            if (!failure) {
                failure = std::current_exception();
            }
        }
    };
    const std::size_t chunk = (count + threads - 1) / threads;
    {
        std::vector<std::jthread> helpers;
        helpers.reserve(threads - 1);
        for (std::size_t t = 1; t < threads; ++t) {
            const std::size_t begin = t * chunk;
            const std::size_t end = std::min(count, begin + chunk);
            if (begin < end) {
                helpers.emplace_back(run, begin, end);
            }
        }
        run(0, std::min(count, chunk));
    } // joins the helpers
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace studyapp::canvas::detail
