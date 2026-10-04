#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include <QElapsedTimer>
#include <QLabel>
#include <QComboBox>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
void AdjustmentsPanel::requestHistogram(const core::Layer* layer)
{
    const auto type=currentType();
    if(!isVisible()||filtersCategoryActive()||!layer||!target_||layer->id!=*target_
        ||(type!=core::AdjustmentType::Levels&&type!=core::AdjustmentType::Curves)) {
        histogramTimer_->stop();histogramJob_.reset();return;
    }
    std::shared_ptr<const core::RasterSurface> surface;
    core::AffineTransform pixelsToLocal;
    if(const auto* raster=std::get_if<core::RasterLayer>(&layer->payload))surface=raster->surface;
    else if(layer->renderCache){surface=layer->renderCache->surface;pixelsToLocal=layer->renderCache->pixelsToLocal;}
    const auto cacheIndex=type==core::AdjustmentType::Levels?0U:1U;
    const bool composite=std::holds_alternative<core::AdjustmentLayer>(layer->payload)&&document_;
    if(!composite && (!surface||surface->extent().empty())){
        histogramLabels_[cacheIndex]->setText(QStringLiteral("No rendered source pixels"));return;
    }
    auto upstream=layer->adjustments?*layer->adjustments:core::AdjustmentStack{};
    const auto stage=std::size_t(type);
    for(std::size_t i=stage;i<core::adjustmentCount;++i)upstream.items[i]=core::defaultAdjustment(core::AdjustmentType(i));
    auto state=std::make_shared<const core::AdjustmentStack>(std::move(upstream));
    const auto inputKey=composite?core::adjustmentInputKey(*document_,layer->id):std::vector<std::uint64_t>{};
    const auto matches=[&](const HistogramCache& entry){
        if(composite)return entry.target==layer->id&&entry.document==document_&&entry.inputKey==inputKey
            &&core::equivalentAdjustments(entry.upstream,state);
        return entry.target==layer->id&&entry.surface==surface&&entry.revision==surface->revision()
            &&entry.pixelsToLocal==pixelsToLocal&&core::equivalentAdjustments(entry.upstream,state);
    };
    if(histogramCaches_[cacheIndex]&&matches(*histogramCaches_[cacheIndex])){
        histogramJob_.reset();histogramTimer_->stop();updateHistogramDisplay();return;
    }
    if(histogramJob_&&histogramJob_->stage==stage&&matches(histogramJob_->result))return;
    HistogramJob job;
    job.result.target=layer->id;job.result.surface=surface;
    job.result.revision=composite?document_->revision():surface->revision();
    job.result.document=composite?document_:nullptr;
    job.result.inputKey=inputKey;
    job.result.pixelsToLocal=pixelsToLocal;job.result.upstream=state;
    job.program=core::compileAdjustmentStack(state);job.stage=stage;
    const auto extent=composite?document_->canvas().extent:surface->extent();
    job.result.extent=extent;
    if(composite)try {
        job.composite=std::make_shared<core::PinnedDocumentSampler>(*document_,std::nullopt,core::ColorSampleSource::MergedVisible,
            core::SampleFiltering::AlphaAware,std::span<const core::SampleCacheOverride>{},core::ActiveReferenceAppearance::Intrinsic,layer->id);
    }catch(const std::exception&) {
        histogramJob_.reset();histogramTimer_->stop();histogramLabels_[cacheIndex]->setText(QStringLiteral("Preparing input histogram…"));return;
    }
    constexpr std::size_t maximumSamples=262144;
    job.step=std::max<std::size_t>(1,std::size_t(std::ceil(std::sqrt(double(extent.width)*extent.height/maximumSamples))));
    job.columns=(extent.width+job.step-1)/job.step;job.rows=(extent.height+job.step-1)/job.step;
    // Extreme aspect ratios need a larger stride than sqrt(area / budget).
    while(job.columns*job.rows>maximumSamples){++job.step;job.columns=(extent.width+job.step-1)/job.step;job.rows=(extent.height+job.step-1)/job.step;}
    histogramJob_=std::move(job);
    histogramLabels_[cacheIndex]->setText(QStringLiteral("Updating input histogram…"));
    histogramTimer_->start(0);
}
void AdjustmentsPanel::advanceHistogram()
{
    if(!histogramJob_)return;
    auto& job=*histogramJob_;
    // All reads occur on the GUI thread in bounded slices. RasterSurface is
    // mutable, not thread-safe: never send live source pointers to workers.
    // A changed revision invalidates the complete partial result, not a row.
    if(!target_||*target_!=job.result.target||!isVisible()
        ||(job.composite ? document_!=job.result.document || core::adjustmentInputKey(*document_,job.result.target)!=job.result.inputKey
                         : job.result.surface->revision()!=job.result.revision)){
        histogramJob_.reset();if(onHistogramRequested)onHistogramRequested();return;
    }
    const auto extent=job.result.extent;
    QElapsedTimer elapsed;elapsed.start();
    const auto end=std::min(job.cursor+4096,job.columns*job.rows);
    std::array<std::byte,4> bytes;
    for(;job.cursor<end;++job.cursor){
        if((job.cursor&63)==0&&elapsed.nsecsElapsed()>4'000'000)break;
        const auto x=std::min<std::size_t>((job.cursor%job.columns)*job.step+job.step/2,extent.width-1);
        const auto y=std::min<std::size_t>((job.cursor/job.columns)*job.step+job.step/2,extent.height-1);
        core::PremultipliedColor color;
        if(job.composite)color=job.composite->sampleAdjustmentInput({double(x)+.5,double(y)+.5},job.result.target,job.stage);
        else {
        job.result.surface->copyRgba8({int(x),int(y),1,1},bytes,4);
        const core::Rgba8 raw{std::to_integer<std::uint8_t>(bytes[0]),std::to_integer<std::uint8_t>(bytes[1]),
            std::to_integer<std::uint8_t>(bytes[2]),std::to_integer<std::uint8_t>(bytes[3])};
        color=core::evaluateAdjustments(job.program,core::decodeColor(raw),
            job.result.pixelsToLocal.map({double(x)+.5,double(y)+.5}),job.stage);
        }
        ++job.result.sampled;
        if(color[3]<=0)continue;
        const auto encoded=core::encodeColor(color);
        const double weight=color[3];
        job.result.bins[1][encoded.red]+=weight;job.result.bins[2][encoded.green]+=weight;job.result.bins[3][encoded.blue]+=weight;
        // Composite RGB counts all three channels equally, not luminance.
        for(auto bin:{encoded.red,encoded.green,encoded.blue})job.result.bins[0][bin]+=weight/3;
    }
    if(job.cursor<job.columns*job.rows){histogramTimer_->start(1);return;}
    const auto cacheIndex=job.stage==std::size_t(core::AdjustmentType::Levels)?0U:1U;
    histogramCaches_[cacheIndex]=std::move(job.result);histogramJob_.reset();++completedHistogramJobs_;
    updateHistogramDisplay();
}
void AdjustmentsPanel::updateHistogramDisplay()
{
    for(std::size_t i=0;i<2;++i){
        if(!histogramCaches_[i])continue;
        const auto& cache=*histogramCaches_[i];
        auto* plot=i==0?levelsHistogram_:curve_;
        plot->setHistogram(cache.bins,i==0?levelsChannel_->currentIndex():curvesChannel_->currentIndex());
        const auto extent=cache.extent;
        const bool sampled=cache.sampled<std::uint64_t(extent.width)*extent.height;
        histogramLabels_[i]->setText(QStringLiteral("%1 input · %2 texels · alpha-weighted sRGB")
            .arg(sampled?QStringLiteral("Sampled"):QStringLiteral("Source")).arg(cache.sampled));
    }
}
} // namespace imageeditor::ui
