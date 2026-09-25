#include <studyapp/core/Clock.hpp>

namespace studyapp::core {

Timestamp SystemClock::now() const {
    return std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

} // namespace studyapp::core
