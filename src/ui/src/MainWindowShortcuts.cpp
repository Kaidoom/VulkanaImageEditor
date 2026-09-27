#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QSettings>
#include <QWindow>

namespace imageeditor::ui {
namespace {
QWidget* inputOwner(QWidget* widget)
{
    for (auto* p = widget; p; p = p->parentWidget()) {
        // A spin box owns its internal line edit even in slide mode.
        if (qobject_cast<QLineEdit*>(p) && qobject_cast<QAbstractSpinBox*>(p->parentWidget())) return p->parentWidget();
        if (qobject_cast<QLineEdit*>(p) || qobject_cast<QAbstractSpinBox*>(p)
            || qobject_cast<QAbstractSlider*>(p) || qobject_cast<QComboBox*>(p)
            || qobject_cast<QAbstractItemView*>(p) || dynamic_cast<AdjustmentCurveEditor*>(p)
            || (qobject_cast<QAbstractButton*>(p) && p->focusPolicy() != Qt::NoFocus)) return p;
    }
    return nullptr;
}
bool navigationKey(const QKeyEvent& key)
{
    switch (key.key()) {
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
    case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
    case Qt::Key_Space: case Qt::Key_Return: case Qt::Key_Enter: return true;
    default: return false;
    }
}
bool typingWidget(QWidget* widget)
{
    if (auto* line = qobject_cast<QLineEdit*>(widget)) return !line->isReadOnly();
    if (auto* value = dynamic_cast<CompactValueControl*>(widget)) return value->isManualEntryActive();
    if (auto* combo = qobject_cast<QComboBox*>(widget)) return combo->isEditable();
    return qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QTextEdit*>(widget) || qobject_cast<QPlainTextEdit*>(widget);
}
}
void MainWindow::initializeShortcuts()
{
    canvasWindow_->setExternalShortcutRouting(true);
    undoAction_->setObjectName("UndoAction"); redoAction_->setObjectName("RedoAction");
    // Commands without a menu entry still have the same stable action identity.
    for (const auto& d : shortcutDefinitions()) {
        auto* action = findChild<QAction*>(d.id);
        if (!action) {
            action = new QAction(d.label, this); action->setObjectName(d.id);
            if (d.id == "ErasePixelsAction") connect(action, &QAction::triggered, this, [this] {
                if (shortcutGestureIdle()) startFill(false, true, {}, true);
            });
            else if (d.id == "FinishOperationAction") connect(action, &QAction::triggered, this, [this] { finishCanvasOperation(); });
            else if (d.id == "RemovePointAction") connect(action, &QAction::triggered, this, [this] {
                if (auto* curve = dynamic_cast<AdjustmentCurveEditor*>(inputOwner(QApplication::focusWidget()))) curve->removeSelectedPoint();
                else if (shapeCreation_) removeShapeVertex();
                else if (anchoredLassoActive()) removeLassoAnchor();
            });
            else if (d.id == "FinishTextAction") connect(action, &QAction::triggered, this, [this] { if (textController_) textController_->finish(); });
        }
        registerEditorWindowAction(action);
        const auto hint = action->toolTip();
        bool containsDefaultKey = false;
        for (const auto& key : d.defaults) containsDefaultKey |= hint.contains(key.toString(QKeySequence::NativeText));
        if (hint != action->text() && !containsDefaultKey && !hint.isEmpty())
            action->setProperty("shortcutDescription", hint);
        shortcutActions_.insert(d.id, action);
    }
    auto bindings = defaultShortcutBindings();
    if (persistWindowState_) { QSettings settings; bindings = loadShortcutBindings(settings); }
    applyShortcutBindings(bindings);
}
void MainWindow::applyShortcutBindings(const ShortcutBindings& bindings)
{
    shortcuts_ = bindings;
    for (const auto& d : shortcutDefinitions()) {
        auto* action = shortcutActions_.value(d.id);
        if (!action) continue;
        action->setShortcuts(shortcuts_.value(d.id));
        action->setAutoRepeat(d.repeat);
        auto hint = d.label + " · " + shortcutLabel(shortcuts_, d.id);
        if (!action->property("shortcutDescription").toString().isEmpty())
            hint += "\n" + action->property("shortcutDescription").toString();
        if (d.id == "LayerTransformAction")
            hint += tr("\n%1 applies · Escape cancels").arg(shortcutLabel(shortcuts_, "FinishOperationAction"));
        action->setToolTip(hint);
    }
    if (textController_) textController_->setShortcutBindings(shortcuts_);
    updateShortcutHints(this, shortcuts_);
    updateShortcutHints(workspace_->panelOverlay(), shortcuts_);
    // Curve removal is also usable in a standalone panel, with the same binding.
    for (auto* w : workspace_->panelOverlay()->findChildren<QWidget*>())
        if (auto* curve = dynamic_cast<AdjustmentCurveEditor*>(w)) curve->setShortcutBindings(shortcuts_);
    if (auto* label = workspace_->panelOverlay()->findChild<QLabel*>("PixelPreviewBadgeLabel"))
        label->setText(tr("Pixel Preview · %1").arg(shortcutLabel(shortcuts_, "PixelPreviewAction")));
    if (auto* exit = workspace_->panelOverlay()->findChild<QWidget*>("ExitPixelPreview"))
        exit->setToolTip(tr("Exit Pixel Preview · %1").arg(shortcutLabel(shortcuts_, "PixelPreviewAction")));
    if (deleteLayerButton_ && deleteLayerButton_->isEnabled())
        deleteLayerButton_->setToolTip(tr("Delete selected layers · %1").arg(shortcutLabel(shortcuts_, "DeleteSelectedLayersAction")));
}
bool MainWindow::shortcutGestureIdle(bool allowMenuCapture) const
{
    // A menu's opening press briefly owns capture before its popup takes over.
    // Menu availability must not mistake that press for an editing gesture.
    const auto* owner = pointerRouter_->captureOwner();
    return (pointerRouter_->captureDomain() == CrossWindowPointerRouter::CaptureDomain::None
        || (allowMenuCapture && (qobject_cast<const QMenu*>(owner) || qobject_cast<const QMenuBar*>(owner))))
        && !activeBrushStroke_ && !activeCloneStroke_ && !activeLocalBlurStroke_ && !activeSpotHealStroke_
        && !spotHealJob_ && !activeFill_ && !layerCrop_ && !activeLayerMove_ && !shapeResize_ && !shapeEdit_
        && !shapeCreation_ && !selectionGesture_ && !selectionRasterizer_ && !selectionRotation_
        && !colorSelectionEditing_ && !smartInteractionActive() && !adjustmentEdit_ && !filterEdit_ && !effectEdit_
        && !canvasWindow_->transformDragging() && !canvasWindow_->panDragging() && !canvasWindow_->measureDragging();
}
bool MainWindow::routePanelKeyboard(QObject* watched, QEvent* event)
{
    if (layerRenameEditor_ || forwardingPanelKey_ || QApplication::activePopupWidget() || QApplication::activeModalWidget()) return false;
    const bool press = event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick
        || event->type() == QEvent::TabletPress;
    if (press && watched == canvasWindow_) {
        keyboardPanelTarget_.clear();
        canvasContainer_->setFocusProxy(nullptr);
        if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
        canvasContainer_->setFocus(Qt::MouseFocusReason);
    } else if ((press || event->type() == QEvent::FocusIn) && qobject_cast<QWidget*>(watched)) {
        auto* target = inputOwner(static_cast<QWidget*>(watched));
        if (target && keyboardPanelTarget_ != target) {
            keyboardPanelTarget_ = target;
            if (press && !qobject_cast<QAbstractButton*>(target)) {
                // Keep native Vulkan input routed to the meaningful QWidget
                // owner without activating a separate native panel window.
                canvasContainer_->setFocusProxy(nullptr);
                QT_WARNING_PUSH
                QT_WARNING_DISABLE_DEPRECATED
                QApplication::setActiveWindow(window());
                canvasContainer_->setFocus(Qt::OtherFocusReason);
                canvasContainer_->setFocusProxy(target);
                QApplication::setActiveWindow(target->window());
                QT_WARNING_POP
                target->setFocus(Qt::MouseFocusReason);
            }
        }
    }
    const bool key = event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
        || event->type() == QEvent::ShortcutOverride || event->type() == QEvent::InputMethod;
    auto* target = keyboardPanelTarget_.data();
    const bool ownsFocus = target && (target->hasFocus() || target->isAncestorOf(QApplication::focusWidget()));
    if (key && qobject_cast<QWindow*>(watched) && ownsFocus && target->isEnabled() && target->isVisible()) {
        const QScopedValueRollback guard(forwardingPanelKey_, true);
        QCoreApplication::sendEvent(QApplication::focusWidget() && target->isAncestorOf(QApplication::focusWidget())
            ? QApplication::focusWidget() : target, event);
        event->accept(); return true;
    }
    return false;
}
bool MainWindow::routeEditorShortcut(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease && event->type() != QEvent::ShortcutOverride) return false;
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    // Escape and gesture modifiers remain in the existing cancellation/input
    // pipeline. QAction lookup must not steal a reserved or context-owned key.
    if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
    if ((measureAccessHeld_ && key->key() == measureAccessKey_) || (panAccessKey_ && key->key() == panAccessKey_)) {
        event->accept(); return true; // Physical release is handled before focus/modal routing.
    }
    if (event->type() != QEvent::KeyPress || key->key() == Qt::Key_Escape) return false;
    for (const auto* id : {"CloseDocumentAction", "NextDocumentAction", "PreviousDocumentAction"}) {
        if (shortcutMatches(shortcuts_, id, *key)) {
            if (!key->isAutoRepeat()) if (auto* action=shortcutActions_.value(id);action&&action->isEnabled()) action->trigger();
            event->accept();return true;
        }
    }
    auto* owner = inputOwner(QApplication::focusWidget());
    const bool field = editorTextInputActive() || typingWidget(qobject_cast<QWidget*>(watched));
    if (field) return false;
    const bool curve = dynamic_cast<AdjustmentCurveEditor*>(owner);
    const bool layers = owner == layerList_;
    const bool control = owner && !curve && !layers;
    // Native widget navigation is fixed, not an accidental editor shortcut.
    if ((layers || control) && navigationKey(*key)) return false;
    unsigned context = curve ? CurveKeys : layers ? LayerListKeys : control ? ControlKeys
        : (textController_ && textController_->active()) ? TextKeys
        : (layerTransform_ || selectionTransform_ || layerCrop_) ? TransformKeys
        : (shapeCreation_ || anchoredLassoActive() || selectionGesture_) ? ConstructionKeys
        : (activeBrushStroke_ || activeCloneStroke_ || activeLocalBlurStroke_ || activeSpotHealStroke_) ? StrokeKeys : CanvasKeys;
    for (const auto& d : shortcutDefinitions()) {
        const bool gestureShift = d.id.startsWith("Nudge")
            || (context == ConstructionKeys && (d.id == "FinishOperationAction" || d.id == "RemovePointAction"));
        if (!(d.contexts & context) || !shortcutMatches(shortcuts_, d.id, *key, gestureShift)) continue;
        event->accept();
        if (key->isAutoRepeat() && !d.repeat) return true;
        if (d.id.startsWith("Nudge")) {
            if (shortcutGestureIdle() && session().document()) {
                const double step = key->modifiers().testFlag(Qt::ShiftModifier) ? shiftNudgePixels_ : 1;
                nudgeTarget({d.id == "NudgeLeftAction" ? -step : d.id == "NudgeRightAction" ? step : 0,
                    d.id == "NudgeUpAction" ? -step : d.id == "NudgeDownAction" ? step : 0});
            }
        } else if (d.id == "TemporaryMeasureAction") {
            measureAccessHeld_ = true; measureAccessKey_ = key->key();
            if (shortcutGestureIdle() && !layerTransform_ && !selectionTransform_ && session().document()) canvasWindow_->setTemporaryMeasure(true);
        } else if (d.id == "PanCanvasAction") {
            panAccessKey_ = key->key(); canvasWindow_->setSpacePanHeld(true);
        } else if (auto* action = shortcutActions_.value(d.id); action && action->isEnabled()) action->trigger();
        return true;
    }
    return false;
}
}
