#pragma once

#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace imageeditor::ui::mcpdetail {

inline QJsonObject geometryResponseSchema()
{
    return {{"type", "string"}, {"enum", QJsonArray{"ids", "geometry"}}, {"default", "geometry"},
        {"description", "geometry (default) includes logical bounds; ids omits optional geometry and its measurement. Document ID, revision, changed and result IDs are always retained."}};
}
inline bool wantsGeometry(const QJsonObject& arguments)
{ return arguments.value("response").toString("geometry") == "geometry"; }

inline QJsonObject geometryRectangle(core::RectD bounds)
{ return {{"x", bounds.x}, {"y", bounds.y}, {"width", bounds.width}, {"height", bounds.height}}; }

inline core::RectD geometryRectangle(const QJsonObject& bounds)
{ return {bounds["x"].toDouble(), bounds["y"].toDouble(), bounds["width"].toDouble(), bounds["height"].toDouble()}; }

// Logical geometry is independent of viewport caches, pixel alpha, masks,
// crop, stroke and effects. Measuring text performs layout, never rasterization.
inline QJsonValue layerGeometryData(const core::Layer& layer)
{
    core::RectD local;
    if (const auto* text = std::get_if<core::TextLayer>(&layer.payload)) {
        const auto bounds = QtTextLayout(*text).bounds();
        local = {bounds.x(), bounds.y(), bounds.width(), bounds.height()};
    } else if (const auto* shape = std::get_if<core::ShapeLayer>(&layer.payload)) {
        local = {0, 0, shape->size.width, shape->size.height};
    } else if (const auto* raster = std::get_if<core::RasterLayer>(&layer.payload); raster && raster->surface) {
        const auto extent = raster->surface->extent();
        local = {layer.rasterOrigin.x, layer.rasterOrigin.y, double(extent.width), double(extent.height)};
    } else {
        return QJsonValue::Null; // Adjustments have no independent layout box.
    }
    if (!layer.localToDocument.validOver(local)) return QJsonValue::Null;
    double left = std::numeric_limits<double>::max(), top = left, right = -left, bottom = -left;
    QJsonArray quad;
    for (const auto corner : std::array{core::Vec2d{local.x, local.y}, core::Vec2d{local.right(), local.y},
             core::Vec2d{local.right(), local.bottom()}, core::Vec2d{local.x, local.bottom()}}) {
        const auto p = layer.localToDocument.map(corner);
        left = std::min(left, p.x); top = std::min(top, p.y);
        right = std::max(right, p.x); bottom = std::max(bottom, p.y);
        quad.append(QJsonObject{{"x", p.x}, {"y", p.y}});
    }
    return QJsonObject{{"localBounds", geometryRectangle(local)},
        {"documentBounds", geometryRectangle({left, top, right - left, bottom - top})},
        {"documentQuad", quad}, {"includesEffects", false}};
}

// The lookup overload also measures private pending creations/copies, before
// a command changes the live document. Containers have no local transform;
// their document box is the union of content descendants, including hidden ones.
template<class Lookup>
inline QJsonValue itemGeometryData(const core::LayerTree& tree, core::LayerId item, Lookup&& lookup)
{
    if (const auto* layer = lookup(item)) return layerGeometryData(*layer);
    if (!tree.container(item)) return QJsonValue::Null;
    std::optional<core::RectD> bounds;
    for (auto id : tree.descendants(item)) {
        const auto* layer = lookup(id);
        if (!layer) return QJsonValue::Null;
        if (std::holds_alternative<core::AdjustmentLayer>(layer->payload)) continue;
        const auto geometry = layerGeometryData(*layer);
        if (!geometry.isObject()) return QJsonValue::Null;
        const auto rect = geometryRectangle(geometry.toObject()["documentBounds"].toObject());
        if (!bounds) bounds = rect;
        else {
            const auto left = std::min(bounds->x, rect.x), top = std::min(bounds->y, rect.y);
            bounds = core::RectD{left, top, std::max(bounds->right(), rect.right()) - left,
                std::max(bounds->bottom(), rect.bottom()) - top};
        }
    }
    if (!bounds) return QJsonValue::Null;
    QJsonArray quad;
    for (const auto corner : std::array{core::Vec2d{bounds->x, bounds->y}, core::Vec2d{bounds->right(), bounds->y},
             core::Vec2d{bounds->right(), bounds->bottom()}, core::Vec2d{bounds->x, bounds->bottom()}})
        quad.append(QJsonObject{{"x", corner.x}, {"y", corner.y}});
    return QJsonObject{{"localBounds", QJsonValue::Null}, {"documentBounds", geometryRectangle(*bounds)},
        {"documentQuad", quad}, {"includesEffects", false}};
}

inline QJsonValue itemGeometryData(const core::Document& document, core::LayerId item)
{ return itemGeometryData(document.tree(), item, [&](core::LayerId id) { return document.layer(id); }); }

} // namespace imageeditor::ui::mcpdetail
