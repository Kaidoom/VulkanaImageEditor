#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/RasterLimits.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include <QStatusBar>
#include <QVBoxLayout>
namespace imageeditor::ui {
void MainWindow::createCanvasResizeControls()
{
    canvasResizeOptionsPage_ = new CropOptionsPage(nullptr, CropOptionsPage::Mode::Canvas);
    const auto name = i18n::text("canvas.resize.action");
    toolOptionsBar_->registerToolPage(core::ToolId::CanvasResize, name, canvasResizeOptionsPage_);
    QVBoxLayout* content { };
    propertiesPanel_->addToolPage(
        core::ToolId::CanvasResize, name, i18n::text("canvas.resize.hint"), content);
    canvasResizeOptionsPage_->onChanged = [this](core::RectD r) {
        if (!canvasResize_)
            return;
        (void)canvasResize_->set(r);
        refreshQuickCanvasResize();
    };
    canvasResizeOptionsPage_->onActionFinished = [this] {
        if (canvasResize_) {
            (void)runTransformAction([&] { canvasResize_->complete(); return true; });
            refreshQuickCanvasResize();
            updateActionState();
        }
    };
    canvasResizeOptionsPage_->onRemove = [this] {
        if (canvasResize_) {
            (void)runTransformAction([&] { canvasResize_->reset(); return true; });
            refreshQuickCanvasResize();
            updateActionState();
        }
    };
    canvasResizeOptionsPage_->onApply = [this] { finishQuickCanvasResize(true); };
    canvasResizeOptionsPage_->onCancel = [this] { finishQuickCanvasResize(false); };
}
void MainWindow::beginQuickCanvasResize()
try
{
    if (fileBusy_ || workspaceDialog_ || canvasResize_ || !session().document() || !shortcutGestureIdle(true))
        return;
    if (!settleForFileOperation())
        return;
    auto frame = std::make_unique<core::CanvasResizeFrame>(
        session().document()->canvas().extent, kMaximumRasterDimension, kMaximumRasterPixels);
    const auto previous = session().activeTool();
    setActiveTool(core::ToolId::CanvasResize);
    if (session().activeTool() != core::ToolId::CanvasResize)
        return;
    toolBeforeCanvasResize_ = previous == core::ToolId::Transform || previous == core::ToolId::Crop
        ? core::ToolId::Move
        : previous;
    canvasResize_ = std::move(frame);
    canvasResizeRevision_ = session().document()->revision();
    refreshQuickCanvasResize();
    updateActionState();
    canvasWindow_->requestActivate();
} catch (const std::exception& e) {
    reportFileError(QString::fromUtf8(e.what()));
}
void MainWindow::refreshQuickCanvasResize()
{
    if (!canvasResize_)
        return;
    if (!session().document() || session().document()->revision() != canvasResizeRevision_) {
        finishQuickCanvasResize(false);
        return;
    }
    const auto r = canvasResize_->frame();
    canvasWindow_->setTransformOverlay(render::TransformOverlay { { 1, 0, double(r.x), 0, 1, double(r.y) },
        { std::uint32_t(r.width), std::uint32_t(r.height) }, 0, { }, false });
    canvasResizeOptionsPage_->setFrame({ double(r.x), double(r.y), double(r.width), double(r.height) });
}
void MainWindow::finishQuickCanvasResize(bool apply)
{
    if (!canvasResize_ || (apply && canvasWindow_->transformDragging()))
        return;
    if (apply)
        canvasResizeOptionsPage_->finishNumericInput();
    if (!canvasResize_)
        return;
    const auto r = canvasResize_->frame();
    const auto previousTool = toolBeforeCanvasResize_;
    if (apply) {
        const auto* doc = session().document();
        if (!doc || doc->revision() != canvasResizeRevision_) {
            finishQuickCanvasResize(false);
            return;
        }
        if (!rasterExtentWithinLimits(std::uint32_t(r.width), std::uint32_t(r.height)))
            return;
        const auto before = doc->canvas();
        try {
            // No-op Apply is a clean exit and must not discard a redo branch.
            if (r.x || r.y || r.width != int(before.extent.width) || r.height != int(before.extent.height)) {
                if (!session().execute(std::make_unique<core::ReframeCanvasCommand>(r)))
                    return;
                fileState().untouched = false;
                adjustViewAfterCanvasResize(before);
            }
        } catch (const std::exception& e) {
            reportFileError(QString::fromUtf8(e.what()));
            return;
        }
    }
    canvasResize_.reset();
    pointerRouter_->cancelCapture();
    canvasWindow_->cancelTransformInput();
    canvasWindow_->setTransformOverlay({ });
    setActiveTool(previousTool);
    synchronizeUi(false, false);
    if (apply)
        statusBar()->showMessage(i18n::text("canvas.resize.done").arg(r.width).arg(r.height), 3500);
}
void MainWindow::stepCanvasResizeHistory(bool redo)
{
    if (!canvasResize_)
        return;
    canvasResizeOptionsPage_->finishNumericInput();
    if (canvasWindow_->transformDragging()) {
        pointerRouter_->cancelCapture();
        canvasWindow_->cancelTransformInput();
    } else
        (void)runTransformAction([&] { canvasResize_->step(redo); return true; });
    refreshQuickCanvasResize();
    updateActionState();
}
void MainWindow::adjustViewAfterCanvasResize(core::CanvasSpec before)
{
    const auto hint = session().history().canvasOriginHint();
    if (!hint || !session().document())
        return;
    const auto after = session().document()->canvas();
    auto view = canvasWindow_->scene().viewport;
    // Keep the same world location under the viewport on Apply, Undo and Redo.
    view.panBy({ (hint->x + (double(after.extent.width) - before.extent.width) * .5) * view.zoom(),
        (hint->y + (double(after.extent.height) - before.extent.height) * .5) * view.zoom() });
    const core::Vec2d delta { -double(hint->x), -double(hint->y) };
    auto measurement = canvasWindow_->scene().measurement;
    if (measurement) {
        measurement->a = measurement->a + delta;
        measurement->b = measurement->b + delta;
    }
    if (cloneAnchor_) {
        cloneAnchor_->point = cloneAnchor_->point + delta;
        cloneAnchor_->sourceTransform
            = core::composeTransform({ 1, 0, delta.x, 0, 1, delta.y }, cloneAnchor_->sourceTransform);
    }
    canvasWindow_->restoreDocumentView(view, measurement);
}
} // namespace imageeditor::ui
