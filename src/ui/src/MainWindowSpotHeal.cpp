#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/CloningOptionsPage.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/render/CanvasWindow.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QDebug>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTimer>
#include <QTabBar>
#include <algorithm>
#include <chrono>

namespace imageeditor::ui {
namespace {
using ProfileClock = std::chrono::steady_clock;
bool profileSpotHeal()
{
    static const bool enabled = qEnvironmentVariableIntValue("VULKANA_PROFILE_SPOT_HEAL") > 0;
    return enabled;
}
double profileMilliseconds(ProfileClock::time_point begin, ProfileClock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
}

// The worker owns frozen reference bytes and marks only. No Document, QWidget,
// writable surface or renderer callback crosses the worker boundary.
struct MainWindow::SpotHealJob {
    std::atomic_bool cancelled {false};
    std::atomic<double> progress {0};
    std::shared_ptr<const core::SpotHealWork> work;
    core::SpotHealSolved solved;
    std::shared_ptr<DocumentContext> context; // UI-owned lifetime; solver never reads it
    std::unique_ptr<core::SpotHealStroke> stroke;
    std::uint64_t generation {0};
    core::LayerId target {0};
    core::SpotHealOptions options;
    QElapsedTimer elapsed; // read by UI only
    ProfileClock::time_point releaseStarted, dispatched, workerStarted, workerFinished;
    double captureMilliseconds {0};
};

bool MainWindow::beginSpotHealStroke(const core::NormalizedPointerSample& sample)
{
    if (spotHealJob_ || cloneProcessing_ || fileBusy_) return false;
    cancelActiveBrushStroke();
    auto* document = session().document();
    const auto target = session().activeLayer();
    const auto* layer = document && target ? document->layer(*target) : nullptr;
    if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
        statusBar()->showMessage(tr("Spot Heal needs a raster layer. Text and shapes remain editable."), 5000);
        return false;
    }
    if (const auto diagnostic = core::spotHealTargetDiagnostic(*document, *target, cloneSettings_.source);
        !diagnostic.empty()) {
        statusBar()->showMessage(QString::fromStdString(diagnostic), 6000);
        return false;
    }
    if (document->selection() && document->selection()->bounds().empty()) {
        statusBar()->showMessage(tr("Spot Heal · the active selection is empty."), 4000);
        return false;
    }
    const auto unavailable = brushAssets_->unavailableMessage(brushSettings_);
    if (!unavailable.isEmpty()) { statusBar()->showMessage(unavailable, 5000); return false; }

    const auto revision = document->revision();
    std::string error;
    std::optional<core::CloneReference> reference;
    const auto captureStarted = profileSpotHeal() ? ProfileClock::now() : ProfileClock::time_point{};
    cloneCancelRequested_ = false;
    {
        // Capture must finish before the press installs pointer capture. Keep
        // releases queued; the expensive spatial preparation runs on the worker.
        QScopedValueRollback capture(cloneProcessing_, true);
        QElapsedTimer cadence; cadence.start();
        const auto cancelled = [&] {
            if (cadence.elapsed() >= 8) {
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 3);
                cadence.restart();
            }
            return cloneCancelRequested_ || session().document() != document
                || document->revision() != revision;
        };
        try {
            std::vector<core::SampleCacheOverride> prepared;
            std::size_t pixels = 0;
            constexpr auto budget = core::CloneReference::defaultSnapshotLimit / 4;
            if (cloneSettings_.source != core::CloneSampleSource::SourceLayer) {
                bool below = true;
                for (const auto& source : document->layers()) {
                    if ((below || cloneSettings_.source == core::CloneSampleSource::AllVisible)
                        && document->isEffectivelyVisible(source.id)
                        && !std::holds_alternative<core::RasterLayer>(source.payload)) {
                        auto cache = prepareDocumentSampleCache(source, budget - pixels);
                        if (!cache || !cache->surface)
                            throw std::runtime_error("Unable to prepare editable reference content for Spot Heal.");
                        const auto extent = cache->surface->extent();
                        pixels += std::size_t(extent.width) * extent.height;
                        if (pixels > budget) throw std::length_error("Spot Heal reference exceeds the snapshot budget.");
                        prepared.push_back({source.id, std::move(cache)});
                    }
                    if (source.id == *target) below = false;
                    if (cancelled()) break;
                }
            }
            if (!cancelled())
                reference = core::CloneReference::capture(*document, *target, *target, cloneSettings_.source,
                    error, core::CloneReference::defaultSnapshotLimit, false, cancelled, prepared, true);
        } catch (const std::exception& exception) { error = exception.what(); }
    }
    if (profileSpotHeal())
        spotHealReferenceCaptureMilliseconds_ = profileMilliseconds(captureStarted, ProfileClock::now());
    if (!reference || cloneCancelRequested_) {
        statusBar()->showMessage(error.empty() ? tr("Spot Heal capture cancelled")
                                              : QString::fromStdString(error), 6000);
        return false;
    }
    try {
        activeSpotHealStroke_ = std::make_unique<core::SpotHealStroke>(*document, *target,
            brushSettings_, cloneSettings_, std::move(*reference), brushAssets_.get());
        if (!activeSpotHealStroke_->begin(sample)) {
            const auto detail = QString::fromStdString(activeSpotHealStroke_->diagnostic());
            activeSpotHealStroke_.reset();
            statusBar()->showMessage(detail.isEmpty() ? tr("Unable to mark a repair at this target scale.") : detail, 6000);
            return false;
        }
    } catch (const std::exception& exception) {
        activeSpotHealStroke_.reset();
        statusBar()->showMessage(tr("Unable to start Spot Heal: %1").arg(QString::fromUtf8(exception.what())), 6000);
        return false;
    }
    if (!spotHealTimer_) {
        spotHealTimer_ = new QTimer(this);
        spotHealTimer_->setInterval(33);
        connect(spotHealTimer_, &QTimer::timeout, this, &MainWindow::pollSpotHeal);
    }
    spotHealPreviewDirty_ = true;
    refreshSpotHealPreview();
    if (!activeSpotHealStroke_) return false;
    spotHealTimer_->start();
    updateActionState();
    statusBar()->showMessage(tr("Mark the whole blemish · release to repair · Escape cancels"));
    return true;
}

bool MainWindow::moveSpotHealStroke(const core::NormalizedPointerSample& sample)
{
    if (!activeSpotHealStroke_ || spotHealJob_) return false;
    if (!activeSpotHealStroke_->append(sample)) { cancelSpotHeal(); return false; }
    spotHealPreviewDirty_ = true;
    if (const auto angle = activeSpotHealStroke_->brush().lastResolvedTipAngleDegrees())
        canvasWindow_->setResolvedBrushCursorAngle(*angle);
    canvasWindow_->setConstrainedBrushPosition(activeSpotHealStroke_->brush().constrainedPosition());
    canvasWindow_->scheduleFrame();
    return true;
}

void MainWindow::refreshSpotHealPreview()
{
    if (!activeSpotHealStroke_ || !spotHealPreviewDirty_) return;
    spotHealPreviewDirty_ = false;
    try {
        const auto mask = activeSpotHealStroke_->previewMask();
        canvasWindow_->setRepairRegion(mask
            ? std::make_shared<const std::vector<core::SelectionEdge>>(mask->boundaryEdges()) : nullptr);
    } catch (const std::exception&) {
        cancelSpotHeal();
        statusBar()->showMessage(tr("Spot Heal mark exceeds the preview memory budget. Try a smaller repair."), 6000);
    }
}

bool MainWindow::endSpotHealStroke(const core::NormalizedPointerSample& sample)
{
    const auto releaseStarted = profileSpotHeal() ? ProfileClock::now() : ProfileClock::time_point{};
    if (!activeSpotHealStroke_ || spotHealJob_) return false;
    try {
        auto work = activeSpotHealStroke_->finishInput(sample);
        if (!work) {
            const auto detail = QString::fromStdString(activeSpotHealStroke_->diagnostic());
            const auto result = detail.isEmpty() ? activeSpotHealStroke_->commitNoop(session().history())
                                                : core::RasterEditCommitResult::NoChanges;
            cancelSpotHeal();
            statusBar()->showMessage(detail.isEmpty() ? tr("Spot Heal · no writable pixels") : detail, 6000);
            return detail.isEmpty() && result == core::RasterEditCommitResult::NoChanges;
        }
        spotHealPreviewDirty_ = true;
        refreshSpotHealPreview();
        if (!activeSpotHealStroke_) return false;
        auto job = std::make_shared<SpotHealJob>();
        job->work = std::move(work);
        job->context = activeDocument_;
        job->generation = activeDocument_->cancellationGeneration;
        job->stroke = std::move(activeSpotHealStroke_);
        job->target = *session().activeLayer();
        job->options.adaptation = static_cast<float>(cloneSettings_.adaptation);
        job->options.captureProfile = profileSpotHeal();
        job->captureMilliseconds = spotHealReferenceCaptureMilliseconds_;
        job->releaseStarted = releaseStarted;
        job->elapsed.start();
        spotHealJob_ = job;
        canvasWindow_->setRepairProcessing(true);
        cloningOptionsPage_->setProcessing(true, 0, tr("Reconstructing Spot Heal…"));
        updateActionState();
        statusBar()->clearMessage();
        // A single in-flight task, with no UI event pumping inside the solver.
        if (profileSpotHeal()) job->dispatched = ProfileClock::now();
        spotHealFuture_ = std::async(std::launch::async, [job] {
            if (job->options.captureProfile) job->workerStarted = ProfileClock::now();
            auto options = job->options;
            options.cancelled = [job] { return job->cancelled.load(std::memory_order_relaxed); };
            options.progress = [job](double value) { job->progress.store(value, std::memory_order_relaxed); };
            try { job->solved = core::SpotHealStroke::solve(job->work, options); }
            catch (const std::exception& e) {
                job->solved.repair.status = core::SpotHealStatus::LimitExceeded;
                job->solved.repair.diagnostics.message = std::string("Spot Heal failed safely: ") + e.what();
            }
            if (job->options.captureProfile) job->workerFinished = ProfileClock::now();
        });
        return true;
    } catch (const std::exception& e) {
        cancelSpotHeal(true);
        statusBar()->showMessage(tr("Unable to prepare Spot Heal: %1").arg(QString::fromUtf8(e.what())), 6000);
        return false;
    }
}

void MainWindow::pollSpotHeal()
{
    if (!spotHealJob_) { refreshSpotHealPreview(); return; }
    const auto job = spotHealJob_;
    const bool foreground = job->context == activeDocument_;
    if (job->context->closed || job->generation != job->context->cancellationGeneration
        || !job->stroke || !job->stroke->targetMatches())
        job->cancelled.store(true, std::memory_order_relaxed);
    if (!spotHealFuture_.valid()
        || spotHealFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (foreground) cloningOptionsPage_->setProcessing(true, job->progress.load(std::memory_order_relaxed),
            job->cancelled ? tr("Cancelling Spot Heal…") : tr("Reconstructing Spot Heal…"));
        return;
    }
    spotHealFuture_.get();
    const auto publicationStarted = profileSpotHeal() ? ProfileClock::now() : ProfileClock::time_point{};
    const bool cancelled = job->cancelled.load(std::memory_order_relaxed);
    auto result = core::RasterEditCommitResult::NoChanges;
    QString detail = QString::fromStdString(job->solved.repair.diagnostics.message);
    if (!cancelled && job->stroke && job->solved.repair) {
        result = job->stroke->publish(job->solved, job->context->session.history());
        if (!job->stroke->diagnostic().empty())
            detail = QString::fromStdString(job->stroke->diagnostic());
        else if (result != core::RasterEditCommitResult::Committed)
            detail = result == core::RasterEditCommitResult::NoChanges ? tr("Spot Heal · no changed pixels")
                : tr("Spot Heal could not publish the repair; no changes were kept.");
    }
    const auto publicationFinished = profileSpotHeal() ? ProfileClock::now() : ProfileClock::time_point{};
    if (job->stroke) job->stroke->cancel();
    job->stroke.reset();
    spotHealJob_.reset();
    spotHealTimer_->stop();
    if (result == core::RasterEditCommitResult::Committed) job->context->untouched = false;
    refreshDocumentTabs();
    if (foreground) {
        refreshSpotHealOwnership();
        canvasWindow_->setConstrainedBrushPosition({});
        synchronizeUi(false, false);
        statusBar()->showMessage(cancelled ? tr("Spot Heal cancelled · no pixels changed")
        : result == core::RasterEditCommitResult::Committed
        ? tr("Spot Heal · %1 ms").arg(job->elapsed.elapsed())
        : !detail.isEmpty() ? detail : tr("Spot Heal · no changed pixels"), 6500);
    }
    if (profileSpotHeal()) {
        qInfo().nospace() << "spot_heal_ui_profile status=" << (cancelled ? "cancelled" : "finished")
            << " capture_ms=" << job->captureMilliseconds
            << " release_preparation_ms=" << profileMilliseconds(job->releaseStarted, job->dispatched)
            << " dispatch_ms=" << profileMilliseconds(job->dispatched, job->workerStarted)
            << " worker_ms=" << profileMilliseconds(job->workerStarted, job->workerFinished)
            << " worker_preparation_ms=" << job->solved.workerPreparationMilliseconds
            << " solver_ms=" << job->solved.solveMilliseconds
            << " ready_to_publication_ms=" << profileMilliseconds(job->workerFinished, publicationStarted)
            << " publication_ms=" << profileMilliseconds(publicationStarted, publicationFinished)
            << " ui_completion_ms=" << profileMilliseconds(publicationFinished, ProfileClock::now())
            << " release_to_ui_ms=" << profileMilliseconds(job->releaseStarted, ProfileClock::now())
            << " estimated_solver_bytes=" << job->solved.repair.diagnostics.estimatedPeakWorkingBytes
            << " prepared_bytes=" << job->solved.preparedBytes;
    }
}

void MainWindow::cancelSpotHeal(bool wait)
{
    if (!wait && spotHealJob_ && spotHealJob_->context != activeDocument_) return;
    if (!spotHealJob_ && !activeSpotHealStroke_ && (!spotHealTimer_ || !spotHealTimer_->isActive())) return;
    if (spotHealJob_) spotHealJob_->cancelled.store(true, std::memory_order_relaxed);
    if (activeSpotHealStroke_) activeSpotHealStroke_->cancel();
    activeSpotHealStroke_.reset();
    if (wait && spotHealFuture_.valid()) spotHealFuture_.get();
    if (wait) spotHealJob_.reset();
    if (spotHealJob_) return; // retain one bounded task until cooperative exit
    if (spotHealTimer_) spotHealTimer_->stop();
    spotHealPreviewDirty_ = false;
    if (canvasWindow_) {
        canvasWindow_->setRepairProcessing(false);
        canvasWindow_->setRepairRegion({});
        canvasWindow_->setConstrainedBrushPosition({});
    }
    if (cloningOptionsPage_) cloningOptionsPage_->setProcessing(false);
    if (!wait) {
        updateActionState();
        statusBar()->showMessage(tr("Spot Heal cancelled · no pixels changed"), 2000);
    }
}

bool MainWindow::routeSpotHealProcessingInput(QObject* watched, QEvent* event)
{
    if (!spotHealBusyForActiveDocument()) return false;
    const auto type = event->type();
    if (type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        for (const auto* action : {"NextDocumentAction", "PreviousDocumentAction", "CloseDocumentAction", "NewDocumentAction", "OpenDocumentAction"})
            if (shortcutMatches(shortcuts_, action, *key)) return false;
        if (type == QEvent::KeyPress && !key->isAutoRepeat()
            && (key->key() == Qt::Key_Escape || shortcutMatches(shortcuts_, "UndoAction", *key)
                || shortcutMatches(shortcuts_, "RedoAction", *key))) cancelSpotHeal();
        if (shortcutMatches(shortcuts_, "PanCanvasAction", *key)) {
            if (type != QEvent::ShortcutOverride && !key->isAutoRepeat())
                { panAccessKey_ = type == QEvent::KeyPress ? key->key() : 0; canvasWindow_->setSpacePanHeld(type == QEvent::KeyPress); }
        }
        event->accept(); return true;
    }
    const auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && (widget == documentTabs_ || documentTabs_->isAncestorOf(widget))) return false;
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseMove) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (QRect(documentTabs_->mapToGlobal(QPoint{}), documentTabs_->size()).contains(mouse->globalPosition().toPoint())) return false;
    }
    const auto* cancel = cloningOptionsPage_->findChild<QPushButton*>(QStringLiteral("SpotHealCancelProcessing"));
    bool cancelButton = widget == cancel;
    if (cancel && (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
        || type == QEvent::MouseMove || type == QEvent::MouseButtonDblClick)) {
        // Native carriers are filtered before Qt chooses the child control.
        // Admit this one control by its global footprint, and retain its drag
        // release even after the pointer leaves the button/panel window.
        const auto* mouse = static_cast<QMouseEvent*>(event);
        cancelButton = cancelButton || cancel->isDown()
            || QRect(cancel->mapToGlobal(QPoint{}), cancel->size()).contains(mouse->globalPosition().toPoint());
    }
    // Canvas native events retain the usual cross-panel pan capture. The
    // canvas refuses editing presses while busy. QWidget controls cannot edit
    // sources, targets, selection or history while a result is being prepared.
    const bool canvasRoute = watched == canvasWindow_ || watched == windowHandle()
        || (canvasWindow_->panDragging() && (type == QEvent::MouseMove || type == QEvent::MouseButtonRelease));
    switch (type) {
    case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick: case QEvent::MouseButtonRelease:
    case QEvent::MouseMove: case QEvent::TabletPress: case QEvent::TabletMove: case QEvent::TabletRelease:
    case QEvent::Wheel:
        if (canvasRoute || cancelButton) return false;
        event->accept(); return true;
    case QEvent::Shortcut: case QEvent::InputMethod: case QEvent::Drop: case QEvent::DragEnter:
        event->accept(); return true;
    default: return false;
    }
}
bool MainWindow::spotHealBusyForActiveDocument() const
{
    return spotHealJob_ && spotHealJob_->context == activeDocument_;
}
void MainWindow::refreshSpotHealOwnership()
{
    const bool busy = spotHealBusyForActiveDocument();
    canvasWindow_->setRepairProcessing(busy);
    canvasWindow_->setRepairRegion({});
    cloningOptionsPage_->setProcessing(busy, busy ? spotHealJob_->progress.load() : 0, tr("Reconstructing Spot Heal…"));
    if (busy && spotHealJob_->stroke) {
        const auto mask = spotHealJob_->stroke->previewMask();
        if (mask) canvasWindow_->setRepairRegion(std::make_shared<const std::vector<core::SelectionEdge>>(mask->boundaryEdges()));
    }
}
void MainWindow::cancelDocumentRepair(DocumentInstanceId id, bool wait)
{
    if (spotHealJob_ && spotHealJob_->context->id == id) {
        spotHealJob_->cancelled.store(true);
        if (wait && spotHealFuture_.valid()) { spotHealFuture_.wait(); pollSpotHeal(); }
    }
}
} // namespace imageeditor::ui
