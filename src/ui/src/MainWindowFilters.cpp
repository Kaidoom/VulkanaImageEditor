#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QListView>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
void prepareCanonicalFilterSource(core::Layer& layer, const core::Document& document)
{
    if ((!core::hasActiveSpatialFilters(layer.filters)&&!core::hasActiveLayerEffects(layer.effects))
        || std::holds_alternative<core::RasterLayer>(layer.payload)) return;
    const bool shape = std::holds_alternative<core::ShapeLayer>(layer.payload);
    const auto revision = shape ? layer.shapeRevision : layer.textRevision;
    if (layer.renderCache && !layer.renderCache->rasterizedDocumentTransform
        && layer.renderCache->density == 1 && layer.renderCache->contentRevision == revision) return;
    std::size_t budget = 16ULL * 1024 * 1024;
    if (shape) {
        const auto count = std::ranges::count_if(document.layers(), [](const auto& item) {
            return std::holds_alternative<core::ShapeLayer>(item.payload);
        });
        budget = std::min(budget, std::size_t(64ULL * 1024 * 1024
            / std::size_t(std::max(std::ptrdiff_t {1}, count))));
    }
    auto cache = std::make_shared<core::LayerRenderCache>(*prepareDocumentSampleCache(layer, budget));
    if (cache->surface->extent().width > 8192 || cache->surface->extent().height > 8192)
        throw std::runtime_error("Typed filter source exceeds the display dimension limit");
    cache->contentRevision = revision;
    layer.renderCache = std::move(cache);
}
}
// Immutable snapshots cross the worker boundary; Qt/document/history/GPU work
// and revision-checked publication remain on the document-owning thread.
struct MainWindow::FilterJob {
    core::Document* document {};
    core::Layer key;
    core::SurfaceId sourceId {};
    core::Revision sourceRevision {};
    core::AffineTransform sourcePixelsToLocal;
    std::atomic<bool> cancelled {false};
    std::atomic<double> progress {0};
    std::shared_ptr<const core::LayerSpatialFilterCache> result;
    std::shared_ptr<const core::LayerEffectCache> effects;
    std::shared_ptr<const core::FilterInputSnapshot> input;
    std::string error;
    bool sourceMatches(core::Document* current,const core::Layer& layer) const {
        const auto source=core::intrinsicSurface(layer);
        return current==document&&key.id==layer.id&&source
            &&source->id()==sourceId&&source->revision()==sourceRevision
            &&core::intrinsicPixelsToLocal(layer)==sourcePixelsToLocal;
    }
    bool matches(core::Document* current,const core::Layer& layer) const {
        return sourceMatches(current,layer)
            &&core::equivalentAdjustments(key.adjustments,layer.adjustments)
            &&core::equivalentSpatialFilters(key.filters,layer.filters)
            &&core::equivalentLayerEffectGeometry(key.effects,layer.effects);
    }
};

void MainWindow::createFiltersPanel()
{
    filtersPanel_=adjustmentsPanel_->filtersPanel();
    adjustmentsPanel_->onFiltersCategoryChanged=[this](bool filters){
        capturedFilterRegionActive_=filters;
        if(adjustmentsPanel_->onCapturedRegionChanged)adjustmentsPanel_->onCapturedRegionChanged();
        if(filtersPanel_->onCapturedRegionChanged)filtersPanel_->onCapturedRegionChanged();
    };
    filtersPanel_->onInteractionStarted=[this](core::SpatialFilterType type){return beginFilterEdit(type);};
    filtersPanel_->onPreview=[this](core::SpatialFilterState state){previewFilterEdit(std::move(state));};
    filtersPanel_->onInteractionFinished=[this](bool commit){finishFilterEdit(commit);};
    filtersPanel_->onCaptureSelection=[this](core::SpatialFilterType type){captureFilterSelection(type);};
    filtersPanel_->onComparison=[this](bool enabled){
        const auto target=filtersPanel_->target();
        canvasWindow_->setFilterBypassLayer(enabled&&target&&session().activeLayer()==target?target:std::nullopt);
    };
    filtersPanel_->onCapturedRegionChanged=[this]{
        if(capturedFilterRegionActive_||!filtersPanel_->capturedRegionVisible())
            canvasWindow_->setCapturedFilterRegion(filtersPanel_->capturedRegionVisible()
                ?filtersPanel_->target():std::nullopt,filtersPanel_->currentType());
    };
    filtersPanel_->onCancelProcessing=[this]{if(filterEdit_)finishFilterEdit(false);else cancelFilterPreparation();};
    filterPreparationTimer_=new QTimer(this);
    filterPreparationTimer_->setSingleShot(true);
    filterPreparationTimer_->setObjectName(QStringLiteral("FilterPreparationTimer"));
    connect(filterPreparationTimer_,&QTimer::timeout,this,&MainWindow::advanceFilterPreparation);
    canvasWindow_->onFilterPreparationRequested=[this]{scheduleFilterPreparation();};
}

bool MainWindow::beginFilterEdit(core::SpatialFilterType)
{
    if(fileBusy_||!session().document()||!session().activeLayer())return false;
    if(const auto* l=session().document()->layer(*session().activeLayer());l&&std::holds_alternative<core::AdjustmentLayer>(l->payload))return false;
    if(filterEdit_)return filterEdit_->active()&&filterEdit_->target()==*session().activeLayer();
    if(canvasWindow_->pointerGestureActive()||activeBrushStroke_||activeCloneStroke_||cloneProcessing_
        ||activeLocalBlurStroke_||localBlurProcessing_||activeFill_||shapeCreation_||shapeResize_
        ||selectionGesture_||selectionRasterizer_||colorSelectionEditing_||smartInteractionActive())return false;
    finishAdjustmentEdit(true);if(adjustmentEdit_)return false;
    finishEffectEdit(true);if(effectEdit_)return false;
    if(layerTransform_||layerCrop_||selectionTransform_){
        finishCanvasOperation();if(layerTransform_||layerCrop_||selectionTransform_)return false;
    }
    if(textController_&&textController_->active()){
        textController_->finish();if(textController_->active())return false;
    }
    finishLayerMove(true);if(activeLayerMove_)return false;
    cancelPendingEdits();
    try{
        auto transaction=std::make_unique<core::FilterEditTransaction>(*session().document(),*session().activeLayer());
        if(!transaction->active())return false;
        filterEdit_=std::move(transaction);capturedFilterRegionActive_=true;updateActionState();return true;
    }catch(const std::exception& e){statusBar()->showMessage(tr("Could not begin filter: %1").arg(QString::fromUtf8(e.what())),5000);return false;}
}
void MainWindow::previewFilterEdit(core::SpatialFilterState state)
{
    if(!filterEdit_||!session().document()||session().activeLayer()!=filterEdit_->target())return;
    try{
        const auto* target=session().document()->layer(filterEdit_->target());
        if(!target){finishFilterEdit(false);return;}
        auto candidate=*target;
        candidate.filters=state;
        // A native document-grid preview may be rotated/sheared relative to
        // the model. Filters need their canonical local source before admission.
        prepareCanonicalFilterSource(candidate, *session().document());
        (void)core::preflightLayerSpatialFilters(candidate);
        if(filterEdit_->update(std::move(state))){
            session().document()->layer(filterEdit_->target())->renderCache=std::move(candidate.renderCache);
            canvasWindow_->setDocument(session().document()->snapshot(),false);refreshFiltersPanel();
            if(layerList_)layerList_->viewport()->update();
        }else if(!filterEdit_->active())finishFilterEdit(false);
    }catch(const std::exception& e){statusBar()->showMessage(tr("Filter preview failed: %1").arg(QString::fromUtf8(e.what())),5000);finishFilterEdit(false);}
}
void MainWindow::finishFilterEdit(bool commit)
{
    if(finishingFilterEdit_||!filterEdit_)return;
    const QScopedValueRollback guard(finishingFilterEdit_,true);
    if(filtersPanel_->interactionActive())filtersPanel_->finishEditing(commit);
    try{
        if(commit){
            if(!filterEdit_->commit(session().history())&&filterEdit_->active()){
                statusBar()->showMessage(tr("Could not record the filter. Preview retained; retry or Escape to cancel."),5000);return;
            }
        }else (void)filterEdit_->cancel();
        filterEdit_.reset();if(session().document())synchronizeUi(false,false);
    }catch(const std::exception& e){statusBar()->showMessage(tr("Could not finish filter: %1").arg(QString::fromUtf8(e.what())),5000);}
}
void MainWindow::captureFilterSelection(core::SpatialFilterType type)
{
    if(!session().document()||!session().document()->selection()||!session().activeLayer()||!beginFilterEdit(type))return;
    const auto* layer=session().document()->layer(filterEdit_->target());
    if(!layer){finishFilterEdit(false);return;}
    try{
        auto state=layer->filters?*layer->filters:core::SpatialFilterStack{};
        state.items[std::size_t(type)].mask=core::captureAdjustmentMask(session().document()->selection(),layer->localToDocument);
        previewFilterEdit(std::make_shared<const core::SpatialFilterStack>(std::move(state)));finishFilterEdit(true);
    }catch(const std::exception& e){statusBar()->showMessage(tr("Could not capture filter mask: %1").arg(QString::fromUtf8(e.what())),5000);finishFilterEdit(false);}
}
void MainWindow::refreshFiltersPanel()
{
    if(!filtersPanel_)return;
    const auto* layer=session().document()&&session().activeLayer()?session().document()->layer(*session().activeLayer()):nullptr;
    filtersPanel_->setTarget(layer&&std::holds_alternative<core::AdjustmentLayer>(layer->payload)?nullptr:layer,session().document()&&bool(session().document()->selection()));scheduleFilterPreparation();
}
void MainWindow::scheduleFilterPreparation()
{
    if(filterPreparationShuttingDown_||!filterPreparationTimer_)return;
    if(filterJob_){
        const auto* layer=session().document()?session().document()->layer(filterJob_->key.id):nullptr;
        if(!layer||!filterJob_->matches(session().document(),*layer))filterJob_->cancelled=true;
    }
    // Coalesce, but never restart a running timer: animation/scrubbing must not
    // starve preparation. One serial worker bounds memory across all layers.
    if(!filterPreparationTimer_->isActive())filterPreparationTimer_->start(filterJob_?20:70);
}
void MainWindow::advanceFilterPreparation()
{
    if(filterPreparationShuttingDown_)return;
    if(filterJob_){
        auto job=filterJob_;
        const auto* layer=session().document()?session().document()->layer(job->key.id):nullptr;
        const bool matches=layer&&job->matches(session().document(),*layer);
        if(!matches)job->cancelled=true;
        if(filterFuture_.wait_for(std::chrono::seconds(0))!=std::future_status::ready){
            filtersPanel_->setProcessing(true,job->progress.load(),tr("Filtering %1…").arg(QString::fromStdString(job->key.name)));
            effectsPanel_->setProcessing(true,job->progress.load());
            filterPreparationTimer_->start(20);return;
        }
        filterFuture_.get();filterJob_.reset();
        if(matches&&!job->cancelled){
            try{
                if(!job->error.empty())throw std::runtime_error(job->error);
                bool published=job->result&&core::publishLayerSpatialFilters(*session().document(),job->key.id,job->result);
                if(job->effects) {
                    std::size_t bytes=0;
                    for(const auto& other:session().document()->layers()) {
                        const auto cache=other.id==job->key.id?job->effects:other.effectCache;
                        if(cache)for(const auto& mask:cache->masks)if(mask)bytes+=mask->coverage->memoryCost();
                    }
                    if(bytes>384ULL*1024*1024)throw std::length_error("Document effect masks exceed the 384 MiB cache budget");
                    auto* target=session().document()->layer(job->key.id);
                    auto candidate=*target;candidate.effectCache=job->effects;
                    if(!core::layerEffectCacheValid(candidate))throw std::runtime_error("Stale prepared layer effects");
                    target->effectCache=job->effects;published=true;
                }
                if(published){
                    failedFilterJobs_.erase(job->key.id);canvasWindow_->setDocument(session().document()->snapshot(),false);
                    if(layerList_)layerList_->viewport()->update();
                }
            }catch(const std::exception& e){
                job->error=e.what();job->result.reset();job->effects.reset();job->input.reset();failedFilterJobs_[job->key.id]=job;
                statusBar()->showMessage(tr("Filter preparation failed: %1").arg(QString::fromUtf8(e.what())),7000);
            }
        }
        job->result.reset(); // The document alone owns an admitted derived image.
        job->effects.reset();
    }
    if(fileBusy_){filterPreparationTimer_->start(70);return;}
    auto* document=session().document();
    if(!document){failedFilterJobs_.clear();frozenFilterInput_.reset();filtersPanel_->setProcessing(false);effectsPanel_->setProcessing(false);return;}
    for(auto it=failedFilterJobs_.begin();it!=failedFilterJobs_.end();){
        const auto* layer=document->layer(it->first);
        if(!layer||!it->second->matches(document,*layer))it=failedFilterJobs_.erase(it);else ++it;
    }
    const core::Layer* candidate=nullptr;
    const auto needsWork=[this](const core::Layer& layer){return core::intrinsicSurface(layer)
        &&((core::hasActiveSpatialFilters(layer.filters)&&!core::layerSpatialFilterCacheValid(layer))||!core::layerEffectCacheValid(layer))
        &&!failedFilterJobs_.contains(layer.id);};
    if(session().activeLayer()){
        const auto* primary=document->layer(*session().activeLayer());if(primary&&needsWork(*primary))candidate=primary;
    }
    if(!candidate)for(const auto& layer:document->layers())if(needsWork(layer)){candidate=&layer;break;}
    if(!candidate){
        QString error;
        if(session().activeLayer())if(const auto it=failedFilterJobs_.find(*session().activeLayer());it!=failedFilterJobs_.end())error=QString::fromStdString(it->second->error);
        filtersPanel_->setProcessing(false,0,error);effectsPanel_->setProcessing(false,0,error);return;
    }
    auto job=std::make_shared<FilterJob>();job->document=document;
    job->key.id=candidate->id;job->key.name=candidate->name;
    job->key.filters=candidate->filters;job->key.adjustments=candidate->adjustments;
    job->key.effects=candidate->effects;
    const auto source=core::intrinsicSurface(*candidate);job->sourceId=source->id();job->sourceRevision=source->revision();
    job->sourcePixelsToLocal=core::intrinsicPixelsToLocal(*candidate);
    try{
        const auto previousSource=candidate->renderCache;
        prepareCanonicalFilterSource(*document->layer(candidate->id),*document);
        if(candidate->renderCache!=previousSource){
            const auto preparedSource=core::intrinsicSurface(*candidate);
            job->sourceId=preparedSource->id();job->sourceRevision=preparedSource->revision();
            job->sourcePixelsToLocal=core::intrinsicPixelsToLocal(*candidate);
            canvasWindow_->setDocument(document->snapshot(),false);
        }
        (void)core::preflightLayerSpatialFilters(*candidate);
        if(frozenFilterInput_&&frozenFilterInput_->input&&frozenFilterInput_->sourceMatches(document,*candidate)){
            auto input=std::make_shared<core::FilterInputSnapshot>(*frozenFilterInput_->input);
            input->layer.filters=candidate->filters;input->layer.adjustments=candidate->adjustments;
            input->layer.filterCache=candidate->filterCache;input->layer.effects=candidate->effects;input->layer.effectCache=candidate->effectCache;
            job->input=std::move(input);
        }else{
            frozenFilterInput_.reset();
            job->input=std::make_shared<const core::FilterInputSnapshot>(core::snapshotFilterInput(*candidate));
        }
        frozenFilterInput_=job;filterJob_=job;
        filterFuture_=std::async(std::launch::async,[job]{
            try{
                core::FilterPreparationOptions options;
                options.cancelled=[job]{return job->cancelled.load();};options.progress=[job](double p){job->progress=p;};
                const auto prepared=core::prepareLayerSpatialEffects(*job->input,options);
                job->result=prepared.filters;job->effects=prepared.effects;
            }catch(const std::exception& e){job->error=e.what();}
        });
        filtersPanel_->setProcessing(true,0,tr("Filtering %1…").arg(QString::fromStdString(candidate->name)));
        filterPreparationTimer_->start(20);
    }catch(const std::exception& e){
        filterJob_.reset();job->error=e.what();job->input.reset();failedFilterJobs_[candidate->id]=job;
        filtersPanel_->setProcessing(false,0,QString::fromUtf8(e.what()));scheduleFilterPreparation();
    }
}
void MainWindow::cancelFilterPreparation(bool discard)
{
    QString message;
    if(filterPreparationTimer_)filterPreparationTimer_->stop();
    if(filterJob_){
        filterJob_->cancelled=true;
        // Wait only for the kernel's bounded cancellation checkpoint. Never
        // retain a worker's deep input snapshot in the failed-key cache.
        if(filterFuture_.valid())filterFuture_.wait();
        if(!discard){
            filterJob_->error="Processing cancelled. Parameters retained; change a filter to retry.";
            message=QString::fromStdString(filterJob_->error);
            filterJob_->result.reset();filterJob_->input.reset();failedFilterJobs_[filterJob_->key.id]=filterJob_;
        }
        filterJob_.reset();
    }
    if(discard){failedFilterJobs_.clear();frozenFilterInput_.reset();}
    if(filtersPanel_)filtersPanel_->setProcessing(false,0,message);
    if(effectsPanel_)effectsPanel_->setProcessing(false,0,message);
    if(!message.isEmpty())statusBar()->showMessage(message,5000);
}
} // namespace imageeditor::ui
