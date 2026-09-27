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
};

inline constexpr std::array allBlendModes {
    BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay,
    BlendMode::SoftLight, BlendMode::HardLight, BlendMode::Darken, BlendMode::Lighten,
    BlendMode::Difference, BlendMode::Exclusion, BlendMode::Hue, BlendMode::Saturation,
    BlendMode::Color, BlendMode::Luminosity,
    BlendMode::ColorDodge, BlendMode::LinearDodge, BlendMode::ColorBurn,
    BlendMode::LinearBurn, BlendMode::Subtract, BlendMode::Divide,
};

[[nodiscard]] constexpr bool isValidBlendMode(BlendMode mode) noexcept
{ return static_cast<std::uint32_t>(mode) < allBlendModes.size(); }

[[nodiscard]] constexpr std::string_view blendModeId(BlendMode mode) noexcept
{
    constexpr std::array<std::string_view, 20> ids {
        "normal", "multiply", "screen", "overlay", "soft-light", "hard-light",
        "darken", "lighten", "difference", "exclusion", "hue", "saturation", "color", "luminosity",
        "color-dodge", "linear-dodge", "color-burn", "linear-burn", "subtract", "divide",
    };
    return isValidBlendMode(mode) ? ids[static_cast<std::uint32_t>(mode)] : std::string_view {};
}

[[nodiscard]] constexpr std::string_view blendModeName(BlendMode mode) noexcept
{
    constexpr std::array<std::string_view, 20> names {
        "Normal", "Multiply", "Screen", "Overlay", "Soft Light", "Hard Light",
        "Darken", "Lighten", "Difference", "Exclusion", "Hue", "Saturation", "Color", "Luminosity",
        "Color Dodge", "Linear Dodge (Add)", "Color Burn", "Linear Burn", "Subtract", "Divide",
    };
    return isValidBlendMode(mode) ? names[static_cast<std::uint32_t>(mode)] : std::string_view {};
}

[[nodiscard]] constexpr std::optional<BlendMode> blendModeFromId(std::string_view id) noexcept
{
    for (const auto mode : allBlendModes)
        if (blendModeId(mode) == id) return mode;
    return std::nullopt;
}

} // namespace imageeditor::core
