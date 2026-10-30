#pragma once

#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <limits>

namespace imageeditor::ui::mcpdetail {

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
    const auto rectangle = [](core::RectD bounds) {
        return QJsonObject{{"x", bounds.x}, {"y", bounds.y}, {"width", bounds.width}, {"height", bounds.height}};
    };
    double left = std::numeric_limits<double>::max(), top = left, right = -left, bottom = -left;
    QJsonArray quad;
    for (const auto corner : std::array{core::Vec2d{local.x, local.y}, core::Vec2d{local.right(), local.y},
             core::Vec2d{local.right(), local.bottom()}, core::Vec2d{local.x, local.bottom()}}) {
        const auto p = layer.localToDocument.map(corner);
        left = std::min(left, p.x); top = std::min(top, p.y);
        right = std::max(right, p.x); bottom = std::max(bottom, p.y);
        quad.append(QJsonObject{{"x", p.x}, {"y", p.y}});
    }
    return QJsonObject{{"localBounds", rectangle(local)},
        {"documentBounds", rectangle({left, top, right - left, bottom - top})},
        {"documentQuad", quad}, {"includesEffects", false}};
}

} // namespace imageeditor::ui::mcpdetail
