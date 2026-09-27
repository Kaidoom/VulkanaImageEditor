#pragma once
#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/CropGeometry.hpp"
#include "imageeditor/core/Geometry.hpp"

namespace imageeditor::core {
[[nodiscard]] inline bool validLayerCrop(RectD r) noexcept
{
    for (double v : { r.x, r.y, r.width, r.height, r.right(), r.bottom() })
        if (!std::isfinite(v) || std::abs(v) > 1e9)
            return false;
    return r.width >= 0 && r.height >= 0;
}
[[nodiscard]] inline bool validLayerCrop(const LayerCrop& crop) noexcept
{
    if (!validLayerCrop(static_cast<const RectD&>(crop)))
        return false;
    for (double v : crop.corners)
        if (!std::isfinite(v) || v < 0 || v > 1e9)
            return false;
    return true;
}
// Exact box-filter coverage: intersect one output pixel's local-space
// parallelogram with the local crop. Never multiply straight RGB by coverage.
[[nodiscard]] float layerCropCoverage(const LayerCrop&, Vec2d localCenter, Vec2d localDx,
    Vec2d localDy) noexcept;
[[nodiscard]] PremultipliedColor
applyLayerCrop(PremultipliedColor, const std::optional<LayerCrop>&,
    const AffineTransform& outputToLocal,
    Vec2d outputPoint) noexcept;
// Editing eligibility is binary, independent of output AA. A texel whose
// source-local cell intersects the crop may be edited; fully hidden cells may
// not.
[[nodiscard]] inline bool cropAllowsTexel(const std::optional<LayerCrop>& crop,
    int x, int y) noexcept
{
    if (!crop)
        return true;
    const auto r = crop->clippedTo({ double(x), double(y), 1, 1 });
    if (r.empty())
        return false;
    const auto c = crop->resolvedCorners();
    // Strict separating axes on the overlapped unit cell reject zero-area
    // contact, without turning AA coverage into destructive pixel alpha.
    return r.right() - crop->x + r.bottom() - crop->y > c[0]
        && crop->right() - r.x + r.bottom() - crop->y > c[1]
        && crop->right() - r.x + crop->bottom() - r.y > c[2]
        && r.right() - crop->x + crop->bottom() - r.y > c[3];
}
} // namespace imageeditor::core
