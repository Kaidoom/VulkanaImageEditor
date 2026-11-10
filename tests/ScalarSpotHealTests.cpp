#include "imageeditor/core/CloneReference.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"
#include "imageeditor/core/SpotHealRepair.hpp"
#include "imageeditor/core/SpotHealStroke.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace imageeditor::core;
int failures=0;
void check(bool value,const char* expression,int line)
{if(!value){std::cerr<<"FAIL "<<line<<": "<<expression<<'\n';++failures;}}
#define CHECK(...) check(bool((__VA_ARGS__)),#__VA_ARGS__,__LINE__)

struct RepairFixture {
    int width{64},height{64};
    std::vector<PremultipliedColor> pixels;
    std::vector<std::uint8_t> unknown,valid;
    explicit RepairFixture(int side=64):width(side),height(side),pixels(std::size_t(side*side),{0,0,0,1}),
        unknown(pixels.size()),valid(pixels.size(),255) {}
    void mark(int radius=8)
    {
        for(int y=height/2-radius;y<height/2+radius;++y)
            for(int x=width/2-radius;x<width/2+radius;++x) {
                const auto i=std::size_t(y*width+x);unknown[i]=255;
                pixels[i]={1,1,1,1};
            }
    }
    SpotHealPatch patch() const
    {return {width,height,pixels,unknown,valid,SpotHealSampleDomain::NumericR8};}
};

void blackIsValidRepairContext()
{
    RepairFixture f;f.mark();
    SpotHealOptions options;options.captureDiagnostics=true;
    const auto clean=repairSpotHeal(f.patch(),options);
    CHECK(clean);if(!clean)return;
    CHECK(clean.diagnostics.validDonorCenters>0);
    for(std::size_t i=0;i<f.pixels.size();++i) {
        CHECK(clean.pixels[i]==(PremultipliedColor{0,0,0,1}));
        if(f.unknown[i])f.pixels[i]={std::numeric_limits<float>::quiet_NaN(),-.5F,20,0};
    }
    const auto corrupt=repairSpotHeal(f.patch(),options);
    CHECK(corrupt);CHECK(corrupt.pixels==clean.pixels);
    f.valid.assign(f.valid.size(),0);
    const auto missing=repairSpotHeal(f.patch(),options);
    CHECK(missing.status==SpotHealStatus::InsufficientContext);
    CHECK(missing.pixels.empty());
    auto invalidDomain=f.patch();invalidDomain.sampleDomain=static_cast<SpotHealSampleDomain>(99);
    CHECK(repairSpotHeal(invalidDomain).status==SpotHealStatus::InvalidInput);
    RepairFixture wrong;wrong.mark();wrong.pixels.front()={0,.5F,0,1};
    CHECK(repairSpotHeal(wrong.patch()).status==SpotHealStatus::InvalidInput);
}

void numericQuantizationAndParallelSchedule()
{
    RepairFixture f(96);
    for(int y=0;y<f.height;++y)for(int x=0;x<f.width;++x) {
        const float value=float(std::lround(35+x*.7+y*.9))/255;
        f.pixels[std::size_t(y*f.width+x)]={value,value,value,1};
    }
    const auto original=f.pixels;f.mark(12);
    SpotHealOptions options;options.captureDiagnostics=true;options.workerCount=1;options.seed=71;
    const auto serial=repairSpotHeal(f.patch(),options);
    CHECK(serial);if(!serial)return;
    CHECK(serial.diagnostics.smoothModelPixels==serial.diagnostics.unknownPixels);
    for(std::size_t i=0;i<f.pixels.size();++i) {
        if(f.unknown[i])CHECK(std::abs(serial.pixels[i][0]-original[i][0])<2.F/255);
        else CHECK(serial.pixels[i]==original[i]);
        CHECK(serial.pixels[i][0]==serial.pixels[i][1]);
        CHECK(serial.pixels[i][0]==serial.pixels[i][2]);
        CHECK(serial.pixels[i][3]==1);
    }
    options.workerCount=4;
    const auto parallel=repairSpotHeal(f.patch(),options);
    CHECK(parallel);CHECK(parallel.pixels==serial.pixels);
    CHECK(parallel.diagnostics.donors==serial.diagnostics.donors);
    options.maxComparisons=1;
    const auto limited=repairSpotHeal(f.patch(),options);
    CHECK(limited.status==SpotHealStatus::LimitExceeded&&limited.pixels.empty());
    options.maxComparisons=256ULL*1024*1024;options.cancelled=[] {return true;};
    const auto cancelled=repairSpotHeal(f.patch(),options);
    CHECK(cancelled.status==SpotHealStatus::Cancelled&&cancelled.pixels.empty());

    RepairFixture grain;
    for(int y=0;y<grain.height;++y)for(int x=0;x<grain.width;++x) {
        const float value=float(120+((x*19+y*31+x*y*7)%3)-1)/255;
        grain.pixels[std::size_t(y*grain.width+x)]={value,value,value,1};
    }
    grain.mark();
    const auto textured=repairSpotHeal(grain.patch());
    CHECK(textured);
    CHECK(textured.diagnostics.smoothModelPixels==0);
}

struct StrokeFixture {
    Document document{CanvasSpec{.extent={64,64}}};
    std::shared_ptr<ScalarRasterSurface> surface;
    std::shared_ptr<ScalarRasterSurface> other{std::make_shared<ScalarRasterSurface>(Extent2u{64,64},93)};
    LayerId id{};
    History history;
    StrokeFixture()
    {
        std::vector<std::byte> pixels(64*64,std::byte{0});
        for(int y=28;y<36;++y)for(int x=28;x<36;++x)pixels[std::size_t(y*64+x)]=std::byte{255};
        surface=std::make_shared<ScalarRasterSurface>(Extent2u{64,64},pixels,64);
        auto layer=Layer::raster("Channel",surface);id=layer.id;
        CHECK(document.insertLayer(0,std::move(layer)));
        CHECK(document.insertLayer(1,Layer::raster("Untouched channel",other)));
    }
    CloneReference reference() const
    {
        std::string message;
        auto result=CloneReference::capture(document,id,id,CloneSampleSource::SourceLayer,message,
            CloneReference::defaultSnapshotLimit,false,{}, {},true);
        if(!result)throw std::runtime_error(message);
        CHECK(result->numericValues());
        return std::move(*result);
    }
};

std::vector<std::byte> data(const RasterSurface& surface)
{
    const auto e=surface.extent();std::vector<std::byte> bytes(std::size_t(e.width)*e.height);
    surface.copyNative({0,0,int(e.width),int(e.height)},bytes,e.width);return bytes;
}
BrushSettings brush(double opacity=1)
{
    auto s=proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    s.sizePixels=24;s.hardness=1;s.opacity=opacity;s.flow=1;s.pressureToSize=s.pressureToFlow=false;
    return s;
}
NormalizedPointerSample point(std::uint64_t time=0)
{return {.documentPosition={32,32},.timestampMicroseconds=time,.pressure=1,.pointerType=PointerType::Mouse,.buttons=PointerButtonPrimary};}
std::shared_ptr<const SpotHealWork> input(SpotHealStroke& stroke)
{
    CHECK(stroke.begin(point()));
    auto work=stroke.finishInput(point(1000));CHECK(work);
    return work;
}

void publishIsNumericUndoableAndSelectionResolvedOnce()
{
    StrokeFixture f;
    CHECK(f.document.setSelection(SelectionMask::filled({64,64},128)));
    const auto selection=f.document.selection();
    const auto before=data(*f.surface),other=data(*f.other);
    const auto revision=f.surface->revision();
    SpotHealStroke stroke(f.document,f.id,brush(.5),{},f.reference());
    const auto work=input(stroke);
    const auto solved=SpotHealStroke::solve(work);
    CHECK(solved.repair);if(!solved.repair){std::cerr<<solved.repair.diagnostics.message<<'\n';return;}
    CHECK(f.surface->revision()==revision&&data(*f.surface)==before);
    CHECK(stroke.stats().surfaceWriteBatches==0);
    CHECK(stroke.publish(solved,f.history)==RasterEditCommitResult::Committed);
    const auto after=data(*f.surface);
    CHECK(after[32*64+32]==std::byte{191});
    CHECK(after!=before&&data(*f.other)==other);
    CHECK(f.document.selection()==selection);
    CHECK(f.history.undoDepth()==1);
    CHECK(f.history.undo(f.document));CHECK(data(*f.surface)==before);
    CHECK(f.history.redo(f.document));CHECK(data(*f.surface)==after);
    CHECK(f.history.undo(f.document));
    const auto redo=f.history.redoDepth();
    SpotHealStroke cancelled(f.document,f.id,brush(),{},f.reference());
    CHECK(cancelled.begin(point()));cancelled.cancel();
    CHECK(data(*f.surface)==before&&f.history.redoDepth()==redo);
}

void failuresAndStalePublicationNeverWrite()
{
    StrokeFixture f;
    const auto before=data(*f.surface),other=data(*f.other);
    {
        SpotHealStroke stroke(f.document,f.id,brush(),{},f.reference());
        SpotHealOptions options;options.maxPixels=1;
        const auto solved=SpotHealStroke::solve(input(stroke),options);
        CHECK(solved.repair.status==SpotHealStatus::LimitExceeded);
        CHECK(stroke.publish(solved,f.history)==RasterEditCommitResult::TargetUnavailable);
        CHECK(data(*f.surface)==before&&!f.history.canUndo());
    }
    {
        SpotHealStroke stroke(f.document,f.id,brush(),{},f.reference());
        const auto solved=SpotHealStroke::solve(input(stroke));CHECK(solved.repair);
        int checks=0;
        CHECK(stroke.publish(solved,f.history,[&] {return ++checks>=3;})==RasterEditCommitResult::TargetUnavailable);
        CHECK(stroke.stats().surfaceWriteBatches>0);
        CHECK(data(*f.surface)==before&&!f.history.canUndo());
    }
    {
        SpotHealStroke stroke(f.document,f.id,brush(),{},f.reference());
        SpotHealOptions options;options.cancelled=[] {return true;};
        const auto solved=SpotHealStroke::solve(input(stroke),options);
        CHECK(solved.repair.status==SpotHealStatus::Cancelled);
        CHECK(stroke.publish(solved,f.history)==RasterEditCommitResult::TargetUnavailable);
        CHECK(data(*f.surface)==before&&!f.history.canUndo());
    }
    {
        SpotHealStroke stroke(f.document,f.id,brush(),{},f.reference());
        const auto solved=SpotHealStroke::solve(input(stroke));CHECK(solved.repair);
        CHECK(stroke.publish(solved,f.history,[] {return true;})==RasterEditCommitResult::TargetUnavailable);
        CHECK(data(*f.surface)==before&&!f.history.canUndo());
    }
    {
        SpotHealStroke stroke(f.document,f.id,brush(),{},f.reference());
        const auto solved=SpotHealStroke::solve(input(stroke));CHECK(solved.repair);
        CHECK(f.document.renameLayer(f.id,"Changed target"));
        CHECK(!stroke.targetMatches());
        CHECK(stroke.publish(solved,f.history)==RasterEditCommitResult::TargetUnavailable);
        CHECK(data(*f.surface)==before&&data(*f.other)==other&&!f.history.canUndo());
    }
    {
        SpotHealStroke stroke(f.document,f.id,brush(),{},f.reference());
        const auto solved=SpotHealStroke::solve(input(stroke));CHECK(solved.repair);
        const std::array<std::byte,1> edit{std::byte{19}};
        const RasterPatch patch{{0,0,1,1},edit,1};
        (void)f.surface->replaceNativeBatch(std::span(&patch,1));
        const auto changed=data(*f.surface);
        CHECK(!stroke.targetMatches());
        CHECK(stroke.publish(solved,f.history)==RasterEditCommitResult::TargetUnavailable);
        CHECK(data(*f.surface)==changed&&data(*f.other)==other&&!f.history.canUndo());
    }
    CHECK(!spotHealTargetDiagnostic(f.document,f.id,CloneSampleSource::AllVisible).empty());
    CHECK(!spotHealTargetDiagnostic(f.document,f.id,CloneSampleSource::CurrentAndBelow).empty());
    CHECK(spotHealTargetDiagnostic(f.document,f.id,CloneSampleSource::SourceLayer).empty());
    Document colour({{64,64}});
    auto layer=Layer::raster("Colour source",std::make_shared<ContiguousRasterSurface>(Extent2u{64,64},Rgba8{20,30,40,255}));
    const auto colourId=layer.id;CHECK(colour.insertLayer(0,std::move(layer)));
    std::string message;
    auto incompatible=CloneReference::capture(colour,colourId,colourId,CloneSampleSource::SourceLayer,message);
    CHECK(incompatible);
    if(incompatible) {
        const auto unchanged=data(*f.surface);
        SpotHealStroke rejected(f.document,f.id,brush(),{},std::move(*incompatible));
        CHECK(!rejected.diagnostic().empty());CHECK(!rejected.begin(point()));
        CHECK(data(*f.surface)==unchanged);
    }
}
}

int main()
{
    try {
        blackIsValidRepairContext();numericQuantizationAndParallelSchedule();
        publishIsNumericUndoableAndSelectionResolvedOnce();failuresAndStalePublicationNeverWrite();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
