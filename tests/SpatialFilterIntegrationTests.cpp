#include "imageeditor/core/FilterCommands.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "../src/ui/src/SpatialFilterCodec.hpp"
#include <QApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QProcess>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sys/resource.h>

namespace c=imageeditor::core;
namespace u=imageeditor::ui;
namespace {
int failures=0;
void check(bool ok,const char* message){if(!ok){++failures;std::cerr<<message<<'\n';}}
std::vector<std::byte> pixels(const c::RasterSurface& surface)
{
    const auto e=surface.extent();std::vector<std::byte> bytes(std::size_t(e.width)*e.height*4);
    surface.copyRgba8({0,0,int(e.width),int(e.height)},bytes,std::size_t(e.width)*4);return bytes;
}
c::SpatialFilterState gaussian(double radius,bool preserve=false,c::SelectionState mask={})
{
    auto state=std::make_shared<c::SpatialFilterStack>();
    state->items[0].enabled=true;state->items[0].preserveAlpha=preserve;
    state->items[0].parameters=c::GaussianBlurParameters{radius,radius};
    if(mask)state->items[0].mask=c::AdjustmentMask{std::move(mask),{}};
    return state;
}
c::Layer impulse()
{
    auto source=std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{9,9},c::Rgba8{255,0,255,0});
    const std::array data{std::byte{128},std::byte{64},std::byte{32},std::byte{255}};
    source->replaceRgba8({4,4,1,1},data,4);return c::Layer::raster("Editable source",source);
}
void cacheAndHistory()
{
    c::Document document({{32,32},96});auto layer=impulse();const auto id=layer.id;
    document.insertLayer(0,layer);document.markSaved();c::History history;
    const auto original=pixels(*c::intrinsicSurface(layer));
    {
        c::FilterEditTransaction edit(document,id);
        check(edit.update(gaussian(1)),"preview changes");
        check(edit.update(gaussian(3)),"scrub changes");
        check(!document.isModified(),"preview leaves content state unchanged");
        check(edit.commit(history)&&history.undoDepth()==1,"one numeric scrub is one history action");
    }
    const auto frozen=c::snapshotFilterInput(*document.layer(id));
    check(c::filterInputMatches(*document.layer(id),frozen),"frozen source key matches");
    const auto cache=c::prepareLayerSpatialFilters(frozen);
    check(cache && cache->pixelsToLocal.m02<0 && cache->pixelsToLocal.m12<0,"padded output preserves negative origin");
    const auto state=document.contentState();const auto revision=document.revision();
    check(c::publishLayerSpatialFilters(document,id,cache),"cache publishes");
    check(document.contentState()==state&&document.revision()==revision,"cache is not persistent content");
    check(c::prepareLayerSpatialFilters(*document.layer(id))==cache,"unchanged cache reused");
    const c::PreparedLayerSampler sampler(*document.layer(id));
    auto center=c::encodeColor(sampler.sample({4.5,4.5}));
    auto neighbor=c::encodeColor(sampler.sample({5.5,4.5}));
    check(center.alpha>0&&center.alpha<255&&neighbor.alpha>0,"blur alpha spreads around isolated impulse");
    check(neighbor.red==128&&neighbor.green==64&&neighbor.blue==32,"hidden magenta cannot contaminate edge");
    check(pixels(*c::intrinsicSurface(*document.layer(id)))==original,"filters retain exact authoritative pixels");
    document.setLayerTransform(id,{0,-2,15,1,0,4});
    check(c::layerSpatialFilterCacheValid(*document.layer(id)),"whole-layer transform reuses local filter cache");
    check(history.undo(document)&&!c::hasActiveSpatialFilters(document.layer(id)->filters),"undo filter");
    {
        c::FilterEditTransaction edit(document,id);edit.update(gaussian(2));edit.cancel();
    }
    check(history.canRedo(),"cancel preserves redo branch");
    check(history.redo(document),"redo filter");
    document.setLayerFilters(id,gaussian(3,true));
    check(!c::publishLayerSpatialFilters(document,id,cache),"stale parameters cannot publish");
    auto retained=c::prepareSpatialFilterLayer(*document.layer(id));
    c::PreparedLayerSampler preserve(retained);
    check(c::encodeColor(preserve.sample(document.layer(id)->localToDocument.map({4.5,4.5}))).alpha==255,"Preserve Alpha retains original support");
    check(c::encodeColor(preserve.sample(document.layer(id)->localToDocument.map({5.5,4.5}))).alpha==0,"Preserve Alpha does not spread silhouettes");
    bool cancelled=false;
    try {c::FilterPreparationOptions options;options.cancelled=[] {return true;};(void)c::prepareLayerSpatialFilters(frozen,options);}
    catch(const std::exception&){cancelled=true;}
    check(cancelled,"cancel produces no partial cache");
    bool budget=false;
    try {c::FilterPreparationOptions options;options.byteBudget=64;(void)c::prepareLayerSpatialFilters(frozen,options);}
    catch(const std::exception&){budget=true;}
    check(budget,"allocation preflight rejects insufficient budget");
}
void maskOutputPipelineAndCopy()
{
    c::Document document({{16,16},96});auto layer=impulse();
    auto adjustments=std::make_shared<c::AdjustmentStack>();
    adjustments->items[0].enabled=true;adjustments->items[0].parameters=c::ExposureParameters{1};
    layer.adjustments=adjustments;layer.filters=gaussian(3);
    layer.crop=c::LayerCrop{3,3,5,5};layer.opacity=.5F;
    const auto id=layer.id;document.insertLayer(0,layer);
    auto prepared=c::prepareSpatialFilterLayer(layer);c::publishLayerSpatialFilters(document,id,prepared.filterCache);
    auto flat=u::flattenDocument(document);check(bool(flat),"flatten filtered layer");
    c::PinnedDocumentSampler sample(document,id,c::ColorSampleSource::MergedVisible);
    if(flat)for(int y=0;y<16;++y)for(int x=0;x<16;++x){
        const auto actual=flat.image.pixelColor(x,y);
        const auto expected=sample.sample({x+.5,y+.5});
        check(actual.red()==expected.red&&actual.green()==expected.green&&actual.blue()==expected.blue&&actual.alpha()==expected.alpha,
            "flatten and merged sample agree exactly");
    }
    check(sample.sample({2.5,4.5}).alpha==0,"crop applied after filtering");
    document.setSelection(c::SelectionMask::filled({16,16},255));
    c::LayerViaCopyCommand copy(id,id);check(copy.apply(document),"selection extraction succeeds");
    const auto& extracted=document.layers().back();
    check(!extracted.filters&&!extracted.adjustments&&extracted.opacity==.5F,"extraction bakes filters once and retains opacity separately");
    c::PreparedLayerSampler original(prepared),copied(extracted);
    for(int y=0;y<16;++y)for(int x=0;x<16;++x)
        check(c::encodeColor(original.sample({x+.5,y+.5}))==c::encodeColor(copied.sample({x+.5,y+.5})),"extraction preserves filtered appearance");
    check(copy.undo(document),"copy undo");
    document.setSelection({});
    c::LayerViaCopyCommand duplicate(id,id);check(duplicate.apply(document),"duplicate");
    check(c::equivalentSpatialFilters(document.layers().back().filters,layer.filters),"duplicate retains editable stack");
    auto masked=impulse();masked.filters=gaussian(3,false,c::SelectionMask::rectangle({9,9},{5,4,1,1}));
    auto maskPrepared=c::prepareSpatialFilterLayer(masked);c::PreparedLayerSampler maskSample(maskPrepared);
    check(c::encodeColor(maskSample.sample({4.5,4.5})).alpha==255,"mask does not alter unselected output");
    check(c::encodeColor(maskSample.sample({5.5,4.5})).alpha>0,"mask can receive neighboring unselected source");
}
void typedPersistenceAndCompatibility()
{
    QTemporaryDir folder;check(folder.isValid(),"temporary fixture directory");
    c::Document document({{64,64},96});
    auto raster=impulse();raster.filters=gaussian(3,false,c::SelectionMask::filled({64,64},128));
    document.insertLayer(0,raster);
    c::TextLayer text;text.utf8="O";text.defaultStyle.sizePixels=20;
    auto textLayer=c::Layer::text("Editable text",text);textLayer.filters=gaussian(2);
    textLayer.localToDocument={-1,0,40,0,1,5};document.insertLayer(1,textLayer);
    c::ShapeLayer shape;shape.size={12,8};shape.fillColor={70,130,210,170};
    auto shapeLayer=c::Layer::shape("Editable shape",shape);
    auto stack=std::make_shared<c::SpatialFilterStack>();stack->items[1].enabled=true;
    stack->items[1].parameters=c::MotionBlurParameters{5,45};
    stack->items[2].enabled=true;stack->items[2].parameters=c::LensBlurParameters{2,5,23};
    shapeLayer.filters=stack;shapeLayer.localToDocument.m02=22;shapeLayer.localToDocument.m12=28;
    document.insertLayer(2,shapeLayer);
    const auto before=u::flattenDocument(document);check(bool(before),"typed filtered output");
    const auto saved=u::saveProject(folder.filePath("filters.vulkana"),document);check(bool(saved),"save editable filters");
    const auto reopened=u::loadProject(folder.filePath("filters.vulkana"));check(bool(reopened),"load editable filters");
    if(reopened){
        check(reopened.document->layers().size()==3,"all typed source layers retained");
        check(std::get<c::TextLayer>(reopened.document->layers()[1].payload)==std::get<c::TextLayer>(document.layers()[1].payload),"text retained editable");
        check(std::get<c::ShapeLayer>(reopened.document->layers()[2].payload)==shape,"shape retained editable");
        for(std::size_t i=0;i<3;++i)check(c::equivalentSpatialFilters(document.layers()[i].filters,reopened.document->layers()[i].filters),"filter state and masks round trip");
        const auto after=u::flattenDocument(*reopened.document);check(after&&before&&after.image==before.image,"reopen output is exact");
    }
    auto descriptor=u::detail::encodeSpatialFilters(*stack,shapeLayer.id);
    descriptor["algorithmVersion"]=999;bool rejected=false;
    try{(void)u::detail::decodeSpatialFilters(descriptor,shapeLayer.id);}catch(const std::exception&){rejected=true;}
    check(rejected,"future essential filter algorithm rejected");
    c::Document old({{16,16},96});old.insertLayer(0,impulse());
    check(bool(u::saveProject(folder.filePath("flat.vulkana"),old)),"save legacy-compatible identity document");
    const auto legacy=u::loadProject(folder.filePath("flat.vulkana"));
    check(legacy&&!legacy.document->layers()[0].filters,"old projects load without filters");
}
void benchmarkCase(c::Extent2u extent)
{
    using Clock=std::chrono::steady_clock;
    const auto ms=[](auto first,auto last){return std::chrono::duration<double,std::milli>(last-first).count();};
    std::vector<std::byte> bytes(std::size_t(extent.width)*extent.height*4);
    for(std::uint32_t y=0;y<extent.height;++y)for(std::uint32_t x=0;x<extent.width;++x) {
        const auto i=(std::size_t(y)*extent.width+x)*4;
        bytes[i]=std::byte((x*3+y)%256);bytes[i+1]=std::byte((x+y*7)%256);
        bytes[i+2]=std::byte((x/13+y/19)%256);
        bytes[i+3]=std::byte((x+y)%32==0?0:128+(x*11+y*13)%128);
    }
    c::Document document({extent,96});
    auto layer=c::Layer::raster("Persistent benchmark",std::make_shared<c::ContiguousRasterSurface>(extent,std::move(bytes)));
    const auto id=layer.id;layer.filters=gaussian(12);document.insertLayer(0,layer);document.markSaved();
    const auto source=c::intrinsicSurface(layer);const auto sourceRevision=source->revision();
    const auto begin=Clock::now();
    const auto input=c::snapshotFilterInput(layer);
    const auto captured=Clock::now();
    std::optional<Clock::time_point> encodeBegin;
    c::FilterPreparationOptions options;
    options.progress=[&](double value){if(value>=.85&&!encodeBegin)encodeBegin=Clock::now();};
    auto cache=c::prepareLayerSpatialFilters(input,options);
    const auto prepared=Clock::now();
    check(c::publishLayerSpatialFilters(document,id,cache),"benchmark publishes coherent cache");
    const auto published=Clock::now();
    constexpr int reuses=1000;
    const auto reuseBegin=Clock::now();
    for(int i=0;i<reuses;++i)check(c::prepareLayerSpatialFilters(*document.layer(id))==cache,"benchmark cache reused");
    const auto reuseEnd=Clock::now();
    check(source->revision()==sourceRevision&&!document.isModified(),"benchmark preserves source and content state");
    rusage usage{};getrusage(RUSAGE_SELF,&usage);
    std::cout<<std::fixed<<std::setprecision(3)
        <<"persistent "<<extent.width<<'x'<<extent.height
        <<" Gaussian radius=12 capture_ms="<<ms(begin,captured)
        <<" prepare_ms="<<ms(captured,prepared)
        <<" decode_kernel_ms="<<(encodeBegin?ms(captured,*encodeBegin):-1)
        <<" final_encode_ms="<<(encodeBegin?ms(*encodeBegin,prepared):-1)
        <<" publish_ms="<<ms(prepared,published)
        <<" total_ms="<<ms(begin,published)
        <<" reuse_mean_us="<<ms(reuseBegin,reuseEnd)*1000/reuses
        <<" process_peak_rss_mib="<<double(usage.ru_maxrss)/1024
        <<" admission_working_mib="<<double(cache->workingBytes)/(1024*1024)
        <<" frozen_source_mib="<<double(input.copiedBytes)/(1024*1024)
        <<" output="<<cache->surface->extent().width<<'x'<<cache->surface->extent().height
        <<"\n";
    // Deadline cancellation covers snapshot/decode/kernel orchestration, not
    // only a bare convolution. The published prior result must remain usable.
    const auto cancelBegin=Clock::now(),deadline=cancelBegin+std::chrono::milliseconds(25);
    c::FilterPreparationOptions cancelled;
    cancelled.cancelled=[&]{return Clock::now()>=deadline;};
    bool stopped=false;
    try{
        const auto frozen=c::snapshotFilterInput(layer,cancelled);
        (void)c::prepareLayerSpatialFilters(frozen,cancelled);
    }catch(const std::exception&){stopped=true;}
    const auto cancelEnd=Clock::now();
    check(stopped&&document.layer(id)->filterCache==cache,"cancelled complete job leaves published cache unchanged");
    std::cout<<"whole_job_cancel elapsed_ms="<<ms(cancelBegin,cancelEnd)
        <<" request_to_return_ms="<<std::max(0.0,ms(deadline,cancelEnd))
        <<" previous_cache_retained=1\n";
}
void benchmark()
{
    std::cout<<"Persistent filter benchmark: Release requested; each size uses a fresh child process. "
        <<"Linux getrusage(RUSAGE_SELF).ru_maxrss includes Qt, original raster, frozen copy, scratch and output. "
        <<"Source-pattern construction and GPU upload excluded; capture/decode/filter/encode/publish included.\n"<<std::flush;
    for(const auto size:std::array{c::Extent2u{3840,2160},c::Extent2u{5120,2880}}){
        QProcess child;child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(),{"--bench-case",QString::number(size.width),QString::number(size.height)});
        if(!child.waitForStarted(5000)||!child.waitForFinished(60000)){
            child.kill();child.waitForFinished();check(false,"persistent benchmark child timeout");continue;
        }
        std::cout<<child.readAll().toStdString();
        check(child.exitStatus()==QProcess::NormalExit&&child.exitCode()==0,"persistent benchmark child succeeds");
    }
}
}
int main(int argc,char** argv)
{
    QApplication app(argc,argv);
    if(app.arguments().contains("--bench-case")){
        try{benchmarkCase({app.arguments().at(2).toUInt(),app.arguments().at(3).toUInt()});}
        catch(const std::exception& e){std::cerr<<e.what()<<'\n';++failures;}
        return failures?1:0;
    }
    if(app.arguments().contains("--bench")){benchmark();return failures?1:0;}
    try{cacheAndHistory();maskOutputPipelineAndCopy();typedPersistenceAndCompatibility();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';++failures;}
    if(!failures)std::cout<<"Spatial filter integration: history, masks, transformed caches, alpha, typed content, copies and project compatibility passed\n";
    return failures?1:0;
}
