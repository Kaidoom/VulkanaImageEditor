#pragma once

#include "imageeditor/core/ScalarPlane.hpp"

namespace imageeditor::core {

class ScalarRasterSurface final : public RasterSurface {
public:
    explicit ScalarRasterSurface(Extent2u, std::uint8_t fill = 0);
    ScalarRasterSurface(Extent2u, std::span<const std::byte> values, std::size_t stride);
    explicit ScalarRasterSurface(ScalarPlaneState);

    [[nodiscard]] SurfaceId id() const noexcept override { return id_; }
    [[nodiscard]] Extent2u extent() const noexcept override { return state_->extent(); }
    [[nodiscard]] Revision revision() const noexcept override { return revision_; }
    [[nodiscard]] RasterDataKind dataKind() const noexcept override { return RasterDataKind::Scalar; }
    [[nodiscard]] std::size_t nativeBytesPerPixel() const noexcept override { return 1; }
    [[nodiscard]] std::size_t allocatedPixelBytes() const noexcept override { return state_->allocatedPixelBytes(); }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
    [[nodiscard]] ScalarPlaneState snapshot() const noexcept { return state_; }
    [[nodiscard]] std::shared_ptr<ScalarRasterSurface> clone() const;
    [[nodiscard]] DirtySet dirtySince(Revision uploadedRevision) const override;

    void copyNative(RectI, std::span<std::byte>, std::size_t stride) const override;
    [[nodiscard]] DirtySet replaceNativeBatch(std::span<const RasterPatch>) override;
    [[nodiscard]] DirtySet swapNativeBatch(std::span<MutableRasterPatch>) override;

    // Compatibility view only: numeric values expand to v,v,v,255. Reject
    // non-grayscale or translucent writes instead of losing information.
    void copyRgba8(RectI, std::span<std::byte>, std::size_t stride) const override;
    [[nodiscard]] DirtySet replaceRgba8Batch(std::span<const RasterPatch>) override;
    [[nodiscard]] DirtySet swapRgba8Batch(std::span<MutableRasterPatch>) override;

private:
    struct DirtyRecord { Revision revision; std::vector<RectI> regions; };
    [[nodiscard]] DirtySet publish(ScalarPlaneState, std::vector<RectI>);
    SurfaceId id_ {makeSurfaceId()};
    Revision revision_ {1};
    ScalarPlaneState state_;
    std::deque<DirtyRecord> dirtyJournal_;
};

} // namespace imageeditor::core
