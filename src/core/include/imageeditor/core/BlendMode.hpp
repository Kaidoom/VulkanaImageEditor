#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace imageeditor::core {

// Stable shader-facing numeric IDs. Project files use the independent textual
// IDs below; adding a mode must never renumber an existing value.
enum class BlendMode : std::uint32_t {
    Normal = 0, Multiply, Screen, Overlay, SoftLight, HardLight,
    Darken, Lighten, Difference, Exclusion, Hue, Saturation, Color, Luminosity,
    ColorDodge = 14, LinearDodge, ColorBurn, LinearBurn, Subtract, Divide,
    Dissolve = 20, DarkerColor, LighterColor, VividLight, LinearLight, PinLight, HardMix,
};

inline constexpr std::array allBlendModes {
    BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay,
    BlendMode::SoftLight, BlendMode::HardLight, BlendMode::Darken, BlendMode::Lighten,
    BlendMode::Difference, BlendMode::Exclusion, BlendMode::Hue, BlendMode::Saturation,
    BlendMode::Color, BlendMode::Luminosity,
    BlendMode::ColorDodge, BlendMode::LinearDodge, BlendMode::ColorBurn,
    BlendMode::LinearBurn, BlendMode::Subtract, BlendMode::Divide,
    BlendMode::Dissolve, BlendMode::DarkerColor, BlendMode::LighterColor,
    BlendMode::VividLight, BlendMode::LinearLight, BlendMode::PinLight, BlendMode::HardMix,
};

[[nodiscard]] constexpr bool isValidBlendMode(BlendMode mode) noexcept
{ return static_cast<std::uint32_t>(mode) < allBlendModes.size(); }

[[nodiscard]] constexpr std::string_view blendModeId(BlendMode mode) noexcept
{
    constexpr std::array<std::string_view, 27> ids {
        "normal", "multiply", "screen", "overlay", "soft-light", "hard-light",
        "darken", "lighten", "difference", "exclusion", "hue", "saturation", "color", "luminosity",
        "color-dodge", "linear-dodge", "color-burn", "linear-burn", "subtract", "divide",
        "dissolve", "darker-color", "lighter-color", "vivid-light", "linear-light", "pin-light", "hard-mix",
    };
    return isValidBlendMode(mode) ? ids[static_cast<std::uint32_t>(mode)] : std::string_view {};
}

[[nodiscard]] constexpr std::string_view blendModeName(BlendMode mode) noexcept
{
    constexpr std::array<std::string_view, 27> names {
        "Normal", "Multiply", "Screen", "Overlay", "Soft Light", "Hard Light",
        "Darken", "Lighten", "Difference", "Exclusion", "Hue", "Saturation", "Color", "Luminosity",
        "Color Dodge", "Linear Dodge (Add)", "Color Burn", "Linear Burn", "Subtract", "Divide",
        "Dissolve", "Darker Color", "Lighter Color", "Vivid Light", "Linear Light", "Pin Light", "Hard Mix",
    };
    return isValidBlendMode(mode) ? names[static_cast<std::uint32_t>(mode)] : std::string_view {};
}

enum class BlendOperation { Rgb, StochasticCoverage };
[[nodiscard]] constexpr BlendOperation blendOperation(BlendMode mode) noexcept
{ return mode == BlendMode::Dissolve ? BlendOperation::StochasticCoverage : BlendOperation::Rgb; }
inline constexpr std::uint32_t defaultBlendSeed = 0x6d2b79f5U;

[[nodiscard]] constexpr std::optional<BlendMode> blendModeFromId(std::string_view id) noexcept
{
    for (const auto mode : allBlendModes)
        if (blendModeId(mode) == id) return mode;
    return std::nullopt;
}

} // namespace imageeditor::core
