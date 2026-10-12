#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/BoundedParallel.hpp"
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
bool SmartSelectionReference::step(std::size_t budget, unsigned workers)
{
    const auto extent = image_->extent;
    const auto count = std::size_t(extent.width) * extent.height;
    const auto start = completed_;
    const auto end = start + std::min(count - start, std::max(std::size_t(1), budget));
    image_->pixels.resize(end);
    image_->valid.resize(end);
    const auto prepare = [&](unsigned rank, unsigned participants) {
        auto& scratch = scratch_[rank];
        const auto begin = start + (end-start) * rank / participants;
        const auto finish = start + (end-start) * (rank+1) / participants;
        // Bound the compositor's per-source row planes across all participants,
        // including large stacks, without copying source pixels or changing math.
        constexpr std::size_t rowBudget = 64ULL * 1024 * 1024;
        const auto maxWidth = std::clamp(rowBudget / participants
            / std::max(std::size_t(1), sampler_.sourceCount()) / sizeof(PremultipliedColor),
            std::size_t(1), std::size_t(2048));
        for (auto i = begin; i < finish;) {
            const auto x = i % extent.width, y = i / extent.width;
            const auto width = std::min({finish-i, std::size_t(extent.width)-x, maxWidth});
            scratch.row.resize(width);
            scratch.bytes.resize(width * 4);
            sampler_.sampleRow(int(x), int(y), scratch.row, scratch.bytes);
            for (std::size_t j = 0; j < width; ++j) {
                const bool valid = sampler_.validSample({double(x + j) + .5, double(y) + .5});
                auto color = valid ? encodeColor(scratch.row[j]) : Rgba8 {};
                if (!color.alpha) color = {0, 0, 0, 0};
                image_->pixels[i+j] = color;
                image_->valid[i+j] = valid ? 1 : 0;
            }
            i += width;
        }
    };
    const auto parallelThreshold = std::max(std::size_t(256),
        std::size_t(16384) / std::max(std::size_t(1), sampler_.sourceCount()));
    if (workers == 1 || end-start < parallelThreshold
        || !tryBoundedParallel(std::min(workers, 8U), prepare))
        prepare(0, 1);
    completed_ = end;
    return completed_ == count;
}
std::shared_ptr<const SmartReferenceImage> SmartSelectionReference::image() const noexcept
{
    return completed_ == std::size_t(image_->extent.width) * image_->extent.height ? image_
                                                                                              : nullptr;
}
}
