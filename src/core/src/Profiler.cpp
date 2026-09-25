#include <studyapp/core/Profiler.hpp>

#include <algorithm>
#include <iterator>

namespace studyapp::core {

void Profiler::record(std::string_view name, std::chrono::nanoseconds duration) {
    auto it = std::find_if(sections_.begin(), sections_.end(),
                           [name](const Section& s) { return s.name == name; });
    if (it == sections_.end()) {
        sections_.push_back(Section{.name = name});
        it = std::prev(sections_.end());
    }
    ++it->count;
    it->last = duration;
    it->max = std::max(it->max, duration);
    it->total += duration;
}

const Profiler::Section* Profiler::find(std::string_view name) const noexcept {
    const auto it = std::find_if(sections_.begin(), sections_.end(),
                                 [name](const Section& s) { return s.name == name; });
    return it == sections_.end() ? nullptr : &*it;
}

} // namespace studyapp::core
