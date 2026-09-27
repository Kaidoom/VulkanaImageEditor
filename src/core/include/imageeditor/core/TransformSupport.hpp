#pragma once
#include "imageeditor/core/LayerGeometry.hpp"
#include <stdexcept>

namespace imageeditor::core
{
// Conservative source/effect support for projective admission, even while the
// derived caches are absent/stale. No source evaluation or pixel allocation.
inline RectD transformEvaluationSupport(RectD raw, const SpatialFilterState &filters,
                                        const LayerEffectState &effects)
{
    for (double value : {raw.x, raw.y, raw.right(), raw.bottom()})
        if (!std::isfinite(value) || std::abs(value) > 1e9)
            throw std::invalid_argument("Invalid transform source support");
    const int x = int(std::floor(raw.x)), y = int(std::floor(raw.y));
    const auto filtered = expandedSpatialOutputBounds(
        filters, {x, y, int(std::ceil(raw.right())) - x, int(std::ceil(raw.bottom())) - y});
    double margin = 2; // source AA/interpolation support
    if (effects)
        for (std::size_t i = 0; i < 5; ++i)
            if (effects->items[i].enabled) {
                const auto &effect = effects->items[i];
                margin = std::max(margin, effect.size + (i == 1 || i == 2 ? effect.distance : 0) + 2);
            }
    return {filtered.x - margin, filtered.y - margin, filtered.width + 2 * margin,
            filtered.height + 2 * margin};
}
inline RectD intrinsicLocalBounds(const Layer &layer)
{
    if (layer.renderCache && layer.renderCache->localSourceBounds &&
        !std::holds_alternative<RasterLayer>(layer.payload))
        return *layer.renderCache->localSourceBounds;
    const auto source = intrinsicSurface(layer);
    if (!source)
        return {};
    const auto e = source->extent();
    const auto mapping = intrinsicPixelsToLocal(layer);
    const std::array points{mapping.map({0, 0}), mapping.map({double(e.width), 0}),
                            mapping.map({0, double(e.height)}),
                            mapping.map({double(e.width), double(e.height)})};
    double x = points[0].x, y = points[0].y, right = x, bottom = y;
    for (auto p : points) {
        x = std::min(x, p.x);
        y = std::min(y, p.y);
        right = std::max(right, p.x);
        bottom = std::max(bottom, p.y);
    }
    return {x, y, right - x, bottom - y};
}
} // namespace imageeditor::core
