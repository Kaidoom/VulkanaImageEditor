#include "imageeditor/core/ProjectLayer.hpp"
#include "ReadOnlyRasterSurface.hpp"

namespace imageeditor::core {
std::shared_ptr<RasterSurface> readOnlyProjectSurface(std::shared_ptr<const RasterSurface> source)
{
    if (auto wrapper = std::dynamic_pointer_cast<const ReadOnlyRasterSurface>(source))
        return std::const_pointer_cast<ReadOnlyRasterSurface>(wrapper);
    return std::make_shared<ReadOnlyRasterSurface>(std::move(source));
}

std::shared_ptr<RasterSurface> copyColorRaster(const RasterSurface& source)
{
    if (source.dataKind() != RasterDataKind::Color)
        throw std::invalid_argument("Expected a color raster");
    const auto extent = source.extent();
    const std::size_t stride = std::size_t(extent.width) * 4;
    std::vector<std::byte> pixels(stride * extent.height);
    source.copyRgba8({0, 0, int(extent.width), int(extent.height)}, pixels, stride);
    return std::make_shared<ContiguousRasterSurface>(extent, std::move(pixels));
}
} // namespace imageeditor::core
