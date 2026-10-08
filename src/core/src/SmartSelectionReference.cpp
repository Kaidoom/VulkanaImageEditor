#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/core/BlendCompositing.hpp"
#include <algorithm>
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
    while (image_->pixels.size() < end) {
        const auto i = image_->pixels.size();
        const auto x = i % extent.width, y = i / extent.width;
        const auto width = std::min({end - i, std::size_t(extent.width) - x, std::size_t(2048)});
        row_.resize(width);
        scratch_.resize(width * 4);
        // Same pinned canonical compositor as exports. Read aligned source
        // spans once instead of making a virtual raster read per pixel. Keep
        // chunks bounded by the caller's cooperative UI work budget.
        sampler_.sampleRow(int(x), int(y), row_, scratch_);
        for (std::size_t j = 0; j < width; ++j) {
            const bool valid = sampler_.validSample({double(x + j) + .5, double(y) + .5});
            auto color = valid ? encodeColor(row_[j]) : Rgba8 {};
            if (!color.alpha)
                color = {0, 0, 0, 0};
            image_->pixels.push_back(color);
            image_->valid.push_back(valid ? 1 : 0);
        }
    }
    return image_->pixels.size() == count;
}
std::shared_ptr<const SmartReferenceImage> SmartSelectionReference::image() const noexcept
{
    return image_->pixels.size() == std::size_t(image_->extent.width) * image_->extent.height ? image_
                                                                                              : nullptr;
}
}
