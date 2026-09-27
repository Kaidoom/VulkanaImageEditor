#include "imageeditor/core/SmartSelection.hpp"
#include <stdexcept>

namespace imageeditor::core {
SmartSelectionReference::SmartSelectionReference(const Document& doc, std::optional<LayerId> layer,
    ColorSampleSource source, std::span<const SampleCacheOverride> caches)
    : sampler_(doc, layer, source, SampleFiltering::AlphaAware, caches, ActiveReferenceAppearance::Rendered)
    , image_(std::make_shared<SmartReferenceImage>())
{
    image_->extent = sampler_.extent();
    const auto count = std::uint64_t(image_->extent.width) * image_->extent.height;
    if (!count || count > maximumPixels)
        throw std::length_error("Smart Select supports up to 64 megapixels");
    image_->pixels.reserve(std::size_t(count));
    image_->valid.reserve(std::size_t(count));
}
bool SmartSelectionReference::step(std::size_t budget)
{
    const auto extent = image_->extent;
    const auto count = std::size_t(extent.width) * extent.height;
    const auto end
        = image_->pixels.size() + std::min(count - image_->pixels.size(), std::max(std::size_t(1), budget));
    for (std::size_t i = image_->pixels.size(); i < end; ++i) {
        const Vec2d p { double(i % extent.width) + .5, double(i / extent.width) + .5 };
        const bool valid = sampler_.validSample(p);
        auto color = valid ? sampler_.sample(p) : Rgba8 { };
        if (!color.alpha)
            color = { 0, 0, 0, 0 };
        image_->pixels.push_back(color);
        image_->valid.push_back(valid ? 1 : 0);
    }
    return image_->pixels.size() == count;
}
std::shared_ptr<const SmartReferenceImage> SmartSelectionReference::image() const noexcept
{
    return image_->pixels.size() == std::size_t(image_->extent.width) * image_->extent.height ? image_
                                                                                              : nullptr;
}
}
