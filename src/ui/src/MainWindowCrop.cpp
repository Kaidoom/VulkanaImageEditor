#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QVBoxLayout>
namespace imageeditor::ui {
void MainWindow::createCropControls()
{
    cropOptionsPage_ = new CropOptionsPage;
    toolOptionsBar_->registerToolPage(core::ToolId::Crop, i18n::text("main.window.crop.layer.crop"), cropOptionsPage_);
    QVBoxLayout* content = nullptr;
    propertiesPanel_->addToolPage(
        core::ToolId::Crop, i18n::text("main.window.crop.layer.crop"),
        i18n::text("main.window.crop.toolaction.crop.crop.the.primary.layer.without.deleting.pixels"),
        content);
    cropOptionsPage_->onChanged = [this](core::RectD r) {
        if (layerCrop_ && runTransformAction([&] { return layerCrop_->setRect(r); }))
            refreshLayerCrop();
    };
    cropOptionsPage_->onActionFinished = [this] {
        if (layerCrop_) {
            (void)runTransformAction([&] { return layerCrop_->completeAction(); });
            refreshLayerCrop();
            updateActionState();
        }
    };
    cropOptionsPage_->onRemove = [this] {
        if (layerCrop_) {
            (void)runTransformAction([&] { return layerCrop_->removeCrop(); });
            refreshLayerCrop();
            updateActionState();
        }
    };
    cropOptionsPage_->onPreviewChanged = [this] { refreshLayerCrop(); };
    cropOptionsPage_->onChamferChanged = [this] {refreshLayerCrop();};
    cropOptionsPage_->onApply = [this] { finishLayerCrop(true); };
    cropOptionsPage_->onCancel = [this] { finishLayerCrop(false); };
}
void MainWindow::beginLayerCrop()
try {
    if (fileBusy_ || layerCrop_ || !session().document() || !session().activeLayer() || canvasWindow_->pointerGestureActive())
        return;
    const auto previous = session().activeTool();
    cancelPendingEdits();
    prepareShapeCaches();
    if (!session().activeLayer())
        return;
    auto edit = std::make_unique<core::LayerCropSession>(*session().document(),
        *session().activeLayer());
    if (!edit->active()) {
        statusBar()->showMessage(
            i18n::text("main.window.crop.layer.crop.needs.a.primary.raster.text.or.shape"),
            5000);
        return;
    }
    toolBeforeCrop_ = previous == core::ToolId::Transform || previous == core::ToolId::Crop
        ? core::ToolId::Move
        : previous;
    {
        const QScopedValueRollback guard(startingCrop_, true);
        setActiveTool(core::ToolId::Crop);
    }
    layerCrop_ = std::move(edit);
    if(const auto* target=session().document()->layer(layerCrop_->layerId());target->crop&&target->crop->hasChamfer())
        cropOptionsPage_->setChamferEnabled(true);
    refreshLayerCrop();
    updateActionState();
    canvasWindow_->requestActivate();
} catch (const std::exception& e) {
    statusBar()->showMessage(
        i18n::text("main.window.crop.could.not.start.layer.crop.1").arg(e.what()), 5000);
}
void MainWindow::refreshLayerCrop()
{
    if (!layerCrop_)
        return;
    if (!layerCrop_->targetAvailable()) {
        finishLayerCrop(false);
        return;
    }
    const auto r = layerCrop_->frame();
    canvasWindow_->setDocument(session().document()->snapshot(), false);
    canvasWindow_->setCropPreviewLayer(cropOptionsPage_->showSource()
            ? std::optional(layerCrop_->layerId())
            : std::nullopt);
    render::TransformOverlay overlay { layerCrop_->frameTransform(),
            { 1, 1 },
            0,
            core::Extent2d { r.width, r.height },
            false };
    auto geometry=session().document()->layer(layerCrop_->layerId())->crop.value_or(core::LayerCrop{r});
    geometry.x=geometry.y=0;geometry.width=r.width;geometry.height=r.height;
    overlay.cropGeometry=geometry;overlay.chamferHandles=cropOptionsPage_->chamferEnabled();
    canvasWindow_->setTransformOverlay(overlay);
    cropOptionsPage_->setFrame(r);
}
void MainWindow::finishLayerCrop(bool apply)
{
    if (!layerCrop_ || (apply && canvasWindow_->transformDragging()))
        return;
    if (apply)
        cropOptionsPage_->finishNumericInput();
    auto edit = std::move(layerCrop_);
    if (!apply)
        pointerRouter_->cancelCapture();
    canvasWindow_->cancelTransformInput();
    if (apply) {
        try {
            const auto result = edit->commit(session().history());
            if (result == core::TransformCommitResult::TargetUnavailable && edit->active()) {
                layerCrop_ = std::move(edit);
                return;
            }
            if (result == core::TransformCommitResult::Committed)
                fileState().untouched = false;
        } catch (const std::exception&) {
            layerCrop_ = std::move(edit);
            statusBar()->showMessage("Could not apply crop history; retry or Cancel.",
                5000);
            return;
        }
    } else
        edit->cancel();
    canvasWindow_->setCropPreviewLayer({ });
    canvasWindow_->setTransformOverlay({ });
    setActiveTool(toolBeforeCrop_);
    synchronizeUi(false, false);
}
void MainWindow::stepCropHistory(bool redo)
{
    if (!layerCrop_)
        return;
    cropOptionsPage_->finishNumericInput();
    if (canvasWindow_->transformDragging()) {
        pointerRouter_->cancelCapture();
        canvasWindow_->cancelTransformInput();
    } else
        (void)runTransformAction(
            [&] { return redo ? layerCrop_->redo() : layerCrop_->undo(); });
    refreshLayerCrop();
    updateActionState();
}
} // namespace imageeditor::ui
