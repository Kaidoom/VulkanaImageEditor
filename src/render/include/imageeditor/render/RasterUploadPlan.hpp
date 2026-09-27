#pragma once

#include "imageeditor/core/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace imageeditor::render {

struct RasterUploadCopy {
    core::RectI region;
    std::uint64_t bufferOffset {0};
    std::uint64_t byteCount {0};
    std::size_t rowBytes {0};
};

struct RasterUploadPlan {
    std::vector<RasterUploadCopy> copies;
    std::uint64_t stagingBytes {0};
    std::uint64_t pixelBytes {0};
};

// Packs tightly stored RGBA8 rectangles into one aligned staging allocation.
// The returned offsets are deterministic and suitable for VkBufferImageCopy.
[[nodiscard]] RasterUploadPlan makeRasterUploadPlan(
    std::span<const core::RectI> regions, std::uint64_t offsetAlignment);

} // namespace imageeditor::render
