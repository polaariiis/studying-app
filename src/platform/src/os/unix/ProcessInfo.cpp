#include "../ProcessInfo.hpp"

#include <cerrno>
#include <limits>
#include <signal.h>
#include <sys/types.h>

namespace studyapp::platform::os {

bool isProcessRunning(std::int64_t processId) noexcept {
    if (processId <= 0 || processId > std::numeric_limits<pid_t>::max()) {
        return false;
    }
    // Signal 0 performs the existence and permission checks without sending anything.
    if (kill(static_cast<pid_t>(processId), 0) == 0) {
        return true;
    }
    return errno == EPERM; // exists, owned by another user
}

} // namespace studyapp::platform::os
