#include <studyapp/document/Patch.hpp>

namespace studyapp::document {

Patch Patch::inverted() const {
    std::vector<AnyChange> reversed;
    reversed.reserve(changes_.size());
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        reversed.push_back(
            std::visit([](const auto& change) { return AnyChange{change.inverted()}; }, *it));
    }
    return Patch(std::move(reversed));
}

} // namespace studyapp::document
