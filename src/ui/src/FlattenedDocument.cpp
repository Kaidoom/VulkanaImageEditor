#include "imageeditor/ui/FlattenedDocument.hpp"

#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace imageeditor::ui {
namespace {
    struct Cancelled { };
    struct ProfileStage {
        using Clock = std::chrono::steady_clock;
        double* result;
        Clock::time_point start;
        explicit ProfileStage(double* value) : result(value), start(value ? Clock::now() : Clock::time_point{}) {}
        void finish() {
            if (result) *result += std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            result=nullptr;
        }
        ~ProfileStage() { finish(); }
    };
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
} // namespace

std::shared_ptr<const core::LayerRenderCache> prepareDocumentSampleCache(const core::Layer& layer,
    std::size_t availablePixels)
{
    // A coherent filtered display cache already owns this immutable typed
    // input. References must not regenerate an identical source merely because
    // the selection/clone tool asks for a fresh sampler.
    const bool filtered=core::hasActiveSpatialFilters(layer.filters)||core::hasActiveLayerEffects(layer.effects);
    if(filtered && layer.renderCache
        && layer.renderCache->contentRevision==(std::holds_alternative<core::ShapeLayer>(layer.payload)?layer.shapeRevision:layer.textRevision)
        && !layer.renderCache->rasterizedDocumentTransform && layer.renderCache->density==1)
        return layer.renderCache;
    if (const auto* shape = std::get_if<core::ShapeLayer>(&layer.payload))
        return filtered?QtShapeRenderService{}.render({*shape,1,availablePixels}):
            QtShapeRenderService{}.renderDocument(*shape,layer.localToDocument,availablePixels);
    if (const auto* text = std::get_if<core::TextLayer>(&layer.payload))
        return filtered?QtTextLayout(*text).rasterize(1,availablePixels):
            QtTextLayout(*text).rasterizeDocument(layer.localToDocument,availablePixels);
    return {};
}

static FlattenedDocumentResult flattenImpl(
    const core::Document& document, FlattenedDocumentProgress progress, FlattenedDocumentLimits limits,
    std::optional<std::span<const core::LayerId>> items, core::Extent2u requested = {},
    std::optional<core::Rgba8> matte = {}, MergeProfile* profile = nullptr, bool intrinsic = false)
{
    if (profile) *profile={};
    ProfileStage setup(profile?&profile->setupMs:nullptr);
    try {
        const FlattenedDocumentLimits hard;
        limits.outputPixels = std::min(limits.outputPixels, hard.outputPixels);
        limits.derivedCacheBytes = std::min(limits.derivedCacheBytes, hard.derivedCacheBytes);
        limits.metadataBytes = std::min(limits.metadataBytes, hard.metadataBytes);
        const auto canvasExtent = document.canvas().extent;
        auto extent = requested.empty() ? canvasExtent : requested;
        auto pixels = std::uint64_t(extent.width) * extent.height;
        if (!items)
            require(!extent.empty() && extent.width <= 32768 && extent.height <= 32768
                    && pixels <= limits.outputPixels,
                "Flattened output exceeds the pixel/dimension limit");
        require(document.layers().size() <= 1024, "Flattened output exceeds the layer limit");
        const core::AffineTransform outputGrid{std::max(1.0,double(extent.width)/canvasExtent.width),0,0,
            0,std::max(1.0,double(extent.height)/canvasExtent.height),0};
        std::vector<core::LayerId> selected;
        if (items) {
            for (auto id : *items)
                require(document.containsItem(id), "Merge target disappeared");
            selected = document.expandedLayers(*items);
            require(!selected.empty(), "There is no content to merge");
        }
        const auto included = [&](core::LayerId id) { return !items || std::ranges::find(selected, id) != selected.end(); };

        // Bound authoritative copies before constructing them. Old render
        // caches and raster surfaces remain shared, never cloned for output.
        std::uint64_t metadataBytes = 0;
        std::vector<std::pair<std::shared_ptr<const core::RasterSurface>, core::Revision>> rasterRevisions;
        for (const auto& layer : document.layers()) {
            if (!included(layer.id))
                continue;
            require(std::isfinite(layer.opacity) && layer.opacity>=0 && layer.opacity<=1
                && core::isValidBlendMode(layer.blendMode),"Invalid layer opacity or blend mode in native output");
            metadataBytes += sizeof(core::Layer) + layer.name.capacity();
            // The immutable stack and coverage planes stay shared, like the
            // original raster surface. Only the compiled evaluation record is
            // allocated for this output job; charging shared masks as copied
            // metadata would reject ordinary 4K/5K selection-scoped exports.
            if (layer.adjustments) metadataBytes += sizeof(core::CompiledAdjustmentStack);
            if (const auto* text = std::get_if<core::TextLayer>(&layer.payload))
                metadataBytes += core::textMemoryCost(*text);
            if (const auto* shape = std::get_if<core::ShapeLayer>(&layer.payload))
                metadataBytes += core::shapeMemoryCost(*shape);
            require(metadataBytes <= limits.metadataBytes, "Flattened output exceeds the metadata budget");
            if (const auto* raster = std::get_if<core::RasterLayer>(&layer.payload)) {
                require(bool(raster->surface), "Raster layer has no pixels for flattened output");
                rasterRevisions.emplace_back(raster->surface, raster->surface->revision());
            }
        }
        const auto startingRevision = document.revision();
        std::vector<core::Layer> layers;
        for (const auto& layer : document.layers())
            if (included(layer.id)) {
                layers.push_back(layer);
                layers.back().visible = document.isEffectivelyVisible(layer.id);
                if (intrinsic) {
                    // Rasterize bakes the internal source, not external layer
                    // composition. Hidden/zero-opacity layers remain editable.
                    layers.back().visible=true;
                    layers.back().opacity=1;
                    layers.back().blendMode=core::BlendMode::Normal;
                }
            }
        auto total = pixels + layers.size();
        auto tick = [&](std::uint64_t done) {
            if (progress && !progress(done, total))
                throw Cancelled { };
            require(document.revision() == startingRevision
                    && std::all_of(rasterRevisions.begin(), rasterRevisions.end(),
                        [](const auto& source) { return source.first->revision() == source.second; }),
                "Document changed while generating flattened output");
        };
        tick(0);
        setup.finish();

        std::uint64_t cacheBytes = 0, completed = 0;
        const auto prepareLayer = [&](core::Layer& layer) {
            layer.renderCache.reset();
            // Raster filter caches are canonical source-resolution results,
            // keyed to the original surface and filter revisions. Typed filter
            // inputs can depend on a display cache and must be rebuilt here.
            if (!std::holds_alternative<core::RasterLayer>(layer.payload)
                || !core::layerSpatialFilterCacheValid(layer)) layer.filterCache.reset();
            if (layer.visible && std::isfinite(layer.opacity) && layer.opacity > 0) {
                require(layer.localToDocument.inverted().has_value(),
                    "Invalid layer transform in flattened output");
                const auto availablePixels = std::size_t((limits.derivedCacheBytes - cacheBytes) / 4);
                const bool filtered=core::hasActiveSpatialFilters(layer.filters)||core::hasActiveLayerEffects(layer.effects);
                // Unfiltered model geometry is painted ON the final integer
                // document/output grid. No local bitmap then affine resampling.
                // Spatial kernels are defined in local source pixels: prepare
                // that canonical 1x source before filtering, then transform once.
                const auto toGrid=core::composeAffine(items?core::AffineTransform{}:outputGrid,layer.localToDocument);
                std::optional<core::RectI> clip=items?std::nullopt:
                    std::optional(core::RectI{0,0,int(std::max(extent.width,canvasExtent.width)),
                        int(std::max(extent.height,canvasExtent.height))});
                if(layer.crop && !filtered) {
                    const auto& c=*layer.crop;
                    double left=std::numeric_limits<double>::max(),top=left,right=-left,bottom=-left;
                    for(auto p:std::array{toGrid.map({c.x,c.y}),toGrid.map({c.right(),c.y}),
                        toGrid.map({c.right(),c.bottom()}),toGrid.map({c.x,c.bottom()})}) {
                        left=std::min(left,p.x);top=std::min(top,p.y);right=std::max(right,p.x);bottom=std::max(bottom,p.y);
                    }
                    left=std::floor(left);top=std::floor(top);right=std::ceil(right);bottom=std::ceil(bottom);
                    require(std::isfinite(left)&&std::isfinite(top)&&std::isfinite(right)&&std::isfinite(bottom)
                        && std::abs(left)<=1e9 && std::abs(top)<=1e9 && right-left<=1e9 && bottom-top<=1e9,
                        "Crop output bounds exceed the coordinate limit");
                    const core::RectI crop{int(left),int(top),int(right-left),int(bottom-top)};
                    clip=clip?clip->clippedTo(crop):crop;
                }
                ProfileStage typed(profile?&profile->typedRasterizationMs:nullptr);
                if (const auto* shape=std::get_if<core::ShapeLayer>(&layer.payload))
                    layer.renderCache=filtered?QtShapeRenderService{}.render({*shape,1,availablePixels}):
                        QtShapeRenderService{}.renderDocument(*shape,toGrid,availablePixels,clip);
                if (const auto* text=std::get_if<core::TextLayer>(&layer.payload))
                    layer.renderCache=filtered?QtTextLayout(*text).rasterize(1,availablePixels):
                        QtTextLayout(*text).rasterizeDocument(toGrid,availablePixels,clip);
                typed.finish();
                if (layer.renderCache) {
                    const auto cacheExtent = layer.renderCache->surface->extent();
                    cacheBytes += std::uint64_t(cacheExtent.width) * cacheExtent.height * 4;
                    require(cacheBytes <= limits.derivedCacheBytes,
                        "Flattened output exceeds the derived-cache budget");
                }
                if(filtered) {
                    ProfileStage effects(profile?&profile->spatialEffectsMs:nullptr);
                    core::FilterPreparationOptions filterOptions;
                    filterOptions.cancelled=[&]{tick(completed);return false;};
                    filterOptions.progress=[&](double){tick(completed);};
                    layer.filterCache=core::prepareLayerSpatialFilters(layer,filterOptions);
                    layer.effectCache=core::prepareLayerEffects(layer,filterOptions);
                    if(layer.filterCache) {
                        const auto e=layer.filterCache->surface->extent();
                        cacheBytes+=std::uint64_t(e.width)*e.height*4;
                        require(cacheBytes<=limits.derivedCacheBytes,"Flattened spatial-filter output exceeds the derived-cache budget");
                    }
                }
            }
        };
        for(auto& layer:layers) {prepareLayer(layer);tick(++completed);}

        core::Vec2d origin { };
        ProfileStage bounds(profile?&profile->boundsMs:nullptr);
        if (items) {
            double left = std::numeric_limits<double>::max(), top = left, right = -left, bottom = -left;
            for (const auto& layer : layers) {
                if (!layer.visible || layer.opacity <= 0)
                    continue;
                const auto surface = core::renderedSurface(layer);
                require(bool(surface), "A merge source could not be prepared");
                // Bound the actual final source quad, not its local AABB
                // transformed twice (which inflates rotated document caches).
                const bool styled=core::hasActiveLayerEffects(layer.effects)&&core::layerEffectCacheValid(layer);
                const auto t=styled?layer.localToDocument:core::renderTransform(layer); const auto e=surface->extent();
                const auto visual=styled?layer.effectCache->visualBounds:core::RectD{0,0,double(e.width),double(e.height)};
                require(t.validOver(visual),"Projective horizon crosses the evaluated layer support");
                double l=std::numeric_limits<double>::max(),topEdge=l,r=-l,b=-l;
                for (auto p : std::array {t.map({visual.x,visual.y}),t.map({visual.right(),visual.y}),
                    t.map({visual.right(),visual.bottom()}),t.map({visual.x,visual.bottom()})}) {
                    require(std::isfinite(p.x) && std::isfinite(p.y), "Merge bounds are not finite");
                    l=std::min(l,p.x);topEdge=std::min(topEdge,p.y);r=std::max(r,p.x);b=std::max(b,p.y);
                }
                if(!t.isAffine()){l-=1;topEdge-=1;r+=1;b+=1;}
                if(layer.crop) {
                    if(layer.crop->empty())continue;
                    const auto& c=*layer.crop; const auto& transform=layer.localToDocument;
                    double cl=std::numeric_limits<double>::max(),ct=cl,cr=-cl,cb=-cl;
                    for(auto p:std::array{transform.map({c.x,c.y}),transform.map({c.right(),c.y}),
                        transform.map({c.right(),c.bottom()}),transform.map({c.x,c.bottom()})}) {
                        cl=std::min(cl,p.x);ct=std::min(ct,p.y);cr=std::max(cr,p.x);cb=std::max(cb,p.y);
                    }
                    // Intersect pixel regions, not continuous boxes: a source
                    // center and crop fringe can occupy the same output pixel
                    // even when their continuous bounds are just disjoint.
                    l=std::max(std::floor(l),std::floor(cl));topEdge=std::max(std::floor(topEdge),std::floor(ct));
                    r=std::min(std::ceil(r),std::ceil(cr));b=std::min(std::ceil(b),std::ceil(cb));
                    const auto pixelsToLocal=core::renderPixelsToLocal(layer);
                    if((std::holds_alternative<core::RasterLayer>(layer.payload)||core::layerSpatialFilterCacheValid(layer))
                        && transform.isAffine() && pixelsToLocal.isAffine() && pixelsToLocal.m01==0 && pixelsToLocal.m10==0) {
                        // Raster/filter sources are rectangular in local space.
                        // Intersect there before rotation, otherwise disjoint
                        // source/crop rectangles can have large overlapping
                        // document AABBs. The half-pixel inverse footprint keeps
                        // analytic crop AA at the source boundary intact.
                        const auto inverse=*transform.inverted();
                        const auto px=.5*(std::abs(inverse.m00)+std::abs(inverse.m01));
                        const auto py=.5*(std::abs(inverse.m10)+std::abs(inverse.m11));
                        const auto local=(styled?layer.effectCache->visualBounds:core::layerSourceBounds(layer)).clippedTo(
                            {c.x-px,c.y-py,c.width+2*px,c.height+2*py});
                        if(local.empty())continue;
                        double ll=std::numeric_limits<double>::max(),lt=ll,lr=-ll,lb=-ll;
                        for(auto p:std::array{transform.map({local.x,local.y}),transform.map({local.right(),local.y}),
                            transform.map({local.right(),local.bottom()}),transform.map({local.x,local.bottom()})}) {
                            ll=std::min(ll,p.x);lt=std::min(lt,p.y);lr=std::max(lr,p.x);lb=std::max(lb,p.y);
                        }
                        l=std::max(l,std::floor(ll));topEdge=std::max(topEdge,std::floor(lt));
                        r=std::min(r,std::ceil(lr));b=std::min(b,std::ceil(lb));
                    }
                }
                if(r<=l || b<=topEdge)continue;
                left=std::min(left,l);top=std::min(top,topEdge);right=std::max(right,r);bottom=std::max(bottom,b);
            }
            // Valid empty/cropped-away content still rasterizes explicitly to
            // a transparent native pixel; no hidden original is retained.
            if(intrinsic && (right<=left || bottom<=top)) {
                const auto p=layers.front().localToDocument.map({});left=p.x;top=p.y;
                right=std::floor(left)+1;bottom=std::floor(top)+1;
            }
            left = std::floor(left);
            top = std::floor(top);
            right = std::ceil(right);
            bottom = std::ceil(bottom);
            const auto width = right - left, height = bottom - top;
            require(width > 0 && height > 0 && width <= 32768 && height <= 32768 && width * height <= double(limits.outputPixels)
                    && std::abs(left) <= 1e9 && std::abs(top) <= 1e9,
                "Merge bounds exceed the pixel/dimension limit, or all selected content is hidden");
            origin = { left, top };
            extent = { std::uint32_t(width), std::uint32_t(height) };
            pixels = std::uint64_t(extent.width) * extent.height;
            total = pixels + layers.size();
        }
        bounds.finish();
        if(profile) {profile->outputBytes=pixels*4;profile->derivedBytes=cacheBytes;}
        ProfileStage samplerSetup(profile?&profile->setupMs:nullptr);
        std::vector<const core::Layer*> prepared;
        prepared.reserve(layers.size());
        for (auto& layer : layers)
            prepared.push_back(&layer);

        // Setup/inverses/source metadata happen once, never per output pixel.
        // Sampling the original document coordinates also preserves exact edge
        // decisions: translating a matrix before inversion can move a source
        // boundary a few ulps and drop a fully visible pixel.
        const core::PinnedDocumentSampler sampler(prepared, items ? extent : canvasExtent, {}, origin);
        samplerSetup.finish();
        ProfileStage allocation(profile?&profile->allocationMs:nullptr);
        QImage output(int(extent.width), int(extent.height), QImage::Format_RGBA8888);
        if (output.isNull())
            throw std::bad_alloc();
        const auto dotsPerMeter = std::lround(
            std::clamp(document.canvas().dotsPerInch / .0254, 1.0, double(std::numeric_limits<int>::max())));
        output.setDotsPerMeterX(int(dotsPerMeter));
        output.setDotsPerMeterY(int(dotsPerMeter));
        allocation.finish();
        // A single plain translated source can be transferred byte-for-byte,
        // including hidden RGB. This also avoids decode/encode round trips.
        const core::Layer* single=nullptr;
        for (const auto* layer:prepared) {
            if (!layer->visible || layer->opacity<=0)continue;
            if(single) {single=nullptr;break;} single=layer;
        }
        if(single && items && !matte && single->opacity==1
            && single->blendMode==core::BlendMode::Normal && !single->crop
            && !core::compileAdjustmentStack(single->adjustments).active
            && !core::hasActiveSpatialFilters(single->filters)&&!core::hasActiveLayerEffects(single->effects)) {
            const auto* raster=std::get_if<core::RasterLayer>(&single->payload);
            const auto t=core::intrinsicTransform(*single);
            if(raster && t.isAffine() && t.m00==1 && t.m11==1 && t.m01==0 && t.m10==0
                && t.m02==std::floor(t.m02) && t.m12==std::floor(t.m12)) {
                output.fill(Qt::transparent);
                ProfileStage copy(profile?&profile->samplingBlendMs:nullptr);
                const auto e=raster->surface->extent();
                const auto offsetX=t.m02-origin.x,offsetY=t.m12-origin.y;
                const auto left=std::max(0.0,offsetX),top=std::max(0.0,offsetY);
                const auto right=std::min(double(extent.width),offsetX+e.width);
                const auto bottom=std::min(double(extent.height),offsetY+e.height);
                if(right>left && bottom>top) for(int y=int(top);y<int(bottom);++y) {
                    auto* dest=reinterpret_cast<std::byte*>(output.scanLine(y))+std::size_t(left)*4;
                    const auto stride=std::size_t(right-left)*4;
                    raster->surface->copyRgba8({int(left-offsetX),int(y-offsetY),int(right-left),1},
                        std::span(dest,stride),stride);
                    if(y%32==0)tick(completed+std::uint64_t(y)*extent.width);
                }
                tick(total); return {std::move(output),{},false,origin};
            }
        }
        constexpr std::uint64_t callbackPixels = 4096;
        std::uint64_t processed = 0;
        const double stepX = items ? 1 : double(canvasExtent.width)/extent.width;
        const double stepY = items ? 1 : double(canvasExtent.height)/extent.height;
        ProfileStage scratchAllocation(profile?&profile->allocationMs:nullptr);
        std::vector<core::PremultipliedColor> rowColors(stepX==1 && stepY==1?extent.width:0);
        std::vector<std::byte> rowScratch(rowColors.size()*4);
        scratchAllocation.finish();
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            auto* row = output.scanLine(int(y));
            ProfileStage sampling(profile?&profile->samplingBlendMs:nullptr);
            if(!rowColors.empty())sampler.sampleRow(0,int(y),rowColors,rowScratch);
            sampling.finish();
            ProfileStage encoding(profile?&profile->encodingMs:nullptr);
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                core::PremultipliedColor linear;
                if (stepX == 1 && stepY == 1)
                    linear = rowColors[x];
                else {
                    // Exact box/area reduction, continuous alpha-aware source
                    // reconstruction on enlarged axes. Never quantize the
                    // composed stack before integrating the output footprint.
                    const double left=x*stepX, right=(x+1)*stepX;
                    const double top=y*stepY, bottom=(y+1)*stepY;
                    const int x0=stepX>1?int(std::floor(left)):0;
                    const int x1=stepX>1?int(std::ceil(right)):1;
                    const int y0=stepY>1?int(std::floor(top)):0;
                    const int y1=stepY>1?int(std::ceil(bottom)):1;
                    std::array<double,4> sum{};
                    for (int sy=y0; sy<y1; ++sy) for (int sx=x0; sx<x1; ++sx) {
                        const auto wx=stepX>1?(std::min(right,double(sx+1))-std::max(left,double(sx)))/stepX:1;
                        const auto wy=stepY>1?(std::min(bottom,double(sy+1))-std::max(top,double(sy)))/stepY:1;
                        const auto c=sampler.sampleLinear({stepX>1?sx+.5:(left+right)*.5,
                            stepY>1?sy+.5:(top+bottom)*.5});
                        for (std::size_t i=0;i<4;++i) sum[i]+=double(c[i])*wx*wy;
                        if (++processed % callbackPixels == 0) tick(completed + std::uint64_t(y)*extent.width+x);
                    }
                    linear={float(sum[0]),float(sum[1]),float(sum[2]),float(sum[3])};
                }
                if (matte) linear=core::compositeLayer(core::decodeColor(*matte),linear,1,core::BlendMode::Normal);
                auto color=core::encodeColor(linear);
                if(color.alpha==0) color={}; // Canonical transparent RGB, including rounded-to-zero alpha.
                const auto offset = std::size_t(x) * 4;
                row[offset] = color.red;
                row[offset + 1] = color.green;
                row[offset + 2] = color.blue;
                row[offset + 3] = color.alpha;
                if (++processed % callbackPixels == 0)
                    tick(completed + std::uint64_t(y)*extent.width+x+1);
            }
        }
        tick(total);
        return { std::move(output), { }, false, origin };
    } catch (const Cancelled&) {
        return { { }, { }, true, { } };
    } catch (const std::bad_alloc&) {
        return { { }, QStringLiteral("Not enough memory for flattened output"), false, { } };
    } catch (const std::exception& error) {
        return { { }, QString::fromUtf8(error.what()), false, { } };
    }
}

FlattenedDocumentResult flattenDocument(const core::Document& doc, FlattenedDocumentProgress progress, FlattenedDocumentLimits limits, MergeProfile* profile)
{
    return flattenImpl(doc, std::move(progress), limits, std::nullopt,{}, {},profile);
}
FlattenedDocumentResult flattenDocumentAtSize(const core::Document& doc, core::Extent2u output,
    FlattenedDocumentProgress progress, FlattenedDocumentLimits limits, std::optional<core::Rgba8> matte)
{
    if(output.empty()) return {{},QStringLiteral("Output dimensions must be positive"),false,{}};
    return flattenImpl(doc,std::move(progress),limits,std::nullopt,output,matte);
}
FlattenedDocumentResult flattenLayerItems(const core::Document& doc, std::span<const core::LayerId> ids, FlattenedDocumentProgress progress, FlattenedDocumentLimits limits, MergeProfile* profile)
{
    return flattenImpl(doc, std::move(progress), limits, ids, {}, {}, profile);
}

FlattenedDocumentResult rasterizeLayerContent(const core::Document& doc, core::LayerId id,
    FlattenedDocumentProgress progress, FlattenedDocumentLimits limits)
{
    if (!doc.layer(id)) return {{},QStringLiteral("Rasterize requires a renderable layer"),false,{}};
    const std::array ids{id};
    return flattenImpl(doc,std::move(progress),limits,ids,{}, {},nullptr,true);
}

} // namespace imageeditor::ui
