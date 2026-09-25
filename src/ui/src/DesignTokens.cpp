#include <studyapp/ui/DesignTokens.hpp>

#include <cstdint>

namespace studyapp::ui {

namespace {

constexpr core::Color hex(std::uint32_t rgb) noexcept {
    return core::Color::fromArgb32(0xFF000000U | rgb);
}

} // namespace

const ColorTokens& lightColorTokens() noexcept {
    static constexpr ColorTokens tokens{
        .background = hex(0xF5F5F5),
        .surface = hex(0xFFFFFF),
        .surfaceElevated = hex(0xFFFFFF),
        .canvas = hex(0xFAFAFA),
        .canvasGrid = hex(0xDCDCDC),
        .border = hex(0xE5E5E5),
        .borderStrong = hex(0xD4D4D4),
        .textPrimary = hex(0x171717),
        .textSecondary = hex(0x525252),
        .textMuted = hex(0x737373),
        .textDisabled = hex(0xA3A3A3),
        .hover = hex(0xEEEEEE),
        .selected = hex(0xE5E5E5),
        .selectedText = hex(0x171717),
        .control = hex(0x171717),
        .controlText = hex(0xFFFFFF),
        .error = hex(0xB42318),
        .warning = hex(0xB54708),
        .success = hex(0x067647),
    };
    return tokens;
}

const ColorTokens& darkColorTokens() noexcept {
    static constexpr ColorTokens tokens{
        .background = hex(0x171717),
        .surface = hex(0x1F1F1F),
        .surfaceElevated = hex(0x262626),
        .canvas = hex(0x1C1C1C),
        .canvasGrid = hex(0x333333),
        .border = hex(0x2A2A2A),
        .borderStrong = hex(0x3D3D3D),
        .textPrimary = hex(0xF5F5F5),
        .textSecondary = hex(0xD4D4D4),
        .textMuted = hex(0xA3A3A3),
        .textDisabled = hex(0x737373),
        .hover = hex(0x262626),
        .selected = hex(0x303030),
        .selectedText = hex(0xF5F5F5),
        .control = hex(0xF5F5F5),
        .controlText = hex(0x171717),
        .error = hex(0xF97066),
        .warning = hex(0xFDB022),
        .success = hex(0x47CD89),
    };
    return tokens;
}

const MetricTokens& metricTokens() noexcept {
    static constexpr MetricTokens tokens{};
    return tokens;
}

} // namespace studyapp::ui
