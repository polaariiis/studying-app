#pragma once

// The benchmark process's current working set (resident memory), for memory counters in
// the stress benchmarks (docs/STRESS_TESTING.md). 0 where it cannot be read.

#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#elif defined(__linux__)
#include <fstream>
#include <unistd.h>
#endif

namespace studyapp::bench {

inline double workingSetMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0;
    std::uint64_t resident = 0;
    if (statm >> pages >> resident) {
        return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) /
               (1024.0 * 1024.0);
    }
    return 0.0;
#else
    return 0.0;
#endif
}

} // namespace studyapp::bench
