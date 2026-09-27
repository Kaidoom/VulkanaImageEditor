#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ShapeOptionsPage.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/TransformOptionsPage.hpp"
#include <QScopedValueRollback>
#include <QStatusBar>
#include <stdexcept>

namespace imageeditor::ui {
void MainWindow::nudgeTarget(core::Vec2d delta)
{
    auto* document = session().document();
    if (!document) return;
    (void)runTransformAction([&] {
        if (selectionTransform_) {
            auto values = selectionTransform_->values;
            values.center = values.center + delta;
            setSelectionTransformValues(values);
            completeSelectionTransformAction();
        } else if (layerTransform_) {
            auto values = layerTransform_->values();
            values.center = values.center + delta;
            if (layerTransform_->setValues(values)) layerTransform_->completeAction();
            refreshLayerTransform();
            updateActionState();
        } else if (core::isSelectionTool(session().activeTool())) {
            const auto selection = document->selection();
            if (!selection || selection->bounds().empty()) return false;
            // Uses the ordinary selection command, which also invalidates
            // tool-local segmentation evidence and preserves redo on no-ops.
            session().execute(std::make_unique<core::SetSelectionCommand>(
                selection->translated(int(delta.x), int(delta.y)), "Move selection"));
            synchronizeUi(false, false);
        } else {
            core::LayerTransformSession move(*document, session().selectedLayers());
            if (!move.active()) return false;
            // Shared document-space delta includes group descendants once;
            // source pixels, shape geometry, and font sizes stay untouched.
            if (move.beginDrag(core::TransformHandle::Move, {})) {
                move.dragTo(delta, {}, false);
                move.endDrag();
                if (move.commit(session().history()) == core::TransformCommitResult::Committed)
                    fileState().untouched = false;
            }
            synchronizeUi(false, false);
        }
        return true;
    });
}

bool MainWindow::runTransformAction(const std::function<bool()>& action)
{
    try {
        return action();
    } catch (const std::exception&) {
        statusBar()->showMessage(QStringLiteral("Could not finish the transform action. Your preview is retained; retry or cancel."), 5000);
        return false;
    }
}
void MainWindow::cancelPendingEdits()
{
    finishLayerCrop(false);
    finishAdjustmentEdit(false);
    finishFilterEdit(false);
    finishEffectEdit(false);
    cancelColorSelection();
    cancelSmartSelection();
    finishShapeCreation(true);
    finishShapeResize(false);
    finishShapeEdit(false);
    if (textController_)
        textController_->finish();
    if (textController_ && textController_->active())
        return;
    cancelFill();
    finishSelectionTransform(false);
    canvasWindow_->cancelSelectionInput();
    finishSelectionGesture(true); // Also cancels a released lasso's pending rasterization.
    finishSelectionRotation(false);
    cancelActiveBrushStroke();
    finishLayerMove(false);
    if (layerTransform_)
        finishLayerTransform(false);
}

bool MainWindow::beginLayerMove(core::Vec2d position, Qt::KeyboardModifiers modifiers)
{
    moveTransferGrab_ = position;
    moveOptionsPage_->finishNumericInput();
    if (activeLayerMove_) {
        finishLayerMove(true);
        if (activeLayerMove_)
            return false; // Failed admission must not lose the retained edit.
    }
    auto* document = session().document();
    if (!document || session().activeTool() != core::ToolId::Move)
        return false;
    const auto extent = document->canvas().extent;
    if (position.x < 0 || position.y < 0 || position.x >= extent.width || position.y >= extent.height) {
        canvasWindow_->dismissLayerOutlines();
        return false;
    }
    const auto hit = hitMoveLayer(position);
    if (!hit) canvasWindow_->dismissLayerOutlines();
    const auto id = moveActiveOnly_ ? session().activeLayer() : hit;
    if (!id)
        return false;
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        session().toggleSelectedLayer(*id);
        synchronizeUi(false, false);
        if (!hit) canvasWindow_->dismissLayerOutlines();
        return false; // A toggle click never doubles as a geometry gesture.
    }
    if (!session().isLayerSelected(*id))
        session().setActiveLayer(*id);
    else
        session().setLayerSelection(session().selectedLayers(), *id, session().selectionAnchor());
    canvasWindow_->setLayerOutlineTargets(document->expandedLayers(session().selectedLayers()), hit.has_value());
    auto move = std::make_unique<core::LayerTransformSession>(*document, session().selectedLayers());
    if (!move->active() || !move->beginDrag(core::TransformHandle::Move, position))
        return false;
    synchronizeUi(false, false);
    activeLayerMove_ = std::move(move);
    if (modifiers.testFlag(Qt::AltModifier))
        duplicateMove_.emplace(DuplicateMove {position, session().layerSelectionState(), {}});
    updateActionState();
    return true;
}

bool MainWindow::prepareDuplicateMove()
{
    if (!duplicateMove_ || !activeLayerMove_ || !activeLayerMove_->targetAvailable()) return false;
    try {
        auto& doc = *session().document();
        auto command = core::duplicateLayerItems(doc, duplicateMove_->before);
        if (!command) return false;
        if (command->memoryCost() > session().history().memoryBudget())
            throw std::runtime_error("Duplication exceeds the undo memory budget");
        // Do not publish to global history until release: Escape/click/no-op
        // must keep the existing redo branch. Original targets never move.
        activeLayerMove_->cancel();
        if (!command->apply(doc)) return false;
        duplicateMove_->command = std::move(command);
        const auto& selection = *duplicateMove_->command->layerSelectionAfter(false);
        session().setLayerSelection(selection.ids, selection.primary, selection.anchor);
        synchronizeUi(true, false); // Prepare editable text/shape bounds too.
        auto move = std::make_unique<core::LayerTransformSession>(doc, session().selectedLayers());
        if (!move->active() || !move->beginDrag(core::TransformHandle::Move, duplicateMove_->press)) return false;
        activeLayerMove_ = std::move(move);
        canvasWindow_->setLayerOutlineTargets(doc.expandedLayers(session().selectedLayers()), true);
        return true;
    } catch (const std::exception& e) {
        statusBar()->showMessage(tr("Could not duplicate layers: %1").arg(QString::fromUtf8(e.what())), 5000);
        return false;
    }
}

bool MainWindow::beginMoveNumericEdit()
{
    if (fileBusy_ || suppressMoveNumeric_ || session().activeTool() != core::ToolId::Move
        || !session().document() || !session().activeLayer() || canvasWindow_->transformDragging())
        return false;
    if (!activeLayerMove_) {
        auto edit = std::make_unique<core::LayerTransformSession>(
            *session().document(), session().selectedLayers());
        if (!edit->active())
            return false;
        activeLayerMove_ = std::move(edit);
    }
    return activeLayerMove_->targetAvailable() && !activeLayerMove_->dragging();
}

void MainWindow::previewMoveValues(const core::TransformValues& values)
{
    if (!runTransformAction([&] { return beginMoveNumericEdit(); }))
        return;
    if (activeLayerMove_->setValues(values)) {
        canvasWindow_->setDocument(session().document()->snapshot(), false);
        refreshMoveControls();
        updateActionState();
    }
}

void MainWindow::flipMoveLayer(bool horizontal)
{
    if (!runTransformAction([&] { return beginMoveNumericEdit(); }))
        return;
    if (runTransformAction([&] { (void)activeLayerMove_->flip(horizontal); return true; }))
        finishLayerMove(true);
}

void MainWindow::refreshMoveControls()
{
    if (!moveOptionsPage_)
        return;
    std::optional<core::TransformValues> values;
    if (activeLayerMove_ && activeLayerMove_->targetAvailable()) {
        values = activeLayerMove_->values();
    } else {
        const auto* layer = session().document() && session().activeLayer()
            ? session().document()->layer(*session().activeLayer())
            : nullptr;
        if (session().document() && (!layer || layer->crop || session().selectedLayers().size() > 1)) {
            core::LayerTransformSession group(*session().document(), session().selectedLayers());
            if (group.active())
                values = group.values();
        } else if (layer)
            values = core::valuesFromTransform(
                layer->localToDocument, core::layerGeometryExtent(*layer));
    }
    // Blocking callbacks also protects focus-loss signals while disabling a
    // page whose target disappeared. A UI refresh must never start an edit.
    const QScopedValueRollback guard(suppressMoveNumeric_, true);
    moveOptionsPage_->setRelativeRotation(session().document()
        && session().document()->expandedLayers(session().selectedLayers()).size() > 1);
    moveOptionsPage_->setValues(values.value_or(core::TransformValues { }));
    moveOptionsPage_->setEnabled(values.has_value());
}

void MainWindow::finishLayerMove(bool apply)
{
    canvasWindow_->setSnapGuides({});
    if (!activeLayerMove_)
        return;
    const QScopedValueRollback guard(suppressMoveNumeric_, true);
    auto move = std::move(activeLayerMove_);
    auto duplicate = std::move(duplicateMove_);
    duplicateMove_.reset();
    if (duplicate && duplicate->command) {
        bool rolledBack = false;
        try {
            auto& doc = *session().document();
            const bool commit = apply && (duplicate->preparedForCommit || (move->targetAvailable()
                && duplicate->command->captureAddedLayerTransforms(doc)));
            duplicate->preparedForCommit = commit;
            move->cancel();
            if (!apply) {
                pointerRouter_->cancelCapture();
                canvasWindow_->cancelTransformInput();
            }
            if (!duplicate->command->undo(doc)) throw std::runtime_error("Duplicate targets changed");
            rolledBack = true;
            const auto& before = duplicate->before;
            session().setLayerSelection(before.ids, before.primary, before.anchor);
            // Reinsert the same prepared copies at their final transforms. No
            // second pixel copy; one undo restores hierarchy and selection.
            if (commit && session().execute(std::move(duplicate->command))) fileState().untouched = false;
        } catch (const std::exception& e) {
            // Structural rollback can allocate. Keep its command available
            // for retry/Escape instead of abandoning untracked preview copies.
            if (!rolledBack) {
                activeLayerMove_ = std::move(move);
                duplicateMove_ = std::move(duplicate);
            }
            statusBar()->showMessage(tr("Could not finish duplicate move: %1").arg(QString::fromUtf8(e.what())), 5000);
        }
        synchronizeUi(true, false);
        return;
    }
    if (!apply) {
        pointerRouter_->cancelCapture();
        canvasWindow_->cancelTransformInput();
        move->cancel();
    } else {
        try {
            move->endDrag();
            const auto result = move->commit(session().history());
            if (result == core::TransformCommitResult::TargetUnavailable && move->active()) {
                activeLayerMove_ = std::move(move);
                return;
            }
            if (result == core::TransformCommitResult::Committed)
                fileState().untouched = false;
        } catch (const std::exception&) {
            activeLayerMove_ = std::move(move);
            statusBar()->showMessage(QStringLiteral("Could not finish the move. Your edits are retained; retry or cancel."), 5000);
            return;
        }
    }
    synchronizeUi(false, false);
}

void MainWindow::stepTransformHistory(bool redo)
{
    if (!layerTransform_)
        return;
    transformOptionsPage_->finishNumericInput();
    if (canvasWindow_->transformDragging()) {
        // A held pointer is a preview, not a completed action to undo.
        pointerRouter_->cancelCapture();
        canvasWindow_->cancelTransformInput();
    } else if (redo) {
        (void)runTransformAction([&] { return layerTransform_->redo(); });
    } else {
        (void)runTransformAction([&] { return layerTransform_->undo(); });
    }
    refreshLayerTransform();
    updateActionState();
}

bool MainWindow::executeDocumentCommand(std::unique_ptr<core::Command> command)
{
    if (fileBusy_)
        return false;
    cancelPendingEdits();
    return session().execute(std::move(command));
}

void MainWindow::beginLayerTransform()
try {
    if(layerCrop_)return;
    shapeOptionsPage_->finishNumericInput();
    if (selectionTransform_)
        return;
    if (core::isSelectionTool(session().activeTool())) {
        beginSelectionTransform();
        return;
    }
    if (layerTransform_)
        return;
    if (session().activeTool() == core::ToolId::Move
        && (!activeLayerMove_ || !activeLayerMove_->dragging()))
        moveOptionsPage_->finishNumericInput();
    finishLayerMove(false);
    cancelActiveBrushStroke();
    // An empty first-time text edit is removed on completion, so settle its
    // creation before capturing a transform target or history baseline.
    const auto targetBeforeTextCompletion = session().activeLayer();
    if (textController_)
        textController_->finish();
    if (textController_ && textController_->active())
        return;
    if (targetBeforeTextCompletion && session().document()
        && !session().document()->containsItem(*targetBeforeTextCompletion))
        return;
    if (!session().document() || !session().activeLayer())
        return;
    auto transaction = std::make_unique<core::LayerTransformSession>(
        *session().document(), session().selectedLayers());
    if (!transaction->active()) {
        statusBar()->showMessage(
            QStringLiteral("Transform requires available, invertible layer geometry"), 3500);
        return;
    }
    toolBeforeTransform_ = session().activeTool();
    // Switch before publishing the session: a normal tool change cancels any
    // pending session, but this deliberate entry must not cancel itself.
    setActiveTool(core::ToolId::Transform);
    layerTransform_ = std::move(transaction);
    refreshLayerTransform();
    updateActionState();
    statusBar()->showMessage(
        QStringLiteral("Transform · drag inside to move, handles to scale, outside corners to "
                       "rotate · %1 applies · Escape cancels").arg(shortcutLabel(shortcuts_, "FinishOperationAction")));
    canvasWindow_->requestActivate();
} catch (const std::exception&) {
    statusBar()->showMessage(QStringLiteral("Could not start the transform. No group edit was applied."), 5000);
}

void MainWindow::refreshLayerTransform()
{
    prepareShapeCaches();
    if (!layerTransform_)
        return;
    if (!layerTransform_->targetAvailable()) {
        finishLayerTransform(false);
        return;
    }
    // No layer-model reset, raster revision, dirty pixels, thumbnails, or GPU
    // uploads: the snapshot just carries the new affine matrix to Vulkan.
    canvasWindow_->setDocument(session().document()->snapshot(), false);
    canvasWindow_->setTransformOverlay(
        render::TransformOverlay { layerTransform_->transform(), layerTransform_->extent(),
            layerTransform_->values().rotationDegrees, layerTransform_->geometryExtent() });
    transformOptionsPage_->setValues(layerTransform_->values());
}

void MainWindow::finishLayerTransform(bool apply)
{
    if (!layerTransform_)
        return;
    // Enter while a pointer is held is ignored. Release ends the gesture, not
    // the session. Escape always releases routing ownership before rollback.
    if (apply && canvasWindow_->transformDragging())
        return;
    auto transaction = std::move(layerTransform_);
    if (!apply)
        pointerRouter_->cancelCapture();
    canvasWindow_->cancelTransformInput();
    core::TransformCommitResult result = core::TransformCommitResult::NoChange;
    if (apply) {
        try {
            result = transaction->commit(session().history());
            if (result == core::TransformCommitResult::TargetUnavailable && transaction->active()) {
                layerTransform_ = std::move(transaction);
                return;
            }
        } catch (const std::exception&) {
            layerTransform_ = std::move(transaction);
            statusBar()->showMessage(QStringLiteral("Could not apply the transform. Your edits are retained; retry or cancel."), 5000);
            return;
        }
    } else
        transaction->cancel();
    // Hide may finish numeric text editing. The session is already absent, so
    // those delayed widget signals cannot resurrect a cancelled preview.
    canvasWindow_->setTransformOverlay(std::nullopt);
    setActiveTool(toolBeforeTransform_);
    if (result == core::TransformCommitResult::Committed)
        fileState().untouched = false;
    synchronizeUi(false, false);
    statusBar()->showMessage(apply ? result == core::TransformCommitResult::Committed
                ? QStringLiteral("Layer transform applied")
                : result == core::TransformCommitResult::NoChange
                ? QStringLiteral("Transform unchanged")
                : QStringLiteral("Transform target is no longer available")
                                   : QStringLiteral("Layer transform cancelled"),
        2500);
}

bool MainWindow::finishCanvasOperation()
{
    if (fileBusy_ || (textController_ && textController_->active()))
        return false;
    if(layerCrop_){canvasWindow_->finishPointerGestureForCommit();finishLayerCrop(true);return true;}
    if (layerTransform_ || selectionTransform_) {
        canvasWindow_->finishPointerGestureForCommit();
        transformOptionsPage_->finishNumericInput();
        if (selectionTransform_)
            finishSelectionTransform(true);
        else
            finishLayerTransform(true);
        return true;
    }
    if (anchoredLassoActive()) {
        const auto point = selectionGesture_->deferredPointer.value_or(selectionGesture_->pointer);
        canvasWindow_->finishPointerGestureForCommit();
        closeLasso(point);
        return true;
    }
    if (selectionGesture_ && session().activeTool() == core::ToolId::Lasso
        && canvasWindow_->selectionDragging()) {
        canvasWindow_->finishPointerGestureForCommit();
        return true;
    }
    if (shapeCreation_ && std::get<core::ShapeLayer>(shapeCreation_->layer.payload).kind == core::ShapeKind::Polygon) {
        finishShapeCreation(false);
        return true;
    }
    return false;
}
}
