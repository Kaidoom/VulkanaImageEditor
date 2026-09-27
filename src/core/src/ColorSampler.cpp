#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerCrop.hpp"

#include <array>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
namespace {

using LinearColor = PremultipliedColor;

std::shared_ptr<const CompiledAdjustmentStack> preparedAdjustments(const AdjustmentState& state)
{
    if (!state) return {};
    auto program = std::make_shared<CompiledAdjustmentStack>(compileAdjustmentStack(state));
    return program->active ? program : nullptr;
}

LinearColor sampleRaster(const AffineTransform& inverse, const RasterSurface& surface,
    Vec2d point, std::size_t& texelsRead, SampleFiltering = SampleFiltering::AlphaAware)
{
    const auto extent = surface.extent();
    if (extent.empty()) {
        return {};
    }
    const auto local = inverse.map(point);
    if (!std::isfinite(local.x) || !std::isfinite(local.y))return {};
    float edgeCoverage=1;
    if(!inverse.isAffine()){
        const auto d=inverse.derivatives(point);
        edgeCoverage=layerCropCoverage(LayerCrop{{0,0,double(extent.width),double(extent.height)}},local,d[0],d[1]);
    }else if(local.x < 0.0 || local.y < 0.0 || local.x >= extent.width || local.y >= extent.height)edgeCoverage=0;
    if(edgeCoverage<=0) {
        return {};
    }
    // Vulkan normalized linear filtering: texel centers are i+0.5, with
    // CLAMP_TO_EDGE only inside the actual transformed layer quad.
    const double x = std::clamp(local.x - 0.5, 0.0, double(extent.width - 1));
    const double y = std::clamp(local.y - 0.5, 0.0, double(extent.height - 1));
    const auto x0 = static_cast<std::int32_t>(std::floor(x));
    const auto y0 = static_cast<std::int32_t>(std::floor(y));
    const auto x1 = static_cast<std::int32_t>(std::ceil(x));
    const auto y1 = static_cast<std::int32_t>(std::ceil(y));
    const RectI region {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    std::array<std::byte, 16> bytes {};
    const auto stride = static_cast<std::size_t>(region.width) * 4;
    surface.copyRgba8(region, bytes, stride);
    texelsRead += static_cast<std::size_t>(region.width * region.height);
    std::array<double,4> result {};
    for (int row = 0; row < region.height; ++row) {
        const auto wy = region.height == 1 ? 1.0 : row == 0 ? 1.0 - (y - y0) : y - y0;
        for (int col = 0; col < region.width; ++col) {
            const auto wx = region.width == 1 ? 1.0 : col == 0 ? 1.0 - (x - x0) : x - x0;
            const auto offset = static_cast<std::size_t>(row) * stride + std::size_t(col) * 4;
            const double alpha = double(std::to_integer<std::uint8_t>(bytes[offset + 3])) / 255.0;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const auto value = std::to_integer<std::uint8_t>(bytes[offset + channel]);
                result[channel] += wx * wy * (channel == 3 ? alpha : srgbToLinear(value) * alpha);
            }
        }
    }
    return {float(result[0])*edgeCoverage,float(result[1])*edgeCoverage,float(result[2])*edgeCoverage,float(result[3])*edgeCoverage};
}

LinearColor sampleLayer(const Layer& layer, const RasterSurface& surface,
    Vec2d point, std::size_t& texelsRead)
{
    const auto inverse = renderTransform(layer).inverted();
    return inverse ? sampleRaster(*inverse, surface, point, texelsRead) : LinearColor {};
}

Rgba8 encode(LinearColor color)
{
    return encodeColor(color);
}

} // namespace

PreparedLayerSampler::PreparedLayerSampler(const Layer& original, bool adjusted)
{
    auto layer=original;
    if(adjusted)layer=prepareSpatialFilterLayer(original);
    else {layer.filters.reset();layer.filterCache.reset();layer.effects.reset();layer.effectCache.reset();}
    const auto inverse = renderTransform(layer).inverted();
    const auto local = layer.localToDocument.inverted();
    if (!inverse || !local) return;
    surface_ = renderedSurface(layer);
    if (!surface_) return;
    revision_ = surface_->revision(); inverse_ = *inverse; documentToLocal_ = *local;
    crop_=layer.crop;
    if (adjusted && !layerSpatialFilterCacheValid(layer)) adjustments_ = preparedAdjustments(layer.adjustments);
    if(adjusted&&hasActiveLayerEffects(layer.effects)){
        effects_=compileLayerEffects(layer.effects,layerEffectReferenceFrame(layer));effectCache_=layer.effectCache;
    }
}
PremultipliedColor PreparedLayerSampler::sample(Vec2d point, std::size_t stopBefore) const
{
    if (!surface_) return {};
    if (surface_->revision() != revision_)
        throw std::runtime_error("Layer pixels changed during prepared sampling");
    std::size_t reads = 0;
    auto color = sampleRaster(inverse_, *surface_, point, reads);
    if(adjustments_)color=evaluateAdjustments(*adjustments_,color,documentToLocal_.map(point),stopBefore);
    if(effects_)color=compositeLayerEffects({},color,*effects_,effectCache_.get(),documentToLocal_.map(point),1,BlendMode::Normal);
    return applyLayerCrop(color,
        crop_,documentToLocal_,point);
}

PinnedDocumentSampler::PinnedDocumentSampler(const Document& document,
    std::optional<LayerId> active, ColorSampleSource source, SampleFiltering filtering,
    std::span<const SampleCacheOverride> prepared, ActiveReferenceAppearance appearance)
    : owner_(&document), snapshot_(document.snapshot()),
      activeOnly_(source == ColorSampleSource::ActiveLayer), filtering_(filtering)
{
    for (auto& layer : snapshot_.layersBottomToTop) {
        if (source==ColorSampleSource::ActiveLayer && layer.id!=active) continue;
        if ((source==ColorSampleSource::MergedVisible || appearance==ActiveReferenceAppearance::Rendered)
            && (!layer.visible || !std::isfinite(layer.opacity) || layer.opacity<=0)) continue;
        const auto replacement = std::find_if(prepared.begin(),prepared.end(),
            [&](const auto& entry) { return entry.id == layer.id; });
        if (replacement != prepared.end()) layer.renderCache = replacement->cache;
        const bool intrinsic=source==ColorSampleSource::ActiveLayer && appearance==ActiveReferenceAppearance::Intrinsic;
        if(intrinsic) {layer.filters.reset();layer.filterCache.reset();layer.effects.reset();layer.effectCache.reset();}
        else layer=prepareSpatialFilterLayer(layer);
        const auto surface=renderedSurface(layer);
        if (!surface) throw std::invalid_argument("Reference layer has no prepared image");
        const auto inverse=renderTransform(layer).inverted();
        const auto local=layer.localToDocument.inverted();
        if (!inverse || !local) continue;
        sources_.push_back({surface,surface->revision(),*inverse,
            source==ColorSampleSource::ActiveLayer && appearance==ActiveReferenceAppearance::Intrinsic
                ? 1.0 : std::clamp(double(layer.opacity),0.0,1.0),
            source==ColorSampleSource::ActiveLayer ? BlendMode::Normal : layer.blendMode,
            *local, source==ColorSampleSource::ActiveLayer && appearance==ActiveReferenceAppearance::Intrinsic
                ? nullptr : layerSpatialFilterCacheValid(layer)?nullptr:preparedAdjustments(layer.adjustments),layer.crop,
            hasActiveLayerEffects(layer.effects)?std::optional(compileLayerEffects(layer.effects,layerEffectReferenceFrame(layer))):std::nullopt,layer.effectCache});
    }
    if (source==ColorSampleSource::ActiveLayer && sources_.empty())
        throw std::invalid_argument("Reference requires an active layer with a prepared image and valid transform");
}
bool PinnedDocumentSampler::validSample(Vec2d point) const noexcept
{
    const auto extent = snapshot_.canvas.extent;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
        || point.x >= extent.width || point.y >= extent.height) return false;
    if (!activeOnly_) return true;
    if (sources_.empty()) return false;
    point=point+sampleOrigin_;
    const auto& source = sources_.front();
    const auto derivatives=source.documentToLocal.derivatives(point);
    if(source.crop && layerCropCoverage(*source.crop,source.documentToLocal.map({std::floor(point.x)+.5,std::floor(point.y)+.5}),
        derivatives[0],derivatives[1])<=0)return false;
    if(source.effects&&source.effectCache){
        const auto p=source.documentToLocal.map(point);const auto& r=source.effectCache->visualBounds;
        return p.x>=r.x&&p.y>=r.y&&p.x<r.right()&&p.y<r.bottom();
    }
    const auto local = source.inverse.map({std::floor(point.x) + .5, std::floor(point.y) + .5});
    const auto size = source.surface->extent();
    return std::isfinite(local.x) && std::isfinite(local.y) && local.x >= 0 && local.y >= 0
        && local.x < size.width && local.y < size.height;
}
bool PinnedDocumentSampler::matches(const Document& document) const noexcept
{
    if (&document!=owner_ || document.revision()!=snapshot_.documentRevision) return false;
    return std::all_of(sources_.begin(),sources_.end(),[](const auto& s) { return s.surface->revision()==s.revision; });
}
PinnedDocumentSampler::PinnedDocumentSampler(std::span<const Layer* const> layers,
    Extent2u extent,const AffineTransform& documentToSample,Vec2d sampleOrigin)
    : sampleOrigin_(sampleOrigin)
{
    snapshot_.canvas={extent,96};
    sources_.reserve(layers.size());
    for(const auto* original:layers) {
        if(!original || !original->visible || !std::isfinite(original->opacity) || original->opacity<=0)continue;
        std::optional<Layer> prepared;
        const auto* layer=original;
        if((hasActiveSpatialFilters(original->filters) && !layerSpatialFilterCacheValid(*original))||!layerEffectCacheValid(*original)) {
            prepared=prepareSpatialFilterLayer(*original); layer=&*prepared;
        }
        const auto surface=renderedSurface(*layer);
        if(!surface)throw std::invalid_argument("Reference layer has no prepared image");
        const auto inverse=composeAffine(documentToSample,renderTransform(*layer)).inverted();
        const auto local=composeAffine(documentToSample,layer->localToDocument).inverted();
        if(inverse && local)sources_.push_back({surface,surface->revision(),*inverse,std::clamp(double(layer->opacity),0.0,1.0),layer->blendMode,
            *local,layerSpatialFilterCacheValid(*layer)?nullptr:preparedAdjustments(layer->adjustments),layer->crop,
            hasActiveLayerEffects(layer->effects)?std::optional(compileLayerEffects(layer->effects,layerEffectReferenceFrame(*layer))):std::nullopt,layer->effectCache});
    }
}
Rgba8 PinnedDocumentSampler::sample(Vec2d point) const
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return {};
    return encode(sampleLinear({std::floor(point.x)+.5,std::floor(point.y)+.5}));
}
void PinnedDocumentSampler::sampleRow(std::int32_t x, std::int32_t y, std::span<PremultipliedColor> output,
    std::span<std::byte> scratch) const
{
    std::fill(output.begin(), output.end(), PremultipliedColor{});
    if (!std::all_of(sources_.begin(),sources_.end(),[](const auto& s) { return s.surface->revision()==s.revision; }))
        throw std::runtime_error("Image reference changed while rendering output");
    const auto extent=snapshot_.canvas.extent;
    if (y<0 || y>=std::int64_t(extent.height) || output.empty()) return;
    const auto first=std::max<std::int64_t>(0,-std::int64_t(x));
    const auto last=std::min<std::int64_t>(std::int64_t(output.size()),std::int64_t(extent.width)-x);
    const double documentX=double(x)+sampleOrigin_.x, documentY=double(y)+sampleOrigin_.y+.5;
    std::vector<std::byte> owned;
    if(scratch.size()<output.size()*4) {owned.resize(output.size()*4);scratch=owned;}
    for (const auto& s : sources_) {
        const auto e=s.surface->extent();
        // Intersect this output row with the inverse-mapped source quad before
        // touching pixels. Sparse separated layers do not scan the union N times.
        double lo=documentX+double(first)+.5, hi=documentX+double(last)-.5;
        const auto clipAxis=[&](double a,double b,double size) {
            if (a==0) return b>=0 && b<size;
            const auto p=-b/a,q=(size-b)/a;
            lo=std::max(lo,std::min(p,q)); hi=std::min(hi,std::max(p,q));
            return hi>=lo-1; // Conservative admission; exact coverage is sampled below.
        };
        const auto r=s.effects&&s.effectCache?s.effectCache->visualBounds:RectD{0,0,double(e.width),double(e.height)};
        const auto& admission=s.effects?s.documentToLocal:s.inverse;
        if (first>=last || (admission.isAffine() && (!clipAxis(admission.m00,admission.m01*documentY+admission.m02-r.x,r.width)
            || !clipAxis(admission.m10,admission.m11*documentY+admission.m12-r.y,r.height)))) continue;
        // Division used for row admission can round to the other side of an
        // exact edge. Admit one extra pixel; sampleRaster's original inverse
        // evaluation remains authoritative for the final coverage decision.
        const auto begin=std::max(first,std::int64_t(std::ceil(lo-documentX-.5))-1);
        const auto end=std::min(last,std::int64_t(std::floor(hi-documentX-.5))+2);
        if (end<=begin) continue;
        const bool aligned=!s.effects && s.inverse.isAffine() && s.inverse.m00==1 && s.inverse.m11==1 && s.inverse.m01==0 && s.inverse.m10==0
            && s.inverse.m02==std::floor(s.inverse.m02) && s.inverse.m12==std::floor(s.inverse.m12)
            && sampleOrigin_.x==std::floor(sampleOrigin_.x) && sampleOrigin_.y==std::floor(sampleOrigin_.y);
        auto b=begin, stop=end;
        if (aligned) {
            b=std::max(b,std::int64_t(-s.inverse.m02-documentX));
            stop=std::min(stop,std::int64_t(e.width-s.inverse.m02-documentX));
            const auto sourceY=double(y)+sampleOrigin_.y+s.inverse.m12;
            if (b>=stop || sourceY<0 || sourceY>=e.height) continue;
            const auto bytes=scratch.first(std::size_t(stop-b)*4);
            s.surface->copyRgba8({int(documentX+double(b)+s.inverse.m02),int(sourceY),int(stop-b),1},bytes,bytes.size());
        }
        for (auto i=b;i<stop;++i) {
            const Vec2d p{documentX+double(i)+.5,documentY};
            PremultipliedColor c;
            if (aligned) {
                const auto* v=scratch.data()+std::size_t(i-b)*4;
                c=decodeColor({std::to_integer<std::uint8_t>(v[0]),std::to_integer<std::uint8_t>(v[1]),
                    std::to_integer<std::uint8_t>(v[2]),std::to_integer<std::uint8_t>(v[3])});
            } else { std::size_t reads=0; c=sampleRaster(s.inverse,*s.surface,p,reads); }
            if (s.adjustments) c=evaluateAdjustments(*s.adjustments,c,s.documentToLocal.map(p));
            if(s.effects){
                const float crop=applyLayerCrop({1,1,1,1},s.crop,s.documentToLocal,p)[3];
                output[std::size_t(i)]=compositeLayerEffects(output[std::size_t(i)],c,*s.effects,s.effectCache.get(),s.documentToLocal.map(p),float(s.opacity),s.blendMode,crop);
            }else{
                c=applyLayerCrop(c,s.crop,s.documentToLocal,p);
                output[std::size_t(i)]=compositeLayer(output[std::size_t(i)],c,float(s.opacity),s.blendMode);
            }
        }
    }
}

PremultipliedColor PinnedDocumentSampler::sampleLinear(Vec2d point) const
{
    if (!std::all_of(sources_.begin(),sources_.end(),[](const auto& s) { return s.surface->revision()==s.revision; }))
        throw std::runtime_error("Image reference changed; selection unchanged");
    const auto extent=snapshot_.canvas.extent;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x<0 || point.y<0
        || point.x>=extent.width || point.y>=extent.height) return {};
    point=point+sampleOrigin_;
    LinearColor merged{}; std::size_t reads=0;
    for (const auto& s:sources_) {
        auto c=sampleRaster(s.inverse,*s.surface,point,reads,filtering_);
        if (s.adjustments) c=evaluateAdjustments(*s.adjustments,c,s.documentToLocal.map(point));
        if(s.effects){
            const float crop=applyLayerCrop({1,1,1,1},s.crop,s.documentToLocal,point)[3];
            merged=compositeLayerEffects(merged,c,*s.effects,s.effectCache.get(),s.documentToLocal.map(point),float(s.opacity),s.blendMode,crop);
        }else{
            c=applyLayerCrop(c,s.crop,s.documentToLocal,point);
            merged = compositeLayer(merged,c,float(s.opacity),s.blendMode);
        }
    }
    return merged;
}

PreparedRasterSampler::PreparedRasterSampler(const Layer& layer)
{
    const auto* raster = std::get_if<RasterLayer>(&layer.payload);
    const auto inverse = layer.localToDocument.inverted();
    if (raster && raster->surface && inverse) { surface_ = raster->surface.get(); inverse_ = *inverse; }
}
Rgba8 PreparedRasterSampler::sample(Vec2d point) const
{
    if (!surface_) return {};
    if(!inverse_.isAffine()){std::size_t reads=0;return encode(sampleRaster(inverse_,*surface_,point,reads));}
    const auto local = inverse_.map(point);
    const auto extent = surface_->extent();
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || local.x < 0 || local.y < 0
        || local.x >= extent.width || local.y >= extent.height) return {};
    const double x = std::clamp(local.x - 0.5, 0.0, double(extent.width - 1));
    const double y = std::clamp(local.y - 0.5, 0.0, double(extent.height - 1));
    if (x == std::floor(x) && y == std::floor(y)) {
        std::array<std::byte, 4> bytes;
        surface_->copyRgba8({int(x), int(y), 1, 1}, bytes, 4);
        return {std::to_integer<std::uint8_t>(bytes[0]), std::to_integer<std::uint8_t>(bytes[1]),
            std::to_integer<std::uint8_t>(bytes[2]), std::to_integer<std::uint8_t>(bytes[3])};
    }
    std::size_t reads = 0;
    return encode(sampleRaster(inverse_, *surface_, point, reads));
}

std::optional<LayerId> hitTestRasterLayer(const Document& document, Vec2d point)
{
    const auto extent = document.canvas().extent;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
        || point.x >= extent.width || point.y >= extent.height)
        return {};
    const auto& layers = document.layers();
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        if (!document.isEffectivelyVisible(it->id) || !std::isfinite(it->opacity) || it->opacity <= 0)
            continue;
        if(!hitLayerCrop(*it,point))continue;
        if(hitTextBounds(*it,point))return it->id;
        if (const auto* shape = std::get_if<ShapeLayer>(&it->payload)) {
            const auto inverse = it->localToDocument.inverted();
            if (inverse && shapeContainsPoint(*shape, inverse->map(point))) return it->id;
            // Exterior strokes use the prepared rendering, not a bounding box.
            // UI path hit-testing separately adds screen-space line tolerance.
            const auto surface = renderedSurface(*it);
            std::size_t reads = 0;
            if (surface && sampleLayer(*it, *surface, point, reads)[3] > 0) return it->id;
            continue;
        }
        const auto* raster = std::get_if<RasterLayer>(&it->payload);
        std::size_t reads = 0;
        if (raster && raster->surface && sampleLayer(*it, *raster->surface, point, reads)[3] > 0)
            return it->id;
    }
    return {};
}

ColorSample sampleDocumentColor(const Document& document,
    std::optional<LayerId> activeLayer, Vec2d documentPosition, ColorSampleSource source)
{
    ColorSample result;
    const auto extent = document.canvas().extent;
    if (!std::isfinite(documentPosition.x) || !std::isfinite(documentPosition.y)
        || documentPosition.x < 0.0 || documentPosition.y < 0.0
        || documentPosition.x >= extent.width || documentPosition.y >= extent.height) {
        return result;
    }
    const Vec2d point {std::floor(documentPosition.x) + 0.5,
        std::floor(documentPosition.y) + 0.5};
    result.status = ColorSampleStatus::Available;
    if (source == ColorSampleSource::ActiveLayer) {
        const auto* layer = activeLayer ? document.layer(*activeLayer) : nullptr;
        const auto surface=layer?intrinsicSurface(*layer):nullptr;
        if (!surface) {
            result.status = ColorSampleStatus::NoActiveRaster;
            return result;
        }
        result.layersVisited = 1;
        const auto inverse=composeAffine(layer->localToDocument,intrinsicPixelsToLocal(*layer)).inverted();
        result.color = inverse?encode(sampleRaster(*inverse,*surface,point,result.texelsRead)):Rgba8{};
        return result;
    }
    LinearColor merged {};
    for (const auto& original : document.layers()) {
        ++result.layersVisited;
        if (!document.isEffectivelyVisible(original.id) || !std::isfinite(original.opacity) || original.opacity <= 0.0F) {
            continue;
        }
        // Eyedropper hover is demand-driven input, not an output-preparation
        // operation. Never perform a full spatial convolution on its UI thread;
        // the canvas coordinator publishes the shared cache asynchronously.
        if((hasActiveSpatialFilters(original.filters)&&!layerSpatialFilterCacheValid(original))||!layerEffectCacheValid(original)) {
            result.status=ColorSampleStatus::UnsupportedLayer;return result;
        }
        const auto& layer=original;
        const auto surface=renderedSurface(layer);
        if (!surface) {
            if(std::holds_alternative<RasterLayer>(layer.payload))continue;
            // Editable text/shapes must prepare their disposable image first.
            // Never silently return an incomplete merged result.
            result.status = ColorSampleStatus::UnsupportedLayer;
            return result;
        }
        auto color = sampleLayer(layer, *surface, point, result.texelsRead);
        if (const auto compiled=layerSpatialFilterCacheValid(layer)?nullptr:preparedAdjustments(layer.adjustments)) {
            if (const auto local=layer.localToDocument.inverted())
                color=evaluateAdjustments(*compiled,color,local->map(point));
        }
        if(hasActiveLayerEffects(layer.effects)){
            const auto inverse=*layer.localToDocument.inverted();
            const float crop=applyLayerCrop({1,1,1,1},layer.crop,inverse,point)[3];
            merged=compositeLayerEffects(merged,color,compileLayerEffects(layer.effects,layerEffectReferenceFrame(layer)),layer.effectCache.get(),inverse.map(point),layer.opacity,layer.blendMode,crop);
        }else{
            if(const auto inverse=layer.localToDocument.inverted())color=applyLayerCrop(color,layer.crop,*inverse,point);
            merged = compositeLayer(merged,color,layer.opacity,layer.blendMode);
        }
    }
    result.color = encode(merged);
    return result;
}

} // namespace imageeditor::core
