#include "imageeditor/core/CloneReference.hpp"

#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/core/ColorMath.hpp"

#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace imageeditor::core {
namespace {

// A newly allocated surface has no alias outside the capture builder. Samplers
// receive only const ownership; the mutable local pointer dies after capture.
class FrozenLayers {
public:
    explicit FrozenLayers(
        std::size_t limit, std::function<bool()> cancelled, std::span<const SampleCacheOverride> prepared)
        : limit_(limit)
        , cancelled_(std::move(cancelled))
        , prepared_(prepared)
    {
    }

    Layer freeze(const Layer& layer, bool visible)
    {
        auto cache = layer.renderCache;
        if (!std::holds_alternative<RasterLayer>(layer.payload))
            for (const auto& replacement : prepared_)
                if (replacement.id == layer.id) {
                    cache = replacement.cache;
                    break;
                }
        const auto source = std::holds_alternative<RasterLayer>(layer.payload) ? intrinsicSurface(layer)
            : cache                                                            ? cache->surface
                                                                               : nullptr;
        if (!source)
            throw std::invalid_argument(
                "A clone reference layer has no prepared image. Display the layer and try again.");
        auto [entry, inserted] = surfaces_.try_emplace(source.get());
        if (inserted) {
            const auto extent = source->extent();
            if (extent.empty() || extent.width > std::uint32_t(std::numeric_limits<std::int32_t>::max())
                || extent.height > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
                throw std::invalid_argument("A clone reference image has an unsupported extent.");
            const auto width = std::size_t(extent.width), height = std::size_t(extent.height);
            if (width > (limit_ - bytes_) / 4 / height)
                throw std::length_error("Clone reference exceeds its snapshot memory limit. Use Source Layer "
                                        "or a smaller reference stack.");
            const auto byteCount = width * height * 4;
            const auto revision = source->revision();
            std::vector<std::byte> pixels(byteCount);
            for (int y = 0; y < int(extent.height); y += 64) {
                if (cancelled_ && cancelled_())
                    throw std::runtime_error("Clone reference capture cancelled.");
                source->copyRgba8({ 0, y, int(extent.width), std::min(64, int(extent.height) - y) },
                    std::span<std::byte>(pixels).subspan(std::size_t(y) * width * 4), width * 4);
            }
            if (source->revision() != revision)
                throw std::runtime_error("Clone reference changed during capture. Start the stroke again.");
            entry->second = std::make_shared<ContiguousRasterSurface>(extent, std::move(pixels));
            bytes_ += byteCount;
        }

        Layer frozen;
        frozen.id = layer.id;
        frozen.visible = visible;
        frozen.opacity = layer.opacity;
        frozen.blendMode = layer.blendMode;
        frozen.adjustments = layer.adjustments;
        frozen.filters = layer.filters;
        frozen.effects = layer.effects;
        frozen.localToDocument = layer.localToDocument;
        frozen.rasterOrigin=layer.rasterOrigin;frozen.rasterEffectFrame=layer.rasterEffectFrame;
        frozen.crop = layer.crop;
        if (std::holds_alternative<RasterLayer>(layer.payload)) {
            frozen.payload = RasterLayer { entry->second };
        } else {
            // Prepared sampling needs typed cache geometry, never text layout
            // or shape payload copies. The live typed source stays editable.
            frozen.payload = TextLayer { };
            if(const auto* shape=std::get_if<ShapeLayer>(&layer.payload)) {
                ShapeLayer frame;frame.size=shape->size;frozen.payload=std::move(frame);
            }
            auto frozenCache = std::make_shared<LayerRenderCache>();
            frozenCache->surface = entry->second;
            frozenCache->pixelsToLocal = cache->pixelsToLocal;
            frozenCache->logicalExtent = cache->logicalExtent;
            frozen.renderCache = std::move(frozenCache);
        }
        return frozen;
    }

    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    std::size_t limit_, bytes_ { 0 };
    std::function<bool()> cancelled_;
    std::span<const SampleCacheOverride> prepared_;
    std::unordered_map<const RasterSurface*, std::shared_ptr<ContiguousRasterSurface>> surfaces_;
};

bool validGeometry(const Layer& layer)
{
    return layer.localToDocument.inverted().has_value() && renderTransform(layer).inverted().has_value();
}

} // namespace

struct CloneReference::Impl {
    std::optional<PreparedLayerSampler> rawSource, rawDestination;
    std::optional<PinnedDocumentSampler> renderedSource, renderedDestination;
    std::size_t bytes { 0 }, sourceCount { 0 };
    Extent2u canvas, rawExtent;
    AffineTransform rawInverse, rawDocumentToLocal;
    std::optional<LayerCrop> rawCrop;
    std::shared_ptr<const RasterSurface> rawSurface;
    AffineTransform rawToDocument;
    std::vector<Layer> pendingRenderedLayers;
    std::size_t pendingBelowCount {0};
    bool pendingSource {false}, pendingDestination {false};
    CloneSampleSource mode {CloneSampleSource::SourceLayer};
    Vec2d support;
};

std::optional<CloneReference> CloneReference::capture(const Document& document, LayerId targetId,
    LayerId sourceId, CloneSampleSource mode, std::string& diagnostic, std::size_t limitBytes,
    bool includeDestinationContext, std::function<bool()> cancelled,
    std::span<const SampleCacheOverride> prepared, bool deferRenderedPreparation)
{
    diagnostic.clear();
    try {
        const auto documentRevision = document.revision();
        std::vector<std::pair<std::shared_ptr<const RasterSurface>, Revision>> revisions;
        for (const auto& layer : document.layers())
            if (auto surface = renderedSurface(layer))
                revisions.emplace_back(surface, surface->revision());
        for (const auto& replacement : prepared)
            if (replacement.cache && replacement.cache->surface)
                revisions.emplace_back(replacement.cache->surface, replacement.cache->surface->revision());
        const auto* target = document.layer(targetId);
        if (!target || !std::holds_alternative<RasterLayer>(target->payload) || !renderedSurface(*target)
            || !validGeometry(*target))
            throw std::invalid_argument("Cloning requires a raster destination with a valid transform.");
        const auto* source = mode == CloneSampleSource::SourceLayer ? document.layer(sourceId) : nullptr;
        if (mode == CloneSampleSource::SourceLayer
            && (!source || !document.isEffectivelyVisible(sourceId) || !validGeometry(*source)))
            throw std::invalid_argument("The identified clone source is missing, hidden, or has an invalid "
                                        "transform. Alt-click a visible source again.");

        auto result = std::make_shared<Impl>();
        result->canvas = document.canvas().extent;
        FrozenLayers frozen(limitBytes, std::move(cancelled), prepared);
        const bool rawContext = mode == CloneSampleSource::SourceLayer && sourceId == targetId;
        if (source) {
            const auto layer = frozen.freeze(*source, true);
            result->rawSource.emplace(layer, false);
            result->rawExtent = renderedSurface(layer)->extent();
            result->rawInverse = *renderTransform(layer).inverted();
            result->rawDocumentToLocal = *intrinsicTransform(layer).inverted();
            result->rawCrop = layer.crop;
            if(result->rawCrop){result->rawCrop->x-=layer.rasterOrigin.x;result->rawCrop->y-=layer.rasterOrigin.y;}
            result->rawSurface = intrinsicSurface(layer);
            result->rawToDocument = intrinsicTransform(layer);
            result->sourceCount = 1;
            if (includeDestinationContext && rawContext)
                result->rawDestination.emplace(layer, false);
        }

        const bool needRenderedSource = mode != CloneSampleSource::SourceLayer;
        const bool needRenderedContext = includeDestinationContext && !rawContext;
        if (needRenderedSource || needRenderedContext) {
            std::vector<Layer> layers;
            layers.reserve(document.layers().size());
            std::size_t belowCount = 0;
            bool reachedTarget = false;
            for (const auto& layer : document.layers()) {
                const bool include = !reachedTarget || mode == CloneSampleSource::AllVisible;
                if (include && document.isEffectivelyVisible(layer.id) && std::isfinite(layer.opacity)
                    && layer.opacity > 0 && validGeometry(layer)) {
                    layers.push_back(frozen.freeze(layer, true));
                    const auto& frozenLayer = layers.back();
                    const auto pixelToLocal = intrinsicPixelsToLocal(frozenLayer);
                    const auto pixelToDocument = composeAffine(frozenLayer.localToDocument, pixelToLocal);
                    const double density = pixelToLocal.m00 > 0 ? 1.0 / pixelToLocal.m00 : 1.0;
                    const auto support = requiredSpatialInputBounds(frozenLayer.filters, {0, 0, 1, 1}, density);
                    double effectSupport=0;
                    if(frozenLayer.effects)for(std::size_t i=0;i<5;++i) {
                        const auto& effect=frozenLayer.effects->items[i];
                        if(effect.enabled)effectSupport=std::max(effectSupport,(effect.size+(i==1||i==2?effect.distance:0))*density+1);
                    }
                    const double rx = std::max(-support.x, support.right() - 1) + 1.0 + effectSupport;
                    const double ry = std::max(-support.y, support.bottom() - 1) + 1.0 + effectSupport;
                    if (pixelToDocument.isAffine()) {
                        result->support.x = std::max(result->support.x,
                            std::abs(pixelToDocument.m00) * rx + std::abs(pixelToDocument.m01) * ry);
                        result->support.y = std::max(result->support.y,
                            std::abs(pixelToDocument.m10) * rx + std::abs(pixelToDocument.m11) * ry);
                    } else {
                        const auto extent=intrinsicSurface(frozenLayer)->extent();
                        const auto scale=pixelToDocument.maximumScaleOver({-rx,-ry,double(extent.width)+2*rx,double(extent.height)+2*ry});
                        if(!std::isfinite(scale)) throw std::invalid_argument("Clone reference support crosses a projective horizon");
                        const auto radius=scale*std::hypot(rx,ry);
                        result->support.x=std::max(result->support.x,radius);
                        result->support.y=std::max(result->support.y,radius);
                    }
                    if (!reachedTarget)
                        belowCount = layers.size();
                }
                if (layer.id == targetId)
                    reachedTarget = true;
            }
            if (deferRenderedPreparation) {
                result->pendingRenderedLayers = std::move(layers);
                result->pendingBelowCount = belowCount;
                result->pendingSource = needRenderedSource;
                result->pendingDestination = needRenderedContext;
                result->mode = mode;
                if (needRenderedSource) result->sourceCount = mode == CloneSampleSource::CurrentAndBelow
                    ? belowCount : result->pendingRenderedLayers.size();
            } else {
            std::vector<const Layer*> pointers;
            pointers.reserve(layers.size());
            for (const auto& layer : layers)
                pointers.push_back(&layer);
            const auto below = std::span<const Layer* const>(pointers.data(), belowCount);
            const auto extent = document.canvas().extent;
            if (needRenderedSource) {
                result->renderedSource.emplace(mode == CloneSampleSource::CurrentAndBelow
                        ? below
                        : std::span<const Layer* const>(pointers),
                    extent);
                result->sourceCount = result->renderedSource->sourceCount();
            }
            if (needRenderedContext)
                result->renderedDestination.emplace(below, extent);
            }
        }
        result->bytes = frozen.bytes();
        if (document.revision() != documentRevision
            || std::any_of(revisions.begin(), revisions.end(),
                [](const auto& entry) { return entry.first->revision() != entry.second; }))
            throw std::runtime_error("Clone reference changed during capture. Start the stroke again.");
        return CloneReference(std::move(result));
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return { };
    }
}

CloneReference CloneReference::prepared(const std::function<bool()>& cancelled, std::size_t byteBudget) const
{
    if (impl_->bytes > byteBudget) throw std::length_error("Spot Heal frozen reference exceeds the processing budget.");
    if (!impl_->pendingSource && !impl_->pendingDestination) return *this;
    auto result = std::make_shared<Impl>(*impl_);
    for (auto& layer : result->pendingRenderedLayers) {
        if (cancelled && cancelled()) throw std::runtime_error("Spot Heal reference preparation cancelled.");
        if (!hasActiveSpatialFilters(layer.filters)&&!hasActiveLayerEffects(layer.effects)) continue;
        FilterPreparationOptions options;
        options.cancelled = cancelled;
        // Aggregate retained outputs across the whole stack. Each new filter's
        // complete temporary working set must fit alongside every frozen
        // source and previously prepared output, not get a fresh per-layer cap.
        options.byteBudget = byteBudget - result->bytes;
        const auto source = intrinsicSurface(layer);
        FilterInputSnapshot frozen{layer, source->id(), source->revision(), 0};
        const auto prepared=prepareLayerSpatialEffects(frozen,options);
        layer.filterCache=prepared.filters;layer.effectCache=prepared.effects;
        if(layer.effectCache)for(const auto& mask:layer.effectCache->masks)if(mask) {
            const auto retained=mask->coverage->memoryCost();
            if(retained>byteBudget-result->bytes)throw std::length_error("Spot Heal effect masks exceed processing budget.");
            result->bytes+=retained;
        }
        if (layer.filterCache && layer.filterCache->surface) {
            const auto e = layer.filterCache->surface->extent();
            const auto retained = std::size_t(e.width) * std::size_t(e.height) * 4;
            if (retained > byteBudget - result->bytes) throw std::length_error("Spot Heal rendered reference exceeds the processing budget.");
            result->bytes += retained;
        }
    }
    std::vector<const Layer*> pointers;
    for (const auto& layer : result->pendingRenderedLayers) pointers.push_back(&layer);
    const auto below = std::span<const Layer* const>(pointers.data(), result->pendingBelowCount);
    if (result->pendingSource)
        result->renderedSource.emplace(result->mode == CloneSampleSource::CurrentAndBelow
            ? below : std::span<const Layer* const>(pointers), result->canvas);
    if (result->pendingDestination) result->renderedDestination.emplace(below, result->canvas);
    result->pendingSource = result->pendingDestination = false;
    result->pendingRenderedLayers.clear();
    return CloneReference(std::move(result));
}

PremultipliedColor CloneReference::rawTexel(int x, int y) const
{
    if (!impl_->rawSurface || x < 0 || y < 0 || x >= int(impl_->rawExtent.width)
        || y >= int(impl_->rawExtent.height)) return {};
    const auto point = impl_->rawToDocument.map({x + .5, y + .5});
    if (!validSample(point)) return {};
    std::array<std::byte, 4> bytes;
    impl_->rawSurface->copyRgba8({x, y, 1, 1}, bytes, 4);
    float alpha = float(std::to_integer<unsigned char>(bytes[3])) / 255.0F;
    const auto derivatives=impl_->rawDocumentToLocal.derivatives(point);
    if (impl_->rawCrop) alpha *= layerCropCoverage(*impl_->rawCrop, {x + .5, y + .5},derivatives[0],derivatives[1]);
    return {float(srgbToLinear(std::to_integer<unsigned char>(bytes[0]))) * alpha,
        float(srgbToLinear(std::to_integer<unsigned char>(bytes[1]))) * alpha,
        float(srgbToLinear(std::to_integer<unsigned char>(bytes[2]))) * alpha, alpha};
}

Extent2u CloneReference::rawExtent() const noexcept { return impl_->rawExtent; }
Vec2d CloneReference::readSupport() const noexcept { return impl_->support; }

PremultipliedColor CloneReference::sample(Vec2d point) const
{
    if (!validSample(point))
        return { };
    if (impl_->rawSource)
        return impl_->rawSource->sample(point);
    return impl_->renderedSource ? impl_->renderedSource->sampleLinear(point) : PremultipliedColor { };
}

PremultipliedColor CloneReference::sampleDestination(Vec2d point) const
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
        || point.x >= impl_->canvas.width || point.y >= impl_->canvas.height)
        return { };
    if (impl_->rawDestination)
        return impl_->rawDestination->sample(point);
    return impl_->renderedDestination ? impl_->renderedDestination->sampleLinear(point)
                                      : PremultipliedColor { };
}

bool CloneReference::validSample(Vec2d point) const noexcept
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
        || point.x >= impl_->canvas.width || point.y >= impl_->canvas.height)
        return false;
    if (!impl_->rawSource)
        return true;
    const auto p = impl_->rawInverse.map(point);
    if (p.x < 0 || p.y < 0 || p.x >= impl_->rawExtent.width || p.y >= impl_->rawExtent.height)
        return false;
    const auto derivatives=impl_->rawDocumentToLocal.derivatives(point);
    if (impl_->rawCrop
        && layerCropCoverage(*impl_->rawCrop, impl_->rawDocumentToLocal.map(point),
               derivatives[0],derivatives[1])
            <= 0)
        return false;
    return true;
}

std::size_t CloneReference::snapshotBytes() const noexcept { return impl_->bytes; }
std::size_t CloneReference::sourceCount() const noexcept { return impl_->sourceCount; }

} // namespace imageeditor::core
