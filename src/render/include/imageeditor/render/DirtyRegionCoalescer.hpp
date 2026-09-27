#pragma once

#include "imageeditor/core/Geometry.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace imageeditor::render {

// Reduces exact CPU dirty rectangles to a deterministic, non-overlapping set
// suitable for GPU transfer. Regions are split at fixed upload-grid boundaries
// before union, so amplification is capped to one grid cell and never changes
// CPU pixels, transaction snapshots, or history precision.
[[nodiscard]] std::vector<core::RectI> coalesceDirtyRegionsForUpload(
    std::span<const core::RectI> regions, core::Extent2u surfaceExtent,
    std::uint32_t uploadGridSize = 64);

} // namespace imageeditor::render
