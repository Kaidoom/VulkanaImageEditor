#pragma once

#include "imageeditor/core/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace imageeditor::core {

using SurfaceId = std::uint64_t;
using Revision = std::uint64_t;
[[nodiscard]] SurfaceId makeSurfaceId() noexcept;

struct DirtySet {
    Revision revision {0};
    bool fullRefresh {false};
    std::vector<RectI> regions;

    [[nodiscard]] bool empty() const noexcept
    {
        return !fullRefresh && regions.empty();
    }
};

struct RasterPatch {
    RectI region;
    std::span<const std::byte> rgbaBytes;
    std::size_t stride {0};
};

struct MutableRasterPatch {
    RectI region;
    std::span<std::byte> rgbaBytes;
    std::size_t stride {0};
};

// Canonical pixels are unpremultiplied RGBA8 in top-to-bottom row order.
// Pixel (0,0) is top-left; X increases right and Y increases down. GPU caches
// must preserve this storage and coordinate convention rather than rewriting
// the CPU-authoritative surface for a graphics API's clip-space convention.
class RasterSurface {
public:
    virtual ~RasterSurface() = default;

    [[nodiscard]] virtual SurfaceId id() const noexcept = 0;
    [[nodiscard]] virtual Extent2u extent() const noexcept = 0;
    [[nodiscard]] virtual Revision revision() const noexcept = 0;
    [[nodiscard]] virtual DirtySet dirtySince(Revision uploadedRevision) const = 0;

    // Strict alpha > 0, independent of visibility, masks and storage padding.
    // Only dirty tiles are rescanned; unchanged geometry queries read no pixels.
    [[nodiscard]] RectI contentBounds() const;

    virtual void copyRgba8(RectI region, std::span<std::byte> destination,
        std::size_t destinationStride) const = 0;
    // A batch is one logical surface mutation: all changed regions share one
    // revision. This keeps a multi-dab input event incremental without turning
    // every dab or journal tile into a separate GPU-cache revision.
    [[nodiscard]] virtual DirtySet replaceRgba8Batch(
        std::span<const RasterPatch> patches) = 0;
    // Swaps the supplied bytes with the surface. Raster history uses this same
    // operation for undo and redo, so only one retained snapshot is required.
    [[nodiscard]] virtual DirtySet swapRgba8Batch(
        std::span<MutableRasterPatch> patches) = 0;

    DirtySet replaceRgba8(RectI region,
        std::span<const std::byte> source, std::size_t sourceStride)
    {
        const RasterPatch patch {region, source, sourceStride};
        return replaceRgba8Batch(std::span<const RasterPatch>(&patch, 1));
    }

    DirtySet swapRgba8(RectI region,
        std::span<std::byte> pixels, std::size_t stride)
    {
        MutableRasterPatch patch {region, pixels, stride};
        return swapRgba8Batch(std::span<MutableRasterPatch>(&patch, 1));
    }
private:
    mutable std::mutex boundsMutex_;
    mutable Revision boundsRevision_ {0};
    mutable Extent2u boundsExtent_;
    mutable std::vector<RectI> tileContentBounds_;
    mutable RectI contentBounds_;
};

class ContiguousRasterSurface final : public RasterSurface {
public:
    explicit ContiguousRasterSurface(Extent2u extent, Rgba8 fill = {});
    ContiguousRasterSurface(Extent2u extent, std::vector<std::byte> rgbaBytes);
    ContiguousRasterSurface(const ContiguousRasterSurface&) = delete;
    ContiguousRasterSurface& operator=(const ContiguousRasterSurface&) = delete;
    ContiguousRasterSurface(ContiguousRasterSurface&&) = delete;
    ContiguousRasterSurface& operator=(ContiguousRasterSurface&&) = delete;

    [[nodiscard]] SurfaceId id() const noexcept override { return id_; }
    [[nodiscard]] Extent2u extent() const noexcept override { return extent_; }
    [[nodiscard]] Revision revision() const noexcept override { return revision_; }
    [[nodiscard]] DirtySet dirtySince(Revision uploadedRevision) const override;

    void copyRgba8(RectI region, std::span<std::byte> destination,
        std::size_t destinationStride) const override;
    [[nodiscard]] DirtySet replaceRgba8Batch(
        std::span<const RasterPatch> patches) override;
    [[nodiscard]] DirtySet swapRgba8Batch(
        std::span<MutableRasterPatch> patches) override;

private:
    struct DirtyRecord {
        Revision revision;
        std::vector<RectI> regions;
    };

    [[nodiscard]] RectI bounds() const noexcept;
    [[nodiscard]] DirtySet recordDirty(std::vector<RectI> regions);

    static constexpr std::size_t kBytesPerPixel = 4;
    // Bounded by logical edit batches, not individual dabs/tiles. At 180 Hz,
    // this retains more than a second of input even if presentation stalls.
    static constexpr std::size_t kDirtyJournalLimit = 256;

    SurfaceId id_ {0};
    Extent2u extent_;
    Revision revision_ {1};
    std::vector<std::byte> pixels_;
    std::deque<DirtyRecord> dirtyJournal_;
};

} // namespace imageeditor::core
