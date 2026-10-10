#include "imageeditor/render/CanvasWindow.hpp"
#include <QContextMenuEvent>
#include "imageeditor/render/PointerTooltip.hpp"

#include "VulkanCanvasBackend.hpp"

#include <QEnterEvent>
#include <QGuiApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QExposeEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMimeData>
#include <QMetaObject>
#include <QPlatformSurfaceEvent>
#include <QPointingDevice>
#include <QResizeEvent>
#include <QScreen>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QUrl>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace imageeditor::render {
namespace {

bool platformProvidesImplicitPointerGrab()
{
    // Wayland guarantees an implicit pointer grab for the lifetime of a button
    // press. Its Qt platform plugin intentionally rejects explicit grabs for
    // ordinary windows (they are reserved for popups).
    return QGuiApplication::platformName() == QStringLiteral("wayland");
}

std::uint32_t normalizedButtons(Qt::MouseButtons buttons) noexcept
{
    std::uint32_t result = core::PointerButtonNone;
    if (buttons.testFlag(Qt::LeftButton)) {
        result |= core::PointerButtonPrimary;
    }
    if (buttons.testFlag(Qt::RightButton)) {
        result |= core::PointerButtonSecondary;
    }
    if (buttons.testFlag(Qt::MiddleButton)) {
        result |= core::PointerButtonMiddle;
    }
    if (buttons.testFlag(Qt::BackButton) || buttons.testFlag(Qt::ForwardButton)) {
        result |= core::PointerButtonBarrel;
    }
    return result;
}

std::uint32_t normalizedModifiers(Qt::KeyboardModifiers modifiers) noexcept
{
    std::uint32_t result = core::PointerModifierNone;
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        result |= core::PointerModifierShift;
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        result |= core::PointerModifierControl;
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        result |= core::PointerModifierAlt;
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
        result |= core::PointerModifierMeta;
    }
    return result;
}

core::PointerType normalizedPointerType(QPointingDevice::PointerType type) noexcept
{
    switch (type) {
    case QPointingDevice::PointerType::Generic:
        return core::PointerType::Mouse;
    case QPointingDevice::PointerType::Pen:
        return core::PointerType::Pen;
    case QPointingDevice::PointerType::Eraser:
        return core::PointerType::Eraser;
    case QPointingDevice::PointerType::Finger:
        return core::PointerType::Touch;
    default:
        return core::PointerType::Unknown;
    }
}

} // namespace

CanvasWindow::CanvasWindow()
    : backend_(std::make_unique<VulkanCanvasBackend>(*this))
{
    setObjectName(QStringLiteral("VulkanCanvasWindow"));
    setTitle(QStringLiteral("ImageEditor Canvas"));
    setSurfaceType(QSurface::VulkanSurface);

    resizeCommitTimer_.setSingleShot(true);
    resizeCommitTimer_.setInterval(kResizeSettleDelayMs);
    connect(&resizeCommitTimer_, &QTimer::timeout, this, [this] {
        commitDeferredResize();
    });
    selectionTimer_.setInterval(80);
    connect(&selectionTimer_, &QTimer::timeout, this, [this] {
        scene_.selectionPhase = std::fmod(scene_.selectionPhase + 1.0, 8.0);
        scheduleFrame();
    });

    connect(this, &QWindow::screenChanged, this, [this](QScreen* newScreen) {
        scene_.devicePixelRatio = devicePixelRatio();
        updateCursorForTool();
        suppressPresentationForResize();
        deferSwapchainResize();
        if (onStatusMessage && newScreen) {
            onStatusMessage(QStringLiteral("Canvas moved to %1 · %2 Hz · DPR %3")
                .arg(newScreen->name())
                .arg(newScreen->refreshRate(), 0, 'f', 0)
                .arg(devicePixelRatio(), 0, 'f', 2));
        }
        if (newScreen) {
            qInfo() << "Canvas screen transition:" << newScreen->name()
                    << newScreen->geometry() << newScreen->refreshRate()
                    << "Hz DPR" << devicePixelRatio();
        }
    });
}

CanvasWindow::~CanvasWindow()
{
    onViewStateChanged = {};
    onTemporaryMeasureChanged = {};
    finishMeasureInput(true);
    // The owning MainWindow cancels its stroke before QObject child teardown;
    // never call back into a partially destructed owner from this destructor.
    stopBrushInput(false);
    onSelectionEnded = {};
    onSelectionCloseRequested = {};
    finishSelectionInput(true);
    selectionTimer_.stop();
    onTransformEnded = {};
    finishTransformInput(false);
    cancelTextInput();
    stopColorPick();
    stopPanning();
    backend_->shutdown();
}

void CanvasWindow::setCanvasColors(core::Rgba8 background, core::Rgba8 light, core::Rgba8 dark)
{
    if (scene_.canvasBackground == background && scene_.checkerLight == light && scene_.checkerDark == dark) return;
    scene_.canvasBackground = background; scene_.checkerLight = light; scene_.checkerDark = dark;
    scheduleFrame(); // Styling only: reuse all document textures and swapchain resources.
}

void CanvasWindow::setDocument(core::DocumentSnapshot document, bool fitToView)
{
    if (document.documentRevision != scene_.document.documentRevision) setSnapGuides({});
    if (fitToView) { cancelMeasureInput(); regularMeasurement_.reset(); temporaryMeasurement_.reset(); }
    if(onPrepareDocumentSnapshot)onPrepareDocumentSnapshot(document);
    scene_.document = std::move(document);
    if(onDocumentPresentationChanged)onDocumentPresentationChanged(scene_.document);
    refreshCapturedAdjustmentRegion();
    refreshLayerOutlines();
    refreshSelectionEdges();
    fitPending_ = fitToView;
    if (fitToView) {
        fitDocumentToView();
    }
    refreshColorSample();
    if (measureActive()) refreshMeasurement();
    scheduleFrame();
}

void CanvasWindow::setActiveTool(core::ToolId tool)
{
    if (tool != scene_.activeTool) {
        setSnapGuides({});
        cancelMeasureInput();
        setTemporaryMeasure(false);
    }
    if (tool != scene_.activeTool) cancelShapeInput();
    if (tool != scene_.activeTool) setPointerTooltip({});
    if (tool != scene_.activeTool) cancelSelectionInput();
    if (tool != scene_.activeTool) {
        cancelColorSampling();
    }
    if (tool != scene_.activeTool && brushing_) {
        stopBrushInput(true);
    }
    scene_.activeTool = tool;
    refreshLayerOutlines();
    refreshMeasurement();
    refreshColorSample();
    updateCursorForTool();
    scheduleFrame();
}

std::array<core::Vec2d, 8> CanvasWindow::logicalTransformHandles() const
{
    if (!scene_.transformOverlay) return {};
    auto handles = scene_.transformOverlay->handles();
    for (auto& point : handles)
        point = scene_.viewport.documentToViewport(point, documentExtent(), viewportExtent());
    return handles;
}

void CanvasWindow::setTransformOverlay(std::optional<TransformOverlay> overlay)
{
    if (!overlay) cancelTransformInput();
    scene_.transformOverlay = std::move(overlay);
    refreshLayerOutlines();
    updateTransformTooltip();
    updateTransformHover();
    scheduleFrame();
}

core::TransformHandle CanvasWindow::hitTransformOverlay(core::Vec2d logical) const
{
    if (!scene_.transformOverlay)
        return core::TransformHandle::None;
    const auto hit = core::hitTestTransform(logicalTransformHandles(), logical);
    const auto& overlay = *scene_.transformOverlay;
    if (overlay.cropGeometry) {
        if (hit >= core::TransformHandle::TopLeft && hit <= core::TransformHandle::Left)
            return hit;
        const auto inverse = overlay.localToDocument.inverted();
        return inverse && overlay.cropGeometry->contains(inverse->map(documentPositionForLogical({ logical.x, logical.y })))
            ? core::TransformHandle::Move
            : core::TransformHandle::None;
    }
    return hit;
}
void CanvasWindow::updateTransformHover()
{
    if (!transforming_) scene_.transformHighlight = scene_.transformOverlay && scene_.cursorInside
        ? hitTransformOverlay(scene_.cursorLogical)
        : core::TransformHandle::None;
    if(scene_.transformOverlay && !scene_.transformOverlay->rotationEnabled && scene_.transformHighlight==core::TransformHandle::Rotate)
        scene_.transformHighlight=core::TransformHandle::None;
    if (!transforming_ && scene_.activeTool == core::ToolId::Shape) {
        if (shaping_ || shapeConstruction_)
            scene_.transformHighlight = core::TransformHandle::None;
        else if (scene_.transformHighlight == core::TransformHandle::Move
            && (selectionModifiers_.testFlag(Qt::ShiftModifier) || !onShapeHit
                || !onShapeHit(documentPositionForLogical({scene_.cursorLogical.x,scene_.cursorLogical.y}),true)))
            scene_.transformHighlight = core::TransformHandle::None;
    }
    if (!transforming_ && scene_.transformHighlight == core::TransformHandle::Rotate)
        selectRotationCorner(scene_.cursorLogical);
    updateCursorForTool();
}

void CanvasWindow::selectRotationCorner(core::Vec2d position)
{
    const auto handles = logicalTransformHandles();
    auto closest = std::numeric_limits<double>::max();
    for (std::size_t corner = 0; corner < handles.size(); corner += 2) {
        const auto delta = handles[corner] - position;
        const auto distance = std::hypot(delta.x, delta.y);
        if (distance < closest) {
            closest = distance;
            rotationCorner_ = corner;
        }
    }
}

void CanvasWindow::setOverlayAccent(core::Rgba8 accent)
{
    if (scene_.overlayAccent == accent)
        return;
    scene_.overlayAccent = accent;
    scheduleFrame();
}

void CanvasWindow::setPointerTooltip(std::string text)
{
    if (scene_.pointerTooltip == text) return;
    scene_.pointerTooltip = std::move(text);
    scheduleFrame();
}

void CanvasWindow::setPointerSizeTooltip(core::Extent2d size)
{
    setPointerTooltip(pointerSizeText(size));
}

void CanvasWindow::setPointerTooltipArea(core::RectI area)
{
    if (scene_.pointerTooltipArea == area) return;
    scene_.pointerTooltipArea = area;
    if (!scene_.pointerTooltip.empty()) scheduleFrame();
}

void CanvasWindow::updateTransformTooltip()
{
    if (!transforming_ || !scene_.transformOverlay) return;
    const auto& overlay = *scene_.transformOverlay;
    if (scene_.transformHighlight == core::TransformHandle::Rotate) {
        setPointerTooltip(pointerAngleText(overlay.rotationDegrees));
    } else if (overlay.cropGeometry && overlay.chamferHandles
        && scene_.transformHighlight >= core::TransformHandle::TopLeft
        && scene_.transformHighlight <= core::TransformHandle::Left && int(scene_.transformHighlight) % 2 == 0) {
        const auto value = overlay.cropGeometry->resolvedCorners()[std::size_t(scene_.transformHighlight) / 2];
        setPointerTooltip(QStringLiteral("Cut: %1 px").arg(value, 0, 'f', 1).toStdString());
    } else if (scene_.transformHighlight >= core::TransformHandle::TopLeft
        && scene_.transformHighlight <= core::TransformHandle::Left) {
        const auto corners = overlay.frameHandles();
        const auto width = corners[2] - corners[0], height = corners[6] - corners[0];
        setPointerSizeTooltip({std::hypot(width.x, width.y), std::hypot(height.x, height.y)});
    } else {
        setPointerTooltip({});
    }
}

void CanvasWindow::beginTransformInput(QPointF position, std::optional<core::TransformHandle> forced)
{
    if (transforming_ || (!forced && !scene_.transformOverlay && scene_.activeTool != core::ToolId::Move)) return;
    const auto hit = forced ? *forced : scene_.transformOverlay
        ? hitTransformOverlay({position.x(), position.y()})
        : core::TransformHandle::Move;
    if (hit == core::TransformHandle::None || !onTransformBegan) return;
    if(hit==core::TransformHandle::Rotate && scene_.transformOverlay && !scene_.transformOverlay->rotationEnabled)return;
    if (hit == core::TransformHandle::Rotate)
        selectRotationCorner({position.x(), position.y()});
    transforming_ = onTransformBegan(hit, documentPositionForLogical(position));
    distorting_ = transforming_ && scene_.activeTool==core::ToolId::Transform
        && selectionModifiers_.testFlag(Qt::ControlModifier) && hit>=core::TransformHandle::TopLeft
        && hit<=core::TransformHandle::Left && int(hit)%2==0;
    if (transforming_) {
        scene_.transformHighlight = hit;
        updateTransformTooltip();
        if (!platformProvidesImplicitPointerGrab())
            explicitTransformGrab_ = setMouseGrabEnabled(true);
    }
    updateCursorForTool();
    scheduleFrame();
}

void CanvasWindow::moveTransformInput(QPointF position, Qt::KeyboardModifiers modifiers)
{
    selectionModifiers_ = modifiers;
    const auto hit = scene_.transformHighlight;
    distorting_ = transforming_ && scene_.activeTool == core::ToolId::Transform
        && modifiers.testFlag(Qt::ControlModifier) && hit >= core::TransformHandle::TopLeft
        && hit <= core::TransformHandle::Left && int(hit) % 2 == 0;
    if (transforming_ && onTransformMoved && !onTransformMoved(documentPositionForLogical(position),
        {modifiers.testFlag(Qt::ShiftModifier), modifiers.testFlag(Qt::AltModifier), modifiers.testFlag(Qt::ControlModifier)}))
        finishTransformInput(true);
    updateTransformTooltip();
    updateCursorForTool();
}

void CanvasWindow::updateTransformModifiers(Qt::KeyboardModifiers modifiers)
{
    moveTransformInput({scene_.cursorLogical.x, scene_.cursorLogical.y}, modifiers);
}

void CanvasWindow::setSnapGuides(core::SnapGuides guides)
{
    if (scene_.snapGuides == guides) return;
    scene_.snapGuides = std::move(guides);
    scheduleFrame();
}

void CanvasWindow::finishTransformInput(bool cancel)
{
    if (!transforming_) return;
    transforming_ = false;
    distorting_ = false;
    setSnapGuides({});
    setPointerTooltip({});
    if (explicitTransformGrab_) {
        explicitTransformGrab_ = false;
        setMouseGrabEnabled(false);
    }
    if (cancel) { if (onTransformDragCancelled) onTransformDragCancelled(); }
    else if (onTransformEnded) onTransformEnded();
    updateTransformHover();
    scheduleFrame();
}

void CanvasWindow::cancelTransformInput() { finishTransformInput(true); }

void CanvasWindow::finishPointerGestureForCommit()
{
    // Use the last accepted geometry. The completion click must not create a
    // final pointer update/vertex at a different position.
    finishTransformInput(false);
    finishSelectionInput(false);
}

void CanvasWindow::updateShapeModifiers(Qt::KeyboardModifiers modifiers)
{
    selectionModifiers_=modifiers;
    if(scene_.activeTool!=core::ToolId::Shape) return;
    if ((shaping_ || shapeConstruction_) && onShapeMoved)
        onShapeMoved(documentPositionForLogical({scene_.cursorLogical.x,scene_.cursorLogical.y}),modifiers);
    updateTransformHover();
}
void CanvasWindow::setShapeConstructionActive(bool active)
{ shapeConstruction_=active; updateTransformHover(); }
void CanvasWindow::beginShapeInput(QPointF position, Qt::KeyboardModifiers modifiers)
{
    selectionModifiers_=modifiers;
    if(!shapeConstruction_ && scene_.transformOverlay) {
        const auto hit=core::hitTestTransform(logicalTransformHandles(),{position.x(),position.y()});
        if(hit!=core::TransformHandle::None && hit!=core::TransformHandle::Move) {
            beginTransformInput(position,hit); return;
        }
    }
    if(!onShapePressed)return;
    const auto action=onShapePressed(documentPositionForLogical(position),modifiers);
    if(action==ShapePress::Move)beginTransformInput(position,core::TransformHandle::Move);
    else if(action==ShapePress::Create) {
        shaping_=true;
        if(!platformProvidesImplicitPointerGrab())explicitShapeGrab_=setMouseGrabEnabled(true);
    }
    updateTransformHover();scheduleFrame();
}
void CanvasWindow::finishShapeInput(bool cancel)
{
    if(!shaping_ && !(cancel&&shapeConstruction_))return;
    shaping_=false;
    if(cancel)shapeConstruction_=false;
    if(explicitShapeGrab_){explicitShapeGrab_=false;setMouseGrabEnabled(false);}
    if(onShapeEnded)onShapeEnded(cancel);
    updateTransformHover();scheduleFrame();
}
void CanvasWindow::cancelShapeInput(){finishShapeInput(true);}

void CanvasWindow::setWorkingColor(core::Rgba8 color)
{
    workingColor_ = color;
    if (!picking_) {
        scene_.eyedropperReference = color;
    }
    scheduleFrame();
}

void CanvasWindow::setTemporaryEyedropper(bool held)
{
    temporaryEyedropper_ = held;
    refreshColorSample();
}

void CanvasWindow::stopColorPick()
{
    picking_ = false;
    if (explicitPickerGrab_) {
        explicitPickerGrab_ = false;
        setMouseGrabEnabled(false);
    }
    scene_.eyedropperReference = workingColor_;
}

void CanvasWindow::cancelColorSampling()
{
    temporaryEyedropper_ = false;
    stopColorPick();
    refreshColorSample();
}

void CanvasWindow::refreshColorSample()
{
    const bool temporaryPickerTool = scene_.activeTool == core::ToolId::Brush
        || scene_.activeTool == core::ToolId::Eraser || scene_.activeTool == core::ToolId::Fill;
    scene_.eyedropperActive = !measureActive() && !brushing_ && !panning_ && !spaceHeld_
        && (scene_.activeTool == core::ToolId::Eyedropper || picking_
            || (temporaryPickerTool && temporaryEyedropper_));
    scene_.eyedropperSampleValid = false;
    if (scene_.eyedropperActive && scene_.cursorInside && onColorSampleRequested) {
        const auto sample = onColorSampleRequested(documentPositionForLogical(
            {scene_.cursorLogical.x, scene_.cursorLogical.y}));
        scene_.eyedropperSampleValid = sample.available();
        scene_.eyedropperCandidate = sample.color;
    }
    updateCursorForTool();
    scheduleFrame();
}

void CanvasWindow::applyColorSample()
{
    if (scene_.eyedropperSampleValid && onColorPicked) {
        onColorPicked(scene_.eyedropperCandidate);
    }
}

void CanvasWindow::setBrushCursor(const core::BrushSettings& settings)
{
    scene_.brushSizeDocument = std::clamp(settings.sizePixels, 0.25, 4096.0);
    scene_.brushHardness = std::clamp(settings.hardness, 0.0, 1.0);
    scene_.brushTipAspectRatio = std::clamp(
        settings.tip.aspectRatio, 0.05, 1.0);
    scene_.brushTipBaseAngleDegrees = std::isfinite(settings.tip.angleDegrees)
        ? std::remainder(settings.tip.angleDegrees, 360.0) : 0.0;
    scene_.brushTipAngleDegrees = scene_.brushTipBaseAngleDegrees;
    scheduleFrame();
}

void CanvasWindow::setQuickSelectionCursor(bool quick,double diameter)
{
    if(scene_.quickSelectionCursor==quick && scene_.quickSelectionSize==diameter)return;
    scene_.quickSelectionCursor=quick;scene_.quickSelectionSize=diameter;
    if(scene_.activeTool==core::ToolId::SmartSelect){updateCursorForTool();scheduleFrame();}
}

void CanvasWindow::setResolvedBrushCursorAngle(double angleDegrees)
{
    if (!std::isfinite(angleDegrees)) {
        return;
    }
    scene_.brushTipAngleDegrees = std::remainder(angleDegrees, 360.0);
    scheduleFrame();
}

core::Vec2d CanvasWindow::documentPositionForLogical(
    QPointF logicalPosition) const noexcept
{
    return scene_.viewport.viewportToDocument(
        {logicalPosition.x(), logicalPosition.y()}, documentExtent(), viewportExtent());
}

void CanvasWindow::fitDocumentToView()
{
    const auto view = viewportExtent();
    const auto document = documentExtent();
    if (view.width <= 0.0 || view.height <= 0.0 || document.width <= 0.0 || document.height <= 0.0) {
        fitPending_ = true;
        return;
    }
    scene_.viewport.fit(document, view, 52.0);
    fitPending_ = false;
    publishZoom();
    refreshColorSample();
    scheduleFrame();
}

void CanvasWindow::restoreDocumentView(core::ViewportState view, std::optional<core::MeasurementLine> measurement)
{
    scene_.viewport = view;
    fitPending_ = false;
    cancelMeasureInput();
    regularMeasurement_ = measurement;
    temporaryMeasurement_.reset();
    scene_.measurement = measurement;
    publishZoom();
    refreshColorSample();
    scheduleFrame();
}

void CanvasWindow::resetTo100Percent()
{
    scene_.viewport.reset100Percent();
    publishZoom();
    refreshColorSample();
    scheduleFrame();
}

void CanvasWindow::scheduleFrame()
{
    if(onFilterPreparationRequested && std::any_of(scene_.document.layersBottomToTop.begin(),
        scene_.document.layersBottomToTop.end(),[](const auto& layer){return core::hasActiveSpatialFilters(layer.filters)||core::hasActiveLayerEffects(layer.effects);}))
        onFilterPreparationRequested();
    scene_.logicalViewport = viewportExtent();
    scene_.devicePixelRatio = devicePixelRatio();
    if (onViewStateChanged) onViewStateChanged();
    updateSelectionAnimation();
    if (framePending_ || resizeCommitPending_) {
        return;
    }
    framePending_ = true;
    // QWindow::requestUpdate() may intentionally suppress child-window updates
    // while an embedded Wayland subsurface is transiently unexposed. Queue the
    // on-demand frame on this object's event loop instead; Qt's presentation
    // hooks still provide the platform-specific minimized-window throttling.
    QMetaObject::invokeMethod(this, [this] {
        framePending_ = false;
        // A resize can arrive after this update was queued. Never enter the
        // expensive out-of-date/recreation path until the resize burst has
        // settled; doing so would put the GUI thread behind the pointer again.
        if (resizeCommitPending_ || !surfaceAvailable_ || width() <= 0 || height() <= 0) {
            return;
        }
        if (onViewportUpdated)
            onViewportUpdated();
        if (backend_->render(scene_)) {
            scheduleFrame();
        }
    }, Qt::QueuedConnection);
}

void CanvasWindow::cancelTextInput()
{
    selectingText_ = false;
    if (explicitTextGrab_) {
        explicitTextGrab_ = false;
        setMouseGrabEnabled(false);
    }
}

RendererStats CanvasWindow::rendererStats() const
{
    auto stats = backend_ ? backend_->stats() : RendererStats {};
    stats.deferredResizeEvents = deferredResizeEventCount_;
    stats.resizeCommits = resizeCommitCount_;
    stats.resizePresentationSuspends = resizePresentationSuspendCount_;
    stats.resizePresentationResumes = resizePresentationResumeCount_;
    return stats;
}

void CanvasWindow::prepareForHostResize()
{
    suppressPresentationForResize();

    // QWindowContainer normally follows with a CanvasWindow resize event, but
    // arm the settle timer here as well. Some QPA plugins defer hidden-child
    // resize delivery until the child is shown again.
    resizeCommitPending_ = true;
    resizeCommitTimer_.start();
}

void CanvasWindow::setHostPresentationAvailable(bool available)
{
    hostPresentationAvailable_ = available;
    if (available) {
        restorePresentationAfterResize();
    }
}

void CanvasWindow::exposeEvent(QExposeEvent* event)
{
    QWindow::exposeEvent(event);
    if (isExposed()) {
        // QWindowContainer can remap its child as the host becomes exposed
        // before the QWidget-side Show notification settles. Finish any
        // deferred resize recovery from the authoritative surface event.
        restorePresentationAfterResize();
        scheduleFrame();
    }
}

void CanvasWindow::resizeEvent(QResizeEvent* event)
{
    QWindow::resizeEvent(event);
    scene_.logicalViewport = viewportExtent();
    scene_.devicePixelRatio = devicePixelRatio();
    deferSwapchainResize();
}

void CanvasWindow::notifyOutsideDocumentPress(QPointF position)
{
    // Never retarget during a latched gesture, multi-click construction, pan,
    // measurement or explicit transform (whose handles can be off canvas).
    // Move performs its own empty-pasteboard handling after hit testing, so an
    // off-canvas hit does not collapse the selected group before dragging it.
    if (!onOutsideDocumentPressed || pointerGestureActive() || spaceHeld_
        || measureActive() || scene_.transformOverlay || scene_.activeTool == core::ToolId::Move)
        return;
    const auto point = documentPositionForLogical(position);
    const auto extent = scene_.document.canvas.extent;
    if (point.x < 0 || point.y < 0 || point.x >= extent.width || point.y >= extent.height)
        onOutsideDocumentPressed();
}

void CanvasWindow::mousePressEvent(QMouseEvent* event)
{
    // A fresh press proves that button's previous release happened, even if
    // another app received it after focus-loss cancellation. Other held
    // buttons still belong to the consumed gesture.
    completedGestureButtons_ &= event->buttons() & ~Qt::MouseButtons(event->button());
    if(completedGestureButtons_ != Qt::NoButton) { event->accept(); return; }
    suppressFinishContextMenu_=false;
    if (event->button() == Qt::RightButton && measureActive() && !panning_) {
        clearMeasurement();
        // Share the existing completion-release/context-menu guard. A held
        // left button cannot become a different gesture if R is released next.
        completedGestureButtons_ = event->buttons() | Qt::RightButton;
        suppressFinishContextMenu_ = true;
        event->accept(); return;
    }
    if(event->button()==Qt::RightButton && !picking_ && !panning_ && !brushing_
        && !selectingText_ && onFinishRequested && onFinishRequested()) {
        completedGestureButtons_=event->buttons() | Qt::RightButton;
        suppressFinishContextMenu_=true;
        event->accept(); return;
    }
    selectionModifiers_ = event->modifiers();
    // A gesture owns its mode until its initiating button is released. An
    // extra button must not steal sampling capture or start a paint stroke.
    if (picking_ || transforming_ || selecting_ || panning_ || brushing_ || shaping_ || measuring_) {
        event->accept();
        return;
    }
    lastPointerPosition_ = event->position();
    scene_.cursorLogical = {event->position().x(), event->position().y()};
    scene_.cursorInside = true;
    temporaryEyedropper_ = event->modifiers().testFlag(Qt::AltModifier);
    resetBrushCursorAngleForHover();
    const bool middlePan = event->button() == Qt::MiddleButton;
    const bool spacePan = event->button() == Qt::LeftButton && spaceHeld_;
    if (middlePan || spacePan) {
        explicitPanGrab_ = false;
        if (platformProvidesImplicitPointerGrab()) {
            lastPanGrabSucceeded_ = true;
        } else {
            explicitPanGrab_ = setMouseGrabEnabled(true);
            lastPanGrabSucceeded_ = explicitPanGrab_;
        }
        if (!lastPanGrabSucceeded_) {
            qWarning() << "Platform refused explicit canvas mouse capture; continuing with button tracking";
            if (onStatusMessage) {
                onStatusMessage(QStringLiteral("Unable to capture the pointer for canvas panning"));
            }
        }
        panning_ = true;
        panButton_ = event->button();
        refreshColorSample();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    // A background repair owns edits, but not navigation. Pan is handled
    // above, and wheel zoom keeps its ordinary route.
    if (scene_.repairProcessing) { event->accept(); return; }
    if (event->button() == Qt::LeftButton)
        notifyOutsideDocumentPress(event->position());
    if (event->button() == Qt::LeftButton && measureActive()) {
        beginMeasureInput(event->position()); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && core::isSelectionTool(scene_.activeTool)) {
        if(onSelectionSample)onSelectionSample(mouseSample(*event));
        beginSelectionInput(event->position(), event->modifiers());
        event->accept(); return;
    }
    if(event->button()==Qt::LeftButton && scene_.activeTool==core::ToolId::Shape) {
        beginShapeInput(event->position(),event->modifiers());event->accept();return;
    }
    if (event->button() == Qt::LeftButton && scene_.activeTool == core::ToolId::Text && onTextPressed) {
        selectingText_ = onTextPressed(
            documentPositionForLogical(event->position()), event->modifiers(), false);
        if (selectingText_ && !platformProvidesImplicitPointerGrab())
            explicitTextGrab_ = setMouseGrabEnabled(true);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && (scene_.transformOverlay || scene_.activeTool == core::ToolId::Move)) {
        beginTransformInput(event->position());
        event->accept();
        return;
    }
    refreshColorSample();
    if(event->button()==Qt::LeftButton && scene_.activeTool==core::ToolId::Cloning && !scene_.spotHealActive
        && event->modifiers().testFlag(Qt::AltModifier)) {
        completedGestureButtons_=event->buttons();
        if(onCloneSourcePicked)onCloneSourcePicked(documentPositionForLogical(event->position()));
        event->accept();return;
    }
    if (event->button() == Qt::LeftButton && scene_.eyedropperActive) {
        picking_ = true;
        scene_.eyedropperReference = workingColor_;
        if (!platformProvidesImplicitPointerGrab()) {
            explicitPickerGrab_ = setMouseGrabEnabled(true);
        }
        applyColorSample();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && scene_.activeTool == core::ToolId::Fill) {
        if (onFillRequested) onFillRequested(documentPositionForLogical(event->position()));
        event->accept(); return;
    }
    if (event->button() == Qt::LeftButton
        && (scene_.activeTool == core::ToolId::Brush
            || scene_.activeTool == core::ToolId::Eraser || scene_.activeTool==core::ToolId::Cloning
            || scene_.activeTool==core::ToolId::LocalBlur)
        && onBrushStrokeBegan) {
        explicitBrushGrab_ = false;
        if (!platformProvidesImplicitPointerGrab()) {
            explicitBrushGrab_ = setMouseGrabEnabled(true);
        }
        brushing_ = onBrushStrokeBegan(mouseSample(*event));
        if (!brushing_ && explicitBrushGrab_) {
            setMouseGrabEnabled(false);
            explicitBrushGrab_ = false;
        }
        scheduleFrame();
        event->accept();
        return;
    }
    scheduleFrame();
    QWindow::mousePressEvent(event);
}

void CanvasWindow::mouseMoveEvent(QMouseEvent* event)
{
    completedGestureButtons_ &= event->buttons();
    if(completedGestureButtons_ != Qt::NoButton) { event->accept(); return; }
    selectionModifiers_ = event->modifiers();
    scene_.cursorLogical = {event->position().x(), event->position().y()};
    scene_.cursorInside = true;
    temporaryEyedropper_ = event->modifiers().testFlag(Qt::AltModifier);
    resetBrushCursorAngleForHover();
    if (panning_) {
        const auto delta = event->position() - lastPointerPosition_;
        scene_.viewport.panBy({delta.x(), delta.y()});
        lastPointerPosition_ = event->position();
    } else if (measuring_) {
        moveMeasureInput(event->position(), event->modifiers());
    } else if (selectingText_) {
        if (onTextDragged)
            onTextDragged(documentPositionForLogical(event->position()));
    } else if (selecting_ || selectionConstruction_) {
        if(onSelectionSample)onSelectionSample(mouseSample(*event));
        if (onSelectionMoved) onSelectionMoved(documentPositionForLogical(event->position()));
    } else if (transforming_) {
        moveTransformInput(event->position(), event->modifiers());
    } else if (shaping_ || shapeConstruction_) {
        if(onShapeMoved)onShapeMoved(documentPositionForLogical(event->position()), event->modifiers());
    } else if (brushing_ && onBrushStrokeMoved) {
        if (!onBrushStrokeMoved(mouseSample(*event))) {
            stopBrushInput(true);
        }
    }
    updateTransformHover();
    refreshColorSample();
    if (picking_) {
        applyColorSample();
    }
    scheduleFrame();
    event->accept();
}

void CanvasWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if(completedGestureButtons_.testFlag(event->button())) {
        completedGestureButtons_.setFlag(event->button(),false);
        event->accept(); return;
    }
    if (measuring_ && event->button() == Qt::LeftButton) {
        scene_.cursorLogical = {event->position().x(), event->position().y()};
        moveMeasureInput(event->position(), event->modifiers()); finishMeasureInput(false);
        event->accept(); return;
    }
    if(shaping_ && event->button()==Qt::LeftButton) {
        scene_.cursorLogical={event->position().x(),event->position().y()};
        if(onShapeMoved)onShapeMoved(documentPositionForLogical(event->position()), event->modifiers());
        finishShapeInput(false);event->accept();return;
    }
    if (selectingText_ && event->button() == Qt::LeftButton) {
        if (onTextDragged)
            onTextDragged(documentPositionForLogical(event->position()));
        cancelTextInput();
        event->accept();
        return;
    }
    selectionModifiers_ = event->modifiers();
    scene_.cursorLogical = {event->position().x(), event->position().y()};
    if (selecting_ && event->button() == Qt::LeftButton) {
        if(onSelectionSample)onSelectionSample(mouseSample(*event));
        if (onSelectionMoved) onSelectionMoved(documentPositionForLogical(event->position()));
        finishSelectionInput(false);
        event->accept(); return;
    }
    if (transforming_ && event->button() == Qt::LeftButton) {
        moveTransformInput(event->position(), event->modifiers());
        finishTransformInput(false);
        event->accept();
        return;
    }
    resetBrushCursorAngleForHover();
    temporaryEyedropper_ = event->modifiers().testFlag(Qt::AltModifier);
    if (picking_ && event->button() == Qt::LeftButton) {
        refreshColorSample();
        applyColorSample();
        stopColorPick();
        refreshColorSample();
        event->accept();
        return;
    }
    if (panning_ && event->button() == panButton_) {
        stopPanning();
        if (selectionConstruction_ && onSelectionMoved) onSelectionMoved(documentPositionForLogical(event->position()));
        if (shapeConstruction_ && onShapeMoved) onShapeMoved(documentPositionForLogical(event->position()), event->modifiers());
        refreshColorSample();
        event->accept();
        return;
    }
    if (brushing_ && event->button() == Qt::LeftButton) {
        if (onBrushStrokeEnded) {
            (void)onBrushStrokeEnded(mouseSample(*event));
        }
        stopBrushInput(false);
        refreshColorSample();
        event->accept();
        return;
    }
    QWindow::mouseReleaseEvent(event);
}

void CanvasWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && measureActive()) {
        mousePressEvent(event);
        return;
    }
    if(event->button()==Qt::LeftButton && scene_.activeTool==core::ToolId::Move
        && event->modifiers().testFlag(Qt::ShiftModifier)) {
        mousePressEvent(event);
        return;
    }
    if(completedGestureButtons_!=Qt::NoButton || (event->button()==Qt::RightButton && suppressFinishContextMenu_)) {
        event->accept(); return;
    }
    if(event->button()==Qt::LeftButton && shapeConstruction_) {
        finishShapeInput(false);
        if(onShapeCloseRequested)onShapeCloseRequested(documentPositionForLogical(event->position()));
        event->accept();return;
    }
    if (event->button() == Qt::LeftButton
        && !event->modifiers().testFlag(Qt::ShiftModifier)
        && (scene_.activeTool == core::ToolId::Move || scene_.activeTool == core::ToolId::Text)
        && onTextPressed
        && onTextPressed(documentPositionForLogical(event->position()), event->modifiers(), true)) {
        event->accept();
        return;
    }
    if (event->button()==Qt::LeftButton && selectionConstruction_) {
        if (onSelectionCloseRequested) onSelectionCloseRequested(documentPositionForLogical(event->position()));
        event->accept(); return;
    }
    QWindow::mouseDoubleClickEvent(event);
}

void CanvasWindow::wheelEvent(QWheelEvent* event)
{
    if (transforming_ || selecting_ || shaping_) { event->accept(); return; }
    if (scene_.document.canvas.extent.empty() || event->angleDelta().y() == 0) {
        QWindow::wheelEvent(event);
        return;
    }
    const auto steps = static_cast<double>(event->angleDelta().y()) / 120.0;
    const auto factor = std::pow(1.16, steps);
    scene_.viewport.zoomAround(
        {event->position().x(), event->position().y()}, factor, documentExtent(), viewportExtent());
    publishZoom();
    if (selectionConstruction_ && onSelectionMoved) onSelectionMoved(documentPositionForLogical(event->position()));
    if (shapeConstruction_ && onShapeMoved) onShapeMoved(documentPositionForLogical(event->position()), event->modifiers());
    refreshColorSample();
    scheduleFrame();
    event->accept();
}

void CanvasWindow::tabletEvent(QTabletEvent* event)
{
    if(scene_.activeTool==core::ToolId::Shape || measureActive()
        || completedGestureButtons_ != Qt::NoButton) {
        // Reuse the same latched capture and pan/handle precedence for tablets.
        const auto type=event->type()==QEvent::TabletPress?QEvent::MouseButtonPress:
            event->type()==QEvent::TabletRelease?QEvent::MouseButtonRelease:QEvent::MouseMove;
        QMouseEvent mouse(type,event->position(),event->globalPosition(),
            type==QEvent::MouseMove?Qt::NoButton:event->button(),event->buttons(),event->modifiers());
        if(type==QEvent::MouseButtonPress)mousePressEvent(&mouse);
        else if(type==QEvent::MouseButtonRelease)mouseReleaseEvent(&mouse);
        else mouseMoveEvent(&mouse);
        event->accept();return;
    }
    selectionModifiers_ = event->modifiers();
    scene_.cursorLogical = {event->position().x(), event->position().y()};
    scene_.cursorInside = true;
    if (event->type() == QEvent::TabletPress && event->button() == Qt::LeftButton)
        notifyOutsideDocumentPress(event->position());
    if (selecting_ || (core::isSelectionTool(scene_.activeTool) && !spaceHeld_ && !panning_)) {
        if(onSelectionSample)onSelectionSample(tabletSample(*event));
        if (event->type() == QEvent::TabletPress) beginSelectionInput(event->position(), event->modifiers());
        else if (selecting_ || selectionConstruction_) {
            if (onSelectionMoved) onSelectionMoved(documentPositionForLogical(event->position()));
            if (event->type() == QEvent::TabletRelease) finishSelectionInput(false);
        }
        event->accept(); return;
    }
    if ((scene_.transformOverlay || scene_.activeTool == core::ToolId::Move) && !spaceHeld_) {
        if (event->type() == QEvent::TabletPress) beginTransformInput(event->position());
        else if (transforming_) {
            moveTransformInput(event->position(), event->modifiers());
            if (event->type() == QEvent::TabletRelease) finishTransformInput(false);
        }
        updateTransformHover();
        event->accept();
        return;
    }
    temporaryEyedropper_ = event->modifiers().testFlag(Qt::AltModifier);
    resetBrushCursorAngleForHover();
    refreshColorSample();
    if (scene_.eyedropperActive || picking_) {
        if (event->type() == QEvent::TabletPress) {
            picking_ = true;
            scene_.eyedropperReference = workingColor_;
        }
        if (picking_) {
            applyColorSample();
        }
        if (event->type() == QEvent::TabletRelease) {
            stopColorPick();
            refreshColorSample();
        }
        event->accept();
        return;
    }
    if (scene_.activeTool == core::ToolId::Fill && !spaceHeld_ && !panning_) {
        if (event->type() == QEvent::TabletPress && onFillRequested)
            onFillRequested(documentPositionForLogical(event->position()));
        event->accept(); return;
    }
    if ((scene_.activeTool != core::ToolId::Brush
            && scene_.activeTool != core::ToolId::Eraser && scene_.activeTool!=core::ToolId::Cloning
            && scene_.activeTool!=core::ToolId::LocalBlur)
        || spaceHeld_) {
        QWindow::tabletEvent(event);
        return;
    }
    if (event->type() == QEvent::TabletPress && onBrushStrokeBegan) {
        if(scene_.repairProcessing) { event->accept(); return; }
        if(scene_.activeTool==core::ToolId::Cloning && !scene_.spotHealActive && event->modifiers().testFlag(Qt::AltModifier)) {
            if(onCloneSourcePicked)onCloneSourcePicked(documentPositionForLogical(event->position()));
            event->accept();return;
        }
        brushing_ = onBrushStrokeBegan(tabletSample(*event));
        event->accept();
    } else if (event->type() == QEvent::TabletMove && brushing_
        && onBrushStrokeMoved) {
        if (!onBrushStrokeMoved(tabletSample(*event))) {
            stopBrushInput(true);
        }
        event->accept();
    } else if (event->type() == QEvent::TabletRelease && brushing_) {
        if (onBrushStrokeEnded) {
            (void)onBrushStrokeEnded(tabletSample(*event));
        }
        stopBrushInput(false);
        refreshColorSample();
        event->accept();
    } else {
        QWindow::tabletEvent(event);
    }
    scheduleFrame();
}

void CanvasWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && measureActive()) {
        if (!event->isAutoRepeat()) {
            if (measuring_) cancelMeasureInput(); else clearMeasurement();
        }
        event->accept(); return;
    }
    if (event->key() == Qt::Key_Escape && (selecting_ || selectionConstruction_)) {
        cancelSelectionInput(); event->accept(); return;
    }
    // The active raster gesture owns Escape even when Alt was pressed after
    // the stroke began. Cancelling only temporary picking would leave pixels
    // live and incorrectly commit them on the eventual button release.
    if (event->key() == Qt::Key_Escape && brushing_ && !event->isAutoRepeat()) {
        stopBrushInput(true);
        cancelColorSampling();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Alt && !event->isAutoRepeat()) {
        setTemporaryEyedropper(true);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && (picking_ || temporaryEyedropper_)) {
        cancelColorSampling();
        event->accept();
        return;
    }
    if (!externalShortcutRouting_ && event->key() == Qt::Key_Space && !event->isAutoRepeat()
        && event->modifiers() == Qt::NoModifier) {
        setSpacePanHeld(true);
        event->accept();
        return;
    }
    QWindow::keyPressEvent(event);
}

void CanvasWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Alt && !event->isAutoRepeat()) {
        setTemporaryEyedropper(false);
        event->accept();
        return;
    }
    if (!externalShortcutRouting_ && event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        setSpacePanHeld(false);
        event->accept();
        return;
    }
    QWindow::keyReleaseEvent(event);
}

void CanvasWindow::setSpacePanHeld(bool held)
{
    if (spaceHeld_ == held)
        return;
    spaceHeld_ = held;
    refreshColorSample();
    updateCursorForTool();
}

void CanvasWindow::cancelPanInput()
{
    spaceHeld_ = false;
    stopPanning();
    refreshColorSample();
    updateCursorForTool();
}

void CanvasWindow::focusOutEvent(QFocusEvent* event)
{
    cancelMeasureInput();
    setTemporaryMeasure(false);
    cancelShapeInput();
    cancelTextInput();
    setPointerTooltip({});
    cancelSelectionInput();
    cancelTransformInput();
    scene_.cursorInside = false;
    updateTransformHover();
    spaceHeld_ = false;
    stopBrushInput(true);
    stopPanning();
    cancelColorSampling();
    QWindow::focusOutEvent(event);
}

bool CanvasWindow::event(QEvent* event)
{
    if(event->type()==QEvent::ContextMenu && suppressFinishContextMenu_
        && static_cast<QContextMenuEvent*>(event)->reason()==QContextMenuEvent::Mouse) {
        event->accept();return true;
    }
    if (onTextEvent && onTextEvent(event))
        return true;
    if (event->type() == QEvent::UngrabMouse
        || event->type() == QEvent::WindowDeactivate
        || event->type() == QEvent::Hide
        || event->type() == QEvent::Close
        || event->type() == QEvent::TouchCancel) {
        completedGestureButtons_={};
        suppressFinishContextMenu_=false;
        cancelMeasureInput();
        setTemporaryMeasure(false);
        cancelShapeInput();
        cancelTextInput();
        setPointerTooltip({});
        cancelSelectionInput();
        selectionTimer_.stop();
        cancelTransformInput();
        scene_.cursorInside = false;
        updateTransformHover();
        spaceHeld_ = false;
        stopBrushInput(true);
        stopPanning();
        cancelColorSampling();
    }
    if (event->type() == QEvent::UpdateRequest || event->type() == QEvent::Paint) {
        scheduleFrame();
    } else if (event->type() == QEvent::PlatformSurface) {
        const auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event);
        if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            cancelMeasureInput();
            setTemporaryMeasure(false);
            cancelShapeInput();
            setPointerTooltip({});
            cancelSelectionInput();
            selectionTimer_.stop();
            resizeCommitTimer_.stop();
            resizeCommitPending_ = false;
            surfaceAvailable_ = false;
            backend_->surfaceAboutToBeDestroyed();
        } else if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
            surfaceAvailable_ = true;
            if (resizeCommitPending_) {
                resizeCommitTimer_.start();
            } else {
                backend_->invalidateSwapchain();
                scheduleFrame();
            }
        }
    } else if (event->type() == QEvent::DevicePixelRatioChange) {
        scene_.devicePixelRatio = devicePixelRatio();
        updateCursorForTool();
        suppressPresentationForResize();
        deferSwapchainResize();
    } else if (event->type() == QEvent::DragEnter) {
        auto* dragEvent = static_cast<QDragEnterEvent*>(event);
        const auto urls = dragEvent->mimeData()->urls();
        if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); })) {
            dragEvent->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::DragMove) {
        auto* dragEvent = static_cast<QDragMoveEvent*>(event);
        const auto urls = dragEvent->mimeData()->urls();
        if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); })) {
            dragEvent->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        QStringList paths;
        for (const auto& url : dropEvent->mimeData()->urls()) {
            if (url.isLocalFile()) {
                paths.push_back(url.toLocalFile());
            }
        }
        if (!paths.isEmpty()) {
            if (onFilesDropped) {
                onFilesDropped(std::move(paths));
            }
            dropEvent->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Enter) {
        scene_.cursorInside = true;
        if (const auto* enter = dynamic_cast<QEnterEvent*>(event)) {
            scene_.cursorLogical = {enter->position().x(), enter->position().y()};
        }
        temporaryEyedropper_ = QGuiApplication::keyboardModifiers().testFlag(Qt::AltModifier);
        if (measureActive()) refreshMeasurement();
        refreshColorSample();
        updateTransformHover();
        scheduleFrame();
    } else if (event->type() == QEvent::Leave && !panning_ && !brushing_ && !picking_ && !measuring_) {
        scene_.cursorInside = false;
        updateTransformHover();
        scheduleFrame();
    }
    return QWindow::event(event);
}

core::Extent2d CanvasWindow::documentExtent() const noexcept
{
    return {
        static_cast<double>(scene_.document.canvas.extent.width),
        static_cast<double>(scene_.document.canvas.extent.height),
    };
}

core::Extent2d CanvasWindow::viewportExtent() const noexcept
{
    return {static_cast<double>(width()), static_cast<double>(height())};
}

void CanvasWindow::deferSwapchainResize()
{
    ++deferredResizeEventCount_;
    resizeCommitPending_ = true;
    resizeCommitTimer_.start();
}

void CanvasWindow::commitDeferredResize()
{
    if (!resizeCommitPending_) {
        return;
    }

    resizeCommitPending_ = false;
    ++resizeCommitCount_;
    backend_->invalidateSwapchain();

    restorePresentationAfterResize();

    if (!surfaceAvailable_ || width() <= 0 || height() <= 0) {
        return;
    }
    if (fitPending_) {
        fitDocumentToView();
    } else {
        refreshColorSample();
        scheduleFrame();
    }
}

void CanvasWindow::suppressPresentationForResize()
{
    if (presentationSuppressedForResize_ || !isVisible()) {
        return;
    }

    presentationSuppressedForResize_ = true;
    ++resizePresentationSuspendCount_;
    // Keep QWindowContainer and the QWidget layout visible. Only detach the
    // Vulkan child's current Wayland buffer so the canvas can never transiently
    // stack above a dock while the two surface trees resize asynchronously.
    setVisible(false);
}

void CanvasWindow::restorePresentationAfterResize()
{
    if (!presentationSuppressedForResize_) {
        return;
    }

    if (!nativeParentCanShowCanvas()) {
        return;
    }

    presentationSuppressedForResize_ = false;
    ++resizePresentationResumeCount_;
    setVisible(true);
}

bool CanvasWindow::nativeParentCanShowCanvas() const noexcept
{
    const auto* nativeParent = qobject_cast<const QWindow*>(parent());
    return hostPresentationAvailable_ && nativeParent && nativeParent->isVisible();
}

void CanvasWindow::stopPanning()
{
    if (!panning_) {
        return;
    }
    panning_ = false;
    panButton_ = Qt::NoButton;
    if (explicitPanGrab_) {
        setMouseGrabEnabled(false);
        explicitPanGrab_ = false;
    }
    updateCursorForTool();
}

void CanvasWindow::stopBrushInput(bool cancel)
{
    scene_.constrainedBrushPosition.reset();
    if (!brushing_) {
        return;
    }
    brushing_ = false;
    if (explicitBrushGrab_) {
        setMouseGrabEnabled(false);
        explicitBrushGrab_ = false;
    }
    if (cancel && onBrushStrokeCancelled) {
        onBrushStrokeCancelled();
    }
    scene_.brushTipAngleDegrees = scene_.brushTipBaseAngleDegrees;
    scheduleFrame();
    updateCursorForTool();
}

core::NormalizedPointerSample CanvasWindow::mouseSample(
    const QMouseEvent& event) const noexcept
{
    return {
        .documentPosition = documentPositionForLogical(event.position()),
        .timestampMicroseconds = event.timestamp() * 1000U,
        .pressure = 1.0,
        .tiltX = 0.0,
        .tiltY = 0.0,
        .rotationDegrees = 0.0,
        .barrelRotationDegrees = 0.0,
        .pointerType = core::PointerType::Mouse,
        .buttons = normalizedButtons(event.buttons()),
        .modifiers = normalizedModifiers(event.modifiers()),
    };
}

core::NormalizedPointerSample CanvasWindow::tabletSample(
    const QTabletEvent& event) const noexcept
{
    const auto rotation = static_cast<double>(event.rotation());
    return {
        .documentPosition = documentPositionForLogical(event.position()),
        .timestampMicroseconds = event.timestamp() * 1000U,
        .pressure = std::clamp(static_cast<double>(event.pressure()), 0.0, 1.0),
        .tiltX = std::clamp(static_cast<double>(event.xTilt()) / 60.0, -1.0, 1.0),
        .tiltY = std::clamp(static_cast<double>(event.yTilt()) / 60.0, -1.0, 1.0),
        .rotationDegrees = rotation,
        .barrelRotationDegrees = rotation,
        .pointerType = normalizedPointerType(event.pointerType()),
        .buttons = normalizedButtons(event.buttons()),
        .modifiers = normalizedModifiers(event.modifiers()),
    };
}

void CanvasWindow::resetBrushCursorAngleForHover()
{
    // Before a direction exists (and while merely hovering), show the preset's
    // base/offset angle. During a stroke MainWindow replaces this with the
    // exact resolved BrushDab angle after synchronous dab generation.
    if (!brushing_) {
        scene_.brushTipAngleDegrees = scene_.brushTipBaseAngleDegrees;
    }
}

const QCursor& CanvasWindow::rotationCursor()
{
    // Canonical arch opens along +Y. Point its opening inward, toward the
    // selected corner/center, leaving the curve tangent to the rotation arc.
    // The corner stays locked throughout a drag; deriving orientation from
    // current overlay geometry also follows Shift-snapped/resolved rotation.
    const auto handles = logicalTransformHandles();
    const auto center = (handles[0] + handles[4]) * 0.5;
    const auto radial = handles[rotationCorner_] - center;
    constexpr int degreesPerStep = 5;
    constexpr int steps = 360 / degreesPerStep;
    const double degrees = std::atan2(radial.y, radial.x) * 180.0 / std::acos(-1.0) + 90.0;
    const int orientation = (int(std::lround(degrees / degreesPerStep)) % steps + steps) % steps;
    const int dprKey = std::clamp(int(std::lround(devicePixelRatio() * 100.0)), 50, 800);
    if (!rotationCursors_.contains(dprKey) && rotationCursors_.size() >= 2) {
        std::erase_if(rotationCursors_, [this](const auto& cached) {
            return cached.first != rotationCursorDpr_;
        });
    }
    rotationCursorDpr_ = dprKey;
    auto& variants = rotationCursors_[dprKey];
    if (auto found = variants.find(orientation); found != variants.end())
        return found->second;
    const double dpr = double(dprKey) / 100.0;
    QImage image(QSize(int(std::ceil(32 * dpr)), int(std::ceil(32 * dpr))), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(16, 16);
    painter.rotate(orientation * degreesPerStep);
    painter.translate(-16, -16);
    QPainterPath path;
    path.moveTo(6, 17);
    path.cubicTo(6, 3, 26, 3, 26, 17);
    path.moveTo(3, 12.5); path.lineTo(6, 17); path.lineTo(10, 13.5);
    path.moveTo(22, 13.5); path.lineTo(26, 17); path.lineTo(29, 12.5);
    painter.strokePath(path, QPen(QColor(12, 15, 22), 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.strokePath(path, QPen(QColor(247, 249, 255), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.end();
    return variants.emplace(orientation, QCursor(QPixmap::fromImage(image), 16, 16)).first->second;
}

const QCursor& CanvasWindow::fillCursor()
{
    const int dprKey = std::clamp(int(std::lround(devicePixelRatio() * 100.0)), 50, 800);
    if (const auto found = fillCursors_.find(dprKey); found != fillCursors_.end())
        return found->second;
    // Native cursor, not a canvas texture: create only on the first use at a
    // display scale and retain at most two monitor-scale variants.
    if (fillCursors_.size() >= 2)
        fillCursors_.erase(fillCursors_.begin());
    const double dpr = double(dprKey) / 100.0;
    QImage image(QSize(int(std::ceil(32 * dpr)), int(std::ceil(32 * dpr))),
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath bucket;
    bucket.moveTo(11, 6);
    bucket.lineTo(26, 13);
    bucket.lineTo(20, 23);
    bucket.quadTo(19, 24, 17, 23);
    bucket.lineTo(9, 18);
    bucket.lineTo(11, 6);
    bucket.moveTo(15, 8);
    bucket.cubicTo(12, -1, 28, 2, 23, 11);
    bucket.moveTo(10, 14);
    bucket.lineTo(6, 17);
    bucket.lineTo(6, 20);
    // The precise seed is the unobscured crosshair below the pouring lip.
    bucket.moveTo(5, 23);
    bucket.lineTo(5, 31);
    bucket.moveTo(1, 27);
    bucket.lineTo(9, 27);
    painter.strokePath(bucket, QPen(QColor(12, 15, 22), 3.8, Qt::SolidLine,
        Qt::RoundCap, Qt::RoundJoin));
    painter.strokePath(bucket, QPen(QColor(247, 249, 255), 1.5, Qt::SolidLine,
        Qt::RoundCap, Qt::RoundJoin));
    painter.end();
    return fillCursors_.emplace(dprKey, QCursor(QPixmap::fromImage(image), 5, 27)).first->second;
}

void CanvasWindow::updateCursorForTool()
{
    if (panning_) {
        setCursor(Qt::ClosedHandCursor);
    } else if(shaping_ || shapeConstruction_) {
        setCursor(Qt::CrossCursor);
    } else if (selecting_ || selectionConstruction_) {
        setCursor(movingSelection_ ? Qt::ClosedHandCursor : Qt::CrossCursor);
    } else if (spaceHeld_ && !brushing_ && !picking_ && !transforming_ && !measuring_) {
        setCursor(Qt::OpenHandCursor);
    } else if (measureActive()) {
        setCursor(measuring_ ? Qt::ClosedHandCursor
            : measureEndpointAt(scene_.cursorLogical) >= 0 ? Qt::OpenHandCursor : Qt::CrossCursor);
    } else if (transforming_ && !scene_.transformOverlay) {
        setCursor(Qt::ClosedHandCursor);
    } else if (scene_.transformOverlay) {
        const auto hit = scene_.transformHighlight;
        if (hit == core::TransformHandle::Move) {
            setCursor(transforming_ ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
        } else if (hit == core::TransformHandle::Rotate) {
            const auto& requested = rotationCursor();
            const auto current = cursor();
            // Do not resend an identical native cursor on every pointer
            // sample (or the multiple hover refreshes within one sample).
            if (current.shape() != Qt::BitmapCursor
                || current.pixmap().cacheKey() != requested.pixmap().cacheKey())
                setCursor(requested);
        } else if (distorting_ || (!transforming_ && scene_.activeTool==core::ToolId::Transform
            &&selectionModifiers_.testFlag(Qt::ControlModifier)&&hit>=core::TransformHandle::TopLeft
            &&hit<=core::TransformHandle::Left&&int(hit)%2==0)) {
            setCursor(Qt::SizeAllCursor);
        } else if (hit != core::TransformHandle::None) {
            // Orient the resize cursor to the actual transformed axis, not
            // the layer-local handle name (which stays stable through flips).
            const auto handles = logicalTransformHandles();
            const auto index = static_cast<std::size_t>(hit);
            const auto axis = handles[index] - handles[(index + 4) % 8];
            if (index % 2 == 0) {
                // Corners resize both axes even on very wide/tall frames.
                // Pick the closest diagonal, retaining rotation/flip adaptation.
                setCursor(axis.x * axis.y >= 0.0 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
            } else {
                const auto sector = (static_cast<int>(std::lround(
                    std::atan2(axis.y, axis.x) * 4.0 / std::acos(-1.0))) % 4 + 4) % 4;
                constexpr std::array shapes {Qt::SizeHorCursor, Qt::SizeFDiagCursor,
                    Qt::SizeVerCursor, Qt::SizeBDiagCursor};
                setCursor(shapes[static_cast<std::size_t>(sector)]);
            }
        } else if(scene_.activeTool==core::ToolId::Shape) {
            const bool hit=scene_.cursorInside&&!selectionModifiers_.testFlag(Qt::ShiftModifier)&&onShapeHit
                &&onShapeHit(documentPositionForLogical({scene_.cursorLogical.x,scene_.cursorLogical.y}),false);
            setCursor(hit?Qt::OpenHandCursor:Qt::CrossCursor);
        } else setCursor(Qt::ArrowCursor);
    } else if (core::isSelectionTool(scene_.activeTool)) {
        const auto point = documentPositionForLogical({scene_.cursorLogical.x,scene_.cursorLogical.y});
        const bool movable = scene_.activeTool != core::ToolId::SelectByColor && scene_.activeTool != core::ToolId::SmartSelect && scene_.cursorInside && core::selectionMoveHit(scene_.document.selection,point,
            selectionOperation_,selectionModifiers_.testFlag(Qt::ShiftModifier),selectionModifiers_.testFlag(Qt::AltModifier));
        setCursor(scene_.activeTool==core::ToolId::SmartSelect && scene_.quickSelectionCursor ? Qt::BlankCursor
            : movable ? Qt::OpenHandCursor : Qt::CrossCursor);
    } else if(scene_.activeTool==core::ToolId::Shape) {
        const bool hit=scene_.cursorInside&&!selectionModifiers_.testFlag(Qt::ShiftModifier)&&onShapeHit
            &&onShapeHit(documentPositionForLogical({scene_.cursorLogical.x,scene_.cursorLogical.y}),false);
        setCursor(hit?Qt::OpenHandCursor:Qt::CrossCursor);
    } else if (scene_.activeTool == core::ToolId::Text) {
        setCursor(Qt::IBeamCursor);
    } else if (scene_.activeTool == core::ToolId::Fill) {
        const auto& requested = fillCursor();
        const auto current = cursor();
        if (current.shape() != Qt::BitmapCursor
            || current.pixmap().cacheKey() != requested.pixmap().cacheKey())
            setCursor(requested);
    } else if (scene_.eyedropperActive) {
        setCursor(Qt::BlankCursor);
    } else if (scene_.activeTool == core::ToolId::Brush || scene_.activeTool == core::ToolId::Eraser
        || scene_.activeTool == core::ToolId::Cloning || scene_.activeTool==core::ToolId::LocalBlur) {
        setCursor(Qt::BlankCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void CanvasWindow::publishZoom()
{
    if (onZoomChanged) {
        onZoomChanged(scene_.viewport.zoom());
    }
}

} // namespace imageeditor::render
