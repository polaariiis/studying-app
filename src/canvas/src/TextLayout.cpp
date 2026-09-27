#include <studyapp/canvas/TextLayout.hpp>

#include <algorithm>

namespace studyapp::canvas {

float fallbackTextHeight(std::string_view text) noexcept {
    const auto lines = static_cast<float>(std::count(text.begin(), text.end(), '\n') + 1);
    return lines * kTextSize * 1.4F + 2.0F * kTextPadding;
}

} // namespace studyapp::canvas
