#include <studyapp/document/Records.hpp>

#include <algorithm>
#include <string>

namespace studyapp::document {

core::Result<void> validateName(std::string_view name, std::string_view what) {
    const bool blank = std::all_of(name.begin(), name.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    });
    if (blank) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               std::string(what) + " must not be empty");
    }
    return {};
}

} // namespace studyapp::document
