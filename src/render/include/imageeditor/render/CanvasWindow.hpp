#pragma once

// Vulkan must be included before Qt's Vulkan headers so this target can use
// loader prototypes while Qt owns the cross-platform instance and surface.
#include <vulkan/vulkan.h>

#include "imageeditor/render/CanvasScene.hpp"
#include "imageeditor/render/RendererStats.hpp"
#include "imageeditor/core/BrushEngine.hpp"
#include "imageeditor/core/ColorSampler.hpp"

#include <QPointF>
#include <QStringList>
#include <QTimer>
#include <QWindow>
#include <QCursor>
#include <map>

#include <cstdint>
#include <functional>
#include <memory>

class QExposeEvent;
class QFocusEvent;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;
class QTabletEvent;
class QWheelEvent;

namespace imageeditor::render {

class VulkanCanvasBackend;

class CanvasWindow final : public QWindow {
public:
    CanvasWindow();
    ~CanvasWindow() override;

    void setDocument(core::DocumentSnapshot document, bool fitToView);
    std::function<void(const core::DocumentSnapshot&)> onDocumentPresentationChanged;
    void setPixelPreview(bool enabled,std::shared_ptr<const core::RasterSurface> pixels = {})
    {scene_.pixelPreviewEnabled=enabled;scene_.pixelPreview=std::move(pixels);scheduleFrame();}
    std::function<void()> onFilterPreparationRequested;
    void setFilterBypassLayer(std::optional<core::LayerId> layer)
    { if(scene_.filterBypassLayer!=layer){scene_.filterBypassLayer=layer;scheduleFrame();} }
    void setEffectBypassLayer(std::optional<core::LayerId> layer)
    { if(scene_.effectBypassLayer!=layer){scene_.effectBypassLayer=layer;scheduleFrame();} }
    void setAdjustmentBypassLayer(std::optional<core::LayerId> layer)
    {
        if (scene_.adjustmentBypassLayer == layer) return;
        scene_.adjustmentBypassLayer = layer;
        scheduleFrame();
    }
    // One presentation seam for provisional editable layers. Snapshot producers
    // (including text-density refreshes) cannot accidentally erase a tool draft.
    std::function<void(core::DocumentSnapshot&)> onPrepareDocumentSnapshot;
    void setActiveTool(core::ToolId tool);
    void setCropPreviewLayer(std::optional<core::LayerId> layer)
    {if(scene_.cropPreviewLayer!=layer){scene_.cropPreviewLayer=layer;scheduleFrame();}}
    void setBrushCursor(const core::BrushSettings& settings);
    void setQuickSelectionCursor(bool quick, double diameter);
    void setCloneSource(std::optional<core::Vec2d> anchor, std::optional<core::Vec2d> offset)
    {scene_.cloneSourceAnchor=anchor;scene_.cloneSourceOffset=offset;scheduleFrame();}
    std::function<void(core::Vec2d)> onCloneSourcePicked;
    void setSpotHealActive(bool active) { scene_.spotHealActive=active; }
    void setRepairProcessing(bool processing) { scene_.repairProcessing=processing; }
    void setRepairRegion(std::shared_ptr<const std::vector<core::SelectionEdge>> edges)
    {
        scene_.repairRegionEdges=std::move(edges);
        ++scene_.repairRegionRevision;
        scheduleFrame();
    }
    // Receives the exact orientation already resolved on the painted dab.
    // Follow-direction cursor rendering must never infer a second tangent from
    // raw window-system events.
    void setResolvedBrushCursorAngle(double angleDegrees);
    void setConstrainedBrushPosition(std::optional<core::Vec2d> position)
    { scene_.constrainedBrushPosition = position; }
    void setWorkingColor(core::Rgba8 color);
    void setTemporaryEyedropper(bool held);
    void cancelColorSampling();
    void refreshColorSample();
    void setTransformOverlay(std::optional<TransformOverlay> overlay);
    void setOverlayAccent(core::Rgba8 accent);
    void setCanvasColors(core::Rgba8 background, core::Rgba8 light, core::Rgba8 dark);
    void setPointerTooltip(std::string text);
    void setPointerTooltipArea(core::RectI logicalArea);
    void setPointerSizeTooltip(core::Extent2d size);
    [[nodiscard]] std::array<core::Vec2d, 8> logicalTransformHandles() const;
    [[nodiscard]] bool transformDragging() const noexcept { return transforming_; }
    void updateTransformModifiers(Qt::KeyboardModifiers modifiers);
    void setSnapGuides(core::SnapGuides);
    void cancelTransformInput();
    enum class ShapePress { Ignore, Create, Move };
    std::function<ShapePress(core::Vec2d, Qt::KeyboardModifiers)> onShapePressed;
    std::function<void(core::Vec2d, Qt::KeyboardModifiers)> onShapeMoved;
    std::function<void(bool)> onShapeEnded;
    std::function<void(core::Vec2d)> onShapeCloseRequested;
    // activeOnly distinguishes a passive transform box from actual shape body.
    std::function<bool(core::Vec2d, bool activeOnly)> onShapeHit;
    void setShapeConstructionActive(bool);
    void cancelShapeInput();
    void updateShapeModifiers(Qt::KeyboardModifiers);
    void cancelSelectionInput();
    void setSelectionOperation(core::SelectionOperation);
    void updateSelectionModifiers(Qt::KeyboardModifiers);
    void setSelectionPreview(std::shared_ptr<const std::vector<core::SelectionEdge>>);
    void setSelectionMaskPreview(core::SelectionState incoming, core::SelectionState original,
        core::SelectionOperation);
    // UI-thread-owned path: only the last real segment and closing edge mutate;
    // all older segments form an immutable prefix. Publish after each change.
    void setSelectionPathPreview(std::shared_ptr<const std::vector<core::SelectionEdge>>,
        core::SelectionOperation operation = core::SelectionOperation::Replace,
        std::size_t closingEdges = 1, std::optional<std::size_t> stablePrefix = {},
        std::size_t anchorCount = 0);
    [[nodiscard]] bool selectionDragging() const noexcept { return selecting_; }
    [[nodiscard]] bool selectionConstructionActive() const noexcept { return selectionConstruction_; }
    void setSelectionConstructionActive(bool active);
    [[nodiscard]] bool selectionAnimationActive() const noexcept { return selectionTimer_.isActive(); }
    void fitDocumentToView();
    void restoreDocumentView(core::ViewportState, std::optional<core::MeasurementLine> = {});
    void setDocumentResources(std::uint64_t instance, std::vector<std::weak_ptr<const core::RasterSurface>> sources)
    { scene_.documentInstance = instance; scene_.retainedSources = std::move(sources); }
    void resetTo100Percent();
    void scheduleFrame();
    void setTextQuads(std::vector<CanvasScene::TextQuad> quads) { scene_.textQuads=std::move(quads); scheduleFrame(); }
    void cancelTextInput();
    std::function<bool(core::Vec2d,Qt::KeyboardModifiers,bool)> onTextPressed;
    std::function<void(core::Vec2d)> onTextDragged;
    std::function<bool(QEvent*)> onTextEvent;
    std::function<void()> onViewportUpdated;
    // Shell and native-window key recipients share one momentary pan state.
    void setSpacePanHeld(bool held);
    // Standalone canvas clients retain Space; the editor owns configurable keys.
    void setExternalShortcutRouting(bool enabled) { externalShortcutRouting_ = enabled; }
    void cancelPanInput();
    [[nodiscard]] bool spacePanHeld() const noexcept { return spaceHeld_; }
    [[nodiscard]] bool panDragging() const noexcept { return panning_; }
    [[nodiscard]] bool pointerGestureActive() const noexcept;
    [[nodiscard]] bool measureActive() const noexcept;
    [[nodiscard]] bool measureDragging() const noexcept { return measuring_; }
    [[nodiscard]] bool temporaryMeasureActive() const noexcept { return temporaryMeasure_; }
    bool setTemporaryMeasure(bool held);
    void cancelMeasureInput();
    void clearMeasurement();
    void updateMeasureModifiers(Qt::KeyboardModifiers);
    void setAdvancedMeasurementReadout(bool enabled);
    void setLayerOutlineTargets(std::span<const core::LayerId>, bool reveal = false);
    void setLayerOutlinesVisible(bool enabled);
    [[nodiscard]] bool layerOutlinesVisible() const noexcept { return layerOutlinesVisible_; }
    void setLayerOutlineColor(core::Rgba8);
    void dismissLayerOutlines();
    void setCapturedAdjustmentRegion(std::optional<core::LayerId>, core::AdjustmentType);
    void setCapturedFilterRegion(std::optional<core::LayerId>, core::SpatialFilterType);
    // Notification only: does not consume or replace a tool's pointer gesture.
    std::function<void()> onOutsideDocumentPressed;
    [[nodiscard]] bool advancedMeasurementReadout() const noexcept { return advancedMeasurementReadout_; }
    std::function<void(bool)> onTemporaryMeasureChanged;
    // View observers (e.g. ruler ticks) never mutate the document or its caches.
    std::function<void()> onViewStateChanged;

    // Called by the QWidget host before it applies a live layout resize. A
    // Wayland subsurface is not clipped to its parent, so the prior Vulkan
    // buffer must be unmapped before its native geometry can overlap a panel.
    void prepareForHostResize();
    // The embedding QWidget reports ancestor visibility explicitly because a
    // native child window can remain locally visible while its shell is hidden.
    void setHostPresentationAvailable(bool available);

    [[nodiscard]] const CanvasScene& scene() const noexcept { return scene_; }
    [[nodiscard]] RendererStats rendererStats() const;
    [[nodiscard]] double zoom() const noexcept { return scene_.viewport.zoom(); }
    [[nodiscard]] core::Vec2d documentPositionForLogical(
        QPointF logicalPosition) const noexcept;
    [[nodiscard]] bool lastPanGrabSucceeded() const noexcept { return lastPanGrabSucceeded_; }
    [[nodiscard]] bool presentationSuppressedForResize() const noexcept
    {
        return presentationSuppressedForResize_;
    }

    std::function<void(double)> onZoomChanged;
    std::function<void(QString)> onStatusMessage;
    std::function<void(QStringList)> onFilesDropped;
    std::function<bool(const core::NormalizedPointerSample&)> onBrushStrokeBegan;
    std::function<bool(const core::NormalizedPointerSample&)> onBrushStrokeMoved;
    std::function<bool(const core::NormalizedPointerSample&)> onBrushStrokeEnded;
    std::function<void()> onBrushStrokeCancelled;
    std::function<core::ColorSample(core::Vec2d)> onColorSampleRequested;
    std::function<void(core::Rgba8)> onColorPicked;
    std::function<void(core::Vec2d)> onFillRequested;
    std::function<bool(core::TransformHandle, core::Vec2d)> onTransformBegan;
    std::function<bool(core::Vec2d, core::TransformModifiers)> onTransformMoved;
    std::function<void()> onTransformEnded;
    std::function<void()> onTransformDragCancelled;
    // Canvas-only explicit completion; does not synthesize keyboard input.
    std::function<bool()> onFinishRequested;
    void finishPointerGestureForCommit();
    [[nodiscard]] Qt::KeyboardModifiers pointerModifiers() const noexcept { return selectionModifiers_; }
    std::function<bool(core::Vec2d, Qt::KeyboardModifiers)> onSelectionBegan;
    std::function<void(const core::NormalizedPointerSample&)> onSelectionSample;
    std::function<void(core::Vec2d)> onSelectionMoved;
    std::function<void(bool cancel)> onSelectionEnded;
    std::function<void(core::Vec2d)> onSelectionCloseRequested;

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;

private:
    bool externalShortcutRouting_ {false};
    Qt::MouseButtons completedGestureButtons_ {};
    bool suppressFinishContextMenu_ {false};
    // Embedded native windows receive a resize for nearly every pointer move
    // while a QWidget dock separator is dragged. Rebuilding a Vulkan
    // swapchain synchronously for each one stalls the GUI thread, so resize
    // events only update the logical scene and restart this settle timer.
    // The final surface extent is committed once the burst goes quiet.
    static constexpr int kResizeSettleDelayMs = 50;

    [[nodiscard]] core::Extent2d documentExtent() const noexcept;
    [[nodiscard]] core::Extent2d viewportExtent() const noexcept;
    void deferSwapchainResize();
    void commitDeferredResize();
    void suppressPresentationForResize();
    void restorePresentationAfterResize();
    [[nodiscard]] bool nativeParentCanShowCanvas() const noexcept;
    void stopPanning();
    void stopBrushInput(bool cancel);
    void stopColorPick();
    void notifyOutsideDocumentPress(QPointF position);
    void beginTransformInput(QPointF position, std::optional<core::TransformHandle> forced = {});
    void beginShapeInput(QPointF, Qt::KeyboardModifiers);
    void finishShapeInput(bool cancel);
    void moveTransformInput(QPointF position, Qt::KeyboardModifiers modifiers);
    void finishTransformInput(bool cancel);
    void updateTransformHover();
    [[nodiscard]] core::TransformHandle hitTransformOverlay(core::Vec2d logical) const;
    void updateTransformTooltip();
    void beginSelectionInput(QPointF, Qt::KeyboardModifiers);
    void finishSelectionInput(bool);
    void refreshSelectionEdges();
    void updateSelectionAnimation();
    void applyColorSample();
    [[nodiscard]] core::NormalizedPointerSample mouseSample(
        const QMouseEvent& event) const noexcept;
    [[nodiscard]] core::NormalizedPointerSample tabletSample(
        const QTabletEvent& event) const noexcept;
    void resetBrushCursorAngleForHover();
    void updateCursorForTool();
    const QCursor& rotationCursor();
    const QCursor& fillCursor();
    std::map<int, QCursor> fillCursors_;
    void selectRotationCorner(core::Vec2d logicalPosition);
    // At most two display scales, 72 lazily populated orientations each.
    std::map<int, std::map<int, QCursor>> rotationCursors_;
    int rotationCursorDpr_ {0};
    std::size_t rotationCorner_ {0};
    void publishZoom();
    void beginMeasureInput(QPointF);
    void moveMeasureInput(QPointF, Qt::KeyboardModifiers);
    void resolveMeasurePointer();
    void finishMeasureInput(bool cancel, bool suppressRelease = false);
    void refreshMeasurement();
    [[nodiscard]] int measureEndpointAt(core::Vec2d logical) const;

    bool measuring_ {false}, temporaryMeasure_ {false}, explicitMeasureGrab_ {false};
    bool measureConstrained_ {false}, advancedMeasurementReadout_ {false};
    void refreshLayerOutlines();
    void refreshCapturedAdjustmentRegion();
    std::optional<core::LayerId> capturedRegionTarget_;
    core::AdjustmentType capturedRegionType_ {core::AdjustmentType::Exposure};
    std::optional<core::SpatialFilterType> capturedFilterType_;
    core::SelectionState capturedRegionMask_;
    core::AffineTransform capturedRegionMapping_;
    std::vector<core::LayerId> layerOutlineTargets_;
    bool layerOutlinesVisible_ {true}, layerOutlinesDismissed_ {false};
    core::Vec2d measurePointerDocument_;
    int measureEndpoint_ {1};
    std::optional<core::MeasurementLine> regularMeasurement_, temporaryMeasurement_, measureStart_;

    CanvasScene scene_;
    std::unique_ptr<VulkanCanvasBackend> backend_;
    QTimer resizeCommitTimer_;
    QTimer selectionTimer_;
    bool selecting_ {false};
    bool shaping_ {false}, shapeConstruction_ {false}, explicitShapeGrab_ {false};
    bool selectingText_ {false}, explicitTextGrab_ {false};
    bool selectionConstruction_ {false};
    bool movingSelection_ {false};
    core::SelectionOperation selectionOperation_ {core::SelectionOperation::Replace};
    Qt::KeyboardModifiers selectionModifiers_ {};
    bool explicitSelectionGrab_ {false};
    std::shared_ptr<const std::vector<core::SelectionEdge>> selectionPreview_;
    bool framePending_ {false};
    bool resizeCommitPending_ {false};
    std::uint64_t deferredResizeEventCount_ {0};
    std::uint64_t resizeCommitCount_ {0};
    std::uint64_t resizePresentationSuspendCount_ {0};
    std::uint64_t resizePresentationResumeCount_ {0};
    bool surfaceAvailable_ {false};
    bool presentationSuppressedForResize_ {false};
    bool hostPresentationAvailable_ {true};
    bool fitPending_ {false};
    bool spaceHeld_ {false};
    bool panning_ {false};
    Qt::MouseButton panButton_ {Qt::NoButton};
    bool explicitPanGrab_ {false};
    bool lastPanGrabSucceeded_ {true};
    bool brushing_ {false};
    bool explicitBrushGrab_ {false};
    bool temporaryEyedropper_ {false};
    bool picking_ {false};
    bool explicitPickerGrab_ {false};
    bool transforming_ {false};
    bool distorting_ {false}; // Latched at corner press, never changed mid-gesture.
    bool explicitTransformGrab_ {false};
    core::Rgba8 workingColor_;
    QPointF lastPointerPosition_;
};

} // namespace imageeditor::render
