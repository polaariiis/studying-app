#include <studyapp/canvas/TextLayout.hpp>

#include <algorithm>

namespace studyapp::canvas {

float fallbackTextHeight(std::string_view text, float fontSize) noexcept {
    const auto lines = static_cast<float>(std::count(text.begin(), text.end(), '\n') + 1);
    return lines * fontSize * 1.4F + 2.0F * kTextPadding;
}

} // namespace studyapp::canvas
