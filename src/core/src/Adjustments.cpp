#include "imageeditor/core/Adjustments.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imageeditor::core {
namespace {
constexpr std::array<std::string_view,adjustmentCount> names {"Exposure","Brightness / Contrast","Levels","Curves",
    "Hue / Saturation","Vibrance","Color Balance","Warmth / Tint","Black & White","Invert"};
constexpr std::array<std::string_view,adjustmentCount> identifiers {"exposure","brightness-contrast","levels","curves",
    "hue-saturation","vibrance","color-balance","warmth-tint","black-white","invert"};
// Constructed once, not during neutral checks or pointer scrubbing.
const AdjustmentStack defaults;
bool range(double v,double lo,double hi) { return std::isfinite(v) && v>=lo && v<=hi; }
bool validCurve(const CurveChannel& curve) noexcept
{
    if(curve.points.size()<2 || curve.points.size()>16)return false;
    double previous=-1;
    for(const auto& point:curve.points) {
        if(!range(point.input,0,1)||!range(point.output,0,1)||point.input-previous<1e-6)return false;
        previous=point.input;
    }
    return true;
}
bool sameMask(const std::optional<AdjustmentMask>& a,const std::optional<AdjustmentMask>& b) noexcept
{
    if(a.has_value()!=b.has_value()) return false;
    return !a || (a->localToMask==b->localToMask && (a->coverage==b->coverage
        || (a->coverage && b->coverage && a->coverage->equivalent(*b->coverage))));
}
bool sameItem(const Adjustment& a,const Adjustment& b) noexcept
{ return a.type==b.type && a.enabled==b.enabled && a.parameters==b.parameters && sameMask(a.mask,b.mask); }
bool isDefault(const AdjustmentStack& stack) noexcept
{
    if(stack.algorithmVersion!=adjustmentAlgorithmVersion) return false;
    for(std::size_t i=0;i<adjustmentCount;++i)
        if(!sameItem(stack.items[i],defaults.items[i])) return false;
    return true;
}
std::array<double,16> curveTangents(const CurveChannel& curve)
{
    const auto n=curve.points.size();
    std::array<double,16> h {},d {},m {};
    for(std::size_t i=0;i+1<n;++i) {
        h[i]=curve.points[i+1].input-curve.points[i].input;
        d[i]=(curve.points[i+1].output-curve.points[i].output)/h[i];
    }
    if(n==2) {m[0]=m[1]=d[0];return m;}
    auto endpoint=[](double h0,double h1,double d0,double d1) {
        double result=((2*h0+h1)*d0-h0*d1)/(h0+h1);
        if(result*d0<=0) return 0.0;
        if(d0*d1<0 && std::abs(result)>3*std::abs(d0)) return 3*d0;
        return result;
    };
    m[0]=endpoint(h[0],h[1],d[0],d[1]);
    m[n-1]=endpoint(h[n-2],h[n-3],d[n-2],d[n-3]);
    for(std::size_t i=1;i+1<n;++i) {
        if(d[i-1]*d[i]<=0) continue;
        const double w1=2*h[i]+h[i-1],w2=h[i]+2*h[i-1];
        m[i]=(w1+w2)/(w1/d[i-1]+w2/d[i]);
    }
    return m;
}
void packCurve(const CurveChannel& curve,float* out)
{
    if(curve.points.front().input==0 && curve.points.back().input==1
        && std::all_of(curve.points.begin(),curve.points.end(),[](const CurvePoint& p){return p.input==p.output;})) {
        out[0]=0; // Exact per-channel identity, including when other channels are adjusted.
        return;
    }
    const auto m=curveTangents(curve);
    out[0]=float(curve.points.size());
    for(std::size_t i=0;i<curve.points.size();++i) {
        out[1+i*3]=float(curve.points[i].input);
        out[2+i*3]=float(curve.points[i].output);
        out[3+i*3]=float(m[i]);
    }
}
using namespace blend_detail;
inline float aFloor(float x) { return std::floor(x); }
struct Evaluator {
    const float* values;
#define A_INLINE inline
#define A_PARAM(index) values[index]
#include "imageeditor/core/detail/AdjustmentMath.inc"
#undef A_PARAM
#undef A_INLINE
};
std::size_t stackBytes(const AdjustmentState& state) noexcept
{
    if(!state) return 0;
    std::size_t result=sizeof(AdjustmentStack);
    for(const auto& item:state->items)
        if(const auto* c=std::get_if<CurvesParameters>(&item.parameters))
            for(const auto& channel:c->channels) result+=channel.points.capacity()*sizeof(CurvePoint);
    return result;
}
}
std::string_view adjustmentName(AdjustmentType type) noexcept
{ const auto i=std::size_t(type);return i<names.size()?names[i]:std::string_view("Unknown"); }
std::string_view adjustmentIdentifier(AdjustmentType type) noexcept
{ const auto i=std::size_t(type);return i<identifiers.size()?identifiers[i]:std::string_view(); }
std::optional<AdjustmentType> adjustmentFromIdentifier(std::string_view text) noexcept
{
    for(std::size_t i=0;i<identifiers.size();++i) if(identifiers[i]==text) return allAdjustmentTypes[i];
    return {};
}
Adjustment defaultAdjustment(AdjustmentType type)
{
    Adjustment a;
    a.type=type;
    switch(type) {
    case AdjustmentType::Exposure:a.parameters=ExposureParameters{};break;
    case AdjustmentType::BrightnessContrast:a.parameters=BrightnessContrastParameters{};break;
    case AdjustmentType::Levels:a.parameters=LevelsParameters{};break;
    case AdjustmentType::Curves:a.parameters=CurvesParameters{};break;
    case AdjustmentType::HueSaturation:a.parameters=HueSaturationParameters{};break;
    case AdjustmentType::Vibrance:a.parameters=VibranceParameters{};break;
    case AdjustmentType::ColorBalance:a.parameters=ColorBalanceParameters{};break;
    case AdjustmentType::WarmthTint:a.parameters=WarmthTintParameters{};break;
    case AdjustmentType::BlackWhite:a.parameters=BlackWhiteParameters{};break;
    case AdjustmentType::Invert:a.parameters=InvertParameters{};break;
    }
    return a;
}
AdjustmentStack::AdjustmentStack()
{ for(std::size_t i=0;i<items.size();++i) items[i]=defaultAdjustment(allAdjustmentTypes[i]); }

bool validAdjustments(const AdjustmentStack& stack,std::string* reason)
{
    auto fail=[&](const char* message) {if(reason)*reason=message;return false;};
    if(stack.algorithmVersion!=adjustmentAlgorithmVersion) return fail("Unsupported adjustment algorithm version");
    for(std::size_t i=0;i<stack.items.size();++i) {
        const auto& a=stack.items[i];
        if(std::size_t(a.type)!=i || a.parameters.index()!=i) return fail("Adjustment order/type mismatch");
        if(a.mask) {
            const auto& t=a.mask->localToMask;
            if(!a.mask->coverage || !std::isfinite(t.m00)||!std::isfinite(t.m01)||!std::isfinite(t.m02)
                ||!std::isfinite(t.m10)||!std::isfinite(t.m11)||!std::isfinite(t.m12)||!t.inverted())
                return fail("Invalid adjustment mask mapping");
        }
        const bool valid=std::visit([](const auto& p) {
            using T=std::decay_t<decltype(p)>;
            if constexpr(std::is_same_v<T,ExposureParameters>) return range(p.stops,-20,20);
            else if constexpr(std::is_same_v<T,BrightnessContrastParameters>) return range(p.brightness,-1,1)&&range(p.contrast,-1,1);
            else if constexpr(std::is_same_v<T,LevelsParameters>) {
                for(const auto& c:p.channels)
                    if(!range(c.inputBlack,0,1)||!range(c.inputWhite,c.inputBlack,1)||!range(c.gamma,.1,10)
                        ||!range(c.outputBlack,0,1)||!range(c.outputWhite,0,1)) return false;
                return true;
            } else if constexpr(std::is_same_v<T,CurvesParameters>) {
                for(const auto& c:p.channels)if(!validCurve(c))return false;
                return true;
            } else if constexpr(std::is_same_v<T,HueSaturationParameters>) {
                for(const auto& r:p.ranges) if(!range(r.hue,-180,180)||!range(r.saturation,-1,1)||!range(r.lightness,-1,1))return false;
                return range(p.colorizeHue,0,360)&&range(p.colorizeSaturation,0,1);
            } else if constexpr(std::is_same_v<T,VibranceParameters>)return range(p.amount,-1,1);
            else if constexpr(std::is_same_v<T,ColorBalanceParameters>) {
                for(const auto& tone:p.tones)for(double v:tone)if(!range(v,-1,1))return false;
                return true;
            } else if constexpr(std::is_same_v<T,WarmthTintParameters>)return range(p.warmth,-1,1)&&range(p.tint,-1,1);
            else if constexpr(std::is_same_v<T,BlackWhiteParameters>) {
                for(double v:p.contributions)if(!range(v,-1,1))return false;
                return range(p.tintStrength,0,1);
            } else return true;
        },a.parameters);
        if(!valid)return fail("Adjustment parameter is nonfinite, out of range, or degenerate");
    }
    return true;
}
bool equivalentAdjustments(const AdjustmentState& a,const AdjustmentState& b) noexcept
{
    if(a==b)return true;
    if(!a)return isDefault(*b);
    if(!b)return isDefault(*a);
    if(a->algorithmVersion!=b->algorithmVersion)return false;
    for(std::size_t i=0;i<adjustmentCount;++i)if(!sameItem(a->items[i],b->items[i]))return false;
    return true;
}
bool adjustmentIsNeutral(const Adjustment& a) noexcept
{
    if(!a.enabled)return true;
    if(a.mask && a.mask->coverage && a.mask->coverage->bounds().empty())return true;
    if(a.type==AdjustmentType::BlackWhite || a.type==AdjustmentType::Invert)return false;
    if(a.type==AdjustmentType::Curves) {
        const auto* p=std::get_if<CurvesParameters>(&a.parameters);
        if(!p)return false;
        for(const auto& c:p->channels) {
            if(c.points.empty() || c.points.front().input!=0 || c.points.back().input!=1)return false;
            for(const auto& point:c.points)if(point.input!=point.output)return false;
        }
        return true;
    }
    if(const auto* p=std::get_if<ColorBalanceParameters>(&a.parameters)) {
        for(const auto& tone:p->tones)for(double v:tone)if(v!=0)return false;
        return true;
    }
    if(const auto* p=std::get_if<HueSaturationParameters>(&a.parameters)) {
        if(p->colorize)return false;
        for(const auto& r:p->ranges)if(r.hue!=0||r.saturation!=0||r.lightness!=0)return false;
        return true;
    }
    const auto index=std::size_t(a.type);
    return index<adjustmentCount && a.parameters==defaults.items[index].parameters;
}
std::size_t adjustmentMemoryCost(const AdjustmentState& a,const AdjustmentState& b) noexcept
{
    std::size_t total=stackBytes(a)+(a==b?0:stackBytes(b));
    // Fixed bound: at most 20 masks, no allocating hash set inside noexcept/history accounting.
    std::array<const SelectionMask*,adjustmentCount*2> seen {};
    std::size_t count=0;
    for(const auto* state:{&a,&b}) if(*state)for(const auto& item:(*state)->items) if(item.mask&&item.mask->coverage) {
        const auto* mask=item.mask->coverage.get();
        if(std::find(seen.begin(),seen.begin()+std::ptrdiff_t(count),mask)==seen.begin()+std::ptrdiff_t(count)) {
            seen[count++]=mask;total+=mask->memoryCost();
        }
    }
    return total;
}
std::size_t adjustmentMemoryCost(const AdjustmentState& state) noexcept { return adjustmentMemoryCost(state,{}); }
AdjustmentMask captureAdjustmentMask(SelectionState mask,const AffineTransform& transform)
{
    if(!mask)throw std::invalid_argument("Capture requires an active selection");
    return {std::move(mask),transform};
}
float adjustmentMaskCoverage(const AdjustmentMask& mask,Vec2d local) noexcept
{
    if(!mask.coverage)return 0;
    const auto point=mask.localToMask.map(local);
    const double px=point.x-.5,py=point.y-.5;
    if(!std::isfinite(px)||!std::isfinite(py)||px < -1 || py < -1
        || px >= mask.coverage->extent().width || py >= mask.coverage->extent().height)return 0;
    const auto x=std::int32_t(std::floor(px)),y=std::int32_t(std::floor(py));
    const float fx=float(px-x),fy=float(py-y);
    const auto sample=[&](int dx,int dy){return float(mask.coverage->coverageAtDocumentPixel(x+dx,y+dy))/255.0f;};
    const float a=sample(0,0)*(1-fx)+sample(1,0)*fx,b=sample(0,1)*(1-fx)+sample(1,1)*fx;
    return a*(1-fy)+b*fy;
}
CompiledAdjustmentStack compileAdjustmentStack(const AdjustmentState& state)
{
    CompiledAdjustmentStack result;
    if(!state)return result;
    std::string reason;
    if(!validAdjustments(*state,&reason))throw std::invalid_argument(reason);
    for(std::size_t i=0;i<adjustmentCount;++i) {
        const auto& a=state->items[i];auto* record=result.parameters.data()+i*adjustmentParameterStride;
        record[1]=float(i);
        result.masks[i]=a.mask;
        if(adjustmentIsNeutral(a))continue;
        record[0]=1;result.active=true;
        auto* p=record+8;
        std::visit([&](const auto& parameters) {
            using T=std::decay_t<decltype(parameters)>;
            if constexpr(std::is_same_v<T,ExposureParameters>)p[0]=float(std::exp2(parameters.stops));
            else if constexpr(std::is_same_v<T,BrightnessContrastParameters>) {
                p[0]=float(parameters.brightness);p[1]=float(std::exp2(4*parameters.contrast));
            } else if constexpr(std::is_same_v<T,LevelsParameters>) {
                for(std::size_t c=0;c<4;++c) {
                    const auto& v=parameters.channels[c];
                    p[c*5]=float(v.inputBlack);p[c*5+1]=float(v.gamma);p[c*5+2]=float(v.inputWhite);
                    p[c*5+3]=float(v.outputBlack);p[c*5+4]=float(v.outputWhite);
                }
            } else if constexpr(std::is_same_v<T,CurvesParameters>) {
                for(std::size_t c=0;c<4;++c)packCurve(parameters.channels[c],p+c*49);
            } else if constexpr(std::is_same_v<T,HueSaturationParameters>) {
                for(std::size_t c=0;c<7;++c) {const auto& r=parameters.ranges[c];p[c*3]=float(r.hue);p[c*3+1]=float(r.saturation);p[c*3+2]=float(r.lightness);}
                p[21]=parameters.colorize?1.f:0.f;p[22]=float(parameters.colorizeHue);p[23]=float(parameters.colorizeSaturation);
            } else if constexpr(std::is_same_v<T,VibranceParameters>)p[0]=float(parameters.amount);
            else if constexpr(std::is_same_v<T,ColorBalanceParameters>) {
                for(std::size_t t=0;t<3;++t)for(std::size_t c=0;c<3;++c)p[t*3+c]=float(parameters.tones[t][c]);
                p[9]=parameters.preserveLuminosity?1.f:0.f;
            } else if constexpr(std::is_same_v<T,WarmthTintParameters>) {
                p[0]=float(std::exp2(.5*parameters.warmth+.25*parameters.tint));
                p[1]=float(std::exp2(-.5*parameters.tint));
                p[2]=float(std::exp2(-.5*parameters.warmth+.25*parameters.tint));
            } else if constexpr(std::is_same_v<T,BlackWhiteParameters>) {
                for(std::size_t c=0;c<6;++c)p[c]=float(parameters.contributions[c]);
                p[6]=float(srgbToLinear(parameters.tintColor.red));p[7]=float(srgbToLinear(parameters.tintColor.green));
                p[8]=float(srgbToLinear(parameters.tintColor.blue));p[9]=float(parameters.tintStrength);
            }
        },a.parameters);
    }
    return result;
}
PremultipliedColor evaluateAdjustments(const CompiledAdjustmentStack& stack,PremultipliedColor color,
    Vec2d local,std::size_t stopBefore) noexcept
{
    if(!stack.active||stopBefore==0)return color;
    if(!std::isfinite(color[3])||color[3]<=0)return {};
    const float alpha=std::min(color[3],1.f);
    BVec3 c {0,0,0};
    c.x=color[0]==alpha?1.f:std::isfinite(color[0])?std::clamp(color[0]/alpha,0.f,1.f):0;
    c.y=color[1]==alpha?1.f:std::isfinite(color[1])?std::clamp(color[1]/alpha,0.f,1.f):0;
    c.z=color[2]==alpha?1.f:std::isfinite(color[2])?std::clamp(color[2]/alpha,0.f,1.f):0;
    Evaluator evaluator {stack.parameters.data()};
    bool changed=false;
    for(std::size_t i=0;i<std::min(stopBefore,adjustmentCount);++i) {
        if(stack.parameters[i*adjustmentParameterStride]==0)continue;
        const float mask=stack.masks[i]?adjustmentMaskCoverage(*stack.masks[i],local):1.f;
        if(mask<=0)continue;
        const auto next=evaluator.aEvaluateStage(c,int(i));
        c=mask==1.f?next:c+(next-c)*mask;changed=true;
    }
    return changed?PremultipliedColor{c.x*alpha,c.y*alpha,c.z*alpha,alpha}:color;
}
float evaluateCurve(const CurveChannel& curve,float input)
{
    if(!validCurve(curve)||!std::isfinite(input))throw std::invalid_argument("Invalid curve");
    std::array<float,49> packed{};packCurve(curve,packed.data());
    return Evaluator{packed.data()}.aCurve(std::clamp(input,0.f,1.f),0);
}
} // namespace imageeditor::core
