#pragma once
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
namespace imageeditor::core {
inline AffineTransform composeAffine(const AffineTransform& a, const AffineTransform& b)
{
    return composeTransform(a,b);
}
// An integer document translation leaves an already rasterized typed image
// byte-identical. Rebase only disposable metadata, retaining the surface/GPU
// texture. Fractional moves, rotation and scale must repaint the model.
inline std::shared_ptr<const LayerRenderCache> translatedDocumentRenderCache(
    const std::shared_ptr<const LayerRenderCache>& cache, const AffineTransform& transform)
{
    if (!cache || !cache->rasterizedDocumentTransform) return {};
    const auto& previous = *cache->rasterizedDocumentTransform;
    if(!transform.isAffine()||!previous.isAffine())return {};
    const auto dx = transform.m02 - previous.m02, dy = transform.m12 - previous.m12;
    if (transform.m00 != previous.m00 || transform.m01 != previous.m01
        || transform.m10 != previous.m10 || transform.m11 != previous.m11
        || !std::isfinite(dx) || !std::isfinite(dy) || dx != std::floor(dx) || dy != std::floor(dy)) return {};
    if (dx == 0 && dy == 0) return cache;
    auto result = std::make_shared<LayerRenderCache>(*cache);
    result->rasterizedDocumentTransform = transform;
    result->documentOrigin.x += dx;
    result->documentOrigin.y += dy;
    return result;
}
template <class L> std::shared_ptr<const RasterSurface> intrinsicSurface(const L& layer)
{
    if constexpr (std::is_same_v<L, Layer>) {
        if (const auto* raster = std::get_if<RasterLayer>(&layer.payload))
            return raster->surface;
    } else if (const auto* raster = std::get_if<RasterLayerSnapshot>(&layer.payload))
        return raster->surface;
    return layer.renderCache ? layer.renderCache->surface : nullptr;
}
template <class L> AffineTransform intrinsicPixelsToLocal(const L& layer)
{
    return (std::holds_alternative<TextLayer>(layer.payload)
               || std::holds_alternative<ShapeLayer>(layer.payload)) && layer.renderCache
        ? layer.renderCache->pixelsToLocal : AffineTransform{1,0,layer.rasterOrigin.x,0,1,layer.rasterOrigin.y};
}
template<class L> AffineTransform intrinsicTransform(const L& layer)
{ return composeTransform(layer.localToDocument,intrinsicPixelsToLocal(layer)); }
template <class L> AffineTransform renderPixelsToLocal(const L& layer)
{
    return layerSpatialFilterCacheValid(layer) ? layer.filterCache->pixelsToLocal : intrinsicPixelsToLocal(layer);
}
template <class L> std::shared_ptr<const RasterSurface> renderedSurface(const L& layer)
{
    return layerSpatialFilterCacheValid(layer) ? layer.filterCache->surface : intrinsicSurface(layer);
}
template <class L> AffineTransform renderTransform(const L& layer)
{
    if ((std::holds_alternative<TextLayer>(layer.payload) || std::holds_alternative<ShapeLayer>(layer.payload))
        && !hasActiveSpatialFilters(layer.filters) && layer.renderCache
        && layer.renderCache->rasterizedDocumentTransform == layer.localToDocument) {
        // Avoid inverse(T) * T roundoff introducing a fractional source sample
        // in a raster already painted on exact document pixel centers.
        AffineTransform translation;
        translation.m02 = layer.renderCache->documentOrigin.x;
        translation.m12 = layer.renderCache->documentOrigin.y;
        return translation;
    }
    return composeAffine(layer.localToDocument,renderPixelsToLocal(layer));
}
template<class L> Extent2u layerGeometryExtent(const L& layer)
{
    using Raster = std::conditional_t<std::is_same_v<L, Layer>, RasterLayer, RasterLayerSnapshot>;
    if (const auto* raster = std::get_if<Raster>(&layer.payload))
        return raster->surface ? raster->surface->extent() : Extent2u { };
    if (const auto* shape = std::get_if<ShapeLayer>(&layer.payload))
        return shapeGeometryExtent(*shape);
    return layer.renderCache ? layer.renderCache->logicalExtent : Extent2u { };
}
// Natural source/cache bounds and geometry frame are intentionally distinct.
// Text/shape caches may include negative bearings or stroke/AA padding.
template<class L> RectD layerSourceBounds(const L& layer)
{
    if ((std::holds_alternative<TextLayer>(layer.payload) || std::holds_alternative<ShapeLayer>(layer.payload))
        && !layerSpatialFilterCacheValid(layer) && layer.renderCache && layer.renderCache->localSourceBounds)
        return *layer.renderCache->localSourceBounds;
    const auto surface=renderedSurface(layer);
    if(!surface)return {};
    const auto e=surface->extent();
    const auto pixelsToLocal=renderPixelsToLocal(layer);
    const auto a=pixelsToLocal.map({0,0}),b=pixelsToLocal.map({double(e.width),0}),
        c=pixelsToLocal.map({0,double(e.height)}),d=pixelsToLocal.map({double(e.width),double(e.height)});
    const double x=std::min({a.x,b.x,c.x,d.x}),y=std::min({a.y,b.y,c.y,d.y});
    return {x,y,std::max({a.x,b.x,c.x,d.x})-x,std::max({a.y,b.y,c.y,d.y})-y};
}
template<class L> RectD layerVisibleBounds(const L& layer)
{
    const auto source=layerSourceBounds(layer);
    return layer.crop?croppedBounds(*layer.crop,source):source;
}
// Styling never changes model geometry, transform handles, or the gradient's
// reference frame. Only visual/output consumers request these expanded bounds.
template<class L> RectD layerStyledBounds(const L& layer)
{
    const auto bounds=hasActiveLayerEffects(layer.effects)&&layerEffectCacheValid(layer)&&layer.effectCache
        ?layer.effectCache->visualBounds:layerSourceBounds(layer);
    return layer.crop?croppedBounds(*layer.crop,bounds):bounds;
}
template<class L> RectD layerEffectReferenceFrame(const L& layer)
{
    if(const auto* shape=std::get_if<ShapeLayer>(&layer.payload))return {0,0,shape->size.width,shape->size.height};
    if(std::holds_alternative<TextLayer>(layer.payload)){
        const auto extent=layer.renderCache?layer.renderCache->logicalExtent:Extent2u{};
        return {0,0,double(extent.width),double(extent.height)};
    }
    const auto source=intrinsicSurface(layer);const auto e=source?source->extent():Extent2u{};
    return layer.rasterEffectFrame.value_or(RectD{layer.rasterOrigin.x,layer.rasterOrigin.y,double(e.width),double(e.height)});
}
template<class L> bool hitLayerCrop(const L& layer,Vec2d documentPoint)
{
    if(!layer.crop)return true;
    const auto inverse=layer.localToDocument.inverted();
    return inverse && layer.crop->contains(inverse->map(documentPoint));
}
// Use the same local frame for interaction and snapshot overlays. Raster bounds
// come from cached source alpha, offset by storage origin; typed layers keep their
// geometry frame. Cropping never rebases the canonical local origin.
template<class L> RectD layerInteractionBounds(const L& layer)
{
    using Raster = std::conditional_t<std::is_same_v<L, Layer>, RasterLayer, RasterLayerSnapshot>;
    if (const auto* raster = std::get_if<Raster>(&layer.payload); raster && raster->surface) {
        const auto b = raster->surface->contentBounds();
        if (!b.empty()) {
            const RectD content{layer.rasterOrigin.x+b.x,layer.rasterOrigin.y+b.y,double(b.width),double(b.height)};
            return layer.crop ? croppedBounds(*layer.crop,content) : content;
        }
        // An empty layer still has a usable editing/transform frame.
    }
    if(layer.crop)return layerVisibleBounds(layer);
    const auto e=layerGeometryExtent(layer);
    if(const auto* shape=std::get_if<ShapeLayer>(&layer.payload))return {0,0,shape->size.width,shape->size.height};
    return {layer.rasterOrigin.x,layer.rasterOrigin.y,double(e.width),double(e.height)};
}
inline bool hitTextBounds(const Layer& layer, Vec2d p)
{
    if (!std::holds_alternative<TextLayer>(layer.payload) || !layer.visible || layer.opacity <= 0 || !hitLayerCrop(layer,p))
        return false;
    const auto inverse = layer.localToDocument.inverted();
    if (!inverse)
        return false;
    const auto local = inverse->map(p);
    const auto extent = layerGeometryExtent(layer);
    return !extent.empty() && std::isfinite(layer.opacity) && local.x >= 0 && local.y >= 0
        && local.x <= extent.width && local.y <= extent.height;
}
}
