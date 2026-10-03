#include "imageeditor/core/RasterSurface.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace imageeditor::core {
namespace {

std::atomic<SurfaceId> nextSurfaceId {1};

std::size_t checkedByteCount(Extent2u extent)
{
    if (extent.empty()) {
        throw std::invalid_argument("Raster surface extent must be non-zero");
    }
    constexpr auto bytesPerPixel = std::size_t {4};
    const auto width = static_cast<std::size_t>(extent.width);
    const auto height = static_cast<std::size_t>(extent.height);
    if (width > std::numeric_limits<std::size_t>::max() / height / bytesPerPixel) {
        throw std::overflow_error("Raster surface is too large");
    }
    return width * height * bytesPerPixel;
}

} // namespace

SurfaceId makeSurfaceId() noexcept { return nextSurfaceId.fetch_add(1,std::memory_order_relaxed); }

RectI RasterSurface::contentBounds() const
{
    std::lock_guard lock(boundsMutex_);
    const auto e = extent();
    if (boundsRevision_ == revision() && boundsExtent_ == e) return contentBounds_;
    constexpr int side = 64;
    const int columns = int((e.width + side - 1) / side), rows = int((e.height + side - 1) / side);
    const auto dirty = dirtySince(boundsRevision_);
    const bool all = boundsRevision_ == 0 || boundsExtent_ != e || dirty.fullRefresh;
    std::vector<bool> scan(std::size_t(columns) * std::size_t(rows), all);
    if (all) tileContentBounds_.assign(scan.size(), {});
    else for (const auto r : dirty.regions) {
        const auto clipped = r.clippedTo({0, 0, int(e.width), int(e.height)});
        if (clipped.empty()) continue;
        for (int y = clipped.y / side; y <= (clipped.bottom()-1) / side; ++y)
            for (int x = clipped.x / side; x <= (clipped.right()-1) / side; ++x)
                scan[std::size_t(y * columns + x)] = true;
    }
    std::array<std::byte, side * side * 4> bytes;
    RectI result;
    for (int y = 0; y < rows; ++y) for (int x = 0; x < columns; ++x) {
        const auto index = std::size_t(y * columns + x);
        if (scan[index]) {
            const auto tile = RectI{x*side, y*side, side, side}.clippedTo({0,0,int(e.width),int(e.height)});
            copyRgba8(tile, bytes, std::size_t(tile.width) * 4);
            int left = tile.right(), top = tile.bottom(), right = tile.x, bottom = tile.y;
            for (int row = 0; row < tile.height; ++row) for (int col = 0; col < tile.width; ++col)
                if (bytes[std::size_t(row * tile.width + col)*4+3] != std::byte{0}) {
                    left = std::min(left,tile.x+col); right = std::max(right,tile.x+col+1);
                    top = std::min(top,tile.y+row); bottom = std::max(bottom,tile.y+row+1);
                }
            tileContentBounds_[index] = {left,top,std::max(0,right-left),std::max(0,bottom-top)};
        }
        if (!tileContentBounds_[index].empty())
            result = result.empty() ? tileContentBounds_[index] : result.united(tileContentBounds_[index]);
    }
    contentBounds_ = result;
    boundsExtent_ = e;
    boundsRevision_ = revision();
    return result;
}

ContiguousRasterSurface::ContiguousRasterSurface(Extent2u extent, Rgba8 fill)
    : id_(makeSurfaceId())
    , extent_(extent)
    , pixels_(checkedByteCount(extent))
{
    for (std::size_t offset = 0; offset < pixels_.size(); offset += kBytesPerPixel) {
        pixels_[offset] = static_cast<std::byte>(fill.red);
        pixels_[offset + 1] = static_cast<std::byte>(fill.green);
        pixels_[offset + 2] = static_cast<std::byte>(fill.blue);
        pixels_[offset + 3] = static_cast<std::byte>(fill.alpha);
    }
}

ContiguousRasterSurface::ContiguousRasterSurface(Extent2u extent, std::vector<std::byte> rgbaBytes)
    : id_(makeSurfaceId())
    , extent_(extent)
    , pixels_(std::move(rgbaBytes))
{
    if (pixels_.size() != checkedByteCount(extent_)) {
        throw std::invalid_argument("RGBA byte count does not match raster extent");
    }
}

DirtySet ContiguousRasterSurface::dirtySince(Revision uploadedRevision) const
{
    DirtySet result {
        .revision = revision_,
        .fullRefresh = false,
        .regions = {},
    };
    if (uploadedRevision == revision_) {
        return result;
    }
    if (uploadedRevision == 0 || uploadedRevision > revision_ || dirtyJournal_.empty()) {
        result.fullRefresh = true;
        return result;
    }

    const auto oldestReachable = dirtyJournal_.front().revision - 1;
    if (uploadedRevision < oldestReachable) {
        result.fullRefresh = true;
        return result;
    }

    for (const auto& record : dirtyJournal_) {
        if (record.revision > uploadedRevision) {
            result.regions.insert(result.regions.end(),
                record.regions.begin(), record.regions.end());
        }
    }
    if (result.regions.empty()) {
        result.fullRefresh = true;
    }
    return result;
}

void ContiguousRasterSurface::copyRgba8(RectI region, std::span<std::byte> destination,
    std::size_t destinationStride) const
{
    const auto clipped = region.clippedTo(bounds());
    if (clipped.empty()) {
        return;
    }
    const auto rowBytes = static_cast<std::size_t>(clipped.width) * kBytesPerPixel;
    const auto required = destinationStride * static_cast<std::size_t>(clipped.height - 1) + rowBytes;
    if (destinationStride < rowBytes || destination.size() < required) {
        throw std::invalid_argument("Destination span is too small for raster copy");
    }

    const auto sourceStride = static_cast<std::size_t>(extent_.width) * kBytesPerPixel;
    for (std::int32_t row = 0; row < clipped.height; ++row) {
        const auto sourceOffset = static_cast<std::size_t>(clipped.y + row) * sourceStride
            + static_cast<std::size_t>(clipped.x) * kBytesPerPixel;
        const auto destinationOffset = static_cast<std::size_t>(row) * destinationStride;
        std::memcpy(destination.data() + destinationOffset, pixels_.data() + sourceOffset, rowBytes);
    }
}

DirtySet ContiguousRasterSurface::replaceRgba8Batch(
    std::span<const RasterPatch> patches)
{
    struct ValidatedPatch {
        const RasterPatch* patch {nullptr};
        RectI clipped;
        std::size_t xOffset {0};
        std::size_t yOffset {0};
        std::size_t rowBytes {0};
    };
    std::vector<ValidatedPatch> validated;
    validated.reserve(patches.size());
    for (const auto& patch : patches) {
        if (patch.region.empty()) {
            throw std::invalid_argument("Source region is empty");
        }
        const auto originalRowBytes = static_cast<std::size_t>(patch.region.width)
            * kBytesPerPixel;
        if (patch.stride < originalRowBytes) {
            throw std::invalid_argument("Source stride is invalid for raster replacement");
        }
        const auto clipped = patch.region.clippedTo(bounds());
        if (clipped.empty()) {
            continue;
        }
        const auto xOffset = static_cast<std::size_t>(clipped.x - patch.region.x)
            * kBytesPerPixel;
        const auto yOffset = static_cast<std::size_t>(clipped.y - patch.region.y)
            * patch.stride;
        const auto rowBytes = static_cast<std::size_t>(clipped.width) * kBytesPerPixel;
        const auto required = yOffset
            + patch.stride * static_cast<std::size_t>(clipped.height - 1)
            + xOffset + rowBytes;
        if (patch.rgbaBytes.size() < required) {
            throw std::invalid_argument("Source span is too small for raster replacement");
        }
        validated.push_back({&patch, clipped, xOffset, yOffset, rowBytes});
    }

    std::vector<RectI> dirtyRegions;
    dirtyRegions.reserve(validated.size());
    const auto destinationStride = static_cast<std::size_t>(extent_.width) * kBytesPerPixel;
    for (const auto& item : validated) {
        bool changed = false;
        for (std::int32_t row = 0; row < item.clipped.height; ++row) {
            const auto destinationOffset = static_cast<std::size_t>(item.clipped.y + row)
                    * destinationStride
                + static_cast<std::size_t>(item.clipped.x) * kBytesPerPixel;
            const auto sourceOffset = item.yOffset
                + static_cast<std::size_t>(row) * item.patch->stride + item.xOffset;
            auto* destination = pixels_.data() + destinationOffset;
            const auto* source = item.patch->rgbaBytes.data() + sourceOffset;
            if (std::memcmp(destination, source, item.rowBytes) != 0) {
                changed = true;
                std::memcpy(destination, source, item.rowBytes);
            }
        }
        if (changed) {
            dirtyRegions.push_back(item.clipped);
        }
    }
    return recordDirty(std::move(dirtyRegions));
}

DirtySet ContiguousRasterSurface::swapRgba8Batch(
    std::span<MutableRasterPatch> patches)
{
    struct ValidatedPatch {
        MutableRasterPatch* patch {nullptr};
        RectI clipped;
        std::size_t xOffset {0};
        std::size_t yOffset {0};
        std::size_t rowBytes {0};
    };
    std::vector<ValidatedPatch> validated;
    validated.reserve(patches.size());
    for (auto& patch : patches) {
        if (patch.region.empty()) {
            throw std::invalid_argument("Swap region is empty");
        }
        const auto originalRowBytes = static_cast<std::size_t>(patch.region.width)
            * kBytesPerPixel;
        if (patch.stride < originalRowBytes) {
            throw std::invalid_argument("Swap stride is invalid");
        }
        const auto clipped = patch.region.clippedTo(bounds());
        if (clipped.empty()) {
            continue;
        }
        const auto xOffset = static_cast<std::size_t>(clipped.x - patch.region.x)
            * kBytesPerPixel;
        const auto yOffset = static_cast<std::size_t>(clipped.y - patch.region.y)
            * patch.stride;
        const auto rowBytes = static_cast<std::size_t>(clipped.width) * kBytesPerPixel;
        const auto required = yOffset
            + patch.stride * static_cast<std::size_t>(clipped.height - 1)
            + xOffset + rowBytes;
        if (patch.rgbaBytes.size() < required) {
            throw std::invalid_argument("Swap span is too small");
        }
        validated.push_back({&patch, clipped, xOffset, yOffset, rowBytes});
    }

    std::vector<RectI> dirtyRegions;
    dirtyRegions.reserve(validated.size());
    const auto surfaceStride = static_cast<std::size_t>(extent_.width) * kBytesPerPixel;
    for (const auto& item : validated) {
        bool changed = false;
        for (std::int32_t row = 0; row < item.clipped.height; ++row) {
            const auto surfaceOffset = static_cast<std::size_t>(item.clipped.y + row)
                    * surfaceStride
                + static_cast<std::size_t>(item.clipped.x) * kBytesPerPixel;
            const auto patchOffset = item.yOffset
                + static_cast<std::size_t>(row) * item.patch->stride + item.xOffset;
            auto surfaceRow = std::span<std::byte>(
                pixels_.data() + surfaceOffset, item.rowBytes);
            auto patchRow = item.patch->rgbaBytes.subspan(patchOffset, item.rowBytes);
            if (!std::equal(surfaceRow.begin(), surfaceRow.end(), patchRow.begin())) {
                changed = true;
                std::swap_ranges(surfaceRow.begin(), surfaceRow.end(), patchRow.begin());
            }
        }
        if (changed) {
            dirtyRegions.push_back(item.clipped);
        }
    }
    return recordDirty(std::move(dirtyRegions));
}

RectI ContiguousRasterSurface::bounds() const noexcept
{
    return {0, 0, static_cast<std::int32_t>(extent_.width), static_cast<std::int32_t>(extent_.height)};
}

DirtySet ContiguousRasterSurface::recordDirty(std::vector<RectI> regions)
{
    if (regions.empty()) {
        return {.revision = revision_, .fullRefresh = false, .regions = {}};
    }
    ++revision_;
    dirtyJournal_.push_back({revision_, regions});
    if (dirtyJournal_.size() > kDirtyJournalLimit) {
        dirtyJournal_.pop_front();
    }
    return {.revision = revision_, .fullRefresh = false, .regions = std::move(regions)};
}

} // namespace imageeditor::core
