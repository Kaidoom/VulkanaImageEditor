#include "imageeditor/ui/CrossWindowPointerRouter.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>

#include <array>

namespace imageeditor::ui {
namespace {

Q_LOGGING_CATEGORY(pointerCaptureLog, "imageeditor.input.capture", QtWarningMsg)

QString objectDescription(const QObject* object)
{
    if (!object) {
        return QStringLiteral("<none>");
    }
    const auto name = object->objectName();
    return name.isEmpty()
        ? QString::fromLatin1(object->metaObject()->className())
        : QStringLiteral("%1(%2)")
              .arg(QString::fromLatin1(object->metaObject()->className()), name);
}

bool isMouseEventType(QEvent::Type type)
{
    return type == QEvent::MouseButtonPress
        || type == QEvent::MouseButtonDblClick
        || type == QEvent::MouseMove
        || type == QEvent::MouseButtonRelease;
}

bool isPressEvent(QEvent::Type type)
{
    return type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick;
}

bool isDragAndDropTakeoverEvent(QEvent::Type type)
{
    return type == QEvent::DragEnter
        || type == QEvent::DragMove
        || type == QEvent::DragLeave
        || type == QEvent::Drop;
}

} // namespace

CrossWindowPointerRouter::CrossWindowPointerRouter(QWidget* widgetRoot,
    QWindow* nativeWindow, QWidget* nativeContainer, QObject* parent)
    : QObject(parent)
    , widgetRoot_(widgetRoot)
    , nativeWindow_(nativeWindow)
    , nativeContainer_(nativeContainer)
{
    Q_ASSERT(widgetRoot_);
    Q_ASSERT(nativeWindow_);
    Q_ASSERT(QCoreApplication::instance());
    QCoreApplication::instance()->installEventFilter(this);
}

CrossWindowPointerRouter::~CrossWindowPointerRouter()
{
    // Destructors must not synthesize input into a widget tree which may already
    // be tearing down. Releasing a still-live app-owned QWidget grab is safe and
    // required; CanvasWindow independently unwinds its own explicit grab.
    releaseExplicitWidgetGrabIfOwned();
    finishCapture("router destruction");
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->removeEventFilter(this);
    }
}

QObject* CrossWindowPointerRouter::captureOwner() const noexcept
{
    if (captureDomain_ == CaptureDomain::Widget) {
        return widgetCaptureOwner_;
    }
    if (captureDomain_ == CaptureDomain::NativeWindow) {
        return nativeWindow_;
    }
    return nullptr;
}

void CrossWindowPointerRouter::cancelCapture()
{
    if (captureDomain_ == CaptureDomain::Widget) {
        cancelWidgetCapture();
    } else if (captureDomain_ == CaptureDomain::NativeWindow) {
        cancelNativeCapture();
    }
}

bool CrossWindowPointerRouter::eventFilter(QObject* watched, QEvent* event)
{
    if (!event) {
        return false;
    }
    if (notifyingWidgetCancellation_) {
        return false;
    }
    // Mouse events re-enter QApplication while this router forwards them to
    // the original press window. UngrabMouse can likewise re-enter while an
    // explicit grab is being released. Other event classes must remain live:
    // QDrag::exec() can start synchronously inside a forwarded mouse move and
    // its DragEnter/DragMove event is the definitive ownership handoff.
    if (routingDepth_ != 0
        && (isMouseEventType(event->type()) || event->type() == QEvent::UngrabMouse)) {
        return false;
    }

    if (captureDomain_ == CaptureDomain::Widget
        && event->type() == QEvent::Show) {
        if (auto* popup = qobject_cast<QWidget*>(watched);
            popup && popup->windowType() == Qt::Popup
            && popupBelongsToRoot(popup)) {
            // Qt popups intentionally take the platform mouse grab from the
            // control which opened them. End only our logical seam bridge and
            // let the popup own the still-live press sequence; synthesizing a
            // release here swallows a combo/menu's first selection click.
            ++popupTakeoverCount_;
            qCDebug(pointerCaptureLog).noquote()
                << "finish capture for Qt popup takeover popup="
                << objectDescription(popup)
                << "owner=" << objectDescription(captureOwner());
            finishCapture("Qt popup takeover");
            return false;
        }
    }

    if (event->type() == QEvent::KeyPress && captureDomain_ != CaptureDomain::None) {
        const auto* keyEvent = static_cast<const QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape && !keyEvent->isAutoRepeat()) {
            cancelCapture();
            return false;
        }
    }

    if (captureDomain_ != CaptureDomain::None
        && event->type() == QEvent::UngrabMouse
        && objectBelongsToCaptureScope(watched)) {
        if (captureDomain_ == CaptureDomain::Widget && activeRootPopup()) {
            ++popupTakeoverCount_;
            finishCapture("active Qt popup takeover");
            return false;
        }
        // On Wayland, moving between a QWidgetWindow and an embedded native
        // child surface can emit UngrabMouse even though the compositor's
        // press sequence is still active. Treating that surface handoff as a
        // terminal event used to synthesize an early release, which is exactly
        // what made sliders and dock separators stop at the canvas boundary.
        //
        // Keep the application's logical capture. The next button-bearing move
        // or release will still be routed to the original press domain. Actual
        // cancellation is covered by the definitive focus/window/application,
        // touch, visibility and lifetime events below.
        retainedSeamUngrab_ = true;
        ++retainedUngrabCount_;
        qCDebug(pointerCaptureLog).noquote()
            << "retain capture across platform UngrabMouse recipient="
            << objectDescription(watched)
            << "owner=" << objectDescription(captureOwner())
            << "qtExplicitGrabber=" << objectDescription(QWidget::mouseGrabber())
            << "buttons=" << pressedButtons_;
        // CanvasWindow quite reasonably treats an UngrabMouse delivered to it
        // as a request to stop panning. Suppress only that native-owner copy of
        // the ambiguous seam notification; QWidget recipients must still see
        // it because QDrag uses the normal Qt widget-side grab handoff.
        return captureDomain_ == CaptureDomain::NativeWindow
            && watched == nativeWindow_;
    }

    if (captureDomain_ != CaptureDomain::None
        && isDragAndDropTakeoverEvent(event->type())
        && objectBelongsToCaptureScope(watched)) {
        // QDrag deliberately takes pointer ownership away from the source
        // widget. Do not retain the press-routing bridge during a real Qt DnD
        // operation, and do not synthesize mouse releases into the source
        // control: QAbstractItemView/QDrag own that lifecycle now.
        ++dragTakeoverCount_;
        qCDebug(pointerCaptureLog).noquote()
            << "finish capture for Qt drag-and-drop takeover recipient="
            << objectDescription(watched)
            << "owner=" << objectDescription(captureOwner())
            << "qtExplicitGrabber=" << objectDescription(QWidget::mouseGrabber());
        releaseExplicitWidgetGrabIfOwned();
        finishCapture("Qt drag-and-drop takeover");
        return false;
    }

    if (isCaptureCancellationEvent(watched, event)) {
        if (captureDomain_ == CaptureDomain::Widget && activeRootPopup()) {
            ++popupTakeoverCount_;
            finishCapture("popup focus takeover");
            return false;
        }
        qCDebug(pointerCaptureLog).noquote()
            << "cancel capture for event=" << event->type()
            << "recipient=" << objectDescription(watched)
            << "owner=" << objectDescription(captureOwner())
            << "qtExplicitGrabber=" << objectDescription(QWidget::mouseGrabber())
            << "buttons=" << pressedButtons_;
        // Destruction cannot safely receive a synthetic terminal event. Other
        // cancellation paths first put the active control/canvas back into its
        // released state.
        if (event->type() == QEvent::Destroy
            || event->type() == QEvent::DeferredDelete) {
            finishCapture("capture owner destruction");
        } else {
            cancelCapture();
        }
        return false;
    }

    if (!isMouseEventType(event->type())) {
        return false;
    }

    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    const bool press = isPressEvent(event->type());
    const bool release = event->type() == QEvent::MouseButtonRelease;

    if (captureDomain_ == CaptureDomain::None && press) {
        if (watched == nativeWindow_) {
            beginNativeCapture(*mouseEvent);
        } else if (auto* widget = qobject_cast<QWidget*>(watched);
                   widget && widget != nativeContainer_ && widgetBelongsToRoot(widget)) {
            if (auto* preparation = dynamic_cast<PointerPressPreparation*>(widget)) {
                const QPointer<QWidget> receiver(widget);
                preparation->preparePointerPress(*mouseEvent);
                if (!receiver || !receiver->isVisible() || !receiver->isEnabled())
                    return true;
            }
            beginWidgetCapture(widget, *mouseEvent);
        }
        return false;
    }

    if (captureDomain_ == CaptureDomain::None) {
        return false;
    }

    // A router instance owns only one editor window tree plus its embedded
    // canvas. Events from another independent top-level window in the same
    // QApplication must neither be stolen nor alter this capture's button
    // ledger. Wayland's active press sequence continues to target the surface
    // where it began even when the cursor is outside that surface.
    const bool eventInCaptureScope = objectBelongsToCaptureScope(watched);
    if (!eventInCaptureScope) {
        return false;
    }

    const bool crossedCaptureBoundary = eventCrossesCaptureBoundary(watched);
    // KDE/Wayland may briefly report buttons == NoButton on a move from either
    // native surface while a press sequence crosses the embedded-QWindow seam.
    // That report is not a release: the real MouseButtonRelease follows later.
    // Keep our press ledger authoritative for moves for the whole logical
    // capture, including moves delivered back to the original QWidgetWindow.
    const bool repairLogicalCaptureButtonState = event->type() == QEvent::MouseMove
        && mouseEvent->buttons() == Qt::NoButton
        && pressedButtons_ != Qt::NoButton;
    if (repairLogicalCaptureButtonState) {
        ++repairedButtonStateCount_;
        qCDebug(pointerCaptureLog).noquote()
            << "repair missing move button state during logical capture recipient="
            << objectDescription(watched)
            << "owner=" << objectDescription(captureOwner())
            << "reportedButtons=" << mouseEvent->buttons()
            << "trackedButtons=" << pressedButtons_;
    }

    if (press) {
        pressedButtons_ |= mouseEvent->button();
    } else if (!repairLogicalCaptureButtonState) {
        pressedButtons_ = mouseEvent->buttons();
    }

    bool routed = false;
    if (captureDomain_ == CaptureDomain::Widget) {
        if (crossedCaptureBoundary || repairLogicalCaptureButtonState) {
            routed = routeMouseEvent(watched, widgetCaptureWindow_, *mouseEvent);
        }
    } else if (captureDomain_ == CaptureDomain::NativeWindow
        && (crossedCaptureBoundary || repairLogicalCaptureButtonState)) {
        routed = routeMouseEvent(watched, nativeWindow_, *mouseEvent);
    }

    if (release && mouseEvent->buttons() == Qt::NoButton) {
        if (captureDomain_ == CaptureDomain::Widget) {
            // A receiver gets the release after the application event filter.
            // Check on the next event-loop turn and only unwind a grab that the
            // control failed to release itself.
            QPointer<QWidget> expectedGrabber = QWidget::mouseGrabber();
            if (expectedGrabber && widgetBelongsToRoot(expectedGrabber)) {
                QMetaObject::invokeMethod(this, [this, expectedGrabber] {
                    if (expectedGrabber) {
                        releaseExplicitWidgetGrabIfOwned(expectedGrabber);
                    }
                }, Qt::QueuedConnection);
            }
        }
        finishCapture("last button release");
    }
    return routed;
}

bool CrossWindowPointerRouter::widgetBelongsToRoot(const QWidget* widget) const noexcept
{
    if (!widgetRoot_ || !widget) {
        return false;
    }
    // Related tool/dialog windows can remain QObject-owned by the main shell
    // while leaving its QWidget ancestry. Ownership keeps them in this input
    // domain without admitting unrelated application windows.
    for (const QObject* current = widget; current; current = current->parent()) {
        if (current == widgetRoot_) {
            return true;
        }
    }
    return widgetRoot_->isAncestorOf(widget);
}

bool CrossWindowPointerRouter::windowBelongsToRoot(const QWindow* window) const noexcept
{
    if (!widgetRoot_ || !window) {
        return false;
    }

    // A native QWindowContainer is required for correct stacking and clipping
    // of the Vulkan child on Wayland. Its QWidgetWindow is a native child, not
    // an entry in QApplication::topLevelWidgets(), so include that window and
    // its parent chain in the Widgets-side capture domain explicitly.
    const auto* containerWindow = nativeContainer_
        ? nativeContainer_->windowHandle() : nullptr;
    const auto* rootWindow = widgetRoot_->windowHandle();
    for (const QWindow* current = window; current; current = current->parent()) {
        if (current == containerWindow || current == rootWindow) {
            return true;
        }
    }
    for (auto* topLevel : QApplication::topLevelWidgets()) {
        if (widgetBelongsToRoot(topLevel)
            && topLevel->windowHandle() == window) {
            return true;
        }
    }
    return false;
}

QWindow* CrossWindowPointerRouter::dispatchWindowForWidget(
    const QWidget* widget) const noexcept
{
    if (!widget) {
        return nullptr;
    }

    // QWidget::window() names the logical top-level widget even when the
    // receiver lives inside a native child QWidgetWindow. Walk to the nearest
    // native QWidget instead so Qt's private implicit press owner is updated
    // through the same dispatch window that received the original press.
    for (const auto* current = widget; current; current = current->parentWidget()) {
        if (current->internalWinId() != 0 && current->windowHandle()) {
            return current->windowHandle();
        }
    }
    return widget->window() ? widget->window()->windowHandle() : nullptr;
}

bool CrossWindowPointerRouter::eventComesFromWidgetSide(const QObject* watched) const noexcept
{
    if (const auto* widget = qobject_cast<const QWidget*>(watched)) {
        return widgetBelongsToRoot(widget);
    }
    if (const auto* window = qobject_cast<const QWindow*>(watched)) {
        return windowBelongsToRoot(window);
    }
    return false;
}

bool CrossWindowPointerRouter::objectBelongsToCaptureScope(
    const QObject* watched) const noexcept
{
    return watched == nativeWindow_
        || watched == nativeContainer_
        || eventComesFromWidgetSide(watched);
}

bool CrossWindowPointerRouter::eventCrossesCaptureBoundary(
    const QObject* watched) const noexcept
{
    if (captureDomain_ == CaptureDomain::NativeWindow) {
        return watched != nativeWindow_ && eventComesFromWidgetSide(watched);
    }
    if (captureDomain_ != CaptureDomain::Widget) {
        return false;
    }

    if (watched == nativeWindow_ || watched == nativeContainer_) {
        return true;
    }
    if (const auto* window = qobject_cast<const QWindow*>(watched)) {
        return window != widgetCaptureWindow_ && windowBelongsToRoot(window);
    }
    if (const auto* widget = qobject_cast<const QWidget*>(watched)) {
        auto* eventWindow = dispatchWindowForWidget(widget);
        return widgetBelongsToRoot(widget) && eventWindow
            && eventWindow != widgetCaptureWindow_;
    }
    return false;
}

bool CrossWindowPointerRouter::popupBelongsToRoot(
    const QWidget* popup) const noexcept
{
    if (!popup || popup->windowType() != Qt::Popup) {
        return false;
    }
    if (widgetBelongsToRoot(popup)) {
        return true;
    }

    // Some style/platform popup implementations are not QObject-parented to
    // the opening widget but retain a native transient-parent relationship.
    const auto* popupWindow = popup->windowHandle();
    for (const auto* transient = popupWindow
             ? popupWindow->transientParent() : nullptr;
         transient; transient = transient->transientParent()) {
        if (windowBelongsToRoot(transient)) {
            return true;
        }
    }
    return false;
}

QWidget* CrossWindowPointerRouter::activeRootPopup() const noexcept
{
    auto* popup = QApplication::activePopupWidget();
    return popupBelongsToRoot(popup) ? popup : nullptr;
}

bool CrossWindowPointerRouter::isCaptureCancellationEvent(
    const QObject* watched, const QEvent* event) const noexcept
{
    if (captureDomain_ == CaptureDomain::None || !event) {
        return false;
    }

    const auto type = event->type();
    if (type == QEvent::ApplicationDeactivate) {
        return true;
    }

    const bool terminal = type == QEvent::WindowDeactivate
        || type == QEvent::FocusOut
        || type == QEvent::Hide
        || type == QEvent::Close
        || type == QEvent::Destroy
        || type == QEvent::DeferredDelete
        || type == QEvent::TouchCancel;
    if (!terminal) {
        return false;
    }

    if (captureDomain_ == CaptureDomain::NativeWindow) {
        return watched == nativeWindow_
            || (type == QEvent::WindowDeactivate && eventComesFromWidgetSide(watched));
    }

    return watched == widgetCaptureOwner_
        || watched == widgetCaptureWindow_
        // The canvas is in the same QWindow ancestry, but is NOT the widget
        // press owner. Its delayed deactivation when focus enters a panel must
        // not cancel that panel's new hold/slide. Owner/root deactivation and
        // application deactivation above remain genuine terminal events.
        || (type == QEvent::WindowDeactivate && watched != nativeWindow_
            && eventComesFromWidgetSide(watched));
}

void CrossWindowPointerRouter::beginWidgetCapture(
    QWidget* receiver, const QMouseEvent& event)
{
    widgetCaptureOwner_ = receiver;
    widgetCaptureWindow_ = dispatchWindowForWidget(receiver);
    if (!widgetCaptureWindow_) {
        widgetCaptureOwner_.clear();
        return;
    }
    captureDomain_ = CaptureDomain::Widget;
    pressedButtons_ = event.buttons() | event.button();
    qCDebug(pointerCaptureLog).noquote()
        << "begin widget capture owner=" << objectDescription(receiver)
        << "dispatchWindow=" << objectDescription(widgetCaptureWindow_)
        << "qtExplicitGrabber=" << objectDescription(QWidget::mouseGrabber())
        << "buttons=" << pressedButtons_;
}

void CrossWindowPointerRouter::beginNativeCapture(const QMouseEvent& event)
{
    captureDomain_ = CaptureDomain::NativeWindow;
    pressedButtons_ = event.buttons() | event.button();
    qCDebug(pointerCaptureLog).noquote()
        << "begin native capture owner=" << objectDescription(nativeWindow_)
        << "qtExplicitGrabber=" << objectDescription(QWidget::mouseGrabber())
        << "buttons=" << pressedButtons_;
}

bool CrossWindowPointerRouter::routeMouseEvent(
    QObject* sourceObject, QWindow* destination, QMouseEvent& source)
{
    if (!destination) {
        cancelCapture();
        return false;
    }

    const auto local = destination->mapFromGlobal(source.globalPosition());
    const auto routedButtons = source.type() == QEvent::MouseMove
            && source.buttons() == Qt::NoButton && pressedButtons_ != Qt::NoButton
        ? pressedButtons_ : source.buttons();
    QMouseEvent routed(source.type(), local, local, source.globalPosition(),
        source.button(), routedButtons, source.modifiers(),
        source.source(), source.pointingDevice());
    routed.setTimestamp(source.timestamp());

    logRoute(sourceObject, destination, source);
    ++routingDepth_;
    QCoreApplication::sendEvent(destination, &routed);
    --routingDepth_;
    ++routedEventCount_;
    source.setAccepted(routed.isAccepted());
    // Once the event has crossed the native-window seam it belongs exclusively
    // to the original capture domain, even if that receiver elects to ignore it.
    return true;
}

void CrossWindowPointerRouter::cancelWidgetCapture()
{
    if (!widgetCaptureWindow_) {
        finishCapture("widget capture target disappeared");
        return;
    }

    // Give custom interactions a semantic cancellation before Qt's synthetic
    // releases clear the private implicit press owner. Standard controls may
    // ignore TouchCancel; title drags and resize grips use it to roll back.
    if (widgetCaptureOwner_) {
        notifyingWidgetCancellation_ = true;
        QEvent cancellation(QEvent::TouchCancel);
        QCoreApplication::sendEvent(widgetCaptureOwner_, &cancellation);
        notifyingWidgetCancellation_ = false;
    }

    // Send terminal releases through QWidgetWindow rather than directly to the
    // control. That lets Qt clear its private implicit button-down owner as well
    // as ending QSlider and QMainWindow separator state. The position is outside
    // the top-level widget so a cancellation cannot activate a push button.
    constexpr std::array<Qt::MouseButton, 5> mouseButtons {
        Qt::LeftButton,
        Qt::RightButton,
        Qt::MiddleButton,
        Qt::BackButton,
        Qt::ForwardButton,
    };
    auto remaining = pressedButtons_;
    const QPointF local {-1.0, -1.0};
    const QPointF global = widgetCaptureWindow_->mapToGlobal(local);
    for (const auto button : mouseButtons) {
        if (!remaining.testFlag(button)) {
            continue;
        }
        remaining &= ~Qt::MouseButtons(button);
        QMouseEvent release(QEvent::MouseButtonRelease, local, local, global,
            button, remaining, QGuiApplication::keyboardModifiers());
        ++routingDepth_;
        QCoreApplication::sendEvent(widgetCaptureWindow_, &release);
        --routingDepth_;
    }
    releaseExplicitWidgetGrabIfOwned();
    finishCapture("widget capture cancellation");
}

void CrossWindowPointerRouter::cancelNativeCapture()
{
    if (nativeWindow_) {
        QEvent ungrab(QEvent::UngrabMouse);
        ++routingDepth_;
        QCoreApplication::sendEvent(nativeWindow_, &ungrab);
        --routingDepth_;
    }
    finishCapture("native capture cancellation");
}

void CrossWindowPointerRouter::releaseExplicitWidgetGrabIfOwned(QWidget* expectedGrabber)
{
    auto* grabber = QWidget::mouseGrabber();
    if (!grabber || (expectedGrabber && grabber != expectedGrabber)
        || !widgetBelongsToRoot(grabber)) {
        return;
    }
    qCDebug(pointerCaptureLog).noquote()
        << "release lingering explicit widget grab owner="
        << objectDescription(grabber);
    ++routingDepth_;
    grabber->releaseMouse();
    --routingDepth_;
}

void CrossWindowPointerRouter::finishCapture(const char* reason)
{
    if (captureDomain_ != CaptureDomain::None) {
        qCDebug(pointerCaptureLog).noquote()
            << "finish capture reason=" << reason
            << "owner=" << objectDescription(captureOwner())
            << "qtExplicitGrabber=" << objectDescription(QWidget::mouseGrabber())
            << "routedEvents=" << routedEventCount_;
    }
    captureDomain_ = CaptureDomain::None;
    pressedButtons_ = Qt::NoButton;
    retainedSeamUngrab_ = false;
    widgetCaptureOwner_.clear();
    widgetCaptureWindow_.clear();
}

void CrossWindowPointerRouter::logRoute(const QObject* source,
    const QWindow* destination, const QMouseEvent& event) const
{
    qCDebug(pointerCaptureLog).noquote()
        << "route" << event.type()
        << "from=" << objectDescription(source)
        << "to=" << objectDescription(destination)
        << "trackedOwner=" << objectDescription(captureOwner())
        << "qtExplicitGrabber=" << objectDescription(QWidget::mouseGrabber())
        << "buttons=" << event.buttons()
        << "global=" << event.globalPosition();
}

} // namespace imageeditor::ui
