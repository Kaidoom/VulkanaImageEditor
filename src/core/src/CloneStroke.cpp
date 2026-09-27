#include "imageeditor/core/CloneStroke.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include <algorithm>
#include <stdexcept>

namespace imageeditor::core {
CloneStroke::CloneStroke(Document& document, LayerId target, BrushSettings brush, CloneSettings settings,
    CloneReference reference, Vec2d offset, const IBrushAssetResolver* assets)
    : document_(document)
    , target_(target)
    , settings_(settings)
    , reference_(std::move(reference))
    , offset_(offset)
    , brush_(
          document, target, brush, BrushCompositeMode::Paint, std::make_unique<BasicPixelBrushEngine>(), { },
          { }, assets, { }, [this](Vec2d p) { return reference_.sample(p + offset_); },
          settings.mode == CloneMode::Heal ? "Heal stroke" : "Clone stamp stroke",
          settings.mode == CloneMode::Heal)
{
    const auto* layer = document.layer(target);
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    if (raster && raster->surface && layer->localToDocument.inverted()) {
        localToDocument_ = intrinsicTransform(*layer);
        documentToLocal_ = *localToDocument_.inverted();
        extent_ = raster->surface->extent();
        surface_ = raster->surface->id();
    }
    checkpoint();
}
void CloneStroke::checkpoint() noexcept
{
    revision_ = document_.revision();
    const auto* layer = document_.layer(target_);
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    surfaceRevision_ = raster && raster->surface ? raster->surface->revision() : 0;
}
bool CloneStroke::targetMatches() const noexcept
{
    const auto* layer = document_.layer(target_);
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    return document_.revision() == revision_ && raster && raster->surface && raster->surface->id() == surface_
        && raster->surface->revision() == surfaceRevision_ && intrinsicTransform(*layer) == localToDocument_;
}
bool CloneStroke::begin(const NormalizedPointerSample& sample)
{
    try {
        if (!targetMatches() || !brush_.begin(sample)) {
            cancel();
            return false;
        }
        checkpoint();
        return true;
    } catch (const std::exception& e) {
        diagnostic_ = e.what();
        cancel();
        return false;
    }
}
bool CloneStroke::append(const NormalizedPointerSample& sample)
{
    try {
        if (!targetMatches() || !brush_.append(sample)) {
            cancel();
            return false;
        }
        checkpoint();
        return true;
    } catch (const std::exception& e) {
        diagnostic_ = e.what();
        cancel();
        return false;
    }
}
RasterEditCommitResult CloneStroke::end(
    const NormalizedPointerSample& sample, History& history, const std::function<bool()>& cancelled)
{
    try {
        if (!targetMatches() || !brush_.finishInput(sample)) {
            cancel();
            return RasterEditCommitResult::TargetUnavailable;
        }
        checkpoint();
        const auto interrupted = [&] { return (cancelled && cancelled()) || !targetMatches(); };
        if (settings_.mode == CloneMode::Heal) {
            auto bounds = brush_.coverageBounds();
            if (!bounds.empty()) {
                constexpr int halo = 12;
                bounds = RectI { bounds.x - halo, bounds.y - halo, bounds.width + halo * 2,
                    bounds.height + halo * 2 }
                             .clippedTo({ 0, 0, int(extent_.width), int(extent_.height) });
                const auto count = std::size_t(bounds.width) * std::size_t(bounds.height);
                if (count > 1024U * 1024U)
                    throw std::length_error(
                        "Heal repair exceeds the 1-megapixel support budget. Use shorter strokes.");
                std::vector<PremultipliedColor> source(count), destination(count);
                std::vector<float> coverage(count);
                if (!brush_.copyCoverage(bounds, coverage, interrupted)) {
                    cancel();
                    return RasterEditCommitResult::TargetUnavailable;
                }
                for (int y = 0; y < bounds.height; ++y) {
                    if (interrupted()) {
                        cancel();
                        return RasterEditCommitResult::TargetUnavailable;
                    }
                    for (int x = 0; x < bounds.width; ++x) {
                        const auto i = std::size_t(y) * std::size_t(bounds.width) + std::size_t(x);
                        const auto p = localToDocument_.map({ bounds.x + x + .5, bounds.y + y + .5 });
                        source[i] = reference_.sample(p + offset_);
                        destination[i] = reference_.sampleDestination(p);
                        if (source[i][3] <= 0)
                            coverage[i] = 0;
                    }
                }
                HealingOptions options;
                options.adaptation = static_cast<float>(settings_.adaptation);
                options.cancelled = interrupted;
                healing_ = healPatch({ bounds.width, bounds.height, source, destination, coverage }, options);
                if (healing_.status == HealingStatus::Cancelled) {
                    cancel();
                    return RasterEditCommitResult::TargetUnavailable;
                }
                if (healing_.pixels.size() != count)
                    throw std::runtime_error("Heal could not reconstruct this region; stroke cancelled.");
                if (cancelled && cancelled()) {
                    cancel();
                    return RasterEditCommitResult::TargetUnavailable;
                }
                if (!targetMatches()) {
                    cancel();
                    return RasterEditCommitResult::TargetUnavailable;
                }
                if (!brush_.reapplySampledColor(
                        [this, bounds](Vec2d p) {
                            const auto q = documentToLocal_.map(p);
                            const auto x = int(std::floor(q.x)) - bounds.x,
                                       y = int(std::floor(q.y)) - bounds.y;
                            return x >= 0 && y >= 0 && x < bounds.width && y < bounds.height
                                ? healing_.pixels[std::size_t(y) * std::size_t(bounds.width) + std::size_t(x)]
                                : reference_.sample(p + offset_);
                        },
                        interrupted))
                    throw std::runtime_error("Heal cancelled or target changed; stroke cancelled.");
                checkpoint();
                if (healing_.status == HealingStatus::StampFallback
                    || healing_.status == HealingStatus::PartialFallback)
                    diagnostic_ = healing_.diagnostics.message;
            }
        }
        if (cancelled && cancelled()) {
            cancel();
            return RasterEditCommitResult::TargetUnavailable;
        }
        if (!targetMatches()) {
            cancel();
            return RasterEditCommitResult::TargetUnavailable;
        }
        return brush_.commit(history);
    } catch (const std::exception& e) {
        diagnostic_ = e.what();
        cancel();
        return RasterEditCommitResult::TargetUnavailable;
    }
}
void CloneStroke::cancel() noexcept { brush_.cancel(); }
}
