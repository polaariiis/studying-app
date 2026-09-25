#include "../ProcessInfo.hpp"

#include <windows.h>

namespace studyapp::platform::os {

bool isProcessRunning(std::int64_t processId) noexcept {
    if (processId <= 0 || processId > 0xFFFFFFFFLL) {
        return false;
    }
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr) {
        // The process exists but belongs to someone we may not query.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exitCode = 0;
    const bool running = GetExitCodeProcess(process, &exitCode) != 0 && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

} // namespace studyapp::platform::os
