#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/TransformOptionsPage.hpp"
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <cmath>

namespace imageeditor::ui
{
namespace
{
bool sameGeometry(const core::TransformValues &a, const core::TransformValues &b, core::Extent2u extent)
{
    const auto ma = core::transformFromValues(a, extent), mb = core::transformFromValues(b, extent);
    for (const auto p :
         {core::Vec2d{}, core::Vec2d{double(extent.width), 0},
          core::Vec2d{double(extent.width), double(extent.height)}, core::Vec2d{0, double(extent.height)}}) {
        const auto delta = ma.map(p) - mb.map(p);
        if (std::hypot(delta.x, delta.y) > 1e-8)
            return false;
    }
    return true;
}
// Cheap affine contour preview, clipped to the canvas. The final R8 contour
// (including coverage introduced on the clip perimeter) is resolved on Apply.
std::vector<core::SelectionEdge> mappedEdges(const core::SelectionMask &mask, const core::AffineTransform &m)
{
    std::vector<core::SelectionEdge> result;
    result.reserve(mask.boundaryEdges().size());
    for (const auto &edge : mask.boundaryEdges()) {
        const auto a = m.map(edge.from), b = m.map(edge.to), d = b - a;
        double begin = 0, end = 1;
        const auto clip = [&](double p, double q) {
            if (p == 0)
                return q >= 0;
            const auto t = q / p;
            if (p < 0)
                begin = std::max(begin, t);
            else
                end = std::min(end, t);
            return begin <= end;
        };
        if (clip(-d.x, a.x) && clip(d.x, mask.extent().width - a.x) && clip(-d.y, a.y) &&
            clip(d.y, mask.extent().height - a.y) && begin < end)
            result.push_back({{a.x + d.x * begin, a.y + d.y * begin}, {a.x + d.x * end, a.y + d.y * end}});
    }
    return result;
}
} // namespace

void MainWindow::beginSelectionTransform()
{
    if (selectionTransform_)
        return;
    finishSelectionNumericInput();
    cancelPendingEdits();
    const auto *document = session().document();
    const auto mask = document ? document->selection() : core::SelectionState{};
    if (!mask || mask->bounds().empty()) {
        statusBar()->showMessage(QStringLiteral("Select some coverage before transforming the selection"),
                                 3000);
        return;
    }
    const auto b = mask->bounds();
    SelectionTransform edit;
    edit.owner = activeDocument_;
    edit.before = mask;
    edit.bounds = b;
    edit.values.center = {b.x + b.width * 0.5, b.y + b.height * 0.5};
    edit.checkpoints.push_back(edit.values);
    toolBeforeTransform_ = session().activeTool();
    setActiveTool(core::ToolId::Transform);
    selectionTransform_ = std::move(edit);
    propertiesPanel_->setActiveTool(core::ToolId::Transform);
    if (const auto tool = toolActionMap_.find(toolBeforeTransform_); tool != toolActionMap_.end())
        tool->second->setChecked(true);
    refreshSelectionTransform();
    updateActionState();
    statusBar()->showMessage(
        QStringLiteral(
            "Transform selection only · handles scale · outside corners rotate · %1 applies · Escape cancels")
            .arg(shortcutLabel(shortcuts_, "FinishOperationAction")));
    canvasWindow_->requestActivate();
}

void MainWindow::beginSelectedPixelTransform()
{
    if(session().editingLayerMask()){statusBar()->showMessage(tr("Click the content thumbnail before transforming selected pixels."),3500);return;}
    if (selectionTransform_ || fileBusy_ || !session().document())
        return;
    cancelPendingEdits();
    try {
        if (!session().activeLayer())
            throw std::invalid_argument("Choose a primary raster layer first");
        auto pixels = std::make_unique<core::SelectedPixelTransformSession>(*session().document(),
                                                                            *session().activeLayer());
        beginSelectionTransform();
        if (!selectionTransform_)
            return;
        selectionTransform_->pixels = std::move(pixels);
        refreshSelectionTransform();
        updateActionState();
        statusBar()->showMessage(tr("Transform selected pixels · Ctrl+corner distorts · Enter applies · "
                                    "Escape restores the original"),
                                 5000);
    } catch (const std::exception &error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}

void MainWindow::flushSelectedPixelPreview()
{
    if (selectedPixelPreviewTimer_)
        selectedPixelPreviewTimer_->stop();
    if (!queuedPixelTransform_)
        return;
    auto values = *queuedPixelTransform_;
    queuedPixelTransform_.reset();
    if (!selectionTransform_ || !selectionTransform_->pixels || selectionTransform_->owner != activeDocument_)
        return;
    auto &edit = *selectionTransform_;
    try {
        const auto mapping = core::composeTransform(
            core::transformFromValues(values, edit.extent()),
            core::ProjectiveTransform{1, 0, -double(edit.bounds.x), 0, 1, -double(edit.bounds.y)});
        if (!edit.pixels->preview(mapping)) {
            statusBar()->showMessage(tr("Invalid pixel transform — last valid preview retained"), 3000);
            return;
        }
        edit.values = values;
        edit.snapping.acknowledgeTranslation(session().document()->revision());
        refreshSelectionTransform();
        // Publishing changed pixels invalidates old scene guides. Install the
        // guides for this evaluated result only after its document snapshot.
        canvasWindow_->setSnapGuides(edit.pendingGuides);
    } catch (const std::exception &error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}

void MainWindow::refreshSelectionTransform()
{
    if (!selectionTransform_)
        return;
    const auto &edit = *selectionTransform_;
    if (edit.owner != activeDocument_)
        return;
    if (edit.pixels)
        canvasWindow_->setDocument(session().document()->snapshot(), false);
    canvasWindow_->setSelectionPreview(
        std::make_shared<const std::vector<core::SelectionEdge>>(mappedEdges(*edit.before, edit.mapping())));
    canvasWindow_->setTransformOverlay(
        render::TransformOverlay{edit.matrix(), edit.extent(), edit.values.rotationDegrees});
    transformOptionsPage_->setValues(edit.values);
    toolOptionsBar_->setContextLabel(edit.pixels ? tr("Selected pixels") : tr("Selection"));
}

void MainWindow::setSelectionTransformValues(core::TransformValues values)
{
    if (!selectionTransform_)
        return;
    auto &edit = *selectionTransform_;
    if (!edit.drag) edit.pendingGuides = {}; // Numeric edits are not snapped.
    if (!std::isfinite(values.center.x) || !std::isfinite(values.center.y) ||
        std::abs(values.center.x) > 1e9 || std::abs(values.center.y) > 1e9 || !std::isfinite(values.scaleX) ||
        !std::isfinite(values.scaleY) || !std::isfinite(values.rotationDegrees))
        return;
    const auto scale = [](double next, double previous) {
        return std::copysign(std::clamp(std::abs(next), core::kMinimumLayerScale, core::kMaximumLayerScale),
                             next == 0 ? previous : next);
    };
    values.scaleX = scale(values.scaleX, edit.values.scaleX);
    values.scaleY = scale(values.scaleY, edit.values.scaleY);
    values.rotationDegrees = std::remainder(values.rotationDegrees, 360.0);
    if (!core::transformFromValues(values, edit.extent())
             .validOver({-.5, -.5, double(edit.bounds.width) + 1, double(edit.bounds.height) + 1}))
        return;
    if (edit.pixels) {
        queuedPixelTransform_ = values;
        if (edit.drag) {
            if (!selectedPixelPreviewTimer_) {
                selectedPixelPreviewTimer_ = new QTimer(this);
                selectedPixelPreviewTimer_->setSingleShot(true);
                connect(selectedPixelPreviewTimer_, &QTimer::timeout, this,
                        &MainWindow::flushSelectedPixelPreview);
            }
            if (!selectedPixelPreviewTimer_->isActive())
                selectedPixelPreviewTimer_->start(16);
        } else
            flushSelectedPixelPreview();
        return;
    }
    edit.values = values;
    canvasWindow_->setSnapGuides(edit.pendingGuides);
    refreshSelectionTransform();
}

void MainWindow::completeSelectionTransformAction()
{
    flushSelectedPixelPreview();
    if (!selectionTransform_ || selectionTransform_->drag)
        return;
    auto &edit = *selectionTransform_;
    if (sameGeometry(edit.values, edit.checkpoints[edit.checkpoint], edit.extent())) {
        edit.values = edit.checkpoints[edit.checkpoint];
    } else {
        edit.checkpoints.reserve(edit.checkpoint + 2);
        if (edit.pixels && !runTransformAction([&] { return edit.pixels->completeAction(); }))
            return;
        edit.checkpoints.resize(edit.checkpoint + 1);
        edit.checkpoints.push_back(edit.values);
        ++edit.checkpoint;
    }
    refreshSelectionTransform();
    updateActionState();
}

void MainWindow::stepSelectionTransformHistory(bool redo)
{
    if (!selectionTransform_)
        return;
    transformOptionsPage_->finishNumericInput();
    if (canvasWindow_->transformDragging()) {
        pointerRouter_->cancelCapture();
        canvasWindow_->cancelTransformInput();
    } else {
        auto &edit = *selectionTransform_;
        flushSelectedPixelPreview();
        if (edit.pixels &&
            !runTransformAction([&] { return redo ? edit.pixels->redo() : edit.pixels->undo(); }))
            return;
        if (redo && edit.checkpoint + 1 < edit.checkpoints.size())
            ++edit.checkpoint;
        else if (!redo && edit.checkpoint)
            --edit.checkpoint;
        edit.values = edit.checkpoints[edit.checkpoint];
    }
    refreshSelectionTransform();
    updateActionState();
}

void MainWindow::finishSelectionTransform(bool apply)
{
    if (!selectionTransform_ || (apply && canvasWindow_->transformDragging()))
        return;
    if (apply)
        transformOptionsPage_->finishNumericInput();
    if (apply)
        flushSelectedPixelPreview();
    else {
        queuedPixelTransform_.reset();
        if (selectedPixelPreviewTimer_)
            selectedPixelPreviewTimer_->stop();
    }
    if (selectionTransform_->pixels) {
        auto &pending = *selectionTransform_;
        if (apply) {
            if (pending.owner != activeDocument_)
                return;
            try {
                const auto result = pending.pixels->commit(session().history());
                if (result == core::TransformCommitResult::TargetUnavailable) {
                    statusBar()->showMessage(
                        tr("Transform target changed; cancel or restore the target before applying"), 5000);
                    return;
                }
                if (result == core::TransformCommitResult::Committed)
                    fileState().untouched = false;
            } catch (const std::exception &e) {
                statusBar()->showMessage(QString::fromUtf8(e.what()), 5000);
                return;
            }
        } else
            pending.pixels->cancel();
    }
    auto edit = std::move(*selectionTransform_);
    selectionTransform_.reset(); // late numeric/release signals cannot publish
    pointerRouter_->cancelCapture();
    canvasWindow_->cancelTransformInput();
    if (apply && !edit.pixels && session().document() && session().document()->selection() == edit.before) {
        try {
            if (!sameGeometry(edit.values, edit.checkpoints.front(), edit.extent()))
                session().execute(std::make_unique<core::SetSelectionCommand>(
                    edit.before->transformed(edit.mapping()), "Transform selection"));
        } catch (const std::exception &error) {
            QMessageBox::warning(this, QStringLiteral("Unable to transform selection"),
                                 QString::fromUtf8(error.what()));
        }
    }
    canvasWindow_->setSelectionPreview({});
    canvasWindow_->setSnapGuides({});
    canvasWindow_->setTransformOverlay({});
    setActiveTool(toolBeforeTransform_);
    synchronizeUi(false, false);
    statusBar()->showMessage(apply ? (edit.pixels ? tr("Selected-pixel transform applied")
                                                  : QStringLiteral("Selection transform applied"))
                                   : QStringLiteral("Selection transform cancelled"),
                             2500);
}
} // namespace imageeditor::ui
