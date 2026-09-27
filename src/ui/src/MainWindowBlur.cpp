#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/LocalBlurOptionsPage.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QScopedValueRollback>
#include <QStatusBar>

namespace imageeditor::ui {
void MainWindow::deferLocalBlurInput(QObject* receiver, QEvent* event)
{
    if (!receiver || !event || localBlurCancelRequested_
        || (event->type() != QEvent::MouseMove && event->type() != QEvent::TabletMove
            && event->type() != QEvent::MouseButtonRelease && event->type() != QEvent::TabletRelease)) return;
    // Preserve turns, tablet pressure, modifiers and the final release in FIFO
    // order. Overflow cancels the entire stroke rather than silently changing
    // its geometry. The current callback then clears latched pointer capture.
    if (deferredLocalBlurInput_.size() >= 4096) {
        deferredLocalBlurInput_.clear();
        localBlurCancelRequested_ = true;
        return;
    }
    deferredLocalBlurInput_.emplace_back(QPointer<QObject>(receiver), std::unique_ptr<QEvent>(event->clone()));
}
void MainWindow::flushDeferredLocalBlurInput()
{
    auto input = std::move(deferredLocalBlurInput_);
    deferredLocalBlurInput_.clear();
    if (localBlurCancelRequested_) return;
    for (auto& [receiver, event] : input)
        if (receiver) QCoreApplication::postEvent(receiver, event.release());
}

void MainWindow::createLocalBlurControls()
{
    localBlurOptionsPage_ = new LocalBlurOptionsPage;
    toolOptionsBar_->registerToolPage(core::ToolId::LocalBlur, tr("Local Blur"), localBlurOptionsPage_);
    localBlurOptionsPage_->setBrushSettings(brushSettings_);
    localBlurOptionsPage_->setBlurSettings(localBlurSettings_);
    localBlurOptionsPage_->onBrushSettingsChanged = [this](const core::BrushSettings& settings) {
        applyBrushSettings(settings);
    };
    localBlurOptionsPage_->onBlurSettingsChanged = [this](const core::BlurSettings& settings) {
        cancelLocalBlurStroke();
        localBlurSettings_ = settings;
    };
}

bool MainWindow::beginLocalBlurStroke(const core::NormalizedPointerSample& sample)
{
    if (localBlurProcessing_) return false;
    cancelActiveBrushStroke();
    auto* document = session().document();
    const auto target = session().activeLayer();
    const auto* layer = document && target ? document->layer(*target) : nullptr;
    if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
        statusBar()->showMessage(tr("Local Blur edits raster source colors. Select a raster layer first."), 4500);
        return false;
    }
    const auto unavailable = brushAssets_->unavailableMessage(brushSettings_);
    if (!unavailable.isEmpty()) { statusBar()->showMessage(unavailable, 4500); return false; }
    localBlurCancelRequested_ = false;
    activeLocalBlurStroke_ = std::make_unique<core::LocalBlurStroke>(
        *document, *target, brushSettings_, localBlurSettings_, brushAssets_.get());
    bool started = false;
    {
        QScopedValueRollback processing(localBlurProcessing_, true);
        QElapsedTimer cadence; cadence.start();
        statusBar()->showMessage(tr("Local Blur · preparing original source and neighborhood…"));
        started = activeLocalBlurStroke_->begin(sample, [&] {
            // Shared routing defers the trace until this handler returns and
            // installs capture; Escape remains responsive during convolution.
            if (cadence.elapsed() >= 8) {
                QApplication::processEvents(QEventLoop::AllEvents, 4);
                cadence.restart();
            }
            return localBlurCancelRequested_;
        });
    }
    flushDeferredLocalBlurInput();
    if (!started) {
        const auto detail = QString::fromStdString(activeLocalBlurStroke_->diagnostic());
        activeLocalBlurStroke_.reset();
        statusBar()->showMessage(detail.isEmpty() ? tr("Unable to start Local Blur at this target scale.") : detail, 5500);
        return false;
    }
    canvasWindow_->setResolvedBrushCursorAngle(activeLocalBlurStroke_->brush().lastResolvedTipAngleDegrees().value_or(0));
    canvasWindow_->setConstrainedBrushPosition(activeLocalBlurStroke_->brush().constrainedPosition());
    statusBar()->showMessage(tr("Local Blur · source colors, alpha preserved · Escape cancels"));
    canvasWindow_->scheduleFrame();
    return true;
}

bool MainWindow::moveLocalBlurStroke(const core::NormalizedPointerSample& sample)
{
    if (localBlurProcessing_) return true;
    if (!activeLocalBlurStroke_) return false;
    bool continued = false;
    {
        QScopedValueRollback processing(localBlurProcessing_, true);
        QElapsedTimer cadence; cadence.start();
        continued = activeLocalBlurStroke_->append(sample, [&] {
            // Preserve the full trace and release ordering through the barrier.
            if (cadence.elapsed() >= 8) {
                QApplication::processEvents(QEventLoop::AllEvents, 4);
                cadence.restart();
            }
            return localBlurCancelRequested_;
        });
    }
    flushDeferredLocalBlurInput();
    if (!continued) { cancelLocalBlurStroke(); return false; }
    if (const auto angle = activeLocalBlurStroke_->brush().lastResolvedTipAngleDegrees())
        canvasWindow_->setResolvedBrushCursorAngle(*angle);
    canvasWindow_->setConstrainedBrushPosition(activeLocalBlurStroke_->brush().constrainedPosition());
    canvasWindow_->scheduleFrame();
    return true;
}

bool MainWindow::endLocalBlurStroke(const core::NormalizedPointerSample& sample)
{
    if (!activeLocalBlurStroke_ || localBlurProcessing_) return false;
    core::RasterEditCommitResult result;
    {
        QScopedValueRollback processing(localBlurProcessing_, true);
        QElapsedTimer cadence; cadence.start();
        result = activeLocalBlurStroke_->end(sample, session().history(), [&] {
            if (cadence.elapsed() >= 8) {
                QApplication::processEvents(QEventLoop::AllEvents, 4);
                cadence.restart();
            }
            return localBlurCancelRequested_;
        });
    }
    flushDeferredLocalBlurInput();
    const auto detail = QString::fromStdString(activeLocalBlurStroke_->diagnostic());
    activeLocalBlurStroke_.reset();
    const bool completed = result == core::RasterEditCommitResult::Committed || result == core::RasterEditCommitResult::NoChanges;
    if (result == core::RasterEditCommitResult::Committed) fileState().untouched = false;
    synchronizeUi(false, false);
    statusBar()->showMessage(!detail.isEmpty() ? detail : !completed ? tr("Local Blur cancelled")
        : result == core::RasterEditCommitResult::NoChanges ? tr("Local Blur · no changed pixels")
        : tr("Local Blur applied"), 3500);
    return completed;
}

void MainWindow::cancelLocalBlurStroke()
{
    if (localBlurProcessing_) { localBlurCancelRequested_ = true; return; }
    if (!activeLocalBlurStroke_) return;
    const auto detail = QString::fromStdString(activeLocalBlurStroke_->diagnostic());
    activeLocalBlurStroke_->cancel();
    activeLocalBlurStroke_.reset();
    canvasWindow_->setConstrainedBrushPosition({});
    canvasWindow_->scheduleFrame();
    statusBar()->showMessage(detail.isEmpty() ? tr("Local Blur cancelled") : detail, 3000);
}
}
