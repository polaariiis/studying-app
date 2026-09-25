#pragma once

#include <cstdint>

namespace studyapp::platform::os {

/// True if a process with this id currently exists on this machine (including processes
/// of other users that we may not inspect). Pid reuse is not detected; callers treat a
/// running process as a live lock holder, which errs on the safe side.
[[nodiscard]] bool isProcessRunning(std::int64_t processId) noexcept;

} // namespace studyapp::platform::os
