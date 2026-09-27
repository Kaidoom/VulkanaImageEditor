#include "imageeditor/core/Measurement.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace imageeditor::core {
namespace {
bool finite(Vec2d point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}
}

std::optional<MeasurementValues> measurementValues(MeasurementLine line) noexcept
{
    if (!finite(line.a) || !finite(line.b)) return {};
    auto delta = line.b - line.a;
    const auto distance = std::hypot(delta.x, delta.y);
    if (!finite(delta) || !std::isfinite(distance)) return {};
    // Avoid signed-zero output in readouts and a -180 result solely because a
    // horizontal drag happened to contain a negative floating-point zero.
    if (delta.x == 0) delta.x = 0;
    if (delta.y == 0) delta.y = 0;
    MeasurementValues result {delta, distance, {}};
    if (distance > 0)
        result.angleDegrees = std::atan2(delta.y, delta.x) * (180.0 / std::numbers::pi);
    return result;
}

std::optional<DocumentBounds> selectedLayerBounds(const Document& document,
    std::span<const LayerId> selection)
{
    if (std::ranges::any_of(selection, [&document](auto id) { return !document.containsItem(id); }))
        return {};
    std::optional<DocumentBounds> bounds;
    for (const auto id : document.expandedLayers(selection)) {
        const auto* layer = document.layer(id);
        if (!layer) return {};
        if (layerGeometryExtent(*layer).empty()) return {};
        if (layer->crop && layerInteractionBounds(*layer).empty()) continue;
        const auto part = layerDocumentBounds(*layer);
        if (!part) return {};
        if (!bounds) bounds = part;
        else {
            bounds->minimum.x = std::min(bounds->minimum.x, part->minimum.x);
            bounds->minimum.y = std::min(bounds->minimum.y, part->minimum.y);
            bounds->maximum.x = std::max(bounds->maximum.x, part->maximum.x);
            bounds->maximum.y = std::max(bounds->maximum.y, part->maximum.y);
        }
    }
    if (bounds && (!std::isfinite(bounds->extent().width)
                      || !std::isfinite(bounds->extent().height))) return {};
    return bounds;
}

std::optional<DocumentBounds> layerDocumentBounds(const Layer& layer)
{
    if (layerGeometryExtent(layer).empty()) return {};
    const auto region = layerInteractionBounds(layer);
    if (layer.crop && region.empty()) return {};
    std::optional<DocumentBounds> bounds;
    const auto size = Extent2d {region.width, region.height};
    if (!std::isfinite(size.width) || !std::isfinite(size.height)
        || size.width < 0 || size.height < 0) return {};
    for (const auto local : std::array<Vec2d, 4> {{{0, 0}, {size.width, 0},
             {size.width, size.height}, {0, size.height}}}) {
        const auto point = layer.localToDocument.map(local + Vec2d {region.x, region.y});
        if (!finite(point)) return {};
        if (!bounds) bounds = DocumentBounds {point, point};
        else {
            bounds->minimum.x = std::min(bounds->minimum.x, point.x);
            bounds->minimum.y = std::min(bounds->minimum.y, point.y);
            bounds->maximum.x = std::max(bounds->maximum.x, point.x);
            bounds->maximum.y = std::max(bounds->maximum.y, point.y);
        }
    }
    if (bounds && (!std::isfinite(bounds->extent().width)
                      || !std::isfinite(bounds->extent().height))) return {};
    return bounds;
}

} // namespace imageeditor::core
