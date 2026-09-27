#include "imageeditor/core/SpatialFilters.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>

namespace {
using namespace imageeditor::core;
int failures=0;
void check(bool ok,const char* message)
{if(!ok){if(failures<30)std::cerr<<message<<'\n';++failures;}}
void near(double actual,double expected,double tolerance,const char* message)
{check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,message);}
LayerSpatialFilter gaussian(double x,double y)
{auto f=defaultSpatialFilter(SpatialFilterType::Gaussian);f.enabled=true;f.parameters=GaussianBlurParameters{x,y};return f;}
LayerSpatialFilter motion(double distance,double angle)
{auto f=defaultSpatialFilter(SpatialFilterType::Motion);f.enabled=true;f.parameters=MotionBlurParameters{distance,angle};return f;}
LayerSpatialFilter lens(double radius,std::uint32_t blades=0,double angle=0)
{auto f=defaultSpatialFilter(SpatialFilterType::Lens);f.enabled=true;f.parameters=LensBlurParameters{radius,blades,angle};return f;}
double weight(const SpatialKernel& k,int x,int y)
{
    if(x<k.bounds.x||y<k.bounds.y||x>=k.bounds.right()||y>=k.bounds.bottom())return 0;
    const auto xx=std::size_t(x-k.bounds.x),yy=std::size_t(y-k.bounds.y);
    return k.separable()?k.horizontal[xx]*k.vertical[yy]:k.weights[yy*std::size_t(k.bounds.width)+xx];
}
double sample(SpatialPlaneView p,int x,int y,std::size_t channel)
{
    if(x<p.bounds.x||y<p.bounds.y||x>=p.bounds.right()||y>=p.bounds.bottom())return 0;
    const auto stride=p.rowStride?p.rowStride:std::size_t(p.bounds.width)*p.channels;
    return p.pixels[std::size_t(y-p.bounds.y)*stride+std::size_t(x-p.bounds.x)*p.channels+channel];
}
SpatialPlane randomPlane(RectI bounds,std::uint32_t channels)
{
    SpatialPlane plane{bounds,channels,{}};
    plane.pixels.resize(std::size_t(bounds.width)*std::size_t(bounds.height)*channels);
    std::mt19937 random(123456);std::uniform_real_distribution<float> value(0,1);
    for(std::size_t i=0;i<plane.pixels.size();i+=channels) {
        const float alpha=value(random);
        for(std::uint32_t c=0;c<channels;++c)plane.pixels[i+c]=channels==4?(c==3?alpha:alpha*value(random)):alpha;
    }
    return plane;
}
std::vector<double> gaussianReference(double radius)
{
    if(radius==0)return {1};
    const int support=int(std::ceil(radius));std::vector<double> result(std::size_t(2*support+1));
    // Independent midpoint integration; shipping implementation uses erf.
    constexpr int subdivisions=2048;
    for(int x=-support;x<=support;++x) {
        const double a=std::max(-radius,x-.5),b=std::min(radius,x+.5);
        if(b<=a)continue;
        const double delta=(b-a)/subdivisions;
        for(int s=0;s<subdivisions;++s) {
            const double position=a+(s+.5)*delta;
            result[std::size_t(x+support)]+=std::exp(-4.5*position*position/(radius*radius))*delta;
        }
    }
    const auto sum=std::accumulate(result.begin(),result.end(),0.0);
    for(auto& v:result)v/=sum;
    return result;
}
void gaussianKernels()
{
    for(double radius:{0.0,.001,.49,.5,.50001,.75,1.0,1.00001,3.7,16.25,128.0,256.0}) {
        const auto kernel=makeGaussianKernel(radius),reference=gaussianReference(radius);
        check(kernel.size()==reference.size(),"Gaussian support");
        near(std::accumulate(kernel.begin(),kernel.end(),0.0),1,2e-14,"Gaussian normalized");
        for(std::size_t i=0;i<kernel.size();++i) {
            near(kernel[i],reference[i],2e-8,"Gaussian independent integrated reference");
            near(kernel[i],kernel[kernel.size()-1-i],1e-15,"Gaussian symmetry");
        }
    }
    for(double radius:{.5,1.0,1.5,8.0}) {
        const auto before=makeSpatialKernel(gaussian(radius-1e-6,radius-1e-6));
        const auto after=makeSpatialKernel(gaussian(radius+1e-6,radius+1e-6));
        for(int y=-10;y<=10;++y)for(int x=-10;x<=10;++x)
            near(weight(before,x,y),weight(after,x,y),2e-5,"Gaussian continuous fractional support");
    }
}
void motionKernels()
{
    for(double angle:{0.0,11.7,45.0,90.0,123.4,180.0,-37.0}) {
        const auto k=makeSpatialKernel(motion(9.3,angle));
        std::vector<double> reference(k.weights.size());
        constexpr int count=65536;
        const double a=angle*std::numbers::pi/180;
        for(int s=0;s<count;++s) {
            const double t=(s+.5)/count-.5,x=t*9.3*std::cos(a),y=t*9.3*std::sin(a);
            const int ix=int(std::floor(x)),iy=int(std::floor(y));
            for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
                const int xx=ix+dx-k.bounds.x,yy=iy+dy-k.bounds.y;
                if(xx>=0&&yy>=0&&xx<k.bounds.width&&yy<k.bounds.height)
                    reference[std::size_t(yy)*std::size_t(k.bounds.width)+std::size_t(xx)]
                        +=(dx?x-ix:1-x+ix)*(dy?y-iy:1-y+iy)/count;
            }
        }
        double mass=0,mx=0,my=0;
        for(int y=k.bounds.y;y<k.bounds.bottom();++y)for(int x=k.bounds.x;x<k.bounds.right();++x) {
            const double w=weight(k,x,y);mass+=w;mx+=w*x;my+=w*y;
            near(w,weight(k,-x,-y),2e-15,"Motion centered symmetry");
            near(w,reference[std::size_t(y-k.bounds.y)*std::size_t(k.bounds.width)+std::size_t(x-k.bounds.x)],2e-9,"Motion independent continuous-line integration");
        }
        near(mass,1,1e-13,"Motion normalized");near(mx,0,1e-13,"Motion no X translation");near(my,0,1e-13,"Motion no Y translation");
    }
    const auto horizontal=makeSpatialKernel(motion(8,0)),vertical=makeSpatialKernel(motion(8,90));
    for(int i=-5;i<=5;++i)near(weight(horizontal,i,0),weight(vertical,0,i),1e-13,"Motion axes have identical brightness");
    const auto tiny=makeSpatialKernel(motion(.001,31));near(weight(tiny,0,0),1,.001,"Motion tiny trail approaches identity");
}
double apertureReference(int x,int y,double radius,std::uint32_t blades,double angle)
{
    // Independent vertical integration: intersect scanline with circle/polygon,
    // then integrate overlap with this pixel. No polygon clipping/CDF reuse.
    std::vector<Vec2d> polygon;
    for(std::uint32_t i=0;i<blades;++i) {
        const double a=(angle/360+double(i)/blades)*2*std::numbers::pi;
        polygon.push_back({radius*std::cos(a),radius*std::sin(a)});
    }
    constexpr int steps=4096;double area=0;
    for(int s=0;s<steps;++s) {
        const double yy=y-.5+(s+.5)/steps;double lo=0,hi=0;
        if(blades==0) {if(std::abs(yy)>=radius)continue;hi=std::sqrt(radius*radius-yy*yy);lo=-hi;}
        else {
            lo=std::numeric_limits<double>::infinity();hi=-lo;
            for(std::size_t i=0;i<polygon.size();++i) {
                const auto p=polygon[i],q=polygon[(i+1)%polygon.size()];
                if((p.y<=yy&&q.y>yy)||(q.y<=yy&&p.y>yy)) {
                    const double xx=p.x+(yy-p.y)*(q.x-p.x)/(q.y-p.y);lo=std::min(lo,xx);hi=std::max(hi,xx);
                }
            }
        }
        area+=std::max(0.0,std::min(x+.5,hi)-std::max(x-.5,lo))/steps;
    }
    const double total=blades==0?std::numbers::pi*radius*radius:.5*blades*radius*radius*std::sin(2*std::numbers::pi/blades);
    return area/total;
}
void apertureKernels()
{
    for(std::uint32_t blades:{0U,3U,5U,6U,12U}) {
        const auto k=makeSpatialKernel(lens(3.35,blades,23));double mass=0;
        for(int y=k.bounds.y;y<k.bounds.bottom();++y)for(int x=k.bounds.x;x<k.bounds.right();++x) {
            const double w=weight(k,x,y);mass+=w;
            near(w,apertureReference(x,y,3.35,blades,23),3e-6,"Aperture true area independent scanline reference");
        }
        near(mass,1,1e-13,"Aperture normalized");
    }
    const auto disk=makeSpatialKernel(lens(8.3)),g=makeSpatialKernel(gaussian(8.3,8.3));
    near(weight(disk,0,0),weight(disk,4,0),1e-15,"Disk uniform interior (not Gaussian)");
    check(weight(g,0,0)>weight(g,4,0)*2,"Gaussian impulse distinct from aperture");
    for(int y=-9;y<=9;++y)for(int x=-9;x<=9;++x) {
        near(weight(disk,x,y),weight(disk,-x,y),2e-14,"Disk symmetric X");
        near(weight(disk,x,y),weight(disk,y,x),2e-14,"Disk symmetric axes");
    }
    const auto small=makeSpatialKernel(lens(.001));near(weight(small,0,0),1,1e-15,"Tiny aperture safe identity");
    const auto subnormal=makeSpatialKernel(lens(std::numeric_limits<double>::denorm_min()));
    near(weight(subnormal,0,0),1,0,"Subnormal aperture radius safe identity");
    const auto before=makeSpatialKernel(lens(2.49999)),after=makeSpatialKernel(lens(2.50001));
    for(int y=-4;y<=4;++y)for(int x=-4;x<=4;++x)near(weight(before,x,y),weight(after,x,y),1e-5,"Aperture fractional continuity");
}
void convolutionAndTiles()
{
    SpatialPlane constant{{0,0,39,39},4,std::vector<float>(39*39*4)};
    for(std::size_t i=0;i<constant.pixels.size();i+=4) {
        constant.pixels[i]=.12F;constant.pixels[i+1]=.36F;constant.pixels[i+2]=.6F;constant.pixels[i+3]=.8F;
    }
    for(const auto& f:{gaussian(4,4),motion(8,47),lens(4.4),lens(4.4,3,27)}) {
        const auto result=filterSpatialRegion(constant.view(),constant.bounds,f);
        check(bool(result),"Constant plane filtering");
        if(result)for(std::size_t c=0;c<4;++c)
            near(sample(result.output.view(),19,19,c),constant.pixels[c],1e-7,"Constant brightness preserved away from transparent border");
    }
    for(std::uint32_t channels:{1U,4U}) {
        auto source=randomPlane({-3,4,41,25},channels);
        for(const auto& f:{gaussian(3.3,1.7),motion(6.2,32),lens(4.4),lens(4.4,5,11)}) {
            const auto k=makeSpatialKernel(f);const auto bounds=expandedSpatialOutputBounds(f,source.bounds);
            auto output=convolveSpatialRegion(source.view(),bounds,k);check(bool(output),"Convolution succeeds");if(!output)continue;
            for(int y=bounds.y;y<bounds.bottom();++y)for(int x=bounds.x;x<bounds.right();++x)for(std::size_t c=0;c<channels;++c) {
                double expected=0;
                for(int ky=k.bounds.y;ky<k.bounds.bottom();++ky)for(int kx=k.bounds.x;kx<k.bounds.right();++kx)
                    expected+=sample(source.view(),x-kx,y-ky,c)*weight(k,kx,ky);
                near(sample(output.output.view(),x,y,c),expected,1.2e-7,"Optimized convolution equals direct double reference");
            }
        }
    }
    auto source=randomPlane({0,0,537,39},4);
    for(const auto& f:{gaussian(5.2,2.1),lens(5.2,6,17),motion(8.4,21)}) {
        auto whole=filterSpatialRegion(source.view(),source.bounds,f);check(bool(whole),"Whole tiled convolution");
        for(const RectI tile: {RectI{251,12,19,20},RectI{505,0,32,39}}) {
            const auto input=requiredSpatialInputBounds(f,tile).clippedTo(source.bounds);
            SpatialPlane patch{input,4,{}};patch.pixels.resize(std::size_t(input.width)*std::size_t(input.height)*4);
            for(int y=0;y<input.height;++y)for(int x=0;x<input.width;++x)for(std::size_t c=0;c<4;++c)
                patch.pixels[(std::size_t(y)*std::size_t(input.width)+std::size_t(x))*4+c]=float(sample(source.view(),input.x+x,input.y+y,c));
            auto part=filterSpatialRegion(patch.view(),tile,f);check(bool(part),"Context-padded tile convolution");
            if(whole&&part)for(int y=tile.y;y<tile.bottom();++y)for(int x=tile.x;x<tile.right();++x)for(std::size_t c=0;c<4;++c)
                near(sample(part.output.view(),x,y,c),sample(whole.output.view(),x,y,c),1.2e-7,"No internal tile seams");
        }
    }
}
void alphaMasksBoundsAndModel()
{
    SpatialPlane source{{0,0,9,9},4,std::vector<float>(9*9*4)};
    const auto middle=std::size_t(4*9+4)*4;source.pixels[middle]=.5F;source.pixels[middle+3]=.5F;
    auto filter=gaussian(3,3);auto spread=filterSpatialRegion(source.view(),source.bounds,filter);
    check(bool(spread),"Alpha spread succeeds");
    check(sample(spread.output.view(),3,4,3)>0,"Alpha-changing blur expands silhouette");
    for(int y=0;y<9;++y)for(int x=0;x<9;++x)
        near(sample(spread.output.view(),x,y,0),sample(spread.output.view(),x,y,3),1e-7,"No transparent red fringe contamination");
    filter.preserveAlpha=true;auto preserved=filterSpatialRegion(source.view(),source.bounds,filter);
    for(int y=0;y<9;++y)for(int x=0;x<9;++x)
        near(sample(preserved.output.view(),x,y,3),sample(source.view(),x,y,3),0,"Preserve exact destination alpha");
    near(sample(preserved.output.view(),4,4,0),.5,1e-7,"Alpha-weighted neighbor color retained");
    near(sample(preserved.output.view(),3,4,0),0,0,"Alpha-preserving blur cannot create opaque patches");
    check(expandedSpatialOutputBounds(filter,source.bounds)==source.bounds,"Preserved alpha does not expand bounds");
    check(requiredSpatialInputBounds(filter,{4,4,1,1})==RectI{1,1,7,7},"Preserved alpha still reads neighborhood");
    filter.preserveAlpha=false;
    std::vector<std::uint8_t> mask(81);mask[4*9+3]=128;
    filter.mask=AdjustmentMask{SelectionMask::fromR8({9,9},mask,9),{}};
    auto masked=filterSpatialRegion(source.view(),source.bounds,filter);
    near(sample(masked.output.view(),3,4,3),sample(spread.output.view(),3,4,3)*128/255,1e-7,"Mask restricts contribution, not source neighbors");
    near(sample(masked.output.view(),4,4,3),.5,0,"Mask hole preserves original exactly");
    auto state=std::make_shared<SpatialFilterStack>();state->items[0]=gaussian(2,3);state->items[1]=motion(4,0);state->items[2]=lens(1);
    check(expandedSpatialOutputBounds(state,RectI{0,0,10,10})==RectI{-5,-4,20,18},"Ordered support expansion sums through chain");
    check(validSpatialFilters(*state)&&hasActiveSpatialFilters(state),"Valid explicit fixed filter order");
    check(equivalentSpatialFilters({},std::make_shared<SpatialFilterStack>()),"Absent/default filters equivalent");
    auto changed=std::make_shared<SpatialFilterStack>(*state);changed->algorithmVersion++;
    check(!validSpatialFilters(*changed)&&!equivalentSpatialFilters(state,changed),"Unknown algorithm fails safely");
    changed->algorithmVersion=1;changed->items[0].parameters=GaussianBlurParameters{257,1};
    check(!validSpatialFilters(*changed),"Persistent radius limits enforced");
    for(auto type:allSpatialFilterTypes)check(spatialFilterFromIdentifier(spatialFilterIdentifier(type))==type,"Stable filter identifiers");
    check(!spatialFilterFromIdentifier("future-blur"),"Unknown filter identifier rejected");
    const auto hidden=decodeColor({1,255,64,0});check(hidden==PremultipliedColor{},"Hidden RGB discarded before filtering");
    const float tiny=std::numeric_limits<float>::denorm_min();
    const auto subnormal=finishSpatialFilterPixel({1,0,0,1},{tiny,0,0,tiny},true);
    near(subnormal[0],1,0,"Alpha preservation avoids intermediate subnormal-ratio overflow");
    const auto zero=finishSpatialFilterPixel({0,0,0,0},{0,0,0,0},true);
    check(zero==PremultipliedColor{},"Zero alpha neighborhood stays finite transparent");
    const auto bytes=quantizeSpatialPlane(preserved.output.view());check(bytes[middle+3]==std::byte(128),"Single final RGBA8 quantization");
}
void safetyAndCancellation()
{
    auto source=randomPlane({0,0,96,64},4);const auto f=gaussian(12,12);
    SpatialFilterOptions options;options.maxWorkingBytes=8;
    auto failed=filterSpatialRegion(source.view(),source.bounds,f,options);
    check(failed.status==SpatialFilterStatus::BudgetExceeded&&failed.output.pixels.empty(),"Allocation preflight never publishes partial results");
    options={};options.maxSampleOperations=1;failed=filterSpatialRegion(source.view(),source.bounds,f,options);
    check(failed.status==SpatialFilterStatus::BudgetExceeded,"Work budget preflight");
    int count=0;options={};options.cancelled=[&]{return ++count>10;};
    failed=filterSpatialRegion(source.view(),source.bounds,f,options);
    check(failed.status==SpatialFilterStatus::Cancelled&&failed.output.pixels.empty(),"Cancellation discards all partial output");
    count=0;failed=filterSpatialRegion(source.view(),source.bounds,lens(256,7,13),options);
    check(failed.status==SpatialFilterStatus::Cancelled,"Aperture kernel generation itself cancellable");
    options={};double last=-1;options.progress=[&](double progress){check(progress>=last&&progress<=1,"Monotonic progress");last=progress;};
    check(bool(filterSpatialRegion(source.view(),source.bounds,f,options)),"Progress run succeeds");near(last,1,0,"Progress reaches one");
    auto invalid=f;invalid.parameters=GaussianBlurParameters{std::numeric_limits<double>::quiet_NaN(),2};
    check(filterSpatialRegion(source.view(),source.bounds,invalid).status==SpatialFilterStatus::InvalidInput,"NaN rejected");
    check(filterSpatialRegion(source.view(),{std::numeric_limits<int>::max()-1,0,1,1},f).status==SpatialFilterStatus::InvalidInput,"Neighborhood coordinate overflow rejected");
    options={};options.pixelScale=2;const auto scaled=makeSpatialKernel(f,2),direct=makeSpatialKernel(gaussian(24,24));
    check(scaled.horizontal==direct.horizontal&&scaled.vertical==direct.vertical,"Cache density scales kernel without changing document parameters");
    auto maximum=makeSpatialKernel(gaussian(256,256),8);check(maximum.horizontal.size()==4097,"Bounded high-density maximum support");
    options={};options.cancelled=[] {return true;};bool encodingCancelled=false;
    try {(void)quantizeSpatialPlane(source.view(),options);}catch(const std::runtime_error&) {encodingCancelled=true;}
    check(encodingCancelled,"Final encoding is cancellable too");
    for(const auto& neutral:{gaussian(0,0),motion(0,19),lens(0,5,27)}) {
        const auto result=filterSpatialRegion(source.view(),source.bounds,neutral);
        check(bool(result)&&result.output.pixels==source.pixels,"Every zero parameter is exact identity");
    }
}
}
int main()
{
    gaussianKernels();motionKernels();apertureKernels();convolutionAndTiles();alphaMasksBoundsAndModel();safetyAndCancellation();
    if(failures)std::cerr<<failures<<" spatial filter failures\n";
    else std::cout<<"Spatial filter kernel/reference/alpha/mask/bounds/tile/cancellation tests passed\n";
    return failures?1:0;
}
