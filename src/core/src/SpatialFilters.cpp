#include "imageeditor/core/SpatialFilters.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace imageeditor::core {
namespace {
struct CancelledFilter {};
constexpr std::array<std::string_view,3> names {"Gaussian Blur", "Motion Blur", "Lens / Bokeh Blur"};
constexpr std::array<std::string_view,3> identifiers {"gaussian-blur", "motion-blur", "lens-blur"};
bool range(double v,double a,double b) { return std::isfinite(v)&&v>=a&&v<=b; }
bool sameMask(const std::optional<AdjustmentMask>& a,const std::optional<AdjustmentMask>& b) noexcept
{
    return a.has_value()==b.has_value() && (!a || (a->localToMask==b->localToMask
        && (a->coverage==b->coverage || (a->coverage&&b->coverage&&a->coverage->equivalent(*b->coverage)))));
}
bool sameFilter(const LayerSpatialFilter& a,const LayerSpatialFilter& b) noexcept
{ return a.type==b.type&&a.enabled==b.enabled&&a.preserveAlpha==b.preserveAlpha&&a.parameters==b.parameters&&sameMask(a.mask,b.mask); }
bool validItem(const LayerSpatialFilter& f)
{
    if(std::size_t(f.type)>=spatialFilterCount || f.parameters.index()!=std::size_t(f.type))return false;
    if(f.mask) {
        const auto& t=f.mask->localToMask;
        if(!f.mask->coverage || !std::isfinite(t.m00)||!std::isfinite(t.m01)||!std::isfinite(t.m02)
            ||!std::isfinite(t.m10)||!std::isfinite(t.m11)||!std::isfinite(t.m12)||!t.inverted())return false;
    }
    return std::visit([](const auto& p) {
        using T=std::decay_t<decltype(p)>;
        if constexpr(std::is_same_v<T,GaussianBlurParameters>)return range(p.radiusX,0,maximumSpatialRadius)&&range(p.radiusY,0,maximumSpatialRadius);
        else if constexpr(std::is_same_v<T,MotionBlurParameters>)return range(p.distance,0,maximumMotionDistance)&&range(p.angle,-360000,360000);
        else return range(p.radius,0,maximumSpatialRadius)&&(p.blades==0||(p.blades>=3&&p.blades<=12))&&range(p.rotation,-360000,360000);
    },f.parameters);
}
void requireItem(const LayerSpatialFilter& f,double scale)
{
    if(!validItem(f)||!std::isfinite(scale)||scale<=0||scale>8)
        throw std::invalid_argument("Invalid spatial filter parameters or cache scale");
}
bool validRect(RectI r)
{
    return r.width>=0&&r.height>=0&&std::int64_t(r.x)+r.width<=std::numeric_limits<std::int32_t>::max()
        &&std::int64_t(r.y)+r.height<=std::numeric_limits<std::int32_t>::max();
}
RectI grow(RectI r,int left,int top,int right,int bottom)
{
    if(!validRect(r))throw std::invalid_argument("Invalid spatial filter bounds");
    if(r.empty())return r;
    const std::int64_t x=std::int64_t(r.x)-left,y=std::int64_t(r.y)-top;
    const std::int64_t w=std::int64_t(r.width)+left+right,h=std::int64_t(r.height)+top+bottom;
    constexpr auto lo=std::numeric_limits<std::int32_t>::min(),hi=std::numeric_limits<std::int32_t>::max();
    if(x<lo||y<lo||w>hi||h>hi||x+w>hi||y+h>hi)throw std::overflow_error("Spatial filter bounds exceed coordinate limits");
    return {std::int32_t(x),std::int32_t(y),std::int32_t(w),std::int32_t(h)};
}
RectI kernelBounds(const LayerSpatialFilter& f,double scale)
{
    requireItem(f,scale);
    if(spatialFilterIsNeutral(f))return {0,0,1,1};
    int x=0,y=0;
    if(const auto* p=std::get_if<GaussianBlurParameters>(&f.parameters)) {
        x=int(std::ceil(p->radiusX*scale));y=int(std::ceil(p->radiusY*scale));
    } else if(const auto* p=std::get_if<MotionBlurParameters>(&f.parameters)) {
        const double a=std::remainder(p->angle,360.0)*std::numbers::pi/180;
        // Tent-reconstructed line has one extra sample of support. Trig axes
        // are snapped only at representational roundoff, not spatial epsilon.
        const double dx=std::abs(std::cos(a))*p->distance*scale/2;
        const double dy=std::abs(std::sin(a))*p->distance*scale/2;
        x=dx<1e-12?0:int(std::ceil(dx));y=dy<1e-12?0:int(std::ceil(dy));
    } else {
        const auto& lens=std::get<LensBlurParameters>(f.parameters);
        x=y=int(std::ceil(lens.radius*scale));
    }
    return {-x,-y,2*x+1,2*y+1};
}
void normalize(std::vector<double>& w)
{
    const double sum=std::accumulate(w.begin(),w.end(),0.0);
    if(!(sum>0)||!std::isfinite(sum))throw std::invalid_argument("Spatial kernel has no finite positive mass");
    for(auto& v:w)v/=sum;
}
// Integral over [0,x]x[0,y] of a disk, reflected by signs for a rectangle CDF.
// Circle/pixel intersection areas are analytical, not finite supersampling.
double diskPrimitive(double x,double y,double r)
{
    const double sign=(x<0?-1.0:1.0)*(y<0?-1.0:1.0);
    x=std::min(std::abs(x),r);y=std::min(std::abs(y),r);
    const double split=std::sqrt(std::max(0.0,r*r-y*y));
    auto integral=[r](double u) {
        return .5*(u*std::sqrt(std::max(0.0,r*r-u*u))+r*r*std::asin(std::clamp(u/r,-1.0,1.0)));
    };
    return sign*(y*std::min(x,split)+(x>split?integral(x)-integral(split):0));
}
double diskCell(int x,int y,double r)
{
    const double ax=std::abs(double(x)),ay=std::abs(double(y));
    if((ax+.5)*(ax+.5)+(ay+.5)*(ay+.5)<=r*r)return 1;
    if(std::pow(std::max(0.0,ax-.5),2)+std::pow(std::max(0.0,ay-.5),2)>=r*r)return 0;
    return std::clamp(diskPrimitive(x+.5,y+.5,r)-diskPrimitive(x-.5,y+.5,r)
        -diskPrimitive(x+.5,y-.5,r)+diskPrimitive(x-.5,y-.5,r),0.0,1.0);
}
double polygonCell(std::span<const Vec2d> vertices,int x,int y)
{
    // Exact convex clipping; translating into the cell keeps shoelace stable
    // even at large radii, where absolute-coordinate cancellation would grow.
    std::array<Vec2d,20> a{},b{};
    std::size_t n=vertices.size();
    for(std::size_t i=0;i<n;++i)a[i]={vertices[i].x-x,vertices[i].y-y};
    for(int edge=0;edge<4&&n;++edge) {
        const bool horizontal=edge<2;const double sign=edge%2==0?1:-1;
        auto d=[&](Vec2d p){return .5-sign*(horizontal?p.x:p.y);};
        std::size_t count=0;
        for(std::size_t i=0;i<n;++i) {
            const auto p=a[i],q=a[(i+1)%n];const double dp=d(p),dq=d(q);
            if(dp>=0)b[count++]=p;
            if((dp>=0)!=(dq>=0))b[count++]=p+(q-p)*(dp/(dp-dq));
        }
        n=count;a=b;
    }
    double area=0;
    for(std::size_t i=0;i<n;++i)area+=a[i].x*a[(i+1)%n].y-a[i].y*a[(i+1)%n].x;
    const double value=std::clamp(std::abs(area)*.5,0.0,1.0);
    // A fully covered pixel has exactly unit area; clipping arithmetic can be
    // a few ulps short. Only this numerical equality is canonicalized for runs.
    return value>1-8*std::numeric_limits<double>::epsilon()?1:value;
}
void motionKernel(SpatialKernel& k,double distance,double angle,const std::function<bool()>& cancelled)
{
    const double a=std::remainder(angle,360.0)*std::numbers::pi/180;
    Vec2d v{std::cos(a)*distance,std::sin(a)*distance};
    if(std::abs(v.x)<1e-12)v.x=0;
    if(std::abs(v.y)<1e-12)v.y=0;
    std::vector<double> cuts{-.5,.5};
    for(double component:{v.x,v.y})if(component!=0) {
        for(int grid=int(std::ceil(-std::abs(component)/2));grid<=int(std::floor(std::abs(component)/2));++grid) {
            const double t=grid/component;if(t>-.5&&t<.5)cuts.push_back(t);
        }
    }
    std::sort(cuts.begin(),cuts.end());cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
    // Along each interval, bilinear tent weights are quadratic. Two-point
    // Gauss-Legendre integrates them exactly, with no angle/sample-count bias.
    constexpr double node=0.577350269189625764509;
    for(std::size_t i=1;i<cuts.size();++i) {
        if(cancelled&&cancelled())throw CancelledFilter{};
        const double middle=(cuts[i]+cuts[i-1])*.5,half=(cuts[i]-cuts[i-1])*.5;
        for(double t:{middle-half*node,middle+half*node}) {
            const auto p=v*t;const int x=int(std::floor(p.x)),y=int(std::floor(p.y));
            const double fx=p.x-x,fy=p.y-y;
            for(int oy=0;oy<2;++oy)for(int ox=0;ox<2;++ox) {
                const int xx=x+ox-k.bounds.x,yy=y+oy-k.bounds.y;
                if(xx>=0&&yy>=0&&xx<k.bounds.width&&yy<k.bounds.height)
                    k.weights[std::size_t(yy)*std::size_t(k.bounds.width)+std::size_t(xx)]+=half*(ox?fx:1-fx)*(oy?fy:1-fy);
            }
        }
    }
}
std::size_t stride(SpatialPlaneView p) {return p.rowStride?p.rowStride:std::size_t(p.bounds.width)*p.channels;}
bool validPlane(SpatialPlaneView p)
{
    if(!validRect(p.bounds)||(p.channels!=1&&p.channels!=4))return false;
    if(p.bounds.empty())return true;
    const auto s=stride(p),row=std::size_t(p.bounds.width)*p.channels;
    return s>=row&&s<=(std::numeric_limits<std::size_t>::max()-row)/std::size_t(p.bounds.height)
        &&p.pixels.size()>=s*std::size_t(p.bounds.height-1)+row;
}
const float* pixel(SpatialPlaneView p,int x,int y)
{
    if(x<p.bounds.x||y<p.bounds.y||x>=p.bounds.right()||y>=p.bounds.bottom())return nullptr;
    return p.pixels.data()+std::size_t(y-p.bounds.y)*stride(p)+std::size_t(x-p.bounds.x)*p.channels;
}
SpatialFilterResult failure(SpatialFilterStatus status,std::string error)
{ SpatialFilterResult r;r.status=status;r.error=std::move(error);return r; }
bool stopped(const SpatialFilterOptions& options) {return options.cancelled&&options.cancelled();}
struct Run {int x0,x1,y;double weight;};
std::vector<Run> kernelRuns(const SpatialKernel& k)
{
    std::vector<Run> runs;
    for(int y=0;y<k.bounds.height;++y)for(int x=0;x<k.bounds.width;) {
        const double w=k.weights[std::size_t(y)*std::size_t(k.bounds.width)+std::size_t(x)];
        int end=x+1;
        while(end<k.bounds.width&&k.weights[std::size_t(y)*std::size_t(k.bounds.width)+std::size_t(end)]==w)++end;
        if(w>0)runs.push_back({x+k.bounds.x,end+k.bounds.x,y+k.bounds.y,w});
        x=end;
    }
    return runs;
}
}

std::string_view spatialFilterName(SpatialFilterType t) noexcept
{return std::size_t(t)<names.size()?names[std::size_t(t)]:std::string_view("Unknown");}
std::string_view spatialFilterIdentifier(SpatialFilterType t) noexcept
{return std::size_t(t)<identifiers.size()?identifiers[std::size_t(t)]:std::string_view();}
std::optional<SpatialFilterType> spatialFilterFromIdentifier(std::string_view name) noexcept
{for(std::size_t i=0;i<identifiers.size();++i)if(name==identifiers[i])return allSpatialFilterTypes[i];return {};}
LayerSpatialFilter defaultSpatialFilter(SpatialFilterType t)
{
    LayerSpatialFilter f;f.type=t;
    switch(t) {case SpatialFilterType::Gaussian:f.parameters=GaussianBlurParameters{};break;
    case SpatialFilterType::Motion:f.parameters=MotionBlurParameters{};break;
    case SpatialFilterType::Lens:f.parameters=LensBlurParameters{};break;}
    return f;
}
SpatialFilterStack::SpatialFilterStack()
{for(std::size_t i=0;i<items.size();++i)items[i]=defaultSpatialFilter(allSpatialFilterTypes[i]);}
bool validSpatialFilters(const SpatialFilterStack& stack,std::string* reason)
{
    if(stack.algorithmVersion!=spatialFilterAlgorithmVersion) {if(reason)*reason="Unsupported spatial filter algorithm version";return false;}
    for(std::size_t i=0;i<stack.items.size();++i)if(std::size_t(stack.items[i].type)!=i||!validItem(stack.items[i])) {
        if(reason)*reason="Invalid spatial filter order, parameters, or captured mask";
        return false;
    }
    return true;
}
bool equivalentSpatialFilters(const SpatialFilterState& a,const SpatialFilterState& b) noexcept
{
    if(a==b)return true;
    static const SpatialFilterStack defaults;
    const auto& aa=a?*a:defaults;const auto& bb=b?*b:defaults;
    if(aa.algorithmVersion!=bb.algorithmVersion)return false;
    for(std::size_t i=0;i<spatialFilterCount;++i)if(!sameFilter(aa.items[i],bb.items[i]))return false;
    return true;
}
bool spatialFilterIsNeutral(const LayerSpatialFilter& f) noexcept
{
    if(!f.enabled||(f.mask&&f.mask->coverage&&f.mask->coverage->bounds().empty()))return true;
    return std::visit([](const auto& p) {
        using T=std::decay_t<decltype(p)>;
        if constexpr(std::is_same_v<T,GaussianBlurParameters>)return p.radiusX==0&&p.radiusY==0;
        else if constexpr(std::is_same_v<T,MotionBlurParameters>)return p.distance==0;
        else return p.radius==0;
    },f.parameters);
}
bool hasActiveSpatialFilters(const SpatialFilterState& state) noexcept
{return state&&std::any_of(state->items.begin(),state->items.end(),[](const auto& f){return !spatialFilterIsNeutral(f);});}
std::size_t spatialFilterMemoryCost(const SpatialFilterState& a,const SpatialFilterState& b) noexcept
{
    std::size_t cost=(a?sizeof(SpatialFilterStack):0)+(b&&b!=a?sizeof(SpatialFilterStack):0),count=0;
    std::array<const SelectionMask*,spatialFilterCount*2> seen{};
    for(const auto* state:{&a,&b})if(*state)for(const auto& f:(*state)->items)if(f.mask&&f.mask->coverage) {
        const auto* mask=f.mask->coverage.get();
        if(std::find(seen.begin(),seen.begin()+std::ptrdiff_t(count),mask)==seen.begin()+std::ptrdiff_t(count)) {
            seen[count++]=mask;cost+=mask->memoryCost();
        }
    }
    return cost;
}
std::size_t spatialFilterMemoryCost(const SpatialFilterState& state) noexcept {return spatialFilterMemoryCost(state,{});}

std::vector<double> makeGaussianKernel(double radius)
{
    if(!range(radius,0,maximumSpatialRadius*8))throw std::invalid_argument("Gaussian radius exceeds supported cache range");
    if(radius==0)return {1};
    const int support=int(std::ceil(radius));
    std::vector<double> weights(std::size_t(support*2+1));
    if(radius<=.5) {weights[std::size_t(support)]=1;return weights;}
    const double scale=3/(radius*std::sqrt(2.0));
    for(int i=-support;i<=support;++i) {
        const double a=std::max(-radius,i-.5),b=std::min(radius,i+.5);
        if(b>a)weights[std::size_t(i+support)]=std::erf(b*scale)-std::erf(a*scale);
    }
    normalize(weights);return weights;
}
SpatialKernel makeSpatialKernel(const LayerSpatialFilter& f,double scale,const std::function<bool()>& cancelled)
{
    if(cancelled&&cancelled())throw CancelledFilter{};
    SpatialKernel k;k.bounds=kernelBounds(f,scale);
    if(spatialFilterIsNeutral(f)) {k.horizontal={1};k.vertical={1};return k;}
    if(const auto* p=std::get_if<GaussianBlurParameters>(&f.parameters)) {
        k.horizontal=makeGaussianKernel(p->radiusX*scale);k.vertical=makeGaussianKernel(p->radiusY*scale);return k;
    }
    k.weights.resize(std::size_t(k.bounds.width)*std::size_t(k.bounds.height));
    if(const auto* p=std::get_if<MotionBlurParameters>(&f.parameters))motionKernel(k,p->distance*scale,p->angle,cancelled);
    else {
        const auto& lens=std::get<LensBlurParameters>(f.parameters);const double r=lens.radius*scale;
        if(r<=.5) {
            k.weights[std::size_t(-k.bounds.y)*std::size_t(k.bounds.width)+std::size_t(-k.bounds.x)]=1;
            return k;
        }
        std::vector<Vec2d> vertices;
        for(std::uint32_t i=0;i<lens.blades;++i) {
            const double a=(std::remainder(lens.rotation,360.0)/360+double(i)/lens.blades)*2*std::numbers::pi;
            vertices.push_back({r*std::cos(a),r*std::sin(a)});
        }
        for(int y=0;y<k.bounds.height;++y) {
            if(cancelled&&cancelled())throw CancelledFilter{};
            for(int x=0;x<k.bounds.width;++x)
                k.weights[std::size_t(y)*std::size_t(k.bounds.width)+std::size_t(x)]=lens.blades
                    ?polygonCell(vertices,x+k.bounds.x,y+k.bounds.y):diskCell(x+k.bounds.x,y+k.bounds.y,r);
        }
    }
    normalize(k.weights);return k;
}
RectI requiredSpatialInputBounds(const LayerSpatialFilter& f,RectI output,double scale)
{
    const auto k=kernelBounds(f,scale);
    return grow(output,k.right()-1,k.bottom()-1,-k.x,-k.y);
}
RectI expandedSpatialOutputBounds(const LayerSpatialFilter& f,RectI input,double scale)
{
    const auto k=kernelBounds(f,scale);
    if(!validRect(input))throw std::invalid_argument("Invalid spatial filter bounds");
    if(f.preserveAlpha)return input;
    return grow(input,-k.x,-k.y,k.right()-1,k.bottom()-1);
}
RectI requiredSpatialInputBounds(const SpatialFilterState& state,RectI output,double scale)
{if(state)for(auto i=state->items.rbegin();i!=state->items.rend();++i)output=requiredSpatialInputBounds(*i,output,scale);return output;}
RectI expandedSpatialOutputBounds(const SpatialFilterState& state,RectI input,double scale)
{if(state)for(const auto& f:state->items)input=expandedSpatialOutputBounds(f,input,scale);return input;}

SpatialFilterResult convolveSpatialRegion(SpatialPlaneView source,RectI output,const SpatialKernel& kernel,const SpatialFilterOptions& options)
{
    if(!validPlane(source)||!validRect(output)||!validRect(kernel.bounds)||kernel.bounds.empty()
        ||kernel.bounds.width>4099||kernel.bounds.height>4099||kernel.bounds.x < -4099||kernel.bounds.y < -4099
        ||kernel.bounds.right()>4100||kernel.bounds.bottom()>4100)
        return failure(SpatialFilterStatus::InvalidInput,"Invalid spatial plane or kernel bounds");
    if(stopped(options))return failure(SpatialFilterStatus::Cancelled,"Spatial filtering cancelled");
    try { (void)grow(output,kernel.bounds.right()-1,kernel.bounds.bottom()-1,-kernel.bounds.x,-kernel.bounds.y); }
    catch(const std::exception&) {return failure(SpatialFilterStatus::InvalidInput,"Spatial filter neighborhood exceeds coordinate limits");}
    const auto n=std::size_t(output.width)*std::size_t(output.height)*source.channels;
    if(n>options.maxWorkingBytes/sizeof(float))return failure(SpatialFilterStatus::BudgetExceeded,"Filtered output exceeds memory budget");
    const bool separate=kernel.separable();
    auto validWeights=[](const auto& w) {return !w.empty()&&std::all_of(w.begin(),w.end(),[](double x){return std::isfinite(x)&&x>=0;})
        &&std::abs(std::accumulate(w.begin(),w.end(),0.0)-1)<1e-9;};
    if((separate&&(kernel.horizontal.size()!=std::size_t(kernel.bounds.width)||kernel.vertical.size()!=std::size_t(kernel.bounds.height)
            ||!validWeights(kernel.horizontal)||!validWeights(kernel.vertical)))
        ||(!separate&&(kernel.weights.size()!=std::size_t(kernel.bounds.width)*std::size_t(kernel.bounds.height)||!validWeights(kernel.weights))))
        return failure(SpatialFilterStatus::InvalidInput,"Invalid or unnormalized spatial kernel");
    try {
        const auto runs=separate?std::vector<Run>{}:kernelRuns(kernel);
        const auto tiles=(std::uint64_t(output.width)+255)/256,stripes=(std::uint64_t(output.height)+15)/16;
        // Include startup halos and prefix construction, not just emitted
        // pixels: a tiny requested tile with a huge radius still has real cost.
        const long double estimate=separate
            ?static_cast<long double>(output.width)*source.channels
                *(static_cast<long double>(output.height)+kernel.bounds.height-1)*kernel.bounds.width
                +static_cast<long double>(n)*kernel.bounds.height
            :static_cast<long double>(n)*runs.size()*2
                +(static_cast<long double>(output.width)+tiles*std::uint64_t(kernel.bounds.width-1))*source.channels
                    *(std::uint64_t(output.height)+stripes*std::uint64_t(kernel.bounds.height-1))
                +static_cast<long double>(tiles)*(std::uint64_t(output.height)+stripes*std::uint64_t(kernel.bounds.height-1))*runs.size();
        if(estimate>options.maxSampleOperations)return failure(SpatialFilterStatus::BudgetExceeded,"Spatial filtering exceeds work budget");
        const auto work=std::uint64_t(estimate);
        if(work>options.maxSampleOperations)return failure(SpatialFilterStatus::BudgetExceeded,"Spatial filtering exceeds work budget");
        SpatialFilterResult result;result.sampleOperations=work;
        SpatialPlane plane{output,source.channels,{}};
        const int tileWidth=std::min(256,output.width),stripeHeight=std::min(16,output.height);
        const auto channels=std::size_t(source.channels);
        const std::size_t scratch=separate
            ?std::size_t(tileWidth)*channels*(std::size_t(kernel.bounds.height)*sizeof(float)+sizeof(double))+std::size_t(kernel.bounds.height)*sizeof(int)
            :(std::size_t(tileWidth+kernel.bounds.width)*channels+std::size_t(tileWidth)*channels*std::size_t(stripeHeight))*sizeof(double);
        const auto kernelBytes=(kernel.weights.size()+kernel.horizontal.size()+kernel.vertical.size())*sizeof(double)+runs.size()*sizeof(Run);
        if(scratch>options.maxWorkingBytes-n*sizeof(float)||kernelBytes>options.maxWorkingBytes-n*sizeof(float)-scratch)
            return failure(SpatialFilterStatus::BudgetExceeded,"Spatial filtering scratch exceeds memory budget");
        result.peakWorkingBytes=n*sizeof(float)+scratch+kernelBytes;
        plane.pixels.resize(n);
        if(output.empty()) {result.status=SpatialFilterStatus::Complete;result.output=std::move(plane);return result;}
        if(options.progress)options.progress(0);
        std::uint64_t completed=0;const double total=double(output.width)*output.height;
        for(int tx=0;tx<output.width;tx+=tileWidth) {
            const int width=std::min(tileWidth,output.width-tx),left=output.x+tx;
            if(separate) {
                const auto rowSize=std::size_t(width)*channels;
                const int rows=kernel.bounds.height;
                std::vector<float> ring(rowSize*std::size_t(rows));
                std::vector<int> rowKeys(std::size_t(rows),std::numeric_limits<int>::min());
                std::vector<double> sums(rowSize);
                for(int oy=0;oy<output.height;++oy) {
                    if(stopped(options))return failure(SpatialFilterStatus::Cancelled,"Spatial filtering cancelled");
                    const int y=output.y+oy;
                    std::fill(sums.begin(),sums.end(),0);
                    for(int ky=0;ky<rows;++ky) {
                        const int sy=y-(kernel.bounds.y+ky);
                        const int slot=((sy%rows)+rows)%rows;
                        auto* intermediate=ring.data()+std::size_t(slot)*rowSize;
                        if(rowKeys[std::size_t(slot)]!=sy) {
                            rowKeys[std::size_t(slot)]=sy;
                            std::fill_n(intermediate,rowSize,0.0F);
                            if(sy>=source.bounds.y&&sy<source.bounds.bottom())for(int x=0;x<width;++x) {
                                std::array<double,4> sum{};
                                const int first=int(std::clamp(std::int64_t(left)+x-source.bounds.right()+1-kernel.bounds.x,
                                    std::int64_t(0),std::int64_t(kernel.bounds.width)));
                                const int last=int(std::clamp(std::int64_t(left)+x-source.bounds.x-kernel.bounds.x+1,
                                    std::int64_t(0),std::int64_t(kernel.bounds.width)));
                                for(int kx=first;kx<last;++kx) {
                                    const auto* p=pixel(source,left+x-(kernel.bounds.x+kx),sy);const double w=kernel.horizontal[std::size_t(kx)];
                                    for(std::size_t c=0;c<channels;++c)sum[c]+=double(p[c])*w;
                                }
                                for(std::size_t c=0;c<channels;++c)intermediate[std::size_t(x)*channels+c]=float(sum[c]);
                            }
                        }
                        const double weight=kernel.vertical[std::size_t(ky)];
                        for(std::size_t i=0;i<rowSize;++i)sums[i]+=double(intermediate[i])*weight;
                    }
                    auto* destination=plane.pixels.data()+(std::size_t(oy)*std::size_t(output.width)+std::size_t(tx))*channels;
                    for(std::size_t i=0;i<rowSize;++i)destination[i]=float(sums[i]);
                    completed+=std::uint64_t(width);
                    if(options.progress&&(oy%16==0||oy+1==output.height))options.progress(double(completed)/total);
                }
            } else {
                // Exact equal-weight row spans use a running integral. We do
                // not replace disks/polygons by separable/box/Gaussian kernels.
                // Only a 16-row output stripe and one input prefix row are held.
                const int prefixLeft=left-(kernel.bounds.right()-1);
                const int prefixWidth=width+kernel.bounds.width-1;
                std::vector<double> prefix((std::size_t(prefixWidth)+1)*channels);
                std::vector<double> sums(std::size_t(width)*std::size_t(stripeHeight)*channels);
                for(int oy=0;oy<output.height;oy+=stripeHeight) {
                    const int height=std::min(stripeHeight,output.height-oy),top=output.y+oy;
                    std::fill(sums.begin(),sums.end(),0);
                    const int firstY=std::max(source.bounds.y,top-(kernel.bounds.bottom()-1));
                    const int lastY=std::min(source.bounds.bottom(),top+height-kernel.bounds.y);
                    for(int sy=firstY;sy<lastY;++sy) {
                        if(stopped(options))return failure(SpatialFilterStatus::Cancelled,"Spatial filtering cancelled");
                        std::fill_n(prefix.data(),channels,0.0);
                        for(int x=0;x<prefixWidth;++x) {
                            const auto* p=pixel(source,prefixLeft+x,sy);
                            for(std::size_t c=0;c<channels;++c)prefix[(std::size_t(x)+1)*channels+c]=prefix[std::size_t(x)*channels+c]+(p?double(p[c]):0);
                        }
                        for(const auto& run:runs) {
                            const int y=sy+run.y-top;if(y<0||y>=height)continue;
                            auto* row=sums.data()+std::size_t(y)*std::size_t(width)*channels;
                            for(int x=0;x<width;++x) {
                                const auto i0=std::size_t(left+x-run.x1+1-prefixLeft)*channels;
                                const auto i1=std::size_t(left+x-run.x0+1-prefixLeft)*channels;
                                for(std::size_t c=0;c<channels;++c)row[std::size_t(x)*channels+c]+=(prefix[i1+c]-prefix[i0+c])*run.weight;
                            }
                        }
                    }
                    for(int y=0;y<height;++y) {
                        auto* destination=plane.pixels.data()+(std::size_t(oy+y)*std::size_t(output.width)+std::size_t(tx))*channels;
                        const auto* row=sums.data()+std::size_t(y)*std::size_t(width)*channels;
                        for(std::size_t i=0;i<std::size_t(width)*channels;++i)destination[i]=float(row[i]);
                    }
                    completed+=std::uint64_t(width)*std::uint64_t(height);
                    if(options.progress)options.progress(double(completed)/total);
                }
            }
        }
        if(stopped(options))return failure(SpatialFilterStatus::Cancelled,"Spatial filtering cancelled");
        result.status=SpatialFilterStatus::Complete;result.output=std::move(plane);return result;
    } catch(const std::bad_alloc&) {return failure(SpatialFilterStatus::BudgetExceeded,"Unable to allocate spatial filter buffers");}
}

PremultipliedColor finishSpatialFilterPixel(PremultipliedColor original,PremultipliedColor filtered,bool preserveAlpha,float coverage) noexcept
{
    if(preserveAlpha) {
        for(std::size_t c=0;c<3;++c)filtered[c]=filtered[3]>0
            ?float((double(filtered[c])/filtered[3])*original[3]):0;
        filtered[3]=original[3];
    }
    coverage=std::clamp(coverage,0.0F,1.0F);
    if(coverage==0)return original;
    if(coverage==1)return filtered;
    for(std::size_t c=0;c<4;++c)filtered[c]=original[c]+(filtered[c]-original[c])*coverage;
    return filtered;
}
SpatialFilterResult filterSpatialRegion(SpatialPlaneView source,RectI output,const LayerSpatialFilter& filter,const SpatialFilterOptions& options)
{
    try {
        if(stopped(options))return failure(SpatialFilterStatus::Cancelled,"Spatial filtering cancelled");
        const auto bounds=kernelBounds(filter,options.pixelScale);
        const std::size_t kernelBytes=std::get_if<GaussianBlurParameters>(&filter.parameters)
            ?std::size_t(bounds.width+bounds.height)*sizeof(double)
            :std::size_t(bounds.width)*std::size_t(bounds.height)*sizeof(double);
        if(kernelBytes>options.maxWorkingBytes)return failure(SpatialFilterStatus::BudgetExceeded,"Spatial kernel exceeds memory budget");
        const auto kernel=makeSpatialKernel(filter,options.pixelScale,options.cancelled);
        auto convolutionOptions=options;
        if(options.progress)convolutionOptions.progress=[&](double p){options.progress(p*.95);};
        auto result=convolveSpatialRegion(source,output,kernel,convolutionOptions);
        if(!result)return result;
        for(int y=0;y<output.height;++y) {
            if(stopped(options))return failure(SpatialFilterStatus::Cancelled,"Spatial filtering cancelled");
            for(int x=0;x<output.width;++x) {
                const int px=output.x+x,py=output.y+y;const auto* original=pixel(source,px,py);
                auto* dst=result.output.pixels.data()+(std::size_t(y)*std::size_t(output.width)+std::size_t(x))*source.channels;
                const float coverage=filter.mask?adjustmentMaskCoverage(*filter.mask,{px+.5,py+.5}):1;
                if(source.channels==4) {
                    PremultipliedColor a{},b{};
                    for(std::size_t c=0;c<4;++c){a[c]=original?original[c]:0;b[c]=dst[c];}
                    const auto value=finishSpatialFilterPixel(a,b,filter.preserveAlpha,coverage);
                    std::copy(value.begin(),value.end(),dst);
                } else if(coverage<1) {const float a=original?original[0]:0;dst[0]=a+(dst[0]-a)*coverage;}
            }
            if(options.progress&&(y%16==0||y+1==output.height))options.progress(.95+.05*double(y+1)/std::max(1,output.height));
        }
        return result;
    } catch(const CancelledFilter&) {return failure(SpatialFilterStatus::Cancelled,"Spatial filtering cancelled");}
    catch(const std::bad_alloc&) {return failure(SpatialFilterStatus::BudgetExceeded,"Unable to allocate spatial filter kernel");}
    catch(const std::exception& e) {return failure(SpatialFilterStatus::InvalidInput,e.what());}
}
std::vector<std::byte> quantizeSpatialPlane(SpatialPlaneView plane,const SpatialFilterOptions& options)
{
    if(!validPlane(plane))throw std::invalid_argument("Invalid spatial plane to quantize");
    if(stopped(options))throw std::runtime_error("Spatial filter encoding cancelled");
    const auto count=std::size_t(plane.bounds.width)*std::size_t(plane.bounds.height)*plane.channels;
    if(count>options.maxWorkingBytes)throw std::runtime_error("Encoded spatial filter output exceeds memory budget");
    std::vector<std::byte> bytes(count);
    for(int y=0;y<plane.bounds.height;++y) {
      if(y%16==0) {
        if(stopped(options))throw std::runtime_error("Spatial filter encoding cancelled");
        if(options.progress)options.progress(double(y)/plane.bounds.height);
      }
      for(int x=0;x<plane.bounds.width;++x) {
        const auto* p=pixel(plane,plane.bounds.x+x,plane.bounds.y+y);
        const auto i=(std::size_t(y)*std::size_t(plane.bounds.width)+std::size_t(x))*plane.channels;
        if(plane.channels==1)bytes[i]=std::byte(alphaToByte(p[0]));
        else {
            const auto c=encodeColor({p[0],p[1],p[2],p[3]});
            bytes[i]=std::byte(c.red);bytes[i+1]=std::byte(c.green);bytes[i+2]=std::byte(c.blue);bytes[i+3]=std::byte(c.alpha);
        }
      }
    }
    if(stopped(options))throw std::runtime_error("Spatial filter encoding cancelled");
    if(options.progress)options.progress(1);
    return bytes;
}
} // namespace imageeditor::core
