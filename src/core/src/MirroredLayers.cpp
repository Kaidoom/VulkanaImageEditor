#include "ReadOnlyRasterSurface.hpp"
#include "imageeditor/core/ProjectLayer.hpp"
#include "imageeditor/core/MirroredLayers.hpp"
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include <algorithm>
#include <stdexcept>

namespace imageeditor::core {
namespace {
std::shared_ptr<const RasterSurface> underlying(std::shared_ptr<const RasterSurface> surface)
{
    while (const auto wrapper=std::dynamic_pointer_cast<const ReadOnlyRasterSurface>(surface)) surface=wrapper->source();
    return surface;
}
const LayerEffectStack& effectStack(const LayerEffectState& state)
{
    static const LayerEffectStack defaults;
    return state ? *state : defaults;
}
bool contentMatches(const Layer& mirror,const Layer& source)
{
    if (mirror.payload.index()!=source.payload.index()) return false;
    if (const auto* raster=std::get_if<RasterLayer>(&source.payload)) {
        const auto wrapper=std::dynamic_pointer_cast<ReadOnlyRasterSurface>(std::get<RasterLayer>(mirror.payload).surface);
        return wrapper && wrapper->source()==underlying(raster->surface);
    }
    if (const auto* text=std::get_if<TextLayer>(&source.payload))
        return mirror.textRevision==source.textRevision && std::get<TextLayer>(mirror.payload)==*text;
    if (const auto* shape=std::get_if<ShapeLayer>(&source.payload))
        return mirror.shapeRevision==source.shapeRevision && std::get<ShapeLayer>(mirror.payload)==*shape;
    return false;
}
bool effectsMatch(const Layer& mirror,const Layer& source)
{
    const auto& link=*mirror.mirror;
    const auto& have=effectStack(mirror.effects), &base=effectStack(source.effects);
    if ((!link.pixelAlphaShapesEffectsOverride && have.pixelAlphaShapesEffects!=base.pixelAlphaShapesEffects)
        || (!link.maskEffectsAfterRenderingOverride && have.maskEffectsAfterRendering!=base.maskEffectsAfterRendering)) return false;
    for (std::size_t i=0;i<layerEffectCount;++i) {
        if (link.effects[i]==MirrorEffectMode::Override) continue;
        if (link.effects[i]==MirrorEffectMode::Disabled) {
            if (have.items[i].enabled) return false;
        } else if (have.items[i]!=base.items[i]) return false;
    }
    return true;
}
LayerEffectState inheritedEffects(const Layer& mirror,const Layer& source)
{
    const auto& link=*mirror.mirror;
    if (!link.pixelAlphaShapesEffectsOverride && !link.maskEffectsAfterRenderingOverride
        && std::ranges::all_of(link.effects,[](auto mode){return mode==MirrorEffectMode::Inherit;})) return source.effects;
    auto result=std::make_shared<LayerEffectStack>(effectStack(source.effects));
    const auto& own=effectStack(mirror.effects);
    if (link.pixelAlphaShapesEffectsOverride) result->pixelAlphaShapesEffects=own.pixelAlphaShapesEffects;
    if (link.maskEffectsAfterRenderingOverride) result->maskEffectsAfterRendering=own.maskEffectsAfterRendering;
    for (std::size_t i=0;i<layerEffectCount;++i) {
        if (link.effects[i]==MirrorEffectMode::Override) result->items[i]=own.items[i];
        else if (link.effects[i]==MirrorEffectMode::Disabled) result->items[i].enabled=false;
    }
    return result;
}
}

bool canMirrorLayer(const Layer& layer) noexcept
{
    if (layer.projectSource || std::holds_alternative<AdjustmentLayer>(layer.payload)) return false;
    const auto* raster=std::get_if<RasterLayer>(&layer.payload);
    return std::holds_alternative<TextLayer>(layer.payload) || std::holds_alternative<ShapeLayer>(layer.payload)
        || (raster && raster->surface && raster->surface->dataKind()==RasterDataKind::Color);
}
bool validMirrorStructure(std::span<const Layer> layers) noexcept
{
    for (const auto& layer : layers) if (layer.mirror) {
        if (!canMirrorLayer(layer)) return false;
        const auto* current=&layer;
        for (std::size_t depth=0;current->mirror;++depth) {
            if (depth>=layers.size() || !current->mirror->source) return false;
            for (const auto mode:current->mirror->effects)
                if (mode!=MirrorEffectMode::Inherit && mode!=MirrorEffectMode::Override && mode!=MirrorEffectMode::Disabled) return false;
            const auto source=std::ranges::find(layers,current->mirror->source,&Layer::id);
            if (source==layers.end() || source->id==layer.id || !canMirrorLayer(*source)) return false;
            current=&*source;
        }
    }
    return true;
}
bool mirrorNeedsResolution(const Layer& mirror,const Layer& source)
{
    if (!mirror.mirror) return false;
    const auto& link=*mirror.mirror;
    if (mirror.payload.index()!=source.payload.index() || mirror.rasterOrigin!=source.rasterOrigin
        || mirror.rasterEffectFrame!=source.rasterEffectFrame || mirror.crop!=source.crop || mirror.mask!=source.mask
        || mirror.adjustments!=source.adjustments || mirror.filters!=source.filters
        || (!link.opacityOverride && mirror.opacity!=source.opacity)
        || (!link.fillOpacityOverride && mirror.fillOpacity!=source.fillOpacity)
        || (!link.blendModeOverride && mirror.blendMode!=source.blendMode) || !effectsMatch(mirror,source)) return true;
    return !contentMatches(mirror,source);
}
Layer resolvedMirroredLayer(const Layer& mirror,const Layer& source)
{
    if (!mirror.mirror || !canMirrorLayer(source)) throw std::invalid_argument("Invalid mirrored-layer source");
    Layer result=mirror;
    const bool contentChanged=!contentMatches(mirror,source) || result.rasterOrigin!=source.rasterOrigin;
    if (const auto* raster=std::get_if<RasterLayer>(&source.payload)) {
        const auto base=underlying(raster->surface);
        const auto* current=std::get_if<RasterLayer>(&result.payload);
        const auto wrapper=current?std::dynamic_pointer_cast<ReadOnlyRasterSurface>(current->surface):nullptr;
        if (!wrapper || wrapper->source()!=base) result.payload=RasterLayer{std::make_shared<ReadOnlyRasterSurface>(base)};
    } else result.payload=source.payload;
    result.textRevision=source.textRevision; result.shapeRevision=source.shapeRevision;
    result.rasterOrigin=source.rasterOrigin; result.rasterEffectFrame=source.rasterEffectFrame;
    result.crop=source.crop; result.mask=source.mask;
    if (result.adjustments!=source.adjustments) {result.adjustments=source.adjustments;++result.adjustmentRevision;}
    if (result.filters!=source.filters) {result.filters=source.filters;++result.filterRevision;result.filterCache.reset();}
    if (!mirror.mirror->opacityOverride) result.opacity=source.opacity;
    if (!mirror.mirror->fillOpacityOverride) result.fillOpacity=source.fillOpacity;
    if (!mirror.mirror->blendModeOverride) result.blendMode=source.blendMode;
    const auto effects=inheritedEffects(mirror,source);
    if (!equivalentLayerEffects(result.effects,effects)) {result.effects=effects;++result.effectRevision;}
    if (contentChanged) {result.renderCache.reset();result.filterCache.reset();}
    // The ordinary cache keys already cover source pixels, mask geometry and
    // effect parameters. Opacity/blend changes need no new geometry, and an
    // unchanged source cache can also be shared by independently moved mirrors.
    if (!layerSpatialFilterCacheValid(result) && source.filterCache) {
        auto candidate=result; candidate.filterCache=source.filterCache;
        if (layerSpatialFilterCacheValid(candidate)) result.filterCache=source.filterCache;
    }
    if (!layerEffectCacheValid(result) && source.effectCache) {
        auto candidate=result; candidate.effectCache=source.effectCache;
        if (layerEffectCacheValid(candidate)) result.effectCache=source.effectCache;
    }
    return result;
}
Layer promotedMirroredLayer(const Layer& layer)
{
    Layer result=layer;
    if (auto* raster = std::get_if<RasterLayer>(&result.payload))
        raster->surface = copyColorRaster(*raster->surface);
    result.mirror.reset(); result.filterCache.reset();result.effectCache.reset();
    return result;
}
std::unique_ptr<LayerStructureCommand> createMirroredLayer(const Document& document,const LayerSelectionState& selection)
{
    if (selection.ids.size()!=1) throw std::invalid_argument("Select one raster, text, or shape layer to mirror.");
    const auto* source=document.layer(selection.ids.front());
    if (!source || !canMirrorLayer(*source)) throw std::invalid_argument("This layer cannot be mirrored.");
    Layer mirror=*source;
    mirror.id=makeLayerId();mirror.name=source->name+" mirror";
    mirror.mirror=LayerMirror{.source=source->id};
    mirror=resolvedMirroredLayer(mirror,*source);
    auto tree=document.tree();const auto placement=tree.insertionAbove(source->id);
    auto* siblings=tree.children(placement.parent);
    if (!siblings) throw std::invalid_argument("Source layer position changed.");
    siblings->insert(siblings->begin()+std::ptrdiff_t(placement.index),mirror.id);
    const auto id=mirror.id;
    return std::make_unique<LayerStructureCommand>("Create mirrored layer",document,std::move(tree),std::vector<LayerId>{},
        std::vector<Layer>{std::move(mirror)},selection,LayerSelectionState{{id},id,id});
}
std::unique_ptr<LayerStructureCommand> promoteMirroredLayer(const Document& document,const LayerSelectionState& selection)
{
    if (selection.ids.size()!=1) throw std::invalid_argument("Select one mirrored layer to promote.");
    const auto* layer=document.layer(selection.ids.front());
    if (!layer || !layer->mirror) throw std::invalid_argument("Select a mirrored layer.");
    return std::make_unique<LayerStructureCommand>("Promote mirrored layer",document,document.tree(),std::vector<LayerId>{layer->id},
        std::vector<Layer>{promotedMirroredLayer(*layer)},selection,selection);
}
bool SyncMirroredLayerCommand::apply(Document& document)
{
    const auto* current=document.layer(id_);
    if (!current || !current->mirror) return false;
    if (!before_) {
        // History owns appearance only, never an obsolete source pixel buffer,
        // text payload or inherited mask after the source is edited.
        Layer state;
        state.mirror=current->mirror;
        state.opacity=current->opacity; state.fillOpacity=current->fillOpacity;
        state.blendMode=current->blendMode; state.effects=current->effects;
        before_=state; after_=std::move(state);
        auto& link=*after_->mirror;
        if (field_==MirrorSync::All) link=LayerMirror{.source=link.source};
        else if (field_==MirrorSync::Opacity) link.opacityOverride=false;
        else link.fillOpacityOverride=false;
    }
    return document.restoreMirrorAppearance(id_,*after_);
}
bool SyncMirroredLayerCommand::undo(Document& document)
{ return before_ && document.restoreMirrorAppearance(id_,*before_); }
std::size_t SyncMirroredLayerCommand::memoryCost() const noexcept
{
    return sizeof(*this) + (before_ ? layerEffectMemoryCost(before_->effects) : 0)
        + (after_ && (!before_ || after_->effects!=before_->effects) ? layerEffectMemoryCost(after_->effects) : 0);
}
} // namespace imageeditor::core
