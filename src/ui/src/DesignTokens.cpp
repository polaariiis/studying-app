#include <studyapp/ui/DesignTokens.hpp>

#include <studyapp/canvas/ToolSettings.hpp>

#include <QCoreApplication>

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

std::span<const InkColor> inkPalette() noexcept {
    // Black first: the default ink. Shown on dark paper through the display transform
    // (docs/RENDERING.md §8), so black ink reads light there.
    static constexpr InkColor inks[] = {
        {"black", QT_TRANSLATE_NOOP("Ink", "Black"), core::Color::fromRgba(0x00, 0x00, 0x00)},
        {"graphite", QT_TRANSLATE_NOOP("Ink", "Graphite"), core::Color::fromRgba(0x5C, 0x5C, 0x5C)},
        {"blue", QT_TRANSLATE_NOOP("Ink", "Blue"), core::Color::fromRgba(0x2F, 0x5F, 0xA8)},
        {"red", QT_TRANSLATE_NOOP("Ink", "Red"), core::Color::fromRgba(0xB3, 0x36, 0x2F)},
        {"green", QT_TRANSLATE_NOOP("Ink", "Green"), core::Color::fromRgba(0x2E, 0x7D, 0x4F)},
    };
    return inks;
}

std::span<const PenWidthPreset> penWidthPresets() noexcept {
    static constexpr PenWidthPreset widths[] = {
        {"fine", QT_TRANSLATE_NOOP("Ink", "Fine"), 1.2F},
        {"medium", QT_TRANSLATE_NOOP("Ink", "Medium"), 2.0F},
        {"thick", QT_TRANSLATE_NOOP("Ink", "Thick"), 4.0F},
    };
    return widths;
}

std::span<const InkColor> highlighterPalette() noexcept {
    constexpr std::uint8_t a = canvas::kHighlighterAlpha;
    static constexpr InkColor inks[] = {
        {"yellow", QT_TRANSLATE_NOOP("Ink", "Yellow"), canvas::kDefaultHighlighter.color},
        {"green", QT_TRANSLATE_NOOP("Ink", "Green"), core::Color::fromRgba(0x8C, 0xCF, 0x7E, a)},
        {"blue", QT_TRANSLATE_NOOP("Ink", "Blue"), core::Color::fromRgba(0x86, 0xB6, 0xE8, a)},
        {"pink", QT_TRANSLATE_NOOP("Ink", "Pink"), core::Color::fromRgba(0xEE, 0x9D, 0xB6, a)},
    };
    return inks;
}

std::span<const PenWidthPreset> highlighterWidthPresets() noexcept {
    static constexpr PenWidthPreset widths[] = {
        {"fine", QT_TRANSLATE_NOOP("Ink", "Fine"), 8.0F},
        {"medium", QT_TRANSLATE_NOOP("Ink", "Medium"), canvas::kDefaultHighlighter.width},
        {"thick", QT_TRANSLATE_NOOP("Ink", "Thick"), 22.0F},
    };
    return widths;
}

} // namespace studyapp::ui
