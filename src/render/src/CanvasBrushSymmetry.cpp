#include "imageeditor/render/CanvasWindow.hpp"
#include <cmath>

namespace imageeditor::render {
void CanvasWindow::beginBrushSymmetryPlacement()
{
    if (!scene_.brushSymmetryContextEnabled || pointerGestureActive() || scene_.document.canvas.extent.empty() || scene_.repairProcessing
        || (scene_.activeTool!=core::ToolId::Brush && scene_.activeTool!=core::ToolId::Eraser)) return;
    auto state=scene_.document.brushSymmetry;
    if (!state.center) state.center=core::Vec2d{double(scene_.document.canvas.extent.width)/2,
        double(scene_.document.canvas.extent.height)/2};
    state.enabled=true;
    scene_.brushSymmetryPlacement=state;
    setSnapGuides({});
    if (onBrushSymmetryPlacementBegan) onBrushSymmetryPlacementBegan();
    cancelColorSampling();
    updateCursorForTool();scheduleFrame();
}
void CanvasWindow::setBrushSymmetryContextEnabled(bool enabled)
{
    if (scene_.brushSymmetryContextEnabled==enabled) return;
    scene_.brushSymmetryContextEnabled=enabled;
    if (!enabled) cancelBrushSymmetryPlacement();
    scheduleFrame();
}
void CanvasWindow::moveBrushSymmetryPlacement(QPointF point, Qt::KeyboardModifiers modifiers)
{
    if (!scene_.brushSymmetryPlacement) return;
    const auto center=documentPositionForLogical(point);
    if (!std::isfinite(center.x) || !std::isfinite(center.y)
        || std::abs(center.x)>1e9 || std::abs(center.y)>1e9) return;
    selectionModifiers_=modifiers;
    const auto snapped=onBrushSymmetryTargetMoved ? onBrushSymmetryTargetMoved(center,modifiers)
        : core::SnapResult{center,{}};
    scene_.brushSymmetryPlacement->center=snapped.translation;
    setSnapGuides(snapped.guides);
    scheduleFrame();
}
void CanvasWindow::updateBrushSymmetryModifiers(Qt::KeyboardModifiers modifiers)
{
    if (scene_.brushSymmetryPlacement && scene_.cursorInside)
        moveBrushSymmetryPlacement({scene_.cursorLogical.x,scene_.cursorLogical.y},modifiers);
}
void CanvasWindow::finishBrushSymmetryPlacement(bool cancel)
{
    if (!scene_.brushSymmetryPlacement) return;
    const auto state=*scene_.brushSymmetryPlacement;
    scene_.brushSymmetryPlacement.reset();
    setSnapGuides({});
    if (cancel && symmetryTargetDragging_) completedGestureButtons_ |= Qt::LeftButton;
    symmetryTargetDragging_=false;
    if (explicitSymmetryGrab_) {explicitSymmetryGrab_=false;setMouseGrabEnabled(false);}
    updateCursorForTool();scheduleFrame();
    if (!cancel && onBrushSymmetryPlaced) onBrushSymmetryPlaced(state);
    if (onBrushSymmetryPlacementEnded) onBrushSymmetryPlacementEnded();
}
void CanvasWindow::cancelBrushSymmetryPlacement() { finishBrushSymmetryPlacement(true); }
} // namespace imageeditor::render
