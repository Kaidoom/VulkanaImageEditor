#include "imageeditor/ui/BackgroundRemovalMask.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace imageeditor::ui {
namespace {
constexpr std::uint64_t maximumMaskPixels = 64ULL * 1024 * 1024;

void validateExtent(core::Extent2u extent)
{
    if (extent.empty() || extent.width > 32768 || extent.height > 32768
        || std::uint64_t(extent.width) * extent.height > maximumMaskPixels)
        throw std::invalid_argument("Background removal requires a raster of at most 64 megapixels and 32768 pixels per side.");
}

void validateExistingMask(const core::LayerMaskState& mask)
{
    if (!core::validLayerMask(mask))
        throw std::invalid_argument("The existing layer mask is invalid.");
    if (mask && !mask->enabled)
        throw std::invalid_argument("Enable the existing layer mask before removing the background; its hidden edits will be preserved.");
}

bool visibleInCrop(const BackgroundRemovalSource& source, int x, int y)
{
    return !source.crop || source.crop->contains(
        {source.rasterOrigin.x + x + .5, source.rasterOrigin.y + y + .5});
}
} // namespace

BackgroundRemovalSourceBuilder::BackgroundRemovalSourceBuilder(const core::Layer& layer)
{
    const auto* raster = std::get_if<core::RasterLayer>(&layer.payload);
    if (!raster || !raster->surface)
        throw std::invalid_argument("Background removal requires a raster layer.");
    validateExistingMask(layer.mask);
    if (!std::isfinite(layer.rasterOrigin.x) || !std::isfinite(layer.rasterOrigin.y))
        throw std::invalid_argument("The raster layer has an invalid origin.");
    surface_ = raster->surface;
    const auto extent = surface_->extent();
    validateExtent(extent);
    source_.layerId = layer.id;
    source_.sourceId = surface_->id();
    source_.sourceRevision = surface_->revision();
    source_.rasterOrigin = layer.rasterOrigin;
    source_.originalMask = layer.mask;
    source_.crop = layer.crop;
    source_.rgba = QImage(int(extent.width), int(extent.height), QImage::Format_RGBA8888);
    if (source_.rgba.isNull())
        throw std::runtime_error("Unable to allocate the background-removal source image.");
}

bool BackgroundRemovalSourceBuilder::step(int maximumRows)
{
    if (maximumRows < 1)
        throw std::invalid_argument("The background-removal copy batch must contain at least one row.");
    if (surface_->revision() != source_.sourceRevision)
        throw std::runtime_error("The source layer changed while preparing background removal.");
    if (copiedRows_ == source_.rgba.height())
        return true;
    const int rows = std::min(maximumRows, source_.rgba.height() - copiedRows_);
    const auto stride = std::size_t(source_.rgba.bytesPerLine());
    auto* destination = source_.rgba.scanLine(copiedRows_);
    surface_->copyRgba8({0, copiedRows_, source_.rgba.width(), rows},
        {reinterpret_cast<std::byte*>(destination), stride * std::size_t(rows)}, stride);
    if (surface_->revision() != source_.sourceRevision)
        throw std::runtime_error("The source layer changed while preparing background removal.");
    if (!hasVisibleSource_) {
        for (int row = 0; row < rows && !hasVisibleSource_; ++row) {
            const auto* pixels = destination + std::size_t(row) * stride;
            for (int x = 0; x < source_.rgba.width(); ++x) {
                if (pixels[std::size_t(x) * 4 + 3]
                    && visibleInCrop(source_, x, copiedRows_ + row)) {
                    hasVisibleSource_ = true;
                    break;
                }
            }
        }
    }
    copiedRows_ += rows;
    if (copiedRows_ == source_.rgba.height() && !hasVisibleSource_)
        throw std::invalid_argument("The raster layer contains no visible pixels.");
    return copiedRows_ == source_.rgba.height();
}

BackgroundRemovalSource BackgroundRemovalSourceBuilder::source() const
{
    if (copiedRows_ != source_.rgba.height() || !hasVisibleSource_)
        throw std::logic_error("The background-removal source copy is incomplete.");
    return source_;
}

BackgroundRemovalSource backgroundRemovalSource(const core::Layer& layer)
{
    BackgroundRemovalSourceBuilder builder(layer);
    while (!builder.step()) {}
    return builder.source();
}

core::LayerMaskState backgroundRemovalMask(const BackgroundRemovalSource& source,
    const QImage& generated, const std::function<bool()>& cancelled)
{
    if (source.rgba.isNull() || source.rgba.format() != QImage::Format_RGBA8888)
        throw std::invalid_argument("Background removal requires an owned, straight RGBA source image.");
    const core::Extent2u extent{std::uint32_t(source.rgba.width()), std::uint32_t(source.rgba.height())};
    validateExtent(extent);
    validateExistingMask(source.originalMask);
    if (!std::isfinite(source.rasterOrigin.x) || !std::isfinite(source.rasterOrigin.y))
        throw std::invalid_argument("The raster layer has an invalid origin.");
    if (generated.size() != source.rgba.size() || generated.format() != QImage::Format_Grayscale8)
        throw std::invalid_argument("The foreground mask must be grayscale and match the original raster dimensions.");
    if (cancelled && cancelled())
        throw std::runtime_error("Background removal cancelled.");
    std::vector<std::uint8_t> coverage(std::size_t(extent.width) * extent.height);
    bool retainsVisibleSource = false;
    for (int y = 0; y < generated.height(); ++y) {
        if (cancelled && cancelled())
            throw std::runtime_error("Background removal cancelled.");
        const auto* input = generated.constScanLine(y);
        const auto* pixels = source.rgba.constScanLine(y);
        auto* output = coverage.data() + std::size_t(y) * extent.width;
        for (int x = 0; x < generated.width(); ++x) {
            const float previous = core::layerMaskCoverage(source.originalMask,
                {source.rasterOrigin.x + x + .5, source.rasterOrigin.y + y + .5});
            output[x] = std::uint8_t(std::clamp(std::lround(input[x] * previous), 0L, 255L));
            if (output[x] && pixels[std::size_t(x) * 4 + 3] && visibleInCrop(source, x, y))
                retainsVisibleSource = true;
        }
    }
    if (!retainsVisibleSource)
        throw std::runtime_error("The generated mask would hide every visible source pixel. Nothing was applied.");
    if (cancelled && cancelled())
        throw std::runtime_error("Background removal cancelled.");
    auto mask = std::make_shared<core::LayerMask>();
    mask->coverage = core::SelectionMask::fromR8(extent, coverage, extent.width);
    mask->localToMask = {1, 0, -source.rasterOrigin.x, 0, 1, -source.rasterOrigin.y};
    mask->outside = 0;
    mask->enabled = true;
    if (!core::validLayerMask(mask))
        throw std::invalid_argument("The foreground mask cannot be represented in the layer's local frame.");
    return mask;
}
} // namespace imageeditor::ui
