#pragma once
#include "imageeditor/core/RasterSurface.hpp"
#include <memory>
#include <stdexcept>
#include <utility>
namespace imageeditor::core {
class ReadOnlyRasterSurface : public RasterSurface {
public:
    explicit ReadOnlyRasterSurface(std::shared_ptr<const RasterSurface> source) : source_(std::move(source))
    { if (!source_) throw std::invalid_argument("Missing read-only raster source"); }
    const auto& source() const noexcept { return source_; }
    SurfaceId id() const noexcept override { return source_->id(); }
    Extent2u extent() const noexcept override { return source_->extent(); }
    Revision revision() const noexcept override { return source_->revision(); }
    DirtySet dirtySince(Revision r) const override { return source_->dirtySince(r); }
    RasterDataKind dataKind() const noexcept override { return source_->dataKind(); }
    std::size_t nativeBytesPerPixel() const noexcept override { return source_->nativeBytesPerPixel(); }
    std::size_t allocatedPixelBytes() const noexcept override { return source_->allocatedPixelBytes(); }
    std::size_t memoryCost() const noexcept override { return source_->memoryCost(); }
    void copyRgba8(RectI r,std::span<std::byte> out,std::size_t stride) const override { source_->copyRgba8(r,out,stride); }
    void copyNative(RectI r,std::span<std::byte> out,std::size_t stride) const override { source_->copyNative(r,out,stride); }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch>) override { throw std::logic_error("Linked raster pixels are read-only."); }
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch>) override { throw std::logic_error("Linked raster pixels are read-only."); }
private:
    std::shared_ptr<const RasterSurface> source_;
};
} // namespace imageeditor::core
