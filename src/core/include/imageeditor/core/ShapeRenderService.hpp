#pragma once

#include "imageeditor/core/Layer.hpp"

#include <memory>
#include <optional>

namespace imageeditor::core {

struct ShapeRenderRequest {
    ShapeLayer shape;
    double rasterDensity {1.0};
    // Destructive output must fail before allocation rather than silently
    // lowering resolution to a viewport-cache limit.
    std::optional<std::size_t> exactPixelBudget {};
};

// Prepared logical hit geometry can be retained by stable layer ID and shape
// revision. Moving, rotating or zooming a layer does not rebuild its paths.
class ShapeHitGeometry {
public:
    virtual ~ShapeHitGeometry() = default;
    [[nodiscard]] virtual bool hit(const AffineTransform& localToScreen,
        Vec2d screenPoint, double tolerancePixels = 5.0) const = 0;
};

// The canonical geometry/style remains in ShapeLayer. Implementations only
// produce disposable straight-RGBA8 caches and geometric hit tests; no toolkit
// path, color or raster object becomes document state.
class ShapeRenderService {
public:
    virtual ~ShapeRenderService() = default;
    [[nodiscard]] virtual std::shared_ptr<const LayerRenderCache> render(
        const ShapeRenderRequest& request) = 0;
    [[nodiscard]] virtual std::shared_ptr<const ShapeHitGeometry> prepareHit(
        const ShapeLayer& shape) const = 0;

    // Closed shapes include their enclosed interior, even when hollow. The
    // tolerance is in logical screen pixels, not document/layer coordinates.
    [[nodiscard]] virtual bool hit(const ShapeLayer& shape,
        const AffineTransform& localToScreen, Vec2d screenPoint,
        double tolerancePixels = 5.0) const = 0;
};

} // namespace imageeditor::core
