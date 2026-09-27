#include "imageeditor/core/AnchoredLassoPath.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/FreehandSelectionPath.hpp"
#include "imageeditor/core/LiveWire.hpp"
#include "imageeditor/core/PolygonCoverageRasterizer.hpp"
#include "imageeditor/core/SelectionCommands.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
template<class Function> bool throws(Function&& function)
{
    try { function(); }
    catch (const std::exception&) { return true; }
    return false;
}
bool same(std::span<const Vec2d> a, std::span<const Vec2d> b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}
SelectionState raster(Extent2u extent, std::span<const Vec2d> polygon)
{
    PolygonCoverageRasterizer producer(extent, polygon);
    while (!producer.finished()) producer.step();
    return SelectionMask::fromR8Region(extent, producer.region(), producer.coverage(), producer.stride());
}
void complete(LiveWireSearch& search, std::size_t budget = 1024)
{
    unsigned steps = 0;
    while (search.result() == LiveWireSearch::Result::Searching) {
        search.step(budget);
        if (++steps > 2000000) { CHECK(false); return; }
    }
}
double distanceToSegment(Vec2d p, Vec2d a, Vec2d b)
{
    const auto d = b - a;
    const auto lengthSquared = d.x*d.x + d.y*d.y;
    const auto t = lengthSquared ? std::clamp(((p.x-a.x)*d.x + (p.y-a.y)*d.y)/lengthSquared, 0.0, 1.0) : 0.0;
    return std::hypot(p.x-a.x-t*d.x, p.y-a.y-t*d.y);
}
double distanceToGuide(Vec2d p, std::span<const Vec2d> guide)
{
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i < guide.size(); ++i)
        best = std::min(best, distanceToSegment(p, guide[i-1], guide[i]));
    return best;
}
std::vector<Vec2d> samplePolyline(std::span<const Vec2d> path)
{
    std::vector<Vec2d> samples;
    for(std::size_t i=1;i<path.size();++i) {
        const auto a=path[i-1],d=path[i]-a;
        const auto count=std::max(1,int(std::ceil(std::hypot(d.x,d.y))));
        for(int j=0;j<count;++j) samples.push_back(a+d*(double(j)/count));
    }
    if(!path.empty()) samples.push_back(path.back());
    return samples;
}
void assertPathContract(const LiveWireSearch& search, std::span<const Vec2d> guide, double radius)
{
    CHECK(search.result() != LiveWireSearch::Result::Searching);
    CHECK(!search.path().empty());
    if (search.path().empty()) return;
    CHECK(search.path().front() == guide.front());
    CHECK(search.path().back() == guide.back());
    for (const auto p : search.path()) {
        CHECK(std::isfinite(p.x) && std::isfinite(p.y));
        CHECK(distanceToGuide(p, guide) <= radius + std::sqrt(2.0));
    }
    CHECK(search.stats().expanded <= LiveWireSearch::maximumNodes);
    CHECK(search.stats().prepared <= LiveWireSearch::maximumNodes);
    CHECK(search.memoryBytes() < 64U * 1024U * 1024U);
}
void anchorsAreEditableConstructionNotHistory()
{
    EditorSession session;
    session.replaceDocument(std::make_unique<Document>(CanvasSpec{{64,48},96}));
    const auto original = SelectionMask::rectangle({64,48}, {4,4,12,8});
    CHECK(session.execute(std::make_unique<SetSelectionCommand>(original)));
    CHECK(session.undo());
    const auto redoDepth = session.history().redoDepth();
    AnchoredLassoPath path;
    CHECK(!path.removeLastAnchor());
    CHECK(path.addVertex({2.25,3.5}));
    CHECK(!path.addVertex({2.25,3.5}));
    CHECK(path.anchorCount() == 1);
    CHECK(path.addVertex({24.25,3.5}));
    CHECK(path.addVertex({24.25,20.5}));
    CHECK(path.addVertex({2.25,20.5}));
    CHECK(path.anchorCount() == 4);
    CHECK(path.removeLastAnchor());
    CHECK(path.points().back() == Vec2d({24.25,20.5}));
    CHECK(path.addVertex({2.25,20.5}));
    CHECK(session.history().undoDepth() == 0);
    CHECK(session.history().redoDepth() == redoDepth);
    CHECK(!session.document()->selection());
    const auto incoming = raster({64,48}, path.points());
    CHECK(incoming->coverageAtDocumentPixel(10,10) == 255);
    CHECK(incoming->coverageAtDocumentPixel(2,3) > 0);
    CHECK(incoming->coverageAtDocumentPixel(2,3) < 255);
    CHECK(session.execute(std::make_unique<SetSelectionCommand>(incoming, "Polygonal selection")));
    CHECK(session.history().undoDepth() == 1);
    CHECK(session.history().redoDepth() == 0);
    CHECK(session.undo());
    CHECK(!session.document()->selection());
    CHECK(session.redo());
    CHECK(session.document()->selection()->equivalent(*incoming));
    const auto revision = session.document()->selectionRevision();
    CHECK(!session.execute(std::make_unique<SetSelectionCommand>(incoming)));
    CHECK(session.document()->selectionRevision() == revision);
    const auto extension=SelectionMask::rectangle({64,48},{35,30,4,4});
    CHECK(session.execute(std::make_unique<SetSelectionCommand>(extension)));
    CHECK(session.undo());
    const auto redo=session.history().redoDepth();
    const auto combinedNoOp=combineSelection(incoming,incoming,SelectionOperation::Add);
    CHECK(!session.execute(std::make_unique<SetSelectionCommand>(combinedNoOp)));
    CHECK(session.history().redoDepth()==redo);
}
void stableSegmentsAndMalformedVertices()
{
    AnchoredLassoPath path;
    CHECK(path.addVertex({1,1}));
    const std::array first {Vec2d{1,1}, Vec2d{1,1}, Vec2d{2,2}, Vec2d{3,1}, Vec2d{3,1}};
    CHECK(path.addSegment(first));
    const std::vector<Vec2d> anchored(path.points().begin(), path.points().end());
    CHECK(path.anchorCount() == 2);
    const std::array second {Vec2d{3,1}, Vec2d{4,2}, Vec2d{4,2}, Vec2d{5,3}};
    CHECK(path.addSegment(second));
    CHECK(std::equal(anchored.begin(), anchored.end(), path.points().begin()));
    CHECK(path.anchors() == std::vector<Vec2d>({{1,1},{3,1},{5,3}}));
    CHECK(path.removeLastAnchor());
    CHECK(same(path.points(), anchored));
    const std::array stationary {Vec2d{3,1}, Vec2d{3,1}};
    CHECK(!path.addSegment(stationary));
    CHECK(path.anchorCount() == 2);
    const std::array disconnected {Vec2d{9,9}, Vec2d{10,10}};
    CHECK(throws([&]{path.addSegment(disconnected);}));
    const std::array invalid {Vec2d{3,1}, Vec2d{4,1}, Vec2d{std::numeric_limits<double>::quiet_NaN(),2}};
    CHECK(throws([&]{path.addSegment(invalid);}));
    CHECK(throws([&]{path.addVertex({0,std::numeric_limits<double>::infinity()});}));
    CHECK(throws([&]{path.addVertex({FreehandSelectionPath::maximumCoordinate+1,0});}));
    CHECK(same(path.points(), anchored));
    CHECK(path.anchorCount() == 2);
    CHECK(path.removeLastAnchor());
    CHECK(path.removeLastAnchor());
    CHECK(path.points().empty());
    CHECK(path.anchors().empty());
    CHECK(!path.removeLastAnchor());
}
void polygonClosureEvenOddClippingAndCombinations()
{
    const Extent2u extent {32,24};
    const std::array concave {Vec2d{2,2}, Vec2d{24,2}, Vec2d{24,8}, Vec2d{10,8}, Vec2d{10,20}, Vec2d{2,20}};
    AnchoredLassoPath path;
    for (auto p : concave) path.addVertex(p);
    const auto open = raster(extent,path.points());
    CHECK(open->coverageAtDocumentPixel(5,15) == 255);
    CHECK(open->coverageAtDocumentPixel(18,15) == 0);
    CHECK(path.addVertex(concave.front()));
    CHECK(!path.addVertex(concave.front())); // A double-click closure is not two endpoints.
    CHECK(raster(extent,path.points())->equivalent(*open));
    std::vector<Vec2d> reversed(concave.rbegin(),concave.rend());
    CHECK(raster(extent,reversed)->equivalent(*open));
    const std::array bow {Vec2d{2,2},Vec2d{22,22},Vec2d{2,22},Vec2d{22,2}};
    const auto crossing = raster(extent,bow);
    CHECK(!crossing->bounds().empty()); // Signed area is zero; even-odd lobes remain valid.
    CHECK(crossing->coverageAtDocumentPixel(10,4) == 255);
    CHECK(crossing->coverageAtDocumentPixel(10,19) == 255);
    reversed.assign(bow.rbegin(),bow.rend());
    CHECK(raster(extent,reversed)->equivalent(*crossing));
    const std::array outside {Vec2d{-12,4},Vec2d{12,-8},Vec2d{40,4},Vec2d{12,32}};
    const auto clipped = raster(extent,outside);
    CHECK(clipped->coverageAtDocumentPixel(0,8) == 255);
    CHECK(clipped->coverageAtDocumentPixel(12,0) == 255);
    CHECK(clipped->coverageAtDocumentPixel(-1,8) == 0);
    const std::array line {Vec2d{2,2},Vec2d{5,5},Vec2d{5,5},Vec2d{10,10}};
    CHECK(raster(extent,line)->bounds().empty());
    const std::array tiny {Vec2d{2.25,2.25},Vec2d{2.75,2.25},Vec2d{2.75,2.75},Vec2d{2.25,2.75}};
    CHECK(raster(extent,tiny)->coverageAtDocumentPixel(2,2) == 64);
    const auto base = SelectionMask::rectangle(extent,{7,5,18,12});
    const auto originalRevision = base->revision();
    for (const auto operation : {SelectionOperation::Replace,SelectionOperation::Add,
             SelectionOperation::Subtract,SelectionOperation::Intersect}) {
        const auto result = combineSelection(base,open,operation);
        CHECK(base->revision() == originalRevision);
        CHECK(base->coverageAtDocumentPixel(8,6) == 255);
        if (operation == SelectionOperation::Add) {
            CHECK(result->coverageAtDocumentPixel(3,3) == 255);
            CHECK(result->coverageAtDocumentPixel(20,15) == 255);
        } else if (operation == SelectionOperation::Subtract) {
            CHECK(result->coverageAtDocumentPixel(8,6) == 0);
            CHECK(result->coverageAtDocumentPixel(20,15) == 255);
        } else if (operation == SelectionOperation::Intersect) {
            CHECK(result->coverageAtDocumentPixel(8,6) == 255);
            CHECK(result->coverageAtDocumentPixel(3,3) == 0);
        }
    }
}
void manualFallbacksPreserveExactGuide()
{
    const std::array guide {Vec2d{16.25,10.75},Vec2d{18.5,30.125},Vec2d{15.25,60.75}};
    const auto exercise = [&](MagneticEdgeCache::Sample sampler) {
        MagneticEdgeCache cache({96,80},std::move(sampler));
        LiveWireSearch search(cache,guide,8);
        complete(search,37);
        CHECK(search.result() == LiveWireSearch::Result::ManualFallback);
        CHECK(same(search.path(),guide));
        assertPathContract(search,guide,8);
    };
    exercise([](int,int){return Rgba8{120,120,120,255};});
    exercise([](int x,int y){return Rgba8{std::uint8_t(x*17),std::uint8_t(y*37),std::uint8_t((x+y)*11),0};});
    exercise([](int x,int){const auto v=std::uint8_t(x<16?127:128);return Rgba8{v,v,v,255};});
    MagneticEdgeCache cache({96,80},[](int x,int){return x<48?Rgba8{0,0,0,255}:Rgba8{255,255,255,255};});
    const std::array outside {Vec2d{-5.25,20.5},Vec2d{20,30},Vec2d{50.25,50.5}};
    LiveWireSearch search(cache,outside,8);
    complete(search);
    CHECK(search.result() == LiveWireSearch::Result::ManualFallback);
    CHECK(same(search.path(),outside)); // Outside construction coordinates are never clamped.
}
void alphaAndChromaticEdgesAreVisible()
{
    const auto exercise = [](MagneticEdgeCache::Sample sampler) {
        MagneticEdgeCache cache({128,112},std::move(sampler));
        const auto center = cache.feature(64,56);
        const auto flat = cache.feature(20,56);
        CHECK(std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.strength));
        CHECK(center.strength > flat.strength + 0.01F);
        const std::array guide {Vec2d{61.25,12.75},Vec2d{61.75,96.25}};
        LiveWireSearch search(cache,guide,8);
        complete(search);
        CHECK(search.result() == LiveWireSearch::Result::Edge);
        assertPathContract(search,guide,8);
        unsigned inner=0,near=0;
        for (const auto p : samplePolyline(search.path())) if (p.y>=28 && p.y<=80) {++inner;near+=std::abs(p.x-64)<=2;}
        CHECK(inner > 10);
        CHECK(near * 10 >= inner * 9);
    };
    exercise([](int x,int){return x<64?Rgba8{0,0,0,255}:Rgba8{0,0,0,0};});
    // Red and green chosen to have nearly equal linear-light luminance. A
    // luminance-only detector would miss this high-contrast chromatic edge.
    exercise([](int x,int){return x<64?Rgba8{255,0,0,255}:Rgba8{0,148,0,255};});
    exercise([](int x,int){return x<64?Rgba8{255,255,255,255}:Rgba8{0,0,0,255};});
}
void noisyEdgesCompetingBoundariesAndBudgets()
{
    auto sample=[](int x,int y) {
        const auto hash=std::uint32_t(x)*1664525U+std::uint32_t(y)*1013904223U;
        const auto noise=int((hash^(hash>>13U))%11U)-5;
        // Intended weaker edge at64, much stronger unrelated edge at90.
        const auto value=std::uint8_t(std::clamp((x<64?55:x<90?135:255)+noise,0,255));
        return Rgba8{value,value,value,255};
    };
    MagneticEdgeCache cache({144,128},sample);
    const std::array guide {Vec2d{66.25,12.75},Vec2d{65.25,62.5},Vec2d{66.75,112.25}};
    LiveWireSearch tinyBudget(cache,guide,8);
    complete(tinyBudget,1);
    LiveWireSearch largeBudget(cache,guide,8);
    complete(largeBudget,65536);
    CHECK(tinyBudget.result() == LiveWireSearch::Result::Edge);
    CHECK(largeBudget.result() == tinyBudget.result());
    CHECK(same(tinyBudget.path(),largeBudget.path()));
    assertPathContract(tinyBudget,guide,8);
    unsigned inner=0,near=0;
    for (const auto p:samplePolyline(tinyBudget.path())) if (p.y>=30&&p.y<=95) {++inner;near+=std::abs(p.x-64)<=2;CHECK(p.x<80);}
    CHECK(inner>10);
    CHECK(near*10>=inner*8);
    // Deliver the same piecewise-linear intended trace sparsely or densely;
    // additional collinear input events must not change the resolved path.
    std::vector<Vec2d> dense;
    for(std::size_t i=1;i<guide.size();++i)
        for(unsigned k=0;k<32;++k) dense.push_back(guide[i-1]+(guide[i]-guide[i-1])*(double(k)/32));
    dense.push_back(guide.back());
    LiveWireSearch denseSearch(cache,dense,8);
    complete(denseSearch,97);
    CHECK(same(denseSearch.path(),tinyBudget.path()));
    // Viewport transforms never enter the document-space search interface.
    for (double zoom:{0.125,0.5,1.0,2.0,4.0}) {
        std::vector<Vec2d> unprojected;
        for (auto p:guide) unprojected.push_back({(p.x*zoom+32-32)/zoom,(p.y*zoom+64-64)/zoom});
        LiveWireSearch zoomed(cache,unprojected,8);
        complete(zoomed,113);
        CHECK(same(zoomed.path(),tinyBudget.path()));
    }
}
void corneredGuideAndAnchoredClosure()
{
    MagneticEdgeCache cache({128,128},[](int x,int y){return x>=32&&x<96&&y>=32&&y<96?Rgba8{230,230,230,255}:Rgba8{10,10,10,255};});
    const std::array guide {Vec2d{34.25,48.5},Vec2d{34,34},Vec2d{64,34},Vec2d{94,34},Vec2d{93.75,80.5}};
    LiveWireSearch first(cache,guide,7);
    complete(first,127);
    CHECK(first.result()==LiveWireSearch::Result::Edge);
    assertPathContract(first,guide,7);
    AnchoredLassoPath anchored;
    anchored.addVertex(first.path().front());
    anchored.addSegment(first.path());
    const std::vector<Vec2d> stable(anchored.points().begin(),anchored.points().end());
    const std::array closureGuide {guide.back(),Vec2d{94,94},Vec2d{34,94},guide.front()};
    LiveWireSearch closure(cache,closureGuide,7);
    complete(closure,193);
    assertPathContract(closure,closureGuide,7);
    anchored.addSegment(closure.path());
    CHECK(anchored.points().front()==anchored.points().back());
    CHECK(std::equal(stable.begin(),stable.end(),anchored.points().begin()));
    CHECK(!raster({128,128},anchored.points())->bounds().empty());
    CHECK(anchored.removeLastAnchor());
    CHECK(same(anchored.points(),stable));
}
void boundedCacheAndHighResolutionSearch()
{
    for (const Extent2u extent:{Extent2u{3840,2160},Extent2u{5120,2880}}) {
        std::size_t samples=0;
        const int center=int(extent.width/2);
        MagneticEdgeCache cache(extent,[&](int x,int y){
            CHECK(x>=0&&y>=0&&x<int(extent.width)&&y<int(extent.height));
            ++samples;return x<center?Rgba8{0,0,0,255}:Rgba8{255,255,255,255};
        });
        const std::array guide {Vec2d{center-2.25,1000.5},Vec2d{center-1.25,1250.5}};
        LiveWireSearch search(cache,guide,12);
        complete(search,512);
        CHECK(search.result()==LiveWireSearch::Result::Edge);
        assertPathContract(search,guide,12);
        CHECK(samples<100000); // A local search must not sample a 4K/5K frame.
        const auto firstSamples=samples;
        LiveWireSearch repeated(cache,guide,12);
        complete(repeated,8192);
        CHECK(same(search.path(),repeated.path()));
        CHECK(samples==firstSamples);
        CHECK(cache.stats().samples==samples);
        CHECK(cache.memoryBytes()<16U*1024U*1024U);
        const auto feature=cache.feature(center,1100);
        const auto sampled=samples;
        for (unsigned repeat=0;repeat<100;++repeat) {
            const auto again=cache.feature(center,1100);
            CHECK(feature.x==again.x&&feature.y==again.y&&feature.strength==again.strength);
        }
        CHECK(samples==sampled);
    }
    // Explore scattered regions on a maximum-sized document. Cache footprint
    // must be independent of the number of document pixels visited historically.
    MagneticEdgeCache scattered({32768,32768},[](int x,int y){const auto v=std::uint8_t((x+y)&255);return Rgba8{v,v,v,255};});
    for (int y=128;y<32768;y+=2048)
        for (int x=128;x<32768;x+=1024) scattered.feature(x,y);
    CHECK(scattered.memoryBytes()<16U*1024U*1024U);
    CHECK(scattered.stats().evictions>0);
}
void resourceLimitsDoNotPublishPartialPaths()
{
    MagneticEdgeCache cache({5120,2880},[](int x,int){return x<2560?Rgba8{0,0,0,255}:Rgba8{255,255,255,255};});
    const std::array broad {Vec2d{100,100},Vec2d{4900,2700}};
    LiveWireSearch search(cache,broad,64);
    complete(search);
    CHECK(search.result()==LiveWireSearch::Result::ManualFallback);
    CHECK(same(search.path(),broad));
    CHECK(search.stats().expanded<=LiveWireSearch::maximumNodes);
    CHECK(search.memoryBytes()<64U*1024U*1024U);
    std::vector<Vec2d> tooMany;
    for (std::size_t i=0;i<=LiveWireSearch::maximumGuidePoints;++i)
        tooMany.push_back({100.0+double(i)*0.125,100.0+double(i%3)});
    // Invalid guide/radius requests fail explicitly; oversized graph work uses
    // the exact manual guide instead of an arbitrary truncated edge path.
    CHECK(throws([&]{LiveWireSearch crowded(cache,tooMany,8);}));
    CHECK(throws([&]{LiveWireSearch invalid(cache,broad,0);}));
    CHECK(throws([&]{LiveWireSearch invalid(cache,broad,-1);}));
    CHECK(throws([&]{LiveWireSearch invalid(cache,broad,std::numeric_limits<double>::quiet_NaN());}));
    const std::array nonfinite{Vec2d{1,2},Vec2d{std::numeric_limits<double>::infinity(),3}};
    CHECK(throws([&]{LiveWireSearch invalid(cache,nonfinite,8);}));
    for (std::size_t count:{std::size_t(1),std::size_t(5)}) {
        const std::vector<Vec2d> stationary(count,Vec2d{100.25,100.75});
        LiveWireSearch dot(cache,stationary,8);
        complete(dot);
        CHECK(dot.result()==LiveWireSearch::Result::ManualFallback);
        CHECK(!dot.path().empty());
        for(auto p:dot.path()) CHECK(p==stationary.front());
    }
    AnchoredLassoPath anchored;
    anchored.addVertex({0,0});
    std::vector<Vec2d> longSegment(FreehandSelectionPath::maximumPoints+1);
    for (std::size_t i=0;i<longSegment.size();++i) longSegment[i]={double(i),0};
    CHECK(throws([&]{anchored.addSegment(longSegment);}));
    CHECK(anchored.anchorCount()==1);
    CHECK(anchored.points().size()==1);
}

class ReferenceSurface final : public RasterSurface {
public:
    explicit ReferenceSurface(Extent2u extent, Rgba8 fill):extent_(extent),fill_(fill){}
    SurfaceId id() const noexcept override { return 987654321; }
    Extent2u extent() const noexcept override { return extent_; }
    Revision revision() const noexcept override { return revision_; }
    DirtySet dirtySince(Revision previous) const override
    {return {revision_,previous!=revision_,{}};}
    void copyRgba8(RectI region,std::span<std::byte> output,std::size_t stride) const override
    {
        ++calls;
        CHECK(region.width<=2&&region.height<=2);
        texels+=std::size_t(region.width)*std::size_t(region.height);
        for(int y=0;y<region.height;++y) for(int x=0;x<region.width;++x) {
            const auto offset=std::size_t(y)*stride+std::size_t(x)*4;
            output[offset]=std::byte(fill_.red);output[offset+1]=std::byte(fill_.green);
            output[offset+2]=std::byte(fill_.blue);output[offset+3]=std::byte(fill_.alpha);
        }
    }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch>) override {++revision_;return {revision_,false,{}};}
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch>) override {++revision_;return {revision_,false,{}};}
    mutable std::size_t calls{},texels{};
private:
    Extent2u extent_;
    Rgba8 fill_;
    Revision revision_{1};
};
void pinnedReferenceUsesDocumentContentAndDetectsChanges()
{
    const Extent2u extent{96,80};
    Document doc({extent,96});
    auto bottom=Layer::raster("Bottom",std::make_shared<ContiguousRasterSurface>(extent,Rgba8{180,70,25,192}));
    const auto bottomId=bottom.id;
    CHECK(doc.insertLayer(0,std::move(bottom)));
    auto surface=std::make_shared<ContiguousRasterSurface>(Extent2u{24,20},Rgba8{20,190,230,128});
    auto top=Layer::raster("Transformed",surface);
    const auto topId=top.id;
    // Rotation, nonuniform signed scale and translation, with an invertible
    // basis. Includes fractional texel filtering and document clipping.
    top.localToDocument={-1.2,-0.7,63.5,-0.6,1.4,24.25};
    top.opacity=0.375F;
    CHECK(doc.insertLayer(1,std::move(top)));
    auto hidden=Layer::raster("Hidden",std::make_shared<ContiguousRasterSurface>(extent,Rgba8{255,0,255,255}));
    hidden.visible=false;
    CHECK(doc.insertLayer(2,std::move(hidden)));
    PinnedDocumentSampler merged(doc,topId,ColorSampleSource::MergedVisible);
    PinnedDocumentSampler active(doc,topId,ColorSampleSource::ActiveLayer);
    CHECK(merged.matches(doc)&&active.matches(doc));
    CHECK(merged.extent()==extent);
    for(int y=0;y<int(extent.height);y+=7) for(int x=0;x<int(extent.width);x+=5) {
        const Vec2d point{x+0.125,y+0.875};
        CHECK(merged.sample(point)==sampleDocumentColor(doc,topId,point,ColorSampleSource::MergedVisible).color);
        CHECK(active.sample(point)==sampleDocumentColor(doc,topId,point,ColorSampleSource::ActiveLayer).color);
    }
    CHECK(merged.sample({-0.01,4})==Rgba8{});
    CHECK(merged.sample({96,4})==Rgba8{});
    CHECK(merged.sample({4,80})==Rgba8{});
    CHECK(merged.sample({std::numeric_limits<double>::quiet_NaN(),4})==Rgba8{});
    Document other({extent,96});
    CHECK(!merged.matches(other));
    const auto frozen=merged.sample({40,30});
    CHECK(doc.setLayerOpacity(bottomId,0.25F));
    CHECK(!merged.matches(doc));
    CHECK(merged.sample({40,30})==frozen); // Geometry/opacity metadata is frozen, not silently refreshed.
    PinnedDocumentSampler afterOpacity(doc,topId,ColorSampleSource::MergedVisible);
    CHECK(afterOpacity.matches(doc));
    CHECK(doc.setLayerVisibility(topId,false));
    CHECK(!afterOpacity.matches(doc));
    PinnedDocumentSampler hiddenActive(doc,topId,ColorSampleSource::ActiveLayer);
    CHECK(hiddenActive.sample({40,30})==sampleDocumentColor(doc,topId,{40,30},ColorSampleSource::ActiveLayer).color);
    CHECK(doc.setLayerTransform(topId,{1,0,10,0,-1,40}));
    CHECK(!hiddenActive.matches(doc));
    PinnedDocumentSampler beforeDelete(doc,topId,ColorSampleSource::ActiveLayer);
    CHECK(doc.takeLayer(topId).has_value());
    CHECK(!beforeDelete.matches(doc));
    CHECK(beforeDelete.sample({15,30}).alpha==128); // Retained source lifetime, frozen transform.
    const std::array changed{std::byte{250},std::byte{20},std::byte{20},std::byte{255}};
    surface->replaceRgba8({20,19,1,1},changed,4);
    CHECK(throws([&]{(void)beforeDelete.sample({15,30});})); // Even an uncached/unmodified location rejects the revision.

    Document transparent({{8,8},96});
    auto hiddenRgb=Layer::raster("Hidden RGB",std::make_shared<ContiguousRasterSurface>(Extent2u{8,8},Rgba8{255,127,31,0}));
    const auto hiddenRgbId=hiddenRgb.id;
    CHECK(transparent.insertLayer(0,std::move(hiddenRgb)));
    PinnedDocumentSampler zero(transparent,hiddenRgbId,ColorSampleSource::ActiveLayer);
    CHECK(zero.sample({3,3})==Rgba8{});
    CHECK(throws([&]{PinnedDocumentSampler absent(transparent,LayerId(999999),ColorSampleSource::ActiveLayer);}));

    Document huge({{5120,2880},96});
    auto counted=std::make_shared<ReferenceSurface>(Extent2u{5120,2880},Rgba8{10,30,120,255});
    auto layer=Layer::raster("Synthetic 5K",counted);
    const auto countedId=layer.id;
    CHECK(huge.insertLayer(0,std::move(layer)));
    PinnedDocumentSampler bounded(huge,countedId,ColorSampleSource::MergedVisible);
    CHECK(counted->calls==0); // Pinning does not copy or sample a whole image.
    CHECK(bounded.sample({1000.25,1200.75})==Rgba8({10,30,120,255}));
    CHECK(counted->calls==1&&counted->texels<=4);
    counted->replaceRgba8Batch({});
    CHECK(!bounded.matches(huge));
    const auto previousCalls=counted->calls;
    CHECK(throws([&]{(void)bounded.sample({2000.25,1600.75});}));
    CHECK(counted->calls==previousCalls); // Reject stale pixels before any new sampling.
}
}

int main()
{
    anchorsAreEditableConstructionNotHistory();
    stableSegmentsAndMalformedVertices();
    polygonClosureEvenOddClippingAndCombinations();
    manualFallbacksPreserveExactGuide();
    alphaAndChromaticEdgesAreVisible();
    noisyEdgesCompetingBoundariesAndBudgets();
    corneredGuideAndAnchoredClosure();
    boundedCacheAndHighResolutionSearch();
    resourceLimitsDoNotPublishPartialPaths();
    pinnedReferenceUsesDocumentContentAndDetectsChanges();
    std::cout << "Lasso modes: " << failures << " failures\n";
    return failures ? 1 : 0;
}
