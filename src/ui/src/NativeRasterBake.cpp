#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/core/SpatialFilters.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace imageeditor::ui {
std::shared_ptr<core::RasterSurface> surfaceFromNativeImage(const QImage& image)
{
    if(image.isNull() || image.format()!=QImage::Format_RGBA8888)
        throw std::invalid_argument("Expected native straight RGBA8 pixels");
    const auto stride=std::size_t(image.width())*4;
    std::vector<std::byte> pixels(stride*std::size_t(image.height()));
    for(int y=0;y<image.height();++y)
        std::memcpy(pixels.data()+std::size_t(y)*stride,image.constScanLine(y),stride);
    return std::make_shared<core::ContiguousRasterSurface>(
        core::Extent2u{std::uint32_t(image.width()),std::uint32_t(image.height())},std::move(pixels));
}
bool needsRasterization(const core::Layer& layer)
{
    const auto& t=layer.localToDocument;
    return !std::holds_alternative<core::RasterLayer>(layer.payload) || layer.crop
        || !t.isAffine() || t.m00!=1 || t.m11!=1 || t.m01!=0 || t.m10!=0
        || t.m02!=std::floor(t.m02) || t.m12!=std::floor(t.m12)
        || core::compileAdjustmentStack(layer.adjustments).active || core::hasActiveSpatialFilters(layer.filters)
        || core::hasActiveLayerEffects(layer.effects);
}
namespace {
void removeBakedEffects(core::Layer& baked,const core::Layer& original)
{
    // Keep disabled, unbaked settings. Captured masks are re-based to the new
    // pixel-local frame; their coverage planes remain immutable/shared.
    const auto toOld=core::composeAffine(*original.localToDocument.inverted(),baked.localToDocument);
    if(original.adjustments) {
        auto stack=std::make_shared<core::AdjustmentStack>(*original.adjustments);
        for(auto& item:stack->items) {
            if(item.enabled)item=core::defaultAdjustment(item.type);
            else if(item.mask)item.mask->localToMask=core::composeAffine(item.mask->localToMask,toOld);
        }
        baked.adjustments=core::equivalentAdjustments(stack,{})?core::AdjustmentState{}:std::move(stack);
        ++baked.adjustmentRevision;
    }
    if(original.filters) {
        auto stack=std::make_shared<core::SpatialFilterStack>(*original.filters);
        for(auto& item:stack->items) {
            if(item.enabled)item=core::defaultSpatialFilter(item.type);
            else if(item.mask)item.mask->localToMask=core::composeAffine(item.mask->localToMask,toOld);
        }
        baked.filters=core::equivalentSpatialFilters(stack,{})?core::SpatialFilterState{}:std::move(stack);
        ++baked.filterRevision;
    }
    if(original.effects) {
        auto stack=std::make_shared<core::LayerEffectStack>(*original.effects);
        for(std::size_t i=0;i<core::layerEffectCount;++i)
            if(stack->items[i].enabled)stack->items[i]=core::defaultLayerEffect(core::LayerEffectType(i));
        baked.effects=core::equivalentLayerEffects(stack,{})?core::LayerEffectState{}:std::move(stack);
        ++baked.effectRevision;
    }
    baked.crop.reset();baked.renderCache.reset();baked.filterCache.reset();baked.effectCache.reset();
}
}
RasterizeResult prepareRasterizeLayers(const core::Document& doc,const core::LayerSelectionState& selection,
    std::size_t budget,FlattenedDocumentProgress progress)
{
    try {
        const auto ids=doc.expandedLayers(selection.ids);
        std::vector<core::LayerId> removed;
        std::vector<core::Layer> added;
        std::size_t cost=2*doc.tree().memoryCost()+65536;
        for(auto id:ids)if(needsRasterization(*doc.layer(id))) {
            removed.push_back(id);cost+=core::retainedLayerMemory(*doc.layer(id));
        }
        if(removed.empty())return {};
        if(cost>=budget)throw std::runtime_error("Rasterize exceeds the undo memory budget; no layers were changed");
        const auto revision=doc.revision();
        std::vector<std::pair<std::shared_ptr<const core::RasterSurface>,core::Revision>> sources;
        for(auto id:removed)if(const auto* raster=std::get_if<core::RasterLayer>(&doc.layer(id)->payload)) {
            if(!raster->surface)throw std::runtime_error("Rasterize target has no source pixels");
            sources.emplace_back(raster->surface,raster->surface->revision());
        }
        const auto requireCurrent=[&] {
            if(doc.revision()!=revision || !std::ranges::all_of(sources,[](const auto& s){return s.first->revision()==s.second;}))
                throw std::runtime_error("Rasterize targets changed; no layers were changed");
        };
        for(std::size_t i=0;i<removed.size();++i) {
            FlattenedDocumentLimits limits;
            limits.outputPixels=std::min<std::uint64_t>(limits.outputPixels,(budget-cost)/4);
            auto pixels=rasterizeLayerContent(doc,removed[i],[&](auto done,auto total){
                const auto keepGoing=!progress || progress(i*1000+done*1000/std::max<std::uint64_t>(total,1),removed.size()*1000);
                requireCurrent();return keepGoing;
            },limits);
            if(!pixels)return {{},pixels.error,pixels.cancelled};
            requireCurrent();
            const auto& original=*doc.layer(removed[i]);
            auto baked=original;
            baked.payload=core::RasterLayer{surfaceFromNativeImage(pixels.image)};
            baked.localToDocument={1,0,pixels.origin.x,0,1,pixels.origin.y};
            baked.rasterOrigin={};baked.rasterEffectFrame.reset();
            removeBakedEffects(baked,original);
            cost+=core::retainedLayerMemory(baked);
            if(cost>budget)throw std::runtime_error("Rasterize exceeds the undo memory budget; no layers were changed");
            added.push_back(std::move(baked));
        }
        auto command=std::make_unique<core::LayerStructureCommand>("Rasterize layers",doc,doc.tree(),
            std::move(removed),std::move(added),selection,selection);
        if(command->memoryCost()>budget)throw std::runtime_error("Rasterize exceeds the undo memory budget; no layers were changed");
        return {std::move(command),{},false};
    }catch(const std::exception& e){return {{},QString::fromUtf8(e.what()),false};}
}
}
