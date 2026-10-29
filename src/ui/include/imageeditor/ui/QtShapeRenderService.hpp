#pragma once

#include "imageeditor/core/ShapeRenderService.hpp"
#include <stdexcept>

namespace imageeditor::ui {

class ShapeRasterLimitError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ShapeRasterSize {
    core::Extent2u extent;
    double density;
};

// Qt paths/stroking supply antialiased coverage only. Fill and stroke colors
// are combined in linear light and returned as the editor's straight RGBA8.
class QtShapeRenderService final : public core::ShapeRenderService {
public:
    [[nodiscard]] std::shared_ptr<const core::LayerRenderCache> render(
        const core::ShapeRenderRequest& request) override;
    // Uses the same stroked bounds, rounding and limits as render(), without
    // allocating pixel buffers. Used to budget actual requested cache sizes.
    [[nodiscard]] ShapeRasterSize rasterSize(const core::ShapeRenderRequest& request) const;
    [[nodiscard]] std::shared_ptr<const core::ShapeHitGeometry> prepareHit(
        const core::ShapeLayer& shape) const override;
    [[nodiscard]] bool hit(const core::ShapeLayer& shape,
        const core::AffineTransform& localToScreen, core::Vec2d screenPoint,
        double tolerancePixels = 5.0) const override;

    // Authoritative output rasterization: transform the geometry onto the
    // document pixel grid before antialiasing. No local/viewport bitmap is
    // resampled. Bounds include stroke and a two-document-pixel AA fringe.
    [[nodiscard]] core::RectD documentBounds(const core::ShapeLayer& shape,
        const core::AffineTransform& localToDocument) const;
    [[nodiscard]] std::shared_ptr<const core::LayerRenderCache> renderDocument(
        const core::ShapeLayer& shape, const core::AffineTransform& localToDocument,
        std::size_t exactPixelBudget, std::optional<core::RectI> documentClip = {}) const;

    // Power-of-two tiers avoid regenerating a cache for every fractional zoom
    // update. The cache allocation still applies independent hard limits.
    [[nodiscard]] static double densityForScale(double scale) noexcept;
};

} // namespace imageeditor::ui
