#include "imageeditor/render/RasterUploadPlan.hpp"

#include <limits>
#include <stdexcept>

namespace imageeditor::render {

RasterUploadPlan makeRasterUploadPlan(
    std::span<const core::RectI> regions, std::uint64_t offsetAlignment)
{
    if (offsetAlignment == 0) {
        throw std::invalid_argument("Raster upload alignment must be non-zero");
    }

    RasterUploadPlan plan;
    plan.copies.reserve(regions.size());
    for (const auto region : regions) {
        if (region.empty()) {
            throw std::invalid_argument("Raster upload region must be non-empty");
        }
        const auto remainder = plan.stagingBytes % offsetAlignment;
        const auto padding = remainder == 0 ? 0 : offsetAlignment - remainder;
        const auto rowBytes = static_cast<std::uint64_t>(region.width) * 4U;
        const auto byteCount = rowBytes * static_cast<std::uint64_t>(region.height);
        if (plan.stagingBytes > std::numeric_limits<std::uint64_t>::max() - padding
            || plan.stagingBytes + padding
                > std::numeric_limits<std::uint64_t>::max() - byteCount) {
            throw std::overflow_error("Raster upload staging size overflow");
        }
        if (rowBytes > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            throw std::overflow_error("Raster upload row is not addressable");
        }
        plan.stagingBytes += padding;
        plan.copies.push_back({
            .region = region,
            .bufferOffset = plan.stagingBytes,
            .byteCount = byteCount,
            .rowBytes = static_cast<std::size_t>(rowBytes),
        });
        plan.stagingBytes += byteCount;
        plan.pixelBytes += byteCount;
    }
    return plan;
}

} // namespace imageeditor::render
