#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include <algorithm>
#include <unordered_set>

namespace imageeditor::render {
void CanvasWindow::setCapturedAdjustmentRegion(std::optional<core::LayerId> target, core::AdjustmentType type)
{
    if (!target && capturedFilterType_) return;
    capturedRegionTarget_ = target;
    capturedRegionType_ = type;
    capturedFilterType_.reset();
    refreshCapturedAdjustmentRegion();
}
void CanvasWindow::setCapturedFilterRegion(std::optional<core::LayerId> target, core::SpatialFilterType type)
{
    if (!target && !capturedFilterType_) return;
    capturedRegionTarget_ = target;
    capturedFilterType_ = type;
    refreshCapturedAdjustmentRegion();
}
void CanvasWindow::refreshCapturedAdjustmentRegion()
{
    core::SelectionState mask;
    core::AffineTransform mapping;
    if (capturedRegionTarget_) {
        const auto layer = std::ranges::find(scene_.document.layersBottomToTop, *capturedRegionTarget_, &core::LayerSnapshot::id);
        if (layer != scene_.document.layersBottomToTop.end()) {
            const std::optional<core::AdjustmentMask> captured = capturedFilterType_
                ? (layer->filters ? layer->filters->items.at(std::size_t(*capturedFilterType_)).mask : std::nullopt)
                : (layer->adjustments ? layer->adjustments->items.at(std::size_t(capturedRegionType_)).mask : std::nullopt);
            if (captured && captured->coverage && !captured->coverage->bounds().empty()) {
                if (const auto inverse = captured->localToMask.inverted()) {
                    mask = captured->coverage;
                    // Frozen capture-document -> layer local -> current document.
                    // Do not pin the overlay to the live selection or the viewport.
                    mapping = core::composeAffine(layer->localToDocument, *inverse);
                }
            }
        }
    }
    if (mask == capturedRegionMask_ && mapping == capturedRegionMapping_) return;
    std::vector<core::SelectionEdge> edges;
    if (mask) {
        const auto& boundary = mask->nonzeroBoundaryEdges();
        edges.reserve(boundary.size());
        for (const auto& edge : boundary) {
            const auto from = mapping.map(edge.from), to = mapping.map(edge.to);
            if (std::isfinite(from.x) && std::isfinite(from.y) && std::isfinite(to.x) && std::isfinite(to.y))
                edges.push_back({from, to});
        }
    }
    capturedRegionMask_ = std::move(mask);
    capturedRegionMapping_ = mapping;
    if (scene_.capturedRegionEdges && *scene_.capturedRegionEdges == edges) return;
    if (!scene_.capturedRegionEdges && edges.empty()) return;
    scene_.capturedRegionEdges = edges.empty() ? nullptr
        : std::make_shared<const std::vector<core::SelectionEdge>>(std::move(edges));
    ++scene_.capturedRegionRevision;
    scheduleFrame(); // No animation timer, image invalidation or input footprint.
}
void CanvasWindow::setLayerOutlineTargets(std::span<const core::LayerId> targets, bool reveal)
{
    const bool changed = !std::ranges::equal(targets, layerOutlineTargets_);
    if (changed) layerOutlineTargets_.assign(targets.begin(), targets.end());
    if (changed || reveal) layerOutlinesDismissed_ = false;
    if (changed || reveal) refreshLayerOutlines();
}
void CanvasWindow::setLayerOutlinesVisible(bool enabled)
{
    if (layerOutlinesVisible_ == enabled) return;
    layerOutlinesVisible_ = enabled;
    if (enabled) layerOutlinesDismissed_ = false;
    refreshLayerOutlines();
}
void CanvasWindow::setLayerOutlineColor(core::Rgba8 color)
{
    if (scene_.layerOutlineColor == color) return;
    scene_.layerOutlineColor = color;
    scheduleFrame(); // Push constants only; neither geometry nor artwork uploads.
}
void CanvasWindow::dismissLayerOutlines()
{
    layerOutlinesDismissed_ = true;
    refreshLayerOutlines();
}
void CanvasWindow::refreshLayerOutlines()
{
    std::vector<core::SelectionEdge> edges;
    if (layerOutlinesVisible_ && !layerOutlinesDismissed_
        && scene_.activeTool == core::ToolId::Move && !scene_.measureActive && !scene_.transformOverlay) {
        const std::unordered_set selected(layerOutlineTargets_.begin(), layerOutlineTargets_.end());
        for (const auto& layer : scene_.document.layersBottomToTop) {
            if (!selected.contains(layer.id)) continue;
            if (const auto* raster = std::get_if<core::RasterLayerSnapshot>(&layer.payload)) {
                if (!raster->surface) continue;
            } else if (!std::holds_alternative<core::ShapeLayer>(layer.payload) && !layer.renderCache) continue;
            const auto bounds = core::layerInteractionBounds(layer);
            if (layer.crop && bounds.empty()) continue;
            const core::Extent2d size{bounds.width, bounds.height};
            if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width < 0 || size.height < 0) continue;
            const auto frame = core::composeAffine(layer.localToDocument, {1,0,bounds.x,0,1,bounds.y});
            const auto handles = core::geometryTransformHandles(frame, size);
            if (!std::ranges::all_of(handles, [](auto p) { return std::isfinite(p.x) && std::isfinite(p.y); })) continue;
            // Preserve each content frame's rotation/shear/flips/perspective;
            // neither viewport clipping nor effect padding defines the outline.
            for (std::size_t i = 0; i < 4; ++i)
                edges.push_back({handles[i * 2], handles[((i + 1) % 4) * 2]});
        }
    }
    if (scene_.layerOutlineEdges && *scene_.layerOutlineEdges == edges) return;
    if (!scene_.layerOutlineEdges && edges.empty()) return;
    scene_.layerOutlineEdges = edges.empty() ? nullptr
        : std::make_shared<const std::vector<core::SelectionEdge>>(std::move(edges));
    ++scene_.layerOutlineRevision;
    scheduleFrame();
}
}
