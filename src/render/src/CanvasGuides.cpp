#include "imageeditor/render/CanvasWindow.hpp"
#include <QGuiApplication>
#include <algorithm>
#include <cmath>
#include <limits>

namespace imageeditor::render {
void CanvasWindow::setGuideOptions(core::GuideOrientation orientation, unsigned count)
{
    guideOrientation_ = orientation;
    guideCount_ = std::clamp(count, 1U, 1000U);
    guidePlacement_ = true;
    guidePlacementBlockedAt_.reset();
    refreshGuidePreview();
    updateCursorForTool();
    scheduleFrame();
}
void CanvasWindow::setDocumentGuidesVisible(bool visible)
{
    if (!visible)
        cancelGuideInput();
    scene_.documentGuidesVisible = visible;
    refreshGuidePreview();
    updateCursorForTool();
    scheduleFrame();
}
std::optional<std::uint64_t> CanvasWindow::guideAt(QPointF logical) const
{
    if (!scene_.documentGuidesVisible || scene_.activeTool != core::ToolId::Guides)
        return { };
    const auto p = documentPositionForLogical(logical);
    const auto extent = scene_.document.canvas.extent;
    if (p.x < 0 || p.y < 0 || p.x > extent.width || p.y > extent.height)
        return { };
    double nearest = 6.;
    std::optional<std::uint64_t> result;
    for (const auto& guide : scene_.document.guides.items) {
        const double distance
            = std::abs((guide.orientation == core::GuideOrientation::Horizontal ? p.y : p.x) - guide.position)
            * zoom();
        if (distance < nearest || (distance == nearest && (!result || guide.id < *result))) {
            nearest = distance;
            result = guide.id;
        }
    }
    return result;
}
void CanvasWindow::refreshGuidePreview()
{
    scene_.guidePreview.clear();
    scene_.highlightedGuide.reset();
    scene_.draggedGuide.reset();
    if (!scene_.documentGuidesVisible || scene_.activeTool != core::ToolId::Guides)
        return;
    if (guideDrag_) {
        scene_.guidePreview.push_back(guideDrag_->guide);
        scene_.draggedGuide = guideDrag_->guide.id;
        return;
    }
    if (!scene_.cursorInside || panning_ || spaceHeld_ || temporaryMeasure_)
        return;
    const QPointF logical(scene_.cursorLogical.x, scene_.cursorLogical.y);
    scene_.highlightedGuide = guideAt(logical);
    if (scene_.highlightedGuide || !guidePlacement_)
        return;
    if (guidePlacementBlockedAt_ && (logical - *guidePlacementBlockedAt_).manhattanLength() < 1)
        return;
    const auto point = documentPositionForLogical(logical);
    const auto extent = scene_.document.canvas.extent;
    if (point.x < 0 || point.y < 0 || point.x > extent.width || point.y > extent.height)
        return;
    for (const auto value : core::guidePlacementPositions(guideOrientation_, guideCount_, extent))
        scene_.guidePreview.push_back({ 0, guideOrientation_, value });
}
void CanvasWindow::beginGuideInput(QPointF logical, Qt::MouseButton button)
{
    if (!scene_.documentGuidesVisible || guideDrag_)
        return;
    const auto hit = guideAt(logical);
    if (hit) {
        const auto& guides = scene_.document.guides;
        const auto it = std::find_if(
            guides.items.begin(), guides.items.end(), [&](const auto& g) { return g.id == *hit; });
        if (it == guides.items.end())
            return;
        if (button == Qt::RightButton) {
            guidePlacementBlockedAt_ = logical;
            auto after = guides;
            std::erase_if(after.items, [&](const auto& g) { return g.id == *hit; });
            if (onGuidesChanged)
                onGuidesChanged(guides, std::move(after), "Remove guide");
        } else if (button == Qt::LeftButton) {
            const auto point = documentPositionForLogical(logical);
            const auto coordinate = it->orientation == core::GuideOrientation::Horizontal ? point.y : point.x;
            guideDrag_ = GuideDrag { guides, *it, coordinate - it->position, it->position };
            setSnapGuides({});
            if (onGuideDragBegan)
                onGuideDragBegan(*it);
            if (QGuiApplication::platformName() != QStringLiteral("wayland"))
                explicitGuideGrab_ = setMouseGrabEnabled(true);
        }
    } else if (button == Qt::LeftButton && guidePlacement_ && !scene_.guidePreview.empty()) {
        const auto before = scene_.document.guides;
        auto after = before;
        for (auto guide : scene_.guidePreview) {
            if (std::any_of(after.items.begin(), after.items.end(), [&](const auto& old) {
                    return old.orientation == guide.orientation
                        && std::abs(old.position - guide.position) < 1e-9;
                }))
                continue;
            if (after.nextId == std::numeric_limits<std::uint64_t>::max())
                return;
            guide.id = after.nextId++;
            after.items.push_back(guide);
        }
        if (onGuidesChanged && before != after)
            onGuidesChanged(before, std::move(after), "Add guides");
    }
    refreshGuidePreview();
    updateCursorForTool();
    scheduleFrame();
}
void CanvasWindow::moveGuideInput(QPointF logical, Qt::KeyboardModifiers modifiers)
{
    selectionModifiers_ = modifiers;
    if (guidePlacementBlockedAt_ && (logical - *guidePlacementBlockedAt_).manhattanLength() >= 1)
        guidePlacementBlockedAt_.reset();
    if (guideDrag_) {
        const auto point = documentPositionForLogical(logical);
        const auto extent = scene_.document.canvas.extent;
        guideDrag_->guide.position = guideDrag_->guide.orientation == core::GuideOrientation::Horizontal
            ? std::clamp(point.y - guideDrag_->grabOffset, 0., double(extent.height))
            : std::clamp(point.x - guideDrag_->grabOffset, 0., double(extent.width));
        guideDrag_->moved |= std::abs(guideDrag_->guide.position - guideDrag_->initialPosition) > 1e-9;
        if (guideDrag_->moved && onGuideDragged) {
            const auto snapped = onGuideDragged(guideDrag_->guide, modifiers);
            guideDrag_->guide.position = guideDrag_->guide.orientation == core::GuideOrientation::Horizontal
                ? snapped.position.y : snapped.position.x;
            setSnapGuides(snapped.guides);
        }
    }
    refreshGuidePreview();
    updateCursorForTool();
    scheduleFrame();
}
void CanvasWindow::updateGuideModifiers(Qt::KeyboardModifiers modifiers)
{
    if (guideDrag_)
        moveGuideInput({scene_.cursorLogical.x, scene_.cursorLogical.y}, modifiers);
}
void CanvasWindow::finishGuideInput(bool cancel)
{
    if (!guideDrag_)
        return;
    auto drag = std::move(*guideDrag_);
    guideDrag_.reset();
    setSnapGuides({});
    if (explicitGuideGrab_) {
        explicitGuideGrab_ = false;
        setMouseGrabEnabled(false);
    }
    if (!cancel && onGuidesChanged) {
        auto after = drag.before;
        for (auto& guide : after.items)
            if (guide.id == drag.guide.id)
                guide = drag.guide;
        if (after != drag.before)
            onGuidesChanged(drag.before, std::move(after), "Move guide");
    }
    refreshGuidePreview();
    updateCursorForTool();
    scheduleFrame();
}
void CanvasWindow::cancelGuideInput(bool cancelPlacement)
{
    const bool dragging = guideDrag_.has_value();
    finishGuideInput(true);
    if (dragging)
        completedGestureButtons_ |= Qt::LeftButton;
    if (cancelPlacement)
        guidePlacement_ = false;
    refreshGuidePreview();
    updateCursorForTool();
    scheduleFrame();
}
}
