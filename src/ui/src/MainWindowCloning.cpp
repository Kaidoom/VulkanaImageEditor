#include "imageeditor/core/LayerCrop.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CloningOptionsPage.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QScopedValueRollback>
#include <QStatusBar>

namespace imageeditor::ui {
void MainWindow::createCloningControls()
{
    cloningOptionsPage_ = new CloningOptionsPage;
    toolOptionsBar_->registerToolPage(core::ToolId::Cloning, tr("Cloning"), cloningOptionsPage_);
    toolOptionsBar_->registerToolLeadingWidget(core::ToolId::Cloning, cloningOptionsPage_->modeWidget());
    cloningOptionsPage_->setBrushSettings(brushSettings_);
    cloningOptionsPage_->setCloneSettings(cloneSettings_);
    cloningOptionsPage_->onBrushSettingsChanged
        = [this](const core::BrushSettings& s) { applyBrushSettings(s); };
    cloningOptionsPage_->onModeChanged = [this](core::CloneMode mode) {
        cancelActiveBrushStroke();
        cloneSettings_.mode = mode;
        refreshCloningControls();
    };
    cloningOptionsPage_->onSourceChanged = [this](core::CloneSampleSource source) {
        cancelActiveBrushStroke();
        cloneSettings_.source = source;
        if (cloneAnchor_ && cloneSettings_.mode != core::CloneMode::SpotHeal)
            cloneAnchor_->alignedOffset.reset();
        refreshCloningControls();
    };
    cloningOptionsPage_->onAlignedChanged = [this](bool aligned) {
        cancelActiveBrushStroke();
        cloneSettings_.aligned = aligned;
        if (cloneAnchor_)
            cloneAnchor_->alignedOffset.reset();
        refreshCloningControls();
    };
    cloningOptionsPage_->onAdaptationChanged
        = [this](double adaptation) { cloneSettings_.adaptation = adaptation; };
    canvasWindow_->onCloneSourcePicked = [this](core::Vec2d point) { pickCloneSource(point); };
    cloningOptionsPage_->onCancelProcessing = [this] { cancelSpotHeal(); };
}

void MainWindow::refreshCloningControls()
{
    if (!cloningOptionsPage_)
        return;
    const auto* document = session().document();
    const auto* source = document && cloneAnchor_ ? document->layer(cloneAnchor_->layer) : nullptr;
    if (cloneAnchor_
        && (!source || !document->isEffectivelyVisible(source->id)
            || source->localToDocument != cloneAnchor_->sourceTransform)) {
        cloneAnchor_.reset();
        source = nullptr;
    }
    cloningOptionsPage_->setCloneSettings(cloneSettings_);
    cloningOptionsPage_->setSourceLayerLabel(
        source ? QString::fromStdString(source->name) : QString { }, source != nullptr);
    updateToolContextStatus();
    const bool spot = cloneSettings_.mode == core::CloneMode::SpotHeal;
    canvasWindow_->setSpotHealActive(spot);
    if (spot) {
        canvasWindow_->setCloneSource({}, {});
        return;
    }
    canvasWindow_->setCloneSource(cloneAnchor_ ? std::optional(cloneAnchor_->point) : std::nullopt,
        activeCloneStroke_                           ? std::optional(activeCloneStroke_->offset())
            : cloneAnchor_ && cloneSettings_.aligned ? cloneAnchor_->alignedOffset
                                                     : std::nullopt);
}

void MainWindow::pickCloneSource(core::Vec2d point)
{
    if (cloneSettings_.mode == core::CloneMode::SpotHeal || cloneProcessing_ || activeCloneStroke_ || !session().document() || !session().activeLayer())
        return;
    auto& doc = *session().document();
    const auto* layer = doc.layer(*session().activeLayer());
    if (!layer || !std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
        || point.x >= doc.canvas().extent.width || point.y >= doc.canvas().extent.height) {
        statusBar()->showMessage(tr("Alt-click inside the canvas with a source layer selected."), 4500);
        return;
    }
    prepareShapeCaches();
    if (textController_)
        textController_->prepareCaches();
    if (cloneSettings_.source == core::CloneSampleSource::SourceLayer) {
        const auto surface = core::intrinsicSurface(*layer);
        const auto inverse = core::composeAffine(layer->localToDocument,core::intrinsicPixelsToLocal(*layer)).inverted();
        const auto local = inverse ? inverse->map(point) : core::Vec2d { -1, -1 };
        if (!surface || !inverse || !doc.isEffectivelyVisible(layer->id) || !core::hitLayerCrop(*layer, point)
            || local.x < 0 || local.y < 0 || local.x >= surface->extent().width
            || local.y >= surface->extent().height) {
            statusBar()->showMessage(
                tr("Alt-click within the visible bounds of the selected source layer."), 4500);
            return;
        }
    }
    cloneAnchor_ = core::CloneAnchor { layer->id, point, layer->localToDocument, { } };
    statusBar()->clearMessage();
    refreshCloningControls();
}

bool MainWindow::beginCloneStroke(const core::NormalizedPointerSample& sample)
{
    if (cloneSettings_.mode == core::CloneMode::SpotHeal) return beginSpotHealStroke(sample);
    if (cloneProcessing_)
        return false;
    cancelActiveBrushStroke();
    refreshCloningControls();
    if (!cloneAnchor_) {
        statusBar()->showMessage(
            tr("Set a cloning source first: Alt-click on the selected source layer."), 5000);
        return false;
    }
    auto* doc = session().document();
    const auto target = session().activeLayer();
    const auto* layer = doc && target ? doc->layer(*target) : nullptr;
    if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
        statusBar()->showMessage(
            tr("Cloning writes to raster layers. Select or create a raster retouch layer."), 5000);
        return false;
    }
    const auto unavailable = brushAssets_->unavailableMessage(brushSettings_);
    if (!unavailable.isEmpty()) {
        statusBar()->showMessage(unavailable, 4500);
        return false;
    }
    cloneCancelRequested_ = false;
    const auto referenceRevision = doc->revision();
    std::string error;
    std::optional<core::CloneReference> reference;
    {
        QScopedValueRollback processing(cloneProcessing_, true);
        QElapsedTimer cadence;
        cadence.start();
        statusBar()->showMessage(tr("Preparing immutable clone reference… · Escape cancels"));
        auto cancelled = [&] {
            // The press handler has not installed canvas capture yet.
            // Keep releases queued until it returns; consuming one here
            // would leave a fast click stuck in a newly started stroke.
            if (cadence.elapsed() >= 8) {
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 4);
                cadence.restart();
            }
            if (session().document() != doc || doc->revision() != referenceRevision)
                cloneCancelRequested_ = true;
            return cloneCancelRequested_;
        };
        try {
            // Document-resolution typed references are private to this stroke;
            // never replace viewport caches or trigger Vulkan texture churn.
            std::vector<core::SampleCacheOverride> prepared;
            std::size_t preparedPixels = 0;
            constexpr auto pixelBudget = core::CloneReference::defaultSnapshotLimit / 4;
            const bool needsContext = cloneSettings_.mode == core::CloneMode::Heal;
            bool below = true;
            for (const auto& source : doc->layers()) {
                const bool raw = cloneSettings_.source == core::CloneSampleSource::SourceLayer;
                const bool included = (raw && source.id == cloneAnchor_->layer)
                    || ((!raw || needsContext)
                        && (below || cloneSettings_.source == core::CloneSampleSource::AllVisible));
                if (included && doc->isEffectivelyVisible(source.id)
                    && !std::holds_alternative<core::RasterLayer>(source.payload)) {
                    auto cache = prepareDocumentSampleCache(source, pixelBudget - preparedPixels);
                    if (!cache || !cache->surface)
                        throw std::runtime_error("Unable to prepare clone reference content.");
                    const auto extent = cache->surface->extent();
                    preparedPixels += std::size_t(extent.width) * extent.height;
                    if (preparedPixels > pixelBudget)
                        throw std::length_error("Clone reference exceeds the typed-cache budget.");
                    prepared.push_back({ source.id, std::move(cache) });
                }
                if (source.id == *target)
                    below = false;
                if (cancelled())
                    break;
            }
            if (!cloneCancelRequested_)
                reference
                    = core::CloneReference::capture(*doc, *target, cloneAnchor_->layer, cloneSettings_.source,
                        error, core::CloneReference::defaultSnapshotLimit, needsContext, cancelled, prepared);
        } catch (const std::exception& exception) {
            error = exception.what();
        }
    }
    if (!reference || cloneCancelRequested_) {
        statusBar()->showMessage(
            cloneCancelRequested_ ? tr("Clone stroke cancelled") : QString::fromStdString(error), 5500);
        return false;
    }
    const auto offset = cloneAnchor_->offsetFor(sample.documentPosition, cloneSettings_.aligned);
    activeCloneStroke_ = std::make_unique<core::CloneStroke>(
        *doc, *target, brushSettings_, cloneSettings_, std::move(*reference), offset, brushAssets_.get());
    if (!activeCloneStroke_->begin(sample)) {
        const auto detail = QString::fromStdString(activeCloneStroke_->diagnostic());
        activeCloneStroke_.reset();
        statusBar()->showMessage(
            detail.isEmpty() ? tr("Unable to start clone stroke at this target scale.") : detail, 5500);
        return false;
    }
    refreshCloningControls();
    if (const auto angle = activeCloneStroke_->brush().lastResolvedTipAngleDegrees())
        canvasWindow_->setResolvedBrushCursorAngle(*angle);
    canvasWindow_->setConstrainedBrushPosition(activeCloneStroke_->brush().constrainedPosition());
    const bool rendered = cloneSettings_.source != core::CloneSampleSource::SourceLayer;
    const bool effects = layer->blendMode != core::BlendMode::Normal || layer->opacity != 1.0F
        || core::hasActiveSpatialFilters(layer->filters)
        || core::hasActiveLayerEffects(layer->effects)
        || (layer->adjustments && core::compileAdjustmentStack(layer->adjustments).active);
    statusBar()->showMessage(rendered && effects
            ? tr("Destination effects will be applied again. For matching appearance use a Normal, 100%, "
                 "unadjusted retouch layer.")
            : cloneSettings_.mode == core::CloneMode::Heal
            ? tr("Healing region · release to reconstruct · Escape cancels")
            : tr("Clone stamp · Escape cancels"));
    canvasWindow_->scheduleFrame();
    return true;
}

bool MainWindow::moveCloneStroke(const core::NormalizedPointerSample& sample)
{
    if (cloneProcessing_)
        return true;
    if (!activeCloneStroke_ || !activeCloneStroke_->append(sample)) {
        cancelCloneStroke();
        return false;
    }
    if (const auto angle = activeCloneStroke_->brush().lastResolvedTipAngleDegrees())
        canvasWindow_->setResolvedBrushCursorAngle(*angle);
    canvasWindow_->setConstrainedBrushPosition(activeCloneStroke_->brush().constrainedPosition());
    canvasWindow_->scheduleFrame();
    return true;
}

bool MainWindow::endCloneStroke(const core::NormalizedPointerSample& sample)
{
    if (!activeCloneStroke_ || cloneProcessing_)
        return false;
    core::RasterEditCommitResult result;
    QElapsedTimer elapsed;
    elapsed.start();
    {
        QScopedValueRollback processing(cloneProcessing_, true);
        QElapsedTimer cadence;
        cadence.start();
        if (cloneSettings_.mode == core::CloneMode::Heal)
            statusBar()->showMessage(tr("Reconstructing Heal… · Escape cancels"));
        result = activeCloneStroke_->end(sample, session().history(), [&] {
            if (cadence.elapsed() >= 8) {
                QApplication::processEvents(QEventLoop::AllEvents, 4);
                cadence.restart();
            }
            return cloneCancelRequested_;
        });
    }
    const auto detail = QString::fromStdString(activeCloneStroke_->diagnostic());
    const bool completed = result == core::RasterEditCommitResult::Committed
        || result == core::RasterEditCommitResult::NoChanges;
    if (completed && cloneAnchor_)
        cloneAnchor_->completed(activeCloneStroke_->offset(), cloneSettings_.aligned);
    activeCloneStroke_.reset();
    if (result == core::RasterEditCommitResult::Committed)
        fileState().untouched = false;
    synchronizeUi(false, false);
    statusBar()->showMessage(!detail.isEmpty() ? detail
            : !completed                       ? tr("Clone stroke cancelled")
            : result == core::RasterEditCommitResult::NoChanges
            ? tr("Clone stroke · no changed pixels")
            : tr("%1 stroke · %2 ms")
                  .arg(cloneSettings_.mode == core::CloneMode::Heal ? tr("Heal") : tr("Stamp"))
                  .arg(elapsed.elapsed()),
        6000);
    return completed;
}

void MainWindow::cancelCloneStroke()
{
    if (cloneProcessing_) {
        cloneCancelRequested_ = true;
        return;
    }
    if (!activeCloneStroke_)
        return;
    activeCloneStroke_->cancel();
    activeCloneStroke_.reset();
    canvasWindow_->setConstrainedBrushPosition({ });
    refreshCloningControls();
    canvasWindow_->scheduleFrame();
    statusBar()->showMessage(tr("Clone stroke cancelled"), 2000);
}
}
