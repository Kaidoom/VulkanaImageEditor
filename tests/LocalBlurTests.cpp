#include "imageeditor/core/LocalBlurStroke.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

using namespace imageeditor::core;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)
std::vector<std::byte> bytes(const RasterSurface& source)
{
    const auto e = source.extent(); std::vector<std::byte> result(std::size_t(e.width) * e.height * 4);
    source.copyRgba8({0,0,int(e.width),int(e.height)},result,std::size_t(e.width)*4); return result;
}
Rgba8 pixel(const RasterSurface& source, int x, int y)
{
    std::array<std::byte,4> data; source.copyRgba8({x,y,1,1},data,4);
    return {std::to_integer<std::uint8_t>(data[0]),std::to_integer<std::uint8_t>(data[1]),
        std::to_integer<std::uint8_t>(data[2]),std::to_integer<std::uint8_t>(data[3])};
}
std::shared_ptr<ContiguousRasterSurface> pattern(Extent2u size, bool variedAlpha = false)
{
    std::vector<std::byte> pixels(std::size_t(size.width)*size.height*4);
    for (unsigned y=0;y<size.height;++y) for(unsigned x=0;x<size.width;++x) {
        const auto p=(std::size_t(y)*size.width+x)*4;
        pixels[p]=std::byte((x*37+y*19)%256);pixels[p+1]=std::byte((x*23+y*7)%256);
        pixels[p+2]=std::byte((x*11+y*47)%256);pixels[p+3]=std::byte(variedAlpha?(x*13+y*19)%256:255);
    }
    return std::make_shared<ContiguousRasterSurface>(size,std::move(pixels));
}
LayerId add(Document& doc, const std::shared_ptr<RasterSurface>& surface)
{
    auto layer=Layer::raster("Blur fixture",surface);const auto id=layer.id;
    CHECK(doc.insertLayer(doc.layers().size(),std::move(layer)));return id;
}
BrushSettings brush(double size=20)
{
    auto b=proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    b.sizePixels=size;b.hardness=1;b.flow=1;b.opacity=.13;b.pressureToFlow=false;b.pressureToSize=false;
    b.spacingPercent=10;b.foreground={13,77,233,17};return b;
}
NormalizedPointerSample sample(Vec2d p,std::uint64_t t=0,double pressure=1)
{
    return {.documentPosition=p,.timestampMicroseconds=t,.pressure=pressure,
        .pointerType=PointerType::Pen,.buttons=PointerButtonPrimary};
}
std::vector<double> independentKernel(double radius)
{
    const int support=int(std::ceil(radius)); std::vector<double> kernel(std::size_t(2*support+1));
    const double scale=radius/3*std::sqrt(2.0);double sum=0;
    for(int x=-support;x<=support;++x) {
        const auto a=std::max(-radius,x-.5),b=std::min(radius,x+.5);
        const auto w=b>a?std::erf(b/scale)-std::erf(a/scale):0;
        kernel[std::size_t(x+support)]=w;sum+=w;
    }
    for(auto& weight:kernel)weight/=sum;
    return kernel;
}
PremultipliedColor reference(const Layer& source,Vec2d point,double radius)
{
    const auto weights=independentKernel(radius);const int support=int(weights.size()/2);
    PreparedLayerSampler sampler(source,false);
    const auto at=[&](int x,int y) {
        std::array<double,4> sum{};
        for(int dy=-support;dy<=support;++dy) for(int dx=-support;dx<=support;++dx) {
            const auto p=sampler.sample({x-dx+.5,y-dy+.5});
            const auto w=weights[std::size_t(dx+support)]*weights[std::size_t(dy+support)];
            for(std::size_t c=0;c<4;++c)sum[c]+=p[c]*w;
        }
        return sum;
    };
    const int x=int(std::floor(point.x-.5)),y=int(std::floor(point.y-.5));
    const double fx=point.x-.5-x,fy=point.y-.5-y;PremultipliedColor value{},baseline{};
    for(int j=0;j<2;++j)for(int i=0;i<2;++i){const auto p=at(x+i,y+j);const auto w=(i?fx:1-fx)*(j?fy:1-fy);
        const auto original=sampler.sample({x+i+.5,y+j+.5});
        for(std::size_t c=0;c<4;++c){value[c]+=float(p[c]*w);baseline[c]+=float(original[c]*w);}}
    const auto original=sampler.sample(point);if(value[3]<=0||baseline[3]<=0||original[3]<=0)return {};
    for(std::size_t c=0;c<3;++c)value[c]=float(std::clamp(double(original[c])/original[3]
        +double(value[c])/value[3]-double(baseline[c])/baseline[3],0.0,1.0)*original[3]);
    value[3]=original[3];
    return value;
}
Rgba8 expected(Rgba8 original,PremultipliedColor filtered,double coverage)
{
    if(original.alpha==0||filtered[3]<=0)return original;
    const auto channel=[&](std::uint8_t before,std::size_t c){const auto linear=srgbToLinear(before);
        return linearToSrgb(linear+(double(filtered[c])/filtered[3]-linear)*coverage);};
    return {channel(original.red,0),channel(original.green,1),channel(original.blue,2),original.alpha};
}
void near(Rgba8 a,Rgba8 b,int tolerance=1)
{
    CHECK(std::abs(int(a.red)-int(b.red))<=tolerance);CHECK(std::abs(int(a.green)-int(b.green))<=tolerance);
    CHECK(std::abs(int(a.blue)-int(b.blue))<=tolerance);CHECK(a.alpha==b.alpha);
}
void referenceAlphaAndSelection()
{
    for(const int selection:{255,128}) {
        Document doc({{64,64}});auto pixels=pattern({64,64},true);const auto id=add(doc,pixels);
        auto frozen=*doc.layer(id);frozen.payload=RasterLayer{std::make_shared<ContiguousRasterSurface>(Extent2u{64,64},bytes(*pixels))};
        std::vector<std::uint8_t> mask(64*64,0);mask[31*64+31]=std::uint8_t(selection);
        CHECK(doc.setSelection(SelectionMask::fromR8({64,64},mask,64)));
        const auto before=bytes(*pixels);auto settings=brush();settings.flow=.8;settings.pressureToFlow=true;
        const double pressure=selection==128?.5:1;
        History history;LocalBlurStroke stroke(doc,id,settings,{.75,3.5});
        CHECK(stroke.begin(sample({31.5,31.5},0,pressure)));CHECK(stroke.end(sample({31.5,31.5},0,pressure),history)==RasterEditCommitResult::Committed);
        // Transaction selection coverage is applied once after brush influence.
        const auto original=pixel(*std::get<RasterLayer>(frozen.payload).surface,31,31);
        near(pixel(*pixels,31,31),expected(original,reference(frozen,{31.5,31.5},3.5),.75*.8*pressure*selection/255.0),2);
        const auto after=bytes(*pixels);
        for(std::size_t p=0;p<before.size();p+=4){CHECK(after[p+3]==before[p+3]);if(p!=(31U*64+31)*4)CHECK(std::equal(before.begin()+std::ptrdiff_t(p),before.begin()+std::ptrdiff_t(p+4),after.begin()+std::ptrdiff_t(p)));}
        CHECK(history.undoDepth()==1);CHECK(history.undo(doc));CHECK(bytes(*pixels)==before);CHECK(history.redo(doc));CHECK(bytes(*pixels)==after);
    }
    // Bright hidden RGB must never tint a valid low-alpha neighborhood.
    Document doc({{32,32}});auto source=std::make_shared<ContiguousRasterSurface>(Extent2u{32,32},Rgba8{255,0,0,0});
    std::array<std::byte,4> blue{std::byte{0},std::byte{0},std::byte{255},std::byte{80}};
    source->replaceRgba8({16,16,1,1},blue,4);const auto id=add(doc,source);const auto before=bytes(*source);
    History history;LocalBlurStroke stroke(doc,id,brush(),{1,8});CHECK(stroke.begin(sample({16.5,16.5})));
    CHECK(stroke.end(sample({16.5,16.5}),history)==RasterEditCommitResult::NoChanges);CHECK(bytes(*source)==before);
    CHECK(stroke.stats().uploadedRegionBytes==0 && history.undoDepth()==0);
}
void transformedAndCrop()
{
    for(const AffineTransform transform: {AffineTransform{},AffineTransform{2,0,4,0,.6,10},
            AffineTransform{-1.3,.4,95,.25,1.2,10},AffineTransform{.7,-.7,50,.7,.7,2}}) {
        Document doc({{128,128}});auto source=pattern({48,48});const auto id=add(doc,source);
        CHECK(doc.layer(id)->localToDocument==transform || doc.setLayerTransform(id,transform));
        auto adjustments=std::make_shared<AdjustmentStack>();
        adjustments->items[1].enabled=true;
        std::get<BrightnessContrastParameters>(adjustments->items[1].parameters)={.7,.2};
        CHECK(doc.setLayerAdjustments(id,adjustments));CHECK(doc.setLayerOpacity(id,.35F));
        auto frozen=*doc.layer(id);frozen.payload=RasterLayer{std::make_shared<ContiguousRasterSurface>(Extent2u{48,48},bytes(*source))};
        const auto p=transform.map({23.5,23.5});const auto original=pixel(*source,23,23);
        History history;LocalBlurStroke stroke(doc,id,brush(8),{1,4});CHECK(stroke.begin(sample(p)));
        CHECK(stroke.end(sample(p),history)==RasterEditCommitResult::Committed);
        near(pixel(*source,23,23),expected(original,reference(frozen,p,4),1),1);
        CHECK(doc.layer(id)->localToDocument==transform);
        CHECK(doc.layer(id)->adjustments==adjustments && doc.layer(id)->opacity==.35F);
        const auto baseline=bytes(*source);LocalBlurStroke tiny(doc,id,brush(8),{1,.25});
        CHECK(tiny.begin(sample(p)));CHECK(tiny.end(sample(p),history)==RasterEditCommitResult::NoChanges);
        CHECK(bytes(*source)==baseline && tiny.stats().uploadedRegionBytes==0);
    }
    Document doc({{48,48}});auto source=pattern({48,48});const auto id=add(doc,source);
    CHECK(doc.setLayerCrop(id,LayerCrop{12,12,24,24}));const auto before=bytes(*source);
    History history;LocalBlurStroke stroke(doc,id,brush(40),{1,8});CHECK(stroke.begin(sample({12.5,12.5})));
    CHECK(stroke.end(sample({12.5,12.5}),history)==RasterEditCommitResult::Committed);
    const auto after=bytes(*source);for(int y=0;y<48;++y)for(int x=0;x<48;++x)
        if(x<12||y<12||x>=36||y>=36){const auto p=std::size_t(y*48+x)*4;CHECK(std::equal(before.begin()+std::ptrdiff_t(p),before.begin()+std::ptrdiff_t(p+4),after.begin()+std::ptrdiff_t(p)));}
}
std::vector<std::byte> eventRate(int divisions,bool duplicates)
{
    Document doc({{384,96}});auto source=pattern({384,96});const auto id=add(doc,source);History history;
    auto settings=brush(26);settings.flow=.35;settings.hardness=.4;
    LocalBlurStroke stroke(doc,id,settings,{.72,6});CHECK(stroke.begin(sample({30.5,40.5})));
    for(int i=1;i<=divisions;++i){const auto p=sample({30.5+280.0*i/divisions,40.5},std::uint64_t(i)*100000/std::uint64_t(divisions));
        CHECK(stroke.append(p));if(duplicates)CHECK(stroke.append(p));}
    CHECK(stroke.end(sample({310.5,40.5},100000),history)==RasterEditCommitResult::Committed);
    CHECK(stroke.filterStats().cachedBytes<=LocalBlurStroke::cacheLimit);
    CHECK(stroke.filterStats().cacheHits>stroke.filterStats().filteredTiles);return bytes(*source);
}
void cancellationNoopAndCoherentLateOverlap()
{
    CHECK(eventRate(1,false)==eventRate(47,false));CHECK(eventRate(47,false)==eventRate(47,true));
    Document doc({{320,96}});auto source=pattern({320,96});const auto id=add(doc,source);doc.markSaved();
    const auto before=bytes(*source);auto frozen=*doc.layer(id);frozen.payload=RasterLayer{std::make_shared<ContiguousRasterSurface>(Extent2u{320,96},before)};
    History history;LocalBlurStroke stroke(doc,id,brush(16),{1,8});CHECK(stroke.begin(sample({125.5,48.5})));
    CHECK(stroke.append(sample({140.5,48.5},20000)));
    CHECK(stroke.append(sample({125.5,48.5},40000)));
    near(pixel(*source,129,48),expected(pixel(*std::get<RasterLayer>(frozen.payload).surface,129,48),reference(frozen,{129.5,48.5},8),1));
    CHECK(stroke.end(sample({125.5,48.5},40000),history)==RasterEditCommitResult::Committed);
    const auto first=bytes(*source);CHECK(history.undo(doc));CHECK(bytes(*source)==before);CHECK(!doc.isModified());
    const auto redo=history.redoDepth();
    for(const auto settings:{BlurSettings{0,8},BlurSettings{1,0}}){const auto rev=source->revision();LocalBlurStroke noop(doc,id,brush(),settings);
        CHECK(noop.begin(sample({125.5,48.5})));CHECK(noop.end(sample({125.5,48.5}),history)==RasterEditCommitResult::NoChanges);
        CHECK(source->revision()==rev && history.redoDepth()==redo && noop.stats().uploadedRegionBytes==0);}
    CHECK(doc.setSelection(SelectionMask::fromR8({320,96},std::vector<std::uint8_t>(320*96),320)));
    LocalBlurStroke empty(doc,id,brush(),{1,8});CHECK(empty.begin(sample({125.5,48.5})));
    CHECK(empty.end(sample({125.5,48.5}),history)==RasterEditCommitResult::NoChanges);
    CHECK(empty.filterStats().snapshotBytes==0 && empty.stats().uploadedRegionBytes==0 && history.redoDepth()==redo);
    CHECK(doc.setSelection({}));
    LocalBlurStroke cancelled(doc,id,brush(),{1,8});CHECK(cancelled.begin(sample({125.5,48.5})));cancelled.cancel();
    CHECK(bytes(*source)==before && history.redoDepth()==redo);
    LocalBlurStroke finalCancel(doc,id,brush(),{1,8});CHECK(finalCancel.begin(sample({125.5,48.5})));
    CHECK(finalCancel.end(sample({140.5,48.5}),history,[]{return true;})==RasterEditCommitResult::TargetUnavailable);
    CHECK(bytes(*source)==before && history.redoDepth()==redo);
    CHECK(history.redo(doc));CHECK(bytes(*source)==first);
    LocalBlurStroke second(doc,id,brush(),{1,8});CHECK(second.begin(sample({125.5,48.5})));
    CHECK(second.end(sample({125.5,48.5}),history)==RasterEditCommitResult::Committed);CHECK(bytes(*source)!=first);
    LocalBlurStroke invalid(doc,id,brush(),{1,65});CHECK(!invalid.begin(sample({125.5,48.5})));
    CHECK(!invalid.diagnostic().empty());
    LocalBlurStroke changed(doc,id,brush(),{1,8});const auto baseline=bytes(*source);CHECK(changed.begin(sample({125.5,48.5})));
    CHECK(doc.setLayerTransform(id,{.m02=1}));CHECK(!changed.append(sample({140.5,48.5})));CHECK(bytes(*source)==baseline);
}
void benchmark()
{
    for(auto extent:{Extent2u{3840,2160},Extent2u{5120,2880}}){Document doc({extent});auto source=pattern(extent);auto id=add(doc,source);
        History history;LocalBlurStroke stroke(doc,id,brush(100),{.7,16});auto started=std::chrono::steady_clock::now();
        CHECK(stroke.begin(sample({400.5,400.5})));const auto captured=std::chrono::steady_clock::now();
        CHECK(stroke.append(sample({1400.5,440.5},200000)));const auto painted=std::chrono::steady_clock::now();
        CHECK(stroke.end(sample({1400.5,440.5},200000),history)==RasterEditCommitResult::Committed);
        const auto ended=std::chrono::steady_clock::now();const auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
        std::cout<<extent.width<<"x"<<extent.height<<" begin_ms="<<ms(started,captured)<<" append_ms="<<ms(captured,painted)
            <<" end_ms="<<ms(painted,ended)<<" snapshot="<<stroke.filterStats().snapshotBytes<<" cache="<<stroke.filterStats().cachedBytes
            <<" scratch="<<stroke.filterStats().peakWorkingBytes<<" tiles="<<stroke.filterStats().filteredTiles<<" upload="<<stroke.stats().uploadedRegionBytes<<"\n";
    }
    std::ifstream status("/proc/self/status");std::string line;while(std::getline(status,line))if(line.starts_with("VmHWM:"))std::cout<<line<<"\n";
}
}
int main(int argc,char** argv)
{
    try {referenceAlphaAndSelection();transformedAndCrop();cancellationNoopAndCoherentLateOverlap();
        if(argc>1&&std::string_view(argv[1])=="--bench")benchmark();}
    catch(const std::exception& e){std::cerr<<e.what()<<"\n";++failures;}
    if(!failures)std::cout<<"Local Blur core tests passed\n";
    return failures?1:0;
}
