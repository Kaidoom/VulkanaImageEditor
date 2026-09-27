#include "imageeditor/render/CanvasWindow.hpp"
#include <QGuiApplication>
#include <algorithm>
#include <cmath>

namespace imageeditor::render {
void CanvasWindow::setSelectionConstructionActive(bool active)
{
    selectionConstruction_=active;
    if (!active) {
        setPointerTooltip({});
        selecting_=false; movingSelection_=false;
        if (explicitSelectionGrab_) { explicitSelectionGrab_=false; setMouseGrabEnabled(false); }
        updateCursorForTool();
    }
}
void CanvasWindow::setSelectionOperation(core::SelectionOperation operation)
{
    selectionOperation_ = operation;
    updateCursorForTool();
}
void CanvasWindow::updateSelectionModifiers(Qt::KeyboardModifiers modifiers)
{
    selectionModifiers_ = modifiers;
    if (core::isSelectionTool(scene_.activeTool)) updateCursorForTool();
}
void CanvasWindow::beginSelectionInput(QPointF point, Qt::KeyboardModifiers modifiers)
{
    if (selecting_ || !onSelectionBegan) return;
    movingSelection_ = scene_.activeTool != core::ToolId::SelectByColor && scene_.activeTool != core::ToolId::SmartSelect && !selectionConstruction_ && core::selectionMoveHit(scene_.document.selection, documentPositionForLogical(point),
        selectionOperation_, modifiers.testFlag(Qt::ShiftModifier), modifiers.testFlag(Qt::AltModifier));
    selecting_ = onSelectionBegan(documentPositionForLogical(point), modifiers);
    if (selecting_ && QGuiApplication::platformName() != QStringLiteral("wayland"))
        explicitSelectionGrab_ = setMouseGrabEnabled(true);
    updateCursorForTool();
}
void CanvasWindow::finishSelectionInput(bool cancel)
{
    if (!selecting_) return;
    setPointerTooltip({});
    selecting_ = false;
    movingSelection_ = false;
    if (explicitSelectionGrab_) { explicitSelectionGrab_ = false; setMouseGrabEnabled(false); }
    if (onSelectionEnded) onSelectionEnded(cancel);
    updateCursorForTool();
}
void CanvasWindow::cancelSelectionInput() {
    if (selecting_) finishSelectionInput(true);
    else if (selectionConstruction_ && onSelectionEnded) onSelectionEnded(true);
}
void CanvasWindow::setSelectionPreview(std::shared_ptr<const std::vector<core::SelectionEdge>> edges)
{
    scene_.selectionPathPreview = false;
    scene_.selectionRetainedEdges.reset();
    ++scene_.selectionRetainedRevision;
    scene_.selectionStablePrefix = scene_.selectionClosingEdges = scene_.selectionAnchorCount = 0;
    selectionPreview_ = std::move(edges);
    refreshSelectionEdges();
    scheduleFrame();
}
void CanvasWindow::setSelectionPathPreview(std::shared_ptr<const std::vector<core::SelectionEdge>> edges,
    core::SelectionOperation operation, std::size_t closingEdges, std::optional<std::size_t> stablePrefix,
    std::size_t anchorCount)
{
    const auto count=edges ? edges->size() : 0;
    const auto stable=std::min(count,stablePrefix.value_or(count>2 ? count-2 : 0));
    if (!scene_.selectionPathPreview || selectionPreview_ != edges || stable<scene_.selectionStablePrefix)
        ++scene_.selectionPathEpoch;
    scene_.selectionPathPreview = true;
    selectionPreview_ = std::move(edges);
    scene_.selectionEdges = selectionPreview_;
    scene_.selectionStablePrefix=stable;
    scene_.selectionAnchorCount=std::min(count,anchorCount);
    scene_.selectionClosingEdges=std::min(count-scene_.selectionAnchorCount,closingEdges);
    std::shared_ptr<const std::vector<core::SelectionEdge>> retained;
    if (operation!=core::SelectionOperation::Replace && scene_.document.selection) {
        const auto& mask=scene_.document.selection;
        retained={mask,&mask->boundaryEdges()};
    }
    if (retained!=scene_.selectionRetainedEdges) {
        scene_.selectionRetainedEdges=std::move(retained); ++scene_.selectionRetainedRevision;
    }
    ++scene_.selectionEdgesRevision;
    scheduleFrame();
}
void CanvasWindow::setSelectionMaskPreview(core::SelectionState incoming, core::SelectionState original,
    core::SelectionOperation operation)
{
    const std::shared_ptr<const std::vector<core::SelectionEdge>> edges {incoming, &incoming->boundaryEdges()};
    setSelectionPathPreview(edges, operation, 0, 0);
    // Refinement retains the click operation's original selection, not the
    // previous committed fuzziness result. Both masks are immutable caches.
    std::shared_ptr<const std::vector<core::SelectionEdge>> retained;
    if (original && operation != core::SelectionOperation::Replace)
        retained = {original, &original->boundaryEdges()};
    if (retained != scene_.selectionRetainedEdges) {
        scene_.selectionRetainedEdges = std::move(retained); ++scene_.selectionRetainedRevision;
    }
}
void CanvasWindow::refreshSelectionEdges()
{
    auto edges = selectionPreview_;
    if (!edges && scene_.document.selection) {
        const auto& mask = scene_.document.selection;
        edges = std::shared_ptr<const std::vector<core::SelectionEdge>>(mask, &mask->boundaryEdges());
    }
    if (edges != scene_.selectionEdges) {
        scene_.selectionEdges = std::move(edges);
        ++scene_.selectionEdgesRevision;
    }
}
void CanvasWindow::updateSelectionAnimation()
{
    bool visible = isExposed() && isVisible() && hostPresentationAvailable_
        && !presentationSuppressedForResize_ && !resizeCommitPending_
        && ((scene_.selectionRetainedEdges && !scene_.selectionRetainedEdges->empty())
            || (!scene_.selectionPathPreview && scene_.selectionEdges && !scene_.selectionEdges->empty()));
    if (visible) {
        const auto a = documentPositionForLogical({0,0});
        const auto b = documentPositionForLogical({double(width()),double(height())});
        const double margin = 2.0 / std::max(zoom(), 0.0001);
        const auto& edges=scene_.selectionPathPreview ? scene_.selectionRetainedEdges : scene_.selectionEdges;
        visible = std::any_of(edges->begin(), edges->end(), [&](const auto& e) {
            if (std::max(e.from.x, e.to.x) < a.x-margin || std::min(e.from.x, e.to.x) > b.x+margin
                || std::max(e.from.y, e.to.y) < a.y-margin || std::min(e.from.y, e.to.y) > b.y+margin)
                return false;
            // A slanted lasso edge can have overlapping bounds while missing
            // the viewport entirely. Clip the segment, not only its AABB.
            double enter = 0, leave = 1;
            const auto axis = [&](double origin, double delta, double low, double high) {
                if (delta == 0) return origin >= low && origin <= high;
                double start = (low-origin)/delta, end = (high-origin)/delta;
                if (start > end) std::swap(start, end);
                enter = std::max(enter, start); leave = std::min(leave, end);
                return enter <= leave;
            };
            return axis(e.from.x, e.to.x-e.from.x, a.x-margin, b.x+margin)
                && axis(e.from.y, e.to.y-e.from.y, a.y-margin, b.y+margin);
        });
    }
    if (visible && !selectionTimer_.isActive()) selectionTimer_.start();
    if (!visible) selectionTimer_.stop();
}
}
