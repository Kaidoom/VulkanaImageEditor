#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/BrushSymmetry.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << "FAIL " << __LINE__ << ": " #__VA_ARGS__ "\n"; ++failures; } } while (false)

NormalizedPointerSample point(double x, double y, double pressure = 1, std::uint64_t time = 0)
{
    return {.documentPosition={x,y}, .timestampMicroseconds=time, .pressure=pressure,
        .pointerType=PointerType::Pen, .buttons=PointerButtonPrimary};
}
BrushSettings settings()
{
    auto result = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    result.sizePixels=12; result.opacity=1; result.flow=1; result.hardness=1;
    result.pressureToSize=result.pressureToFlow=false;
    result.foreground={200,45,130,255};
    return result;
}
BrushSymmetry symmetry(BrushSymmetryAxis axis = BrushSymmetryAxis::Vertical,
    Vec2d center = {51,37})
{
    return {.enabled=true, .axis=axis, .center=center};
}
struct Fixture {
    Document document;
    LayerId id{};
    History history;
    explicit Fixture(Extent2u extent={128,96}, Rgba8 background={0,0,0,0}, bool scalar=false)
        : document(CanvasSpec{.extent=extent})
    {
        std::shared_ptr<RasterSurface> surface = scalar
            ? std::static_pointer_cast<RasterSurface>(std::make_shared<ScalarRasterSurface>(extent,0))
            : std::make_shared<ContiguousRasterSurface>(extent,background);
        auto layer=Layer::raster("Paint",std::move(surface));
        id=layer.id; CHECK(document.insertLayer(0,std::move(layer)));
    }
    std::shared_ptr<RasterSurface> surface() const
    { return std::get<RasterLayer>(document.layer(id)->payload).surface; }
};
std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto e=surface.extent(); std::vector<std::byte> result(std::size_t(e.width)*e.height*4);
    surface.copyRgba8({0,0,int(e.width),int(e.height)},result,std::size_t(e.width)*4);
    return result;
}
std::array<std::byte,4> pixel(const RasterSurface& surface,int x,int y)
{
    std::array<std::byte,4> result;surface.copyRgba8({x,y,1,1},result,4);return result;
}
std::unique_ptr<BasicPixelBrushStroke> stroke(Document& document, LayerId id, BrushSettings brush,
    BrushSymmetry mirror, BrushCompositeMode mode=BrushCompositeMode::Paint,
    std::unique_ptr<IBrushTip> tip={}, BrushExecutionOptions execution={},
    RasterEditTransactionOptions transaction={})
{
    return std::make_unique<BasicPixelBrushStroke>(document,id,std::move(brush),mode,
        std::make_unique<BasicPixelBrushEngine>(),std::move(tip),std::unique_ptr<IBrushGrain>{},nullptr,
        transaction,std::function<PremultipliedColor(Vec2d)>{},std::string_view{},false,
        std::function<void(const BrushDab&)>{},execution,mirror);
}
BrushStrokeStats paint(Fixture& fixture, BrushSettings brush, BrushSymmetry mirror,
    std::span<const NormalizedPointerSample> samples, BrushCompositeMode mode=BrushCompositeMode::Paint,
    std::unique_ptr<IBrushTip> tip={}, BrushExecutionOptions execution={})
{
    auto edit=stroke(fixture.document,fixture.id,std::move(brush),mirror,mode,std::move(tip),execution);
    CHECK(edit->valid()); CHECK(edit->begin(samples.front()));
    for(std::size_t i=1;i+1<samples.size();++i)CHECK(edit->append(samples[i]));
    CHECK(edit->end(samples.back(),fixture.history)==RasterEditCommitResult::Committed);
    return edit->stats();
}
// The reference is a regular one-sided stroke. Compare every pixel against
// its reflected source, including areas the mirrored stroke must leave empty.
void checkReflectedResult(const RasterSurface& original,const RasterSurface& mirrored,BrushSymmetry mirror)
{
    const auto e=mirrored.extent();CHECK(original.extent()==e);
    std::size_t mismatches=0;
    for(int y=0;y<int(e.height);++y)for(int x=0;x<int(e.width);++x) {
        const auto other=reflectedBrushPoint({x+.5,y+.5},mirror);
        const int ox=int(std::floor(other.x)),oy=int(std::floor(other.y));
        const auto direct=pixel(original,x,y);
        const auto reflected=ox>=0 && oy>=0 && ox<int(e.width) && oy<int(e.height)
            ?pixel(original,ox,oy):std::array<std::byte,4>{};
        // Very faint stamps retain unassociated RGB even if alpha rounds to
        // zero. An untouched transparent pixel must not win that tie.
        const auto expected=direct!=std::array<std::byte,4>{}
            && std::to_integer<unsigned>(direct[3])>=std::to_integer<unsigned>(reflected[3])?direct:reflected;
        if(pixel(mirrored,x,y)!=expected) {
            if(mismatches<8) {
                std::cerr << "mirror " << int(mirror.axis) << " at " << x << ',' << y << " expected";
                for(auto v:expected)std::cerr << ' ' << std::to_integer<unsigned>(v);
                std::cerr << " got";for(auto v:pixel(mirrored,x,y))std::cerr << ' ' << std::to_integer<unsigned>(v);
                std::cerr << " direct";for(auto v:direct)std::cerr << ' ' << std::to_integer<unsigned>(v);
                std::cerr << " reflected";for(auto v:reflected)std::cerr << ' ' << std::to_integer<unsigned>(v);
                std::cerr << '\n';
            }
            ++mismatches;
        }
    }
    if(mismatches)std::cerr<<mismatches<<" pixels differ from mirrored reference\n";
    CHECK(mismatches==0);
}
void axesPressureAndHistory()
{
    auto brush=settings();brush.sizePixels=10;brush.hardness=.65;brush.flow=.35;brush.opacity=.7;
    brush.tip.assetId=std::string(BrushAssetIds::ProceduralEllipseTip);brush.tip.aspectRatio=.45;
    brush.tip.rotationMode=BrushTipRotationMode::FollowStrokeDirection;
    brush.pressureToSize=brush.pressureToFlow=true;
    brush.smoothing=BrushSmoothingMode::Weighted;brush.smoothingTimeMilliseconds=7;
    const std::array path{point(13,10,.25,0),point(19,14,.65,15000),point(25,17,.9,30000),point(30,21,.45,50000)};
    for(auto axis:{BrushSymmetryAxis::Vertical,BrushSymmetryAxis::Horizontal}) {
        Fixture plain, mirrored;const auto original=bytes(*mirrored.surface());
        paint(plain,brush,{},path);const auto stats=paint(mirrored,brush,symmetry(axis),path);
        CHECK(stats.emittedDabs>1);checkReflectedResult(*plain.surface(),*mirrored.surface(),symmetry(axis));
        CHECK(mirrored.history.undoDepth()==1);
        const auto changed=bytes(*mirrored.surface());
        CHECK(mirrored.history.undo(mirrored.document));CHECK(bytes(*mirrored.surface())==original);
        CHECK(mirrored.history.redo(mirrored.document));CHECK(bytes(*mirrored.surface())==changed);
        CHECK(mirrored.history.undo(mirrored.document));
        auto pending=stroke(mirrored.document,mirrored.id,brush,symmetry(axis));
        CHECK(pending->begin(path.front()));CHECK(pending->append(path.back()));pending->cancel();
        CHECK(bytes(*mirrored.surface())==original);CHECK(mirrored.history.redoDepth()==1);
    }
}
void overlapDoesNotDepositTwice()
{
    auto brush=settings();brush.sizePixels=22;brush.hardness=.3;brush.flow=.22;brush.opacity=.65;
    brush.foreground={0,0,0,255};
    const auto mirror=symmetry();
    const std::array centerPath{point(51,18),point(51,30,1,10000),point(51,45,1,30000)};
    Fixture centerPlain,centerMirrored;
    paint(centerPlain,brush,{},centerPath);paint(centerMirrored,brush,mirror,centerPath);
    CHECK(bytes(*centerPlain.surface())==bytes(*centerMirrored.surface()));
    const std::array dot{point(48.5,32.5)};
    Fixture plain,mirrored;paint(plain,brush,{},dot);paint(mirrored,brush,mirror,dot);
    checkReflectedResult(*plain.surface(),*mirrored.surface(),mirror);
}
std::unique_ptr<IBrushTip> asymmetricTip()
{
    std::array<std::uint8_t,64> data{};
    for(int y=0;y<8;++y)for(int x=0;x<8;++x)
        data[std::size_t(y*8+x)]=std::uint8_t(x<=y ? 40+x*20+y*10 : 0);
    return std::make_unique<BitmapMaskTip>(std::make_shared<GrayscaleMaskAsset>("test.asymmetric",1,8,8,data));
}
void texturedTipsAndGrainReflect()
{
    auto brush=settings();brush.sizePixels=18;brush.tip.angleDegrees=27;
    brush.tip.rotationMode=BrushTipRotationMode::Fixed;
    brush.grain.assetId=std::string(BrushAssetIds::DryInkPaperGrain);
    brush.grain.scalePixels=13;brush.grain.angleDegrees=19;brush.grain.strength=.8;brush.deterministicSeed=127;
    const std::array path{point(20.5,20.5),point(27.5,25.5,1,20000)};
    for(auto axis:{BrushSymmetryAxis::Vertical,BrushSymmetryAxis::Horizontal}) {
        Fixture plain,mirrored;paint(plain,brush,{},path,BrushCompositeMode::Paint,asymmetricTip());
        paint(mirrored,brush,symmetry(axis),path,BrushCompositeMode::Paint,asymmetricTip());
        checkReflectedResult(*plain.surface(),*mirrored.surface(),symmetry(axis));
    }
}
void selectionAndCanvasClipIndependently()
{
    Fixture fixture;const auto selection=SelectionMask::rectangle({128,96},{60,0,68,96},128);
    CHECK(fixture.document.setSelection(selection));auto brush=settings();brush.sizePixels=8;
    const std::array path{point(20.5,20.5)};paint(fixture,brush,symmetry(),path);
    CHECK(pixel(*fixture.surface(),20,20)[3]==std::byte{0});
    CHECK(pixel(*fixture.surface(),81,20)[3]==std::byte{128});
    CHECK(fixture.document.selection()==selection);
    // A primary footprint outside the canvas must not suppress its valid twin.
    Fixture outside;const std::array beyond{point(-8.5,20.5)};
    paint(outside,brush,symmetry(BrushSymmetryAxis::Vertical,{40,37}),beyond);
    CHECK(pixel(*outside.surface(),88,20)[3]==std::byte{255});
}
void transformedLayerUsesDocumentAxis()
{
    Fixture reference,rotated;auto brush=settings();brush.sizePixels=9.5;brush.hardness=.65;brush.flow=.4;
    const std::array path{point(20.5,15.5),point(27.5,26.5,1,20000)};
    paint(reference,brush,symmetry(),path);
    auto surface=std::make_shared<ContiguousRasterSurface>(Extent2u{96,128},Rgba8{0,0,0,0});
    CHECK(rotated.document.setLayerRasterStorage(rotated.id,surface,{},std::nullopt));
    const AffineTransform transform{0,-1,128,1,0,0};CHECK(rotated.document.setLayerTransform(rotated.id,transform));
    paint(rotated,brush,symmetry(),path);
    std::size_t mismatches=0;
    for(int y=0;y<128;++y)for(int x=0;x<96;++x)
        if(pixel(*rotated.surface(),x,y)!=pixel(*reference.surface(),127-y,x))++mismatches;
    CHECK(mismatches==0);CHECK(rotated.document.layer(rotated.id)->localToDocument==transform);
}
void eraserAndScalar()
{
    Fixture colour({128,96},{80,110,140,255});auto brush=settings();brush.opacity=.5;
    const std::array path{point(20.5,20.5)};paint(colour,brush,symmetry(),path,BrushCompositeMode::Erase);
    CHECK(pixel(*colour.surface(),20,20)==pixel(*colour.surface(),81,20));
    CHECK(std::to_integer<unsigned>(pixel(*colour.surface(),20,20)[3])==128);
    CHECK(pixel(*colour.surface(),50,20)[3]==std::byte{255});
    CHECK(colour.history.undoDepth()==1);CHECK(colour.history.undo(colour.document));
    CHECK(pixel(*colour.surface(),20,20)[3]==std::byte{255});

    Fixture scalar({128,96},{},true);auto surface=scalar.surface();
    auto other=std::make_shared<ScalarRasterSurface>(Extent2u{128,96},93);
    CHECK(scalar.document.insertLayer(1,Layer::raster("Other channel",other)));
    brush.foreground={255,255,255,255};paint(scalar,brush,symmetry(),path);
    const auto expected=std::array{std::byte{128},std::byte{128},std::byte{128},std::byte{255}};
    CHECK(pixel(*surface,20,20)==expected);CHECK(pixel(*surface,81,20)==expected);
    CHECK(pixel(*other,20,20)[0]==std::byte{93});
    CHECK(scalar.history.undo(scalar.document));CHECK(pixel(*surface,81,20)[0]==std::byte{0});
    CHECK(scalar.history.redo(scalar.document));brush.opacity=1;
    paint(scalar,brush,symmetry(),path,BrushCompositeMode::Erase);
    CHECK(pixel(*surface,20,20)[0]==std::byte{0});CHECK(pixel(*surface,81,20)[0]==std::byte{0});
}
void maskPaintingIsOneUndoableEdit()
{
    Fixture fixture;const auto unchanged=bytes(*fixture.surface());
    auto mask=std::make_shared<LayerMask>();mask->coverage=SelectionMask::filled({128,96},170);
    CHECK(fixture.document.setLayerMask(fixture.id,mask));
    auto brush=settings();brush.foreground={0,0,0,255};brush.opacity=.5;
    const auto stamp=point(20.5,20.5);
    {
        LayerMaskEdit edit(fixture.document,fixture.id,"Symmetric mask stroke");
        CHECK(!edit.proxy().brushSymmetry().enabled);
        auto paintMask=stroke(edit.proxy(),fixture.id,brush,symmetry(),BrushCompositeMode::MaskCoverage,
            {},{},RasterEditTransactionOptions{.coverageValues=true});
        CHECK(paintMask->begin(stamp));
        CHECK(paintMask->end(stamp,edit.provisionalHistory())==RasterEditCommitResult::Committed);
        CHECK(edit.commit(fixture.history)==RasterEditCommitResult::Committed);
    }
    const auto changed=fixture.document.layer(fixture.id)->mask;
    CHECK(changed->coverage->coverageAtDocumentPixel(20,20)==85);
    CHECK(changed->coverage->coverageAtDocumentPixel(81,20)==85);
    CHECK(changed->coverage->coverageAtDocumentPixel(50,20)==170);
    CHECK(bytes(*fixture.surface())==unchanged);CHECK(fixture.history.undoDepth()==1);
    CHECK(fixture.history.undo(fixture.document));CHECK(fixture.document.layer(fixture.id)->mask==mask);
    CHECK(fixture.history.redo(fixture.document));CHECK(fixture.document.layer(fixture.id)->mask==changed);
    {
        LayerMaskEdit edit(fixture.document,fixture.id,"Cancelled mask stroke");
        brush.foreground={255,255,255,255};
        auto paintMask=stroke(edit.proxy(),fixture.id,brush,symmetry(),BrushCompositeMode::MaskCoverage,
            {},{},RasterEditTransactionOptions{.coverageValues=true});
        CHECK(paintMask->begin(stamp));paintMask->cancel();edit.cancel();
    }
    CHECK(fixture.document.layer(fixture.id)->mask==changed);CHECK(fixture.history.undoDepth()==1);
}
void offsetRasterGrowsForBothMarksAndRestoresOnUndo()
{
    Fixture fixture;
    auto original=std::make_shared<ContiguousRasterSurface>(Extent2u{8,8},Rgba8{17,21,29,0});
    const Vec2d origin{-2,-3};
    CHECK(fixture.document.setLayerRasterStorage(fixture.id,original,origin,std::nullopt));
    const AffineTransform transform{1,0,12,0,1,15};
    CHECK(fixture.document.setLayerTransform(fixture.id,transform));
    const auto originalBytes=bytes(*original);
    auto brush=settings();brush.sizePixels=4;
    const std::array path{point(14.5,16.5)};
    paint(fixture,brush,symmetry(),path);
    const auto* expanded=fixture.document.layer(fixture.id);
    const auto expandedExtent=fixture.surface()->extent();
    const auto expandedOrigin=expanded->rasterOrigin;
    const auto expandedFrame=expanded->rasterEffectFrame;
    CHECK(expandedExtent.width>8);
    CHECK(expanded->localToDocument==transform);
    const auto documentPixel=[&](Vec2d point) {
        const auto local=intrinsicTransform(*fixture.document.layer(fixture.id)).inverted()->map(point);
        return pixel(*fixture.surface(),int(std::floor(local.x)),int(std::floor(local.y)));
    };
    const auto painted=std::array{std::byte{200},std::byte{45},std::byte{130},std::byte{255}};
    CHECK(documentPixel({14.5,16.5})==painted);
    CHECK(documentPixel({87.5,16.5})==painted);
    CHECK(documentPixel({51.5,16.5})[3]==std::byte{0});
    const auto expandedBytes=bytes(*fixture.surface());
    CHECK(fixture.history.undoDepth()==1);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.surface()==original);
    CHECK(fixture.surface()->extent()==(Extent2u{8,8}));
    CHECK(fixture.document.layer(fixture.id)->rasterOrigin==origin);
    CHECK(!fixture.document.layer(fixture.id)->rasterEffectFrame);
    CHECK(fixture.document.layer(fixture.id)->localToDocument==transform);
    CHECK(bytes(*fixture.surface())==originalBytes);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.surface()->extent()==expandedExtent);
    CHECK(fixture.document.layer(fixture.id)->rasterOrigin==expandedOrigin);
    CHECK(fixture.document.layer(fixture.id)->rasterEffectFrame==expandedFrame);
    CHECK(bytes(*fixture.surface())==expandedBytes);
}
class WideOffCanvasTip final : public IBrushTip {
public:
    BrushTipBounds prepareDab(const BrushDab&,double,double) noexcept override
    { return {-4090,0,10,2048}; }
    double coverage(Vec2d) const noexcept override { return 1; }
};
class CountedGrain final : public IBrushGrain {
public:
    explicit CountedGrain(int& calls):calls_(calls) {}
    void prepareDab(const BrushDab&,double) noexcept override { ++calls_; }
    double modulation(Vec2d) const noexcept override { return 1; }
private:
    int& calls_;
};
void oversizedMirroredFootprintFailsBeforeGrainAndWrites()
{
    // The original intersects only a thin canvas strip. Its mirror covers a
    // much larger region; both must be admitted before preparing any grain.
    Document document(CanvasSpec{.extent={4096,2048}});
    auto surface=std::make_shared<ContiguousRasterSurface>(Extent2u{8,8},Rgba8{19,31,47,0});
    auto layer=Layer::raster("Small source",surface);const auto id=layer.id;
    CHECK(document.insertLayer(0,std::move(layer)));
    const auto original=bytes(*surface);int prepared=0;
    BasicPixelBrushStroke edit(document,id,settings(),BrushCompositeMode::Paint,
        std::make_unique<BasicPixelBrushEngine>(),std::make_unique<WideOffCanvasTip>(),
        std::make_unique<CountedGrain>(prepared),nullptr,{}, {}, {},false,{}, {},
        symmetry(BrushSymmetryAxis::Vertical,{1050,32}));
    CHECK(edit.valid());CHECK(!edit.begin(point(5,5)));
    CHECK(edit.failure()==BrushStrokeFailure::RasterWorkLimitExceeded);
    CHECK(edit.stats().rejectedCandidatePixels>kMaximumBrushDabCandidatePixels);
    CHECK(edit.stats().retainedStrokeTiles==0);
    CHECK(prepared==0);CHECK(bytes(*surface)==original);
    CHECK(std::get<RasterLayer>(document.layer(id)->payload).surface==surface);
}
void parallelCoverageMatchesSerial()
{
    Fixture serial({320,192}),parallel({320,192});auto brush=settings();
    brush.sizePixels=170;brush.hardness=.4;brush.opacity=.65;brush.flow=.35;
    const auto mirror=symmetry(BrushSymmetryAxis::Vertical,{157,96});
    const std::array path{point(100,85),point(145,100,1,20000)};
    paint(serial,brush,mirror,path,BrushCompositeMode::Paint,{},BrushExecutionOptions{.workers=1});
    const auto stats=paint(parallel,brush,mirror,path,BrushCompositeMode::Paint,{},BrushExecutionOptions{.workers=2});
    CHECK(stats.parallelDabs>0);CHECK(bytes(*serial.surface())==bytes(*parallel.surface()));
}
}
int main()
{
    try {
        axesPressureAndHistory();overlapDoesNotDepositTwice();texturedTipsAndGrainReflect();
        selectionAndCanvasClipIndependently();transformedLayerUsesDocumentAxis();eraserAndScalar();
        maskPaintingIsOneUndoableEdit();offsetRasterGrowsForBothMarksAndRestoresOnUndo();
        oversizedMirroredFootprintFailsBeforeGrainAndWrites();parallelCoverageMatchesSerial();
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return EXIT_FAILURE; }
    if(failures)std::cerr<<failures<<" brush symmetry checks failed\n";
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
