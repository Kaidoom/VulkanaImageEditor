#include "imageeditor/render/DirtyRegionCoalescer.hpp"

#include <map>
#include <stdexcept>
#include <utility>

namespace imageeditor::render {

std::vector<core::RectI> coalesceDirtyRegionsForUpload(
    std::span<const core::RectI> regions, core::Extent2u surfaceExtent,
    std::uint32_t uploadGridSize)
{
    if (uploadGridSize == 0) {
        throw std::invalid_argument("Raster upload grid size must be non-zero");
    }
    const core::RectI surfaceBounds {0, 0,
        static_cast<std::int32_t>(surfaceExtent.width),
        static_cast<std::int32_t>(surfaceExtent.height)};
    using GridKey = std::pair<std::int32_t, std::int32_t>;
    std::map<GridKey, core::RectI> buckets;
    const auto grid = static_cast<std::int32_t>(uploadGridSize);
    for (const auto input : regions) {
        const auto clipped = input.clippedTo(surfaceBounds);
        if (clipped.empty()) {
            continue;
        }
        const auto firstGridX = clipped.x / grid;
        const auto firstGridY = clipped.y / grid;
        const auto lastGridX = (clipped.right() - 1) / grid;
        const auto lastGridY = (clipped.bottom() - 1) / grid;
        for (auto gridY = firstGridY; gridY <= lastGridY; ++gridY) {
            for (auto gridX = firstGridX; gridX <= lastGridX; ++gridX) {
                const core::RectI cell {gridX * grid, gridY * grid, grid, grid};
                const auto fragment = clipped.clippedTo(cell).clippedTo(surfaceBounds);
                if (fragment.empty()) {
                    continue;
                }
                const GridKey key {gridX, gridY};
                const auto found = buckets.find(key);
                if (found == buckets.end()) {
                    buckets.emplace(key, fragment);
                } else {
                    found->second = found->second.united(fragment);
                }
            }
        }
    }

    std::vector<core::RectI> result;
    result.reserve(buckets.size());
    for (const auto& [key, region] : buckets) {
        (void)key;
        result.push_back(region);
    }
    return result;
}

} // namespace imageeditor::render
