#pragma once

#include "imageeditor/core/Geometry.hpp"

namespace imageeditor::core {

// Document and logical viewport coordinates are both top-left origin, with X
// increasing right and Y increasing down.
class ViewportState {
public:
    [[nodiscard]] double zoom() const noexcept { return zoom_; }
    [[nodiscard]] Vec2d pan() const noexcept { return pan_; }

    void setZoom(double zoom) noexcept;
    void setPan(Vec2d pan) noexcept { pan_ = pan; }
    void panBy(Vec2d delta) noexcept { pan_ = pan_ + delta; }

    [[nodiscard]] Vec2d documentToViewport(Vec2d documentPoint, Extent2d document,
        Extent2d viewport) const noexcept;
    [[nodiscard]] Vec2d viewportToDocument(Vec2d viewportPoint, Extent2d document,
        Extent2d viewport) const noexcept;

    void zoomAround(Vec2d viewportPoint, double factor, Extent2d document,
        Extent2d viewport) noexcept;
    void fit(Extent2d document, Extent2d viewport, double margin = 48.0) noexcept;
    void reset100Percent() noexcept;

private:
    static constexpr double kMinZoom = 0.01;
    static constexpr double kMaxZoom = 64.0;

    double zoom_ {1.0};
    Vec2d pan_;
};

} // namespace imageeditor::core
