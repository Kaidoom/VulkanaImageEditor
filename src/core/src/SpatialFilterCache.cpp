#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
namespace {
void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
void checkCancellation(const FilterPreparationOptions& options)
{ if (options.cancelled && options.cancelled()) throw std::runtime_error("Filter preparation cancelled"); }
bool sameTransform(const AffineTransform& a,const AffineTransform& b) noexcept
{ return a==b; }
template<class L> bool validCache(const L& layer) noexcept
{
    const auto& cache=layer.filterCache;
    if (!cache || !cache->surface || !hasActiveSpatialFilters(layer.filters)) return false;
    const auto source=intrinsicSurface(layer);
    return source && source->id()==cache->sourceId && source->revision()==cache->sourceRevision
        && sameTransform(intrinsicPixelsToLocal(layer),cache->sourcePixelsToLocal)
        && equivalentAdjustments(layer.adjustments,cache->adjustments)
        && equivalentSpatialFilters(layer.filters,cache->filters);
}
std::size_t planeBytes(RectI r)
{
    require(!r.empty() && r.width<=32768 && r.height<=32768, "Filter output exceeds supported dimensions");
    const auto bytes=std::uint64_t(r.width)*std::uint64_t(r.height)*4*sizeof(float);
    require(bytes<=std::numeric_limits<std::size_t>::max(), "Filter output allocation overflow");
    return std::size_t(bytes);
}
std::size_t scalarFilterBudget(const Layer& layer,const RasterSurface& source,const FilterPreparationOptions& options)
{
    require(scalarAdjustmentsSupported(layer.adjustments),"Unsupported scalar adjustment stack");
    require(scalarFiltersSupported(layer.filters),"Unsupported scalar filter stack");
    const auto e=source.extent();
    const auto mapping=intrinsicPixelsToLocal(layer);
    require(mapping.m01==0&&mapping.m10==0&&mapping.m00>0&&mapping.m00==mapping.m11,
        "Unsupported scalar filter density mapping");
    const auto density=1.0/mapping.m00;
    require(density>0&&density<=8,"Scalar filter density exceeds supported cache limit");
    const auto input=requiredSpatialInputBounds(layer.filters,{0,0,int(e.width),int(e.height)},density);
    const auto plane=planeBytes(input)/4;
    const auto retained=source.memoryCost();
    require(retained<=options.byteBudget && plane<=(options.byteBudget-retained)/3,
        "Scalar filter exceeds the working-memory budget; reduce radius or source size");
    return retained+plane*3;
}
std::shared_ptr<const LayerSpatialFilterCache> prepareScalarFilter(
    const FilterInputSnapshot& snapshot,const FilterPreparationOptions& options)
{
    const auto& layer=snapshot.layer;
    const auto source=intrinsicSurface(layer);
    const auto working=scalarFilterBudget(layer,*source,options);
    const auto e=source->extent();
    const RectI bounds{0,0,int(e.width),int(e.height)};
    const auto sourceToLocal=intrinsicPixelsToLocal(layer);
    const auto density=1.0/sourceToLocal.m00;
    auto filter=layer.filters->items[std::size_t(SpatialFilterType::Gaussian)];
    const auto inputBounds=requiredSpatialInputBounds(filter,bounds,density);
    SpatialPlane plane{inputBounds,1,std::vector<float>(std::size_t(inputBounds.width)*std::size_t(inputBounds.height))};
    const auto compiled=compileAdjustmentStack(layer.adjustments);
    std::vector<std::byte> row(e.width);
    std::vector<float> adjusted(e.width);
    int lastY=-1;
    // Numeric images have a fixed grid. Extend valid edge values through the
    // finite Gaussian support rather than interpreting missing pixels as zero.
    for(int y=inputBounds.y;y<inputBounds.bottom();++y) {
        if((y-inputBounds.y)%32==0)checkCancellation(options);
        const int sy=std::clamp(y,0,int(e.height)-1);
        if(sy!=lastY) {
            source->copyNative({0,sy,int(e.width),1},row,row.size());
            for(std::uint32_t x=0;x<e.width;++x)
                adjusted[x]=evaluateScalarAdjustments(compiled,float(std::to_integer<std::uint8_t>(row[x]))/255,
                    sourceToLocal.map({double(x)+.5,double(sy)+.5}));
            lastY=sy;
        }
        auto* destination=plane.pixels.data()+std::size_t(y-inputBounds.y)*std::size_t(inputBounds.width);
        for(int x=inputBounds.x;x<inputBounds.right();++x)
            destination[x-inputBounds.x]=adjusted[std::size_t(std::clamp(x,0,int(e.width)-1))];
    }
    if(filter.mask)filter.mask->localToMask=composeAffine(filter.mask->localToMask,sourceToLocal);
    SpatialFilterOptions run;
    run.pixelScale=density;run.cancelled=options.cancelled;
    run.maxWorkingBytes=options.byteBudget-source->memoryCost()-plane.pixels.size()*sizeof(float);
    run.progress=[&](double p){if(options.progress)options.progress(.85*p);};
    auto filtered=filterSpatialRegion(plane.view(),bounds,filter,run);
    require(bool(filtered),filtered.error.empty()?"Scalar filter preparation failed":filtered.error.c_str());
    std::vector<float>().swap(plane.pixels);
    run.maxWorkingBytes=options.byteBudget-source->memoryCost()-filtered.output.pixels.size()*sizeof(float);
    run.progress=[&](double p){if(options.progress)options.progress(.85+.15*p);};
    auto bytes=quantizeSpatialPlane(filtered.output.view(),run);
    checkCancellation(options);
    auto result=std::make_shared<LayerSpatialFilterCache>();
    result->surface=std::make_shared<ScalarRasterSurface>(e,bytes,e.width);
    result->pixelsToLocal=result->sourcePixelsToLocal=sourceToLocal;
    result->sourceId=snapshot.sourceId;result->sourceRevision=snapshot.sourceRevision;
    result->adjustments=layer.adjustments;result->filters=layer.filters;result->workingBytes=working;
    if(options.progress)options.progress(1);
    return result;
}
}
bool scalarFiltersSupported(const SpatialFilterState& state) noexcept
{
    if(!state)return true;
    if(state->algorithmVersion!=spatialFilterAlgorithmVersion)return false;
    for(const auto& filter:state->items)if(filter.enabled)
        if(filter.type!=SpatialFilterType::Gaussian || filter.preserveAlpha
            || !std::holds_alternative<GaussianBlurParameters>(filter.parameters))return false;
    return true;
}
bool layerSpatialFilterCacheValid(const Layer& layer) noexcept { return validCache(layer); }
bool layerSpatialFilterCacheValid(const LayerSnapshot& layer) noexcept { return validCache(layer); }
bool filterInputMatches(const Layer& layer,const FilterInputSnapshot& input) noexcept
{
    const auto source=intrinsicSurface(layer);
    return layer.id==input.layer.id && source && source->id()==input.sourceId && source->revision()==input.sourceRevision
        && sameTransform(intrinsicPixelsToLocal(layer),intrinsicPixelsToLocal(input.layer))
        && equivalentAdjustments(layer.adjustments,input.layer.adjustments)
        && equivalentSpatialFilters(layer.filters,input.layer.filters);
}

FilterInputSnapshot snapshotFilterInput(const Layer& layer,const FilterPreparationOptions& options)
{
    checkCancellation(options);
    (void)preflightLayerSpatialFilters(layer,options);
    FilterInputSnapshot result{layer};
    const auto source=intrinsicSurface(layer);
    if (!hasActiveSpatialFilters(layer.filters)&&!hasActiveLayerEffects(layer.effects)) return result;
    require(bool(source), "Filter source image is not prepared");
    result.sourceId=source->id(); result.sourceRevision=source->revision();
    if(source->dataKind()==RasterDataKind::Scalar) {
        const auto* scalar=dynamic_cast<const ScalarRasterSurface*>(source.get());
        require(scalar!=nullptr,"Unsupported scalar filter storage");
        result.layer.payload=RasterLayer{scalar->clone()};
        result.copiedBytes=source->memoryCost(); // Frozen tiles are shared, but remain pinned during work.
        return result;
    }
    if (std::holds_alternative<RasterLayer>(layer.payload)) {
        const auto e=source->extent(); const auto stride=std::size_t(e.width)*4;
        result.copiedBytes=stride*e.height;
        require(result.copiedBytes<=options.byteBudget, "Filter source exceeds working-memory budget");
        std::vector<std::byte> pixels(result.copiedBytes);
        constexpr std::uint32_t rows=64;
        for(std::uint32_t y=0;y<e.height;y+=rows) {
            checkCancellation(options);
            const auto h=std::min(rows,e.height-y);
            source->copyRgba8({0,int(y),int(e.width),int(h)},
                std::span(pixels).subspan(std::size_t(y)*stride,std::size_t(h)*stride),stride);
        }
        require(source->revision()==result.sourceRevision, "Filter source changed while preparing input");
        result.layer.payload=RasterLayer{std::make_shared<ContiguousRasterSurface>(e,std::move(pixels))};
    }
    return result;
}

std::size_t preflightLayerSpatialFilters(const Layer& layer,const FilterPreparationOptions& options)
{
    if(!hasActiveSpatialFilters(layer.filters))return 0;
    require(validSpatialFilters(*layer.filters),"Invalid spatial filter stack");
    const auto source=intrinsicSurface(layer);
    require(bool(source),"Filter source image is not prepared");
    if(source->dataKind()==RasterDataKind::Scalar)return scalarFilterBudget(layer,*source,options);
    const auto e=source->extent();const auto mapping=intrinsicPixelsToLocal(layer);
    require(mapping.m01==0&&mapping.m10==0&&mapping.m00>0&&mapping.m00==mapping.m11,
        "Unsupported filter source density mapping");
    const auto density=1.0/mapping.m00;
    require(density>0&&density<=8,"Filter source density exceeds supported 8x cache limit");
    const auto bounds=expandedSpatialOutputBounds(layer.filters,{0,0,int(e.width),int(e.height)},density);
    const auto copied=std::holds_alternative<RasterLayer>(layer.payload)?std::size_t(e.width)*e.height*4:0;
    const auto largest=planeBytes(bounds);
    require(copied<=options.byteBudget&&largest<=(options.byteBudget-copied)/3,
        "Spatial filters exceed the working-memory budget; reduce filter radius or source size");
    return copied+largest*3;
}

std::shared_ptr<const LayerSpatialFilterCache> prepareLayerSpatialFilters(
    const FilterInputSnapshot& snapshot,const FilterPreparationOptions& options)
{
    checkCancellation(options);
    const auto& layer=snapshot.layer;
    if(!hasActiveSpatialFilters(layer.filters))return {};
    require(validSpatialFilters(*layer.filters),"Invalid spatial filter stack");
    const auto source=intrinsicSurface(layer);
    require(bool(source),"Filter source image is not prepared");
    if(source->dataKind()==RasterDataKind::Scalar)return prepareScalarFilter(snapshot,options);
    const auto sourceRevision=source->revision();
    const auto e=source->extent();
    const auto sourceToLocal=intrinsicPixelsToLocal(layer);
    // Typed caches use an isotropic source-density mapping; transforms remain
    // outside the stack. No filter parameter is rewritten for viewport zoom.
    require(sourceToLocal.m01==0 && sourceToLocal.m10==0 && sourceToLocal.m00>0
        && sourceToLocal.m00==sourceToLocal.m11,"Unsupported filter source density mapping");
    const auto density=1.0/sourceToLocal.m00;
    require(density>0 && density<=8,"Filter source density exceeds supported 8x cache limit");
    RectI bounds{0,0,int(e.width),int(e.height)};
    const auto finalBounds=expandedSpatialOutputBounds(layer.filters,bounds,density);
    const auto largestPlane=planeBytes(finalBounds);
    // Source + current + next + convolution scratch + final encoding. The
    // low-level kernel performs its own tighter allocation/operation preflight.
    require(snapshot.copiedBytes<=options.byteBudget
        && largestPlane<= (options.byteBudget-snapshot.copiedBytes)/3,
        "Spatial filters exceed the working-memory budget; reduce filter radius or source size");
    SpatialPlane plane{bounds,4,std::vector<float>(std::size_t(e.width)*e.height*4)};
    const auto compiled=compileAdjustmentStack(layer.adjustments);
    const auto stride=std::size_t(e.width)*4;
    std::vector<std::byte> row(stride);
    for(std::uint32_t y=0;y<e.height;++y) {
        if(y%32==0)checkCancellation(options);
        source->copyRgba8({0,int(y),int(e.width),1},row,stride);
        for(std::uint32_t x=0;x<e.width;++x) {
            const auto p=std::size_t(x)*4;
            const float a=float(std::to_integer<unsigned>(row[p+3]))/255.0F;
            PremultipliedColor c{float(srgbToLinear(std::to_integer<std::uint8_t>(row[p])))*a,
                float(srgbToLinear(std::to_integer<std::uint8_t>(row[p+1])))*a,
                float(srgbToLinear(std::to_integer<std::uint8_t>(row[p+2])))*a,a};
            if(compiled.active)c=evaluateAdjustments(compiled,c,sourceToLocal.map({double(x)+.5,double(y)+.5}));
            const auto i=(std::size_t(y)*e.width+x)*4;
            std::copy(c.begin(),c.end(),plane.pixels.begin()+std::ptrdiff_t(i));
        }
    }
    require(source->revision()==sourceRevision,"Filter source changed during preparation");
    std::size_t index=0,peak=largestPlane*3+snapshot.copiedBytes;
    for(const auto& original:layer.filters->items) {
        checkCancellation(options);
        ++index;
        if(spatialFilterIsNeutral(original))continue;
        auto filter=original;
        if(filter.mask)filter.mask->localToMask=composeAffine(filter.mask->localToMask,sourceToLocal);
        SpatialFilterOptions run;
        run.pixelScale=density;
        run.maxWorkingBytes=options.byteBudget-snapshot.copiedBytes-plane.pixels.size()*sizeof(float);
        run.cancelled=options.cancelled;
        run.progress=[&](double p){if(options.progress)options.progress(.85*(double(index-1)+p)/spatialFilterCount);};
        auto output=filterSpatialRegion(plane.view(),expandedSpatialOutputBounds(filter,plane.bounds,density),filter,run);
        require(bool(output),output.error.empty()?"Filter preparation failed":output.error.c_str());
        plane=std::move(output.output);
    }
    checkCancellation(options);
    SpatialFilterOptions encoding;
    encoding.maxWorkingBytes=options.byteBudget-snapshot.copiedBytes-plane.pixels.size()*sizeof(float);
    encoding.cancelled=options.cancelled;
    encoding.progress=[&](double p){if(options.progress)options.progress(.85+.15*p);};
    auto bytes=quantizeSpatialPlane(plane.view(),encoding);
    checkCancellation(options);
    auto result=std::make_shared<LayerSpatialFilterCache>();
    result->surface=std::make_shared<ContiguousRasterSurface>(
        Extent2u{std::uint32_t(plane.bounds.width),std::uint32_t(plane.bounds.height)},std::move(bytes));
    AffineTransform offset; offset.m02=plane.bounds.x;offset.m12=plane.bounds.y;
    result->pixelsToLocal=composeAffine(sourceToLocal,offset);
    result->sourcePixelsToLocal=sourceToLocal;
    result->sourceId=snapshot.sourceId;result->sourceRevision=snapshot.sourceRevision;
    result->adjustments=layer.adjustments;result->filters=layer.filters;result->workingBytes=peak;
    if(options.progress)options.progress(1);
    return result;
}
std::shared_ptr<const LayerSpatialFilterCache> prepareLayerSpatialFilters(const Layer& layer,const FilterPreparationOptions& options)
{
    if(layerSpatialFilterCacheValid(layer))return layer.filterCache;
    return prepareLayerSpatialFilters(snapshotFilterInput(layer,options),options);
}
bool publishLayerSpatialFilters(Document& document,LayerId id,std::shared_ptr<const LayerSpatialFilterCache> cache)
{
    auto* layer=document.layer(id);
    if(!layer || !cache)return false;
    auto candidate=*layer;candidate.filterCache=cache;
    if(!layerSpatialFilterCacheValid(candidate))return false;
    std::uint64_t bytes=cache->surface->allocatedPixelBytes();
    for(const auto& other:document.layers()) {
        if(other.id==id || !layerSpatialFilterCacheValid(other))continue;
        bytes+=other.filterCache->surface->allocatedPixelBytes();
    }
    require(bytes<=768ULL*1024*1024,"Document spatial-filter caches exceed the 768 MiB cache budget");
    for(const auto& other:document.layers())
        if(other.filterCache && !layerSpatialFilterCacheValid(other))document.layer(other.id)->filterCache.reset();
    layer->filterCache=std::move(cache);
    return true;
}
PreparedLayerSpatialEffects prepareLayerSpatialEffects(const FilterInputSnapshot& input,const FilterPreparationOptions& options)
{
    auto layer=input.layer;
    const auto frozen=intrinsicSurface(layer);
    require(bool(frozen),"Missing frozen effect source");
    // Immutable caches refer to the real source. Re-key only the metadata for
    // evaluating this frozen copy, then restore publication keys afterward.
    if(layer.filterCache&&layer.filterCache->sourceId==input.sourceId&&layer.filterCache->sourceRevision==input.sourceRevision) {
        auto cache=std::make_shared<LayerSpatialFilterCache>(*layer.filterCache);
        cache->sourceId=frozen->id();cache->sourceRevision=frozen->revision();layer.filterCache=std::move(cache);
    }
    if(layer.effectCache&&layer.effectCache->sourceId==input.sourceId&&layer.effectCache->sourceRevision==input.sourceRevision) {
        auto cache=std::make_shared<LayerEffectCache>(*layer.effectCache);
        cache->sourceId=frozen->id();cache->sourceRevision=frozen->revision();layer.effectCache=std::move(cache);
    }
    // This input is already frozen. Do not call the live-layer overload here:
    // that would copy the entire immutable raster again on each filter edit.
    if(hasActiveSpatialFilters(layer.filters)&&!layerSpatialFilterCacheValid(layer))
        layer.filterCache=prepareLayerSpatialFilters(FilterInputSnapshot{layer,frozen->id(),frozen->revision(),0},options);
    if(hasActiveLayerEffects(layer.effects)&&!layerEffectCacheValid(layer))
        layer.effectCache=prepareLayerEffects(layer,options);
    PreparedLayerSpatialEffects result{layer.filterCache,layer.effectCache};
    if(result.filters) {
        auto cache=std::make_shared<LayerSpatialFilterCache>(*result.filters);
        cache->sourceId=input.sourceId;cache->sourceRevision=input.sourceRevision;result.filters=std::move(cache);
    }
    if(result.effects&&result.effects->sourceId==frozen->id()) {
        auto cache=std::make_shared<LayerEffectCache>(*result.effects);
        cache->sourceId=input.sourceId;cache->sourceRevision=input.sourceRevision;result.effects=std::move(cache);
    }
    return result;
}

Layer prepareSpatialFilterLayer(const Layer& layer,const FilterPreparationOptions& options)
{
    auto result=layer;
    if(hasActiveSpatialFilters(layer.filters)&&!layerSpatialFilterCacheValid(layer))
        result.filterCache=prepareLayerSpatialFilters(layer,options);
    if(hasActiveLayerEffects(result.effects)&&!layerEffectCacheValid(result))result.effectCache=prepareLayerEffects(result,options);
    return result;
}
LayerSnapshot prepareSpatialFilterLayer(const LayerSnapshot& source,const FilterPreparationOptions& options)
{
    if((!hasActiveSpatialFilters(source.filters)||layerSpatialFilterCacheValid(source))&&layerEffectCacheValid(source))return source;
    Layer layer;
    layer.id=source.id;layer.adjustments=source.adjustments;layer.filters=source.filters;
    layer.filterCache=source.filterCache;layer.effects=source.effects;layer.effectCache=source.effectCache;
    layer.mask=source.mask;
    layer.localToDocument=source.localToDocument;layer.renderCache=source.renderCache;
    layer.rasterOrigin=source.rasterOrigin;layer.rasterEffectFrame=source.rasterEffectFrame;
    layer.payload=std::visit([](const auto& p)->LayerPayload {
        if constexpr(std::is_same_v<std::decay_t<decltype(p)>,RasterLayerSnapshot>)
            return RasterLayer{std::const_pointer_cast<RasterSurface>(p.surface)};
        else return p;
    },source.payload);
    auto prepared=prepareSpatialFilterLayer(layer,options);auto result=source;
    result.filterCache=prepared.filterCache;result.effectCache=prepared.effectCache;return result;
}
std::shared_ptr<const RasterSurface> prepareScalarLayerPixels(const Layer& layer,const FilterPreparationOptions& options)
{
    checkCancellation(options);
    const auto source=intrinsicSurface(layer);
    const auto* scalar=source?dynamic_cast<const ScalarRasterSurface*>(source.get()):nullptr;
    require(scalar!=nullptr,"Scalar output requires a numeric R8 layer");
    require((!layer.adjustments||validAdjustments(*layer.adjustments))&&scalarAdjustmentsSupported(layer.adjustments),
        "Unsupported scalar adjustment stack");
    require((!layer.filters||validSpatialFilters(*layer.filters))&&scalarFiltersSupported(layer.filters),
        "Unsupported scalar filter stack");
    if(hasActiveSpatialFilters(layer.filters))return prepareLayerSpatialFilters(layer,options)->surface;
    const auto frozen=scalar->clone();
    const auto compiled=compileAdjustmentStack(layer.adjustments);
    if(!compiled.active)return frozen;
    const auto e=frozen->extent();
    const auto count=std::size_t(e.width)*e.height;
    require(frozen->memoryCost()<=options.byteBudget&&count<=(options.byteBudget-frozen->memoryCost())/2,
        "Scalar output exceeds the working-memory budget");
    std::vector<std::byte> bytes(count);
    const auto sourceToLocal=intrinsicPixelsToLocal(layer);
    for(std::uint32_t y=0;y<e.height;++y) {
        if(y%32==0)checkCancellation(options);
        auto row=std::span(bytes).subspan(std::size_t(y)*e.width,e.width);
        frozen->copyNative({0,int(y),int(e.width),1},row,row.size());
        for(std::uint32_t x=0;x<e.width;++x) {
            const auto value=evaluateScalarAdjustments(compiled,float(std::to_integer<std::uint8_t>(row[x]))/255,
                sourceToLocal.map({double(x)+.5,double(y)+.5}));
            row[x]=std::byte(alphaToByte(value));
        }
        if(options.progress&&y%32==0)options.progress(double(y)/e.height);
    }
    checkCancellation(options);
    auto result=std::make_shared<ScalarRasterSurface>(e,bytes,e.width);
    if(options.progress)options.progress(1);
    return result;
}
} // namespace imageeditor::core
