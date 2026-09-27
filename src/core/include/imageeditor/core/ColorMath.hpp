#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace imageeditor::core {

inline double srgbToLinear(std::uint8_t channel) noexcept
{
    // Exact enumeration of all RGBA8 inputs, not an interpolated approximation.
    // Shared by sampling and masked edits; repeated pixel decoding needs no pow.
    static const auto table = [] {
        std::array<double, 256> values {};
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto encoded = static_cast<double>(i) / 255.0;
            values[i] = encoded <= 0.04045 ? encoded / 12.92
                : std::pow((encoded + 0.055) / 1.055, 2.4);
        }
        return values;
    }();
    return table[channel];
}

inline std::uint8_t linearToSrgb(double linear) noexcept
{
    linear = std::clamp(linear, 0.0, 1.0);
    const auto encoded = linear <= 0.0031308 ? linear * 12.92
        : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(std::clamp(std::lround(encoded * 255.0), 0L, 255L));
}

inline std::uint8_t alphaToByte(double alpha) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(
        std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0), 0L, 255L));
}

} // namespace imageeditor::core
