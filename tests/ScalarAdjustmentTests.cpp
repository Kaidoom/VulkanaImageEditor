#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/FilterCommands.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using namespace imageeditor::core;
int failures=0;
void check(bool value,const char* expression,int line)
{if(!value){std::cerr<<"FAIL "<<line<<": "<<expression<<'\n';++failures;}}
#define CHECK(...) check(bool((__VA_ARGS__)),#__VA_ARGS__,__LINE__)
template<class F>bool rejects(F&& f){try{f();}catch(const std::exception&){return true;}return false;}
std::vector<std::byte> pixels(const RasterSurface& surface)
{
    const auto e=surface.extent();const auto stride=std::size_t(e.width)*surface.nativeBytesPerPixel();
    std::vector<std::byte> result(stride*e.height);
    surface.copyNative({0,0,int(e.width),int(e.height)},result,stride);return result;
}
std::shared_ptr<AdjustmentStack> adjustment(AdjustmentType type)
{auto s=std::make_shared<AdjustmentStack>();s->items[std::size_t(type)].enabled=true;return s;}
std::shared_ptr<SpatialFilterStack> gaussian(double radius)
{
    auto s=std::make_shared<SpatialFilterStack>();s->items[0].enabled=true;
    s->items[0].parameters=GaussianBlurParameters{radius,radius};return s;
}

void scalarAdmissionAndNumericEquations()
{
    for(auto type:allAdjustmentTypes) {
        const auto s=adjustment(type);
        CHECK(scalarAdjustmentsSupported(s)==(type==AdjustmentType::Levels||type==AdjustmentType::Curves||type==AdjustmentType::Invert));
    }
    auto levels=adjustment(AdjustmentType::Levels);
    auto& lp=std::get<LevelsParameters>(levels->items[2].parameters);
    lp.channels[1].gamma=2;CHECK(!scalarAdjustmentsSupported(levels));lp.channels[1].gamma=1;
    lp.channels[0]={.2,2,.8,.1,.9};
    auto curve=adjustment(AdjustmentType::Curves);
    auto& cp=std::get<CurvesParameters>(curve->items[3].parameters);
    cp.channels[0].points={{0,0},{1,.5}};
    const auto inv=compileAdjustmentStack(adjustment(AdjustmentType::Invert));
    const auto lev=compileAdjustmentStack(levels),cur=compileAdjustmentStack(curve);
    for(int code=0;code<256;++code) {
        const float v=float(code)/255;
        CHECK(alphaToByte(evaluateScalarAdjustments(inv,v,{}))==255-code);
        const double expected=.1+.8*std::sqrt(std::clamp((double(code)/255-.2)/.6,0.,1.));
        CHECK(std::abs(evaluateScalarAdjustments(lev,v,{})-expected)<1e-6);
        CHECK(std::abs(evaluateScalarAdjustments(cur,v,{})-v*.5F)<1e-6F);
        CHECK(evaluateScalarAdjustments(cur,v,{},3)==v);
    }
    cp.channels[2].points={{0,0},{1,.8}};CHECK(!scalarAdjustmentsSupported(curve));
    for(auto type:allSpatialFilterTypes) {
        auto s=std::make_shared<SpatialFilterStack>();s->items[std::size_t(type)].enabled=true;
        CHECK(scalarFiltersSupported(s)==(type==SpatialFilterType::Gaussian));
    }
    auto g=gaussian(2);g->items[0].preserveAlpha=true;CHECK(!scalarFiltersSupported(g));
}

void capturedMasksFollowLocalOriginAndTransform()
{
    Document doc({{64,64},96});
    auto source=std::make_shared<ScalarRasterSurface>(Extent2u{8,4},64);
    auto layer=Layer::raster("Numeric mask",source);
    layer.rasterOrigin={-3,2};layer.localToDocument={2,0,10,0,2,6};
    const auto id=layer.id;CHECK(doc.insertLayer(0,layer));
    auto s=adjustment(AdjustmentType::Invert);
    s->items[9].mask=captureAdjustmentMask(SelectionMask::rectangle({64,64},{0,0,13,64},128),layer.localToDocument);
    CHECK(doc.setLayerAdjustments(id,s));
    const auto before=pixels(*source);
    const auto revision=source->revision();
    const auto output=prepareScalarLayerPixels(*doc.layer(id));
    const auto values=pixels(*output);
    CHECK(values[0]==std::byte{128});
    CHECK(values[4]==std::byte{96});
    CHECK(values[7]==std::byte{64});
    CHECK(pixels(*source)==before&&source->revision()==revision);
    const PreparedLayerSampler displayed(*doc.layer(id));
    CHECK(encodeColor(displayed.sample({5,11})).red==128);
    CHECK(encodeColor(displayed.sample({19,11})).red==64);
}

void gaussianUsesNumericFixedGridAndReusableCaches()
{
    Document doc({{128,128},96});
    auto source=std::make_shared<ScalarRasterSurface>(Extent2u{128,128},128);
    auto layer=Layer::raster("Scalar Gaussian",source);const auto id=layer.id;
    CHECK(doc.insertLayer(0,std::move(layer)));
    auto filters=gaussian(4);CHECK(doc.setLayerFilters(id,filters));
    const auto snapshot=snapshotFilterInput(*doc.layer(id));
    const auto cache=prepareLayerSpatialFilters(snapshot);
    CHECK(cache->surface->dataKind()==RasterDataKind::Scalar);
    CHECK(cache->surface->extent()==source->extent());
    CHECK(cache->surface->nativeBytesPerPixel()==1);
    const auto result=pixels(*cache->surface);
    CHECK(std::all_of(result.begin(),result.end(),[](auto v){return v==std::byte{128};}));
    CHECK(cache->workingBytes<128U*128U*4U*sizeof(float)*3U);
    CHECK(publishLayerSpatialFilters(doc,id,cache));
    CHECK(prepareScalarLayerPixels(*doc.layer(id))==cache->surface);
    const std::array<std::byte,1> value{std::byte{255}};
    const RasterPatch patch{{64,64,1,1},value,1};(void)source->replaceNativeBatch(std::span(&patch,1));
    CHECK(!layerSpatialFilterCacheValid(*doc.layer(id)));
    CHECK(!publishLayerSpatialFilters(doc,id,cache));
    const auto changed=prepareLayerSpatialFilters(*doc.layer(id));
    CHECK(changed->sourceRevision==source->revision());
    CHECK(changed->surface->extent()==source->extent());
    const auto next=pixels(*changed->surface);
    CHECK(next[64*128+64]>std::byte{128}&&next[64*128+64]<std::byte{255});
    CHECK(next[0]==std::byte{128});CHECK(pixels(*cache->surface)==result);
    CHECK(publishLayerSpatialFilters(doc,id,changed));
    CHECK(rejects([&]{(void)prepareLayerSpatialFilters(*doc.layer(id),{.byteBudget=32});})==false);
    auto invalidated=*doc.layer(id);invalidated.filterCache.reset();
    CHECK(rejects([&]{(void)prepareLayerSpatialFilters(invalidated,{.byteBudget=32});}));
    CHECK(rejects([&]{(void)prepareLayerSpatialFilters(invalidated,{.cancelled=[] {return true;}});}));
    CHECK(pixels(*source)[64*128+64]==std::byte{255});
}

void packingProcessesChannelsAndUndoKeepsCanonicalPixels()
{
    Document doc({{16,16},96});History history;
    auto raster=Layer::raster("Packed",std::make_shared<ContiguousRasterSurface>(Extent2u{16,16},Rgba8{64,88,121,0}));
    const auto id=raster.id;CHECK(doc.insertLayer(0,std::move(raster)));
    CHECK(history.execute(doc,convertToChannelGroup(doc,LayerSelectionState{{id},id,id})));
    const auto roles=doc.tree().container(id)->channelRoles;
    const auto& source=*std::get<RasterLayer>(doc.layer(roles[0])->payload).surface;
    const auto before=pixels(source);
    auto inv=adjustment(AdjustmentType::Invert);
    CHECK(history.execute(doc,std::make_unique<SetLayerAdjustmentsCommand>(roles[0],AdjustmentState{},inv)));
    const auto packed=pixels(*packChannelGroup(doc,id));
    CHECK(packed[0]==std::byte{191}&&packed[1]==std::byte{88}&&packed[2]==std::byte{121}&&packed[3]==std::byte{0});
    CHECK(pixels(source)==before);
    CHECK(history.undo(doc));CHECK(pixels(*packChannelGroup(doc,id))[0]==std::byte{64});
    CHECK(history.redo(doc));CHECK(pixels(*packChannelGroup(doc,id))[0]==std::byte{191});
    CHECK(history.execute(doc,convertChannelGroupToRaster(doc,LayerSelectionState{{id},id,id})));
    CHECK(pixels(*std::get<RasterLayer>(doc.layer(id)->payload).surface)==packed);
    CHECK(history.undo(doc));CHECK(doc.tree().container(id));
    CHECK(pixels(*std::get<RasterLayer>(doc.layer(roles[0])->payload).surface)==before);
    const auto revision=doc.revision();
    const auto undo=history.undoDepth();
    CHECK(!history.execute(doc,std::make_unique<SetLayerAdjustmentsCommand>(roles[0],inv,adjustment(AdjustmentType::Exposure))));
    CHECK(doc.revision()==revision&&history.undoDepth()==undo);
}
}
int main()
{
    try {
        scalarAdmissionAndNumericEquations();capturedMasksFollowLocalOriginAndTransform();
        gaussianUsesNumericFixedGridAndReusableCaches();packingProcessesChannelsAndUndoKeepsCanonicalPixels();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
