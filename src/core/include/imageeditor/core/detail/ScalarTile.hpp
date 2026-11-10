#pragma once

#include "imageeditor/core/Geometry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

namespace imageeditor::core::detail {

// Shared representation for scalar images and selection coverage. Storage has
// no document-coordinate, transparency or selection interpretation.
inline constexpr std::uint32_t scalarTileSize = 128;
using ScalarTilePixels = std::array<std::uint8_t, scalarTileSize * scalarTileSize>;
struct ScalarTile {
    std::uint8_t uniform {0};
    std::shared_ptr<const ScalarTilePixels> pixels;
    RectI bounds; // Tile-local nonzero values, useful for selection coverage.
    [[nodiscard]] std::uint8_t at(std::uint32_t x, std::uint32_t y) const noexcept
    { return pixels ? (*pixels)[y * scalarTileSize + x] : uniform; }
};

inline ScalarTile compressScalarTile(std::shared_ptr<ScalarTilePixels> pixels, Extent2u extent)
{
    ScalarTile tile;
    tile.uniform = (*pixels)[0];
    bool uniform = true;
    for (std::uint32_t y = 0; y < extent.height && uniform; ++y) {
        const auto* row = pixels->data() + std::size_t(y) * scalarTileSize;
        uniform = std::all_of(row, row + extent.width, [&](auto v) { return v == tile.uniform; });
    }
    if (uniform) {
        if (tile.uniform) tile.bounds = {0, 0, int(extent.width), int(extent.height)};
        return tile;
    }
    for (std::uint32_t y = 0; y < extent.height; ++y)
        for (std::uint32_t x = 0; x < extent.width; ++x)
            if ((*pixels)[y * scalarTileSize + x])
                tile.bounds = tile.bounds.united({int(x), int(y), 1, 1});
    tile.pixels = std::move(pixels);
    return tile;
}

} // namespace imageeditor::core::detail
