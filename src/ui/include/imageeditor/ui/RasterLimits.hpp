#pragma once

#include <cstdint>

namespace imageeditor::ui {

inline constexpr std::uint32_t kMaximumRasterDimension = 16384;
inline constexpr std::uint64_t kMaximumRasterPixels = 40ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr bool rasterExtentWithinLimits(
    std::uint32_t width, std::uint32_t height) noexcept
{
    return width != 0 && height != 0
        && width <= kMaximumRasterDimension
        && height <= kMaximumRasterDimension
        && static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height)
            <= kMaximumRasterPixels;
}

} // namespace imageeditor::ui
