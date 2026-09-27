#include "imageeditor/core/ViewportState.hpp"

#include <algorithm>

namespace imageeditor::core {

void ViewportState::setZoom(double zoom) noexcept
{
    zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
}

Vec2d ViewportState::documentToViewport(Vec2d documentPoint, Extent2d document,
    Extent2d viewport) const noexcept
{
    return {
        (documentPoint.x - document.width * 0.5) * zoom_ + viewport.width * 0.5 + pan_.x,
        (documentPoint.y - document.height * 0.5) * zoom_ + viewport.height * 0.5 + pan_.y,
    };
}

Vec2d ViewportState::viewportToDocument(Vec2d viewportPoint, Extent2d document,
    Extent2d viewport) const noexcept
{
    return {
        (viewportPoint.x - viewport.width * 0.5 - pan_.x) / zoom_ + document.width * 0.5,
        (viewportPoint.y - viewport.height * 0.5 - pan_.y) / zoom_ + document.height * 0.5,
    };
}

void ViewportState::zoomAround(Vec2d viewportPoint, double factor, Extent2d document,
    Extent2d viewport) noexcept
{
    const auto anchor = viewportToDocument(viewportPoint, document, viewport);
    setZoom(zoom_ * factor);
    const auto after = documentToViewport(anchor, document, viewport);
    pan_ = pan_ + (viewportPoint - after);
}

void ViewportState::fit(Extent2d document, Extent2d viewport, double margin) noexcept
{
    if (document.width <= 0.0 || document.height <= 0.0
        || viewport.width <= 0.0 || viewport.height <= 0.0) {
        return;
    }
    const auto availableWidth = std::max(1.0, viewport.width - margin * 2.0);
    const auto availableHeight = std::max(1.0, viewport.height - margin * 2.0);
    setZoom(std::min(availableWidth / document.width, availableHeight / document.height));
    pan_ = {};
}

void ViewportState::reset100Percent() noexcept
{
    zoom_ = 1.0;
    pan_ = {};
}

} // namespace imageeditor::core

