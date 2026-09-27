#include "imageeditor/core/Adjustments.hpp"
#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>

namespace {
using namespace imageeditor::core;
using RGB=std::array<double,3>;
int failures=0;
void check(bool condition,std::string_view message)
{if(!condition){if(failures<50)std::cerr<<message<<'\n';++failures;}}
void near(double actual,double expected,double tolerance,std::string_view message)
{if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance){if(failures<50)std::cerr<<message<<": "<<actual<<" vs "<<expected<<'\n';++failures;}}
double clamp(double x){return std::clamp(x,0.0,1.0);}
double decode(double x){x=clamp(x);return x<=.04045?x/12.92:std::pow((x+.055)/1.055,2.4);}
double encode(double x){x=clamp(x);return x==1?1:x<=.0031308?12.92*x:1.055*std::pow(x,1/2.4)-.055;}
RGB encoded(RGB rgb){for(auto& x:rgb)x=encode(x);return rgb;}
RGB decoded(RGB rgb){for(auto& x:rgb)x=decode(x);return rgb;}
double luminance(RGB c){return .2126*c[0]+.7152*c[1]+.0722*c[2];}
double hueWrap(double h){h=std::fmod(h,360);return h<0?h+360:h;}
double smooth(double t){t=clamp(t);return t*t*(3-2*t);}
RGB hsl(RGB c)
{
    const double lo=*std::min_element(c.begin(),c.end()),hi=*std::max_element(c.begin(),c.end());
    const double d=hi-lo,l=(hi+lo)/2;
    if(d==0)return {0,0,l};
    const double s=l<=.5?d/(hi+lo):d/((1-hi)+(1-lo));
    double h;
    if(hi==c[0])h=(c[1]-c[2])/d;
    else if(hi==c[1])h=2+(c[2]-c[0])/d;
    else h=4+(c[0]-c[1])/d;
    return {hueWrap(60*h),s,l};
}
RGB fromHsl(RGB c)
{
    // Independent conventional sector/chroma reconstruction, not shipping
    // CSS triangular-channel code. Uses double throughout the oracle.
    const double chroma=(1-std::abs(2*c[2]-1))*c[1],h=hueWrap(c[0])/60;
    const double x=chroma*(1-std::abs(std::fmod(h,2)-1)),m=c[2]-chroma/2;
    RGB result;
    switch(int(h)) {
    case 0:result={chroma,x,0};break;case 1:result={x,chroma,0};break;
    case 2:result={0,chroma,x};break;case 3:result={0,x,chroma};break;
    case 4:result={x,0,chroma};break;default:result={chroma,0,x};break;
    }
    for(auto& v:result)v=clamp(v+m);
    return result;
}
double rangeWeight(double hue,int i)
{
    double distance=std::abs(hueWrap(hue)-i*60.0);
    if(distance>180)distance=360-distance;
    return 1-smooth(distance/60);
}
RGB preserveLuminance(RGB color,double target)
{
    double offset=target-luminance(color);
    for(auto& c:color)c+=offset;
    double amount=1;
    for(double c:color){if(c<0)amount=std::min(amount,target/(target-c));if(c>1)amount=std::min(amount,(1-target)/(c-target));}
    for(auto& c:color)c=clamp(target+(c-target)*amount);
    return color;
}
double curveReference(const CurveChannel& curve,double x)
{
    const auto& p=curve.points;const auto n=p.size();
    if(x<=p.front().input)return p.front().output;
    if(x>=p.back().input)return p.back().output;
    std::array<double,16> h{},s{},m{};
    for(std::size_t i=1;i<n;++i){h[i-1]=p[i].input-p[i-1].input;s[i-1]=(p[i].output-p[i-1].output)/h[i-1];}
    auto end=[](double a,double b,double u,double v){const double q=((2*a+b)*u-a*v)/(a+b);return q*u<=0?0:u*v<0&&std::abs(q)>3*std::abs(u)?3*u:q;};
    if(n==2)m[0]=m[1]=s[0];
    else {
        m[0]=end(h[0],h[1],s[0],s[1]);m[n-1]=end(h[n-2],h[n-3],s[n-2],s[n-3]);
        for(std::size_t i=1;i+1<n;++i)if(s[i-1]*s[i]>0){const double w=(2*h[i]+h[i-1])/(3*(h[i]+h[i-1]));m[i]=1/(w/s[i-1]+(1-w)/s[i]);}
    }
    std::size_t segment=0;while(x>p[segment+1].input)++segment;
    const double u=x-p[segment].input,dx=h[segment],slope=s[segment];
    // Polynomial coefficient form, independent of shipping Hermite basis.
    const double cubic=(m[segment]+m[segment+1]-2*slope)/(dx*dx);
    const double quadratic=(3*slope-2*m[segment]-m[segment+1])/dx;
    return ((cubic*u+quadratic)*u+m[segment])*u+p[segment].output;
}
double levelsReference(double c,const LevelsChannel& p)
{
    const double v=p.inputBlack==p.inputWhite?(c<=p.inputBlack?0:1):clamp((c-p.inputBlack)/(p.inputWhite-p.inputBlack));
    return p.outputBlack+(p.outputWhite-p.outputBlack)*std::pow(v,1/p.gamma);
}
RGB reference(const Adjustment& a,RGB input)
{
    RGB c=encoded(input),out=input;
    std::visit([&](const auto& p){
        using T=std::decay_t<decltype(p)>;
        if constexpr(std::is_same_v<T,ExposureParameters>)for(auto& v:out)v=clamp(v*std::exp2(p.stops));
        else if constexpr(std::is_same_v<T,BrightnessContrastParameters>){for(auto& v:c)v=clamp((v-.5)*std::exp2(4*p.contrast)+.5+p.brightness);out=decoded(c);}
        else if constexpr(std::is_same_v<T,LevelsParameters>){for(std::size_t i=0;i<3;++i)c[i]=levelsReference(levelsReference(c[i],p.channels[0]),p.channels[i+1]);out=decoded(c);}
        else if constexpr(std::is_same_v<T,CurvesParameters>){for(std::size_t i=0;i<3;++i)c[i]=curveReference(p.channels[i+1],curveReference(p.channels[0],c[i]));out=decoded(c);}
        else if constexpr(std::is_same_v<T,HueSaturationParameters>){
            auto h=hsl(c);double hue=p.ranges[0].hue,sat=p.ranges[0].saturation,light=p.ranges[0].lightness;
            const double confidence=smooth((*std::max_element(c.begin(),c.end())-*std::min_element(c.begin(),c.end()))*255);
            for(int i=0;i<6;++i){const auto& r=p.ranges[std::size_t(i)+1];const double w=rangeWeight(h[0],i)*confidence;hue+=r.hue*w;sat+=r.saturation*w;light+=r.lightness*w;}
            h[0]=hueWrap(h[0]+hue);h[1]=clamp(h[1]*(1+sat));light=std::clamp(light,-1.0,1.0);h[2]=light<0?h[2]*(1+light):h[2]+(1-h[2])*light;
            if(p.colorize){h[0]=p.colorizeHue;h[1]=p.colorizeSaturation;}out=decoded(fromHsl(h));
        } else if constexpr(std::is_same_v<T,VibranceParameters>){auto h=hsl(c);h[1]=clamp(h[1]+p.amount*h[1]*(1-h[1]));out=decoded(fromHsl(h));}
        else if constexpr(std::is_same_v<T,ColorBalanceParameters>){
            const double y=luminance(input),q=encode(y);std::array<double,3> weights{(1-q)*(1-q),2*q*(1-q),q*q};
            for(std::size_t channel=0;channel<3;++channel){for(std::size_t t=0;t<3;++t)c[channel]+=.5*p.tones[t][channel]*weights[t];c[channel]=clamp(c[channel]);}
            out=decoded(c);if(p.preserveLuminosity)out=preserveLuminance(out,y);
        } else if constexpr(std::is_same_v<T,WarmthTintParameters>){out[0]=clamp(input[0]*std::exp2(.5*p.warmth+.25*p.tint));out[1]=clamp(input[1]*std::exp2(-.5*p.tint));out[2]=clamp(input[2]*std::exp2(-.5*p.warmth+.25*p.tint));}
        else if constexpr(std::is_same_v<T,BlackWhiteParameters>){
            const auto h=hsl(c);double offset=0;for(int i=0;i<6;++i)offset+=p.contributions[std::size_t(i)]*rangeWeight(h[0],i);
            const double chroma=*std::max_element(c.begin(),c.end())-*std::min_element(c.begin(),c.end());
            const double y=decode(clamp(encode(luminance(input))+chroma*offset));
            const auto tint=preserveLuminance(decoded({p.tintColor.red/255.0,p.tintColor.green/255.0,p.tintColor.blue/255.0}),y);
            for(std::size_t i=0;i<3;++i)out[i]=y+(tint[i]-y)*p.tintStrength;
        } else {for(auto& v:c)v=1-v;out=decoded(c);}
    },a.parameters);
    return out;
}
std::shared_ptr<AdjustmentStack> examples()
{
    auto s=std::make_shared<AdjustmentStack>();for(auto& a:s->items)a.enabled=true;
    std::get<ExposureParameters>(s->items[0].parameters).stops=.75;
    std::get<BrightnessContrastParameters>(s->items[1].parameters)={-.07,.18};
    auto& levels=std::get<LevelsParameters>(s->items[2].parameters);
    levels.channels[0]={.06,1.4,.94,.02,.97};levels.channels[1]={.01,.7,.97,.04,.85};levels.channels[3]={.08,2,.88,0,1};
    auto& curves=std::get<CurvesParameters>(s->items[3].parameters);
    curves.channels[0].points={{0,.02},{.22,.12},{.5,.65},{.75,.55},{1,1}};
    curves.channels[2].points={{0,0},{.42,.58},{1,.92}};
    auto& hue=std::get<HueSaturationParameters>(s->items[4].parameters);
    hue.ranges[0]={31,.2,-.06};hue.ranges[1]={-18,-.3,.2};hue.ranges[5]={51,.22,.04};
    std::get<VibranceParameters>(s->items[5].parameters).amount=.72;
    std::get<ColorBalanceParameters>(s->items[6].parameters).tones={{{.2,-.13,.08},{-.14,.25,-.06},{.16,.1,-.2}}};
    std::get<WarmthTintParameters>(s->items[7].parameters)={.64,-.33};
    auto& bw=std::get<BlackWhiteParameters>(s->items[8].parameters);bw.contributions={.15,-.2,.35,.1,-.1,.2};bw.tintStrength=.4;
    return s;
}
void independentMath()
{
    const auto settings=examples();
    std::mt19937 random(0xadc01u);std::uniform_real_distribution<float> channel(0,1);
    const std::array<float,7> alphas{1,.51f,1.f/255,254.f/255,1e-8f,0,.1f};
    for(std::size_t operation=0;operation<adjustmentCount;++operation){
        auto one=std::make_shared<AdjustmentStack>();one->items[operation]=settings->items[operation];
        const auto compiled=compileAdjustmentStack(one);
        check(compiled.active,"Example is active");
        for(int i=0;i<1000;++i){
            const float alpha=alphas[std::size_t(i)%alphas.size()];
            PremultipliedColor in{channel(random)*alpha,channel(random)*alpha,channel(random)*alpha,alpha};
            if(i<8){const float q=i<4?0.f:1.f;in={q*alpha,q*alpha,q*alpha,alpha};}
            const auto actual=evaluateAdjustments(compiled,in,{.5,.5});
            RGB unassociated{};if(alpha>0)for(std::size_t c=0;c<3;++c)unassociated[c]=double(in[c])/alpha;
            const auto expected=reference(one->items[operation],unassociated);
            for(std::size_t c=0;c<3;++c)near(actual[c],alpha==0?0:expected[c]*alpha,3e-6,adjustmentName(allAdjustmentTypes[operation]));
            check(actual[3]==alpha,"All adjustments preserve alpha exactly");
        }
    }
    const auto compiled=compileAdjustmentStack(settings);
    for(int n=0;n<400;++n){
        const float alpha=channel(random);
        PremultipliedColor in{channel(random)*alpha,channel(random)*alpha,channel(random)*alpha,alpha};
        RGB value{double(in[0])/alpha,double(in[1])/alpha,double(in[2])/alpha};
        for(std::size_t stage=0;stage<adjustmentCount;++stage){
            value=reference(settings->items[stage],value);
            const auto actual=evaluateAdjustments(compiled,in,{},stage+1);
            for(std::size_t c=0;c<3;++c)near(actual[c],value[c]*alpha,6e-6,"Ordered intermediate stack matches double reference");
        }
    }
}
void neutralEndpointsAndLimits()
{
    auto stack=std::make_shared<AdjustmentStack>();
    for(std::size_t i=0;i<8;++i)stack->items[i].enabled=true;
    check(!compileAdjustmentStack(stack).active,"All neutral tone/color stages compile out");
    std::get<ColorBalanceParameters>(stack->items[6].parameters).preserveLuminosity=false;
    auto& hue=std::get<HueSaturationParameters>(stack->items[4].parameters);hue.colorizeHue=73;hue.colorizeSaturation=.2;
    check(!compileAdjustmentStack(stack).active,"Unused policy fields do not defeat neutral fast path");
    const PremultipliedColor strange{.001f,.1337f,.49999f,.501f};
    check(evaluateAdjustments(compileAdjustmentStack(stack),strange,{})==strange,"Neutral stack is bit exact, no roundtrip conversions");
    auto exposure=std::make_shared<AdjustmentStack>();exposure->items[0].enabled=true;
    std::get<ExposureParameters>(exposure->items[0].parameters).stops=1;
    auto compiled=compileAdjustmentStack(exposure);
    auto output=evaluateAdjustments(compiled,{.05f,.1f,.2f,.5f},{});
    near(output[0],.1,1e-8,"Exposure one stop doubles linear RGB");near(output[2],.4,1e-8,"Exposure independent of alpha");
    std::get<ExposureParameters>(exposure->items[0].parameters).stops=20;
    output=evaluateAdjustments(compileAdjustmentStack(exposure),{.3f,.2f,0,.4f},{});
    check(output==PremultipliedColor{.4f,.4f,0,.4f},"Extreme exposure clips RGB and preserves exact associated white/alpha");
    auto all=examples();
    for(const float alpha:{1.f,1.f/255,1e-20f})for(const float value:{0.f,1.f,std::nextafter(1.f,0.f),1e-20f}){
        const PremultipliedColor input{value*alpha,alpha,std::nextafter(alpha,0.f),alpha};
        for(std::size_t i=0;i<adjustmentCount;++i){auto one=std::make_shared<AdjustmentStack>();one->items[i]=all->items[i];const auto out=evaluateAdjustments(compileAdjustmentStack(one),input,{});for(float c:out)check(std::isfinite(c),"Near endpoints stay finite without denominator epsilon");check(out[3]==alpha,"Near endpoint alpha unchanged");}
    }
    auto invert=std::make_shared<AdjustmentStack>();invert->items[9].enabled=true;
    for(float alpha:{1.f,1.f/255,.37f}){
        check(evaluateAdjustments(compileAdjustmentStack(invert),{alpha,alpha,alpha,alpha},{})==PremultipliedColor{0,0,0,alpha},"White inversion exact");
        check(evaluateAdjustments(compileAdjustmentStack(invert),{0,0,0,alpha},{})==PremultipliedColor{alpha,alpha,alpha,alpha},"Black inversion exact");
    }
    check(evaluateAdjustments(compileAdjustmentStack(all),{1,1,1,0},{})==PremultipliedColor{},"Zero alpha discards hidden RGB, no nonlinear divide");
    const auto bad=evaluateAdjustments(compileAdjustmentStack(all),{NAN,INFINITY,-INFINITY,1},{});
    for(float c:bad)check(std::isfinite(c),"Nonfinite source RGB safely sanitized before operations");
    auto levels=std::make_shared<AdjustmentStack>();levels->items[2].enabled=true;
    auto& lp=std::get<LevelsParameters>(levels->items[2].parameters);lp.channels[0]={.5,1,.5,.2,.8};
    for(float encodedValue:{.2f,.5f,.8f}){
        const float v=float(decode(encodedValue));const auto out=evaluateAdjustments(compileAdjustmentStack(levels),{v,v,v,1},{});
        const double expected=decode(encodedValue<=.5f?.2:.8);near(out[0],expected,1e-6,"Coincident Levels input is a defined threshold");
    }
    lp.channels[0]={0,2,1,0,1};
    const float v=float(decode(.25));output=evaluateAdjustments(compileAdjustmentStack(levels),{v,v,v,1},{});
    near(output[0],decode(.5),2e-7,"Levels gamma midpoint independently known");
}
void curvesAndHue()
{
    CurveChannel identity;for(int i=0;i<=100;++i)near(evaluateCurve(identity,float(i)/100),double(i)/100,1e-7,"Identity curve");
    CurveChannel peak{{{0,0},{.5,1},{1,0}}};
    near(evaluateCurve(peak,.25f),.75,1e-7,"PCHIP symmetric peak quarter fixture");
    near(evaluateCurve(peak,.75f),.75,1e-7,"PCHIP nonmonotonic falling segment fixture");
    CurveChannel curve{{{.05,.2},{.2,.8},{.5,.3},{.6,.9},{.93,.1}}};
    for(int i=0;i<=4096;++i){const float x=float(i)/4096;near(evaluateCurve(curve,x),curveReference(curve,x),1e-6,"PCHIP polynomial double reference, intentional reversals");}
    near(evaluateCurve(curve,0),.2,1e-7,"Edited curve endpoint extends constantly to zero");near(evaluateCurve(curve,1),.1,1e-7,"Edited endpoint extends constantly to one");
    std::mt19937 random(31);std::uniform_real_distribution<double> y(0,1);
    for(int n=0;n<100;++n){CurveChannel c;c.points.clear();for(int i=0;i<16;++i)c.points.push_back({double(i)/15,y(random)});for(int i=0;i<15;++i)for(int j=0;j<=10;++j){const float x=float((double(i)+double(j)/10)/15);const double v=evaluateCurve(c,x);check(v>=std::min(c.points[std::size_t(i)].output,c.points[std::size_t(i)+1].output)-1e-6&&v<=std::max(c.points[std::size_t(i)].output,c.points[std::size_t(i)+1].output)+1e-6,"Each PCHIP interval stays within intentional local range");}}
    auto s=std::make_shared<AdjustmentStack>();s->items[4].enabled=true;auto& p=std::get<HueSaturationParameters>(s->items[4].parameters);p.ranges[0].hue=120;
    auto out=evaluateAdjustments(compileAdjustmentStack(s),{1,0,0,1},{});near(out[0],0,1e-6,"Red+120 becomes green R");near(out[1],1,1e-6,"Red+120 becomes green G");
    p.ranges[0].hue=-120;out=evaluateAdjustments(compileAdjustmentStack(s),{1,0,0,1},{});near(out[2],1,1e-6,"Negative hue wraps red to blue");
    p.ranges[0]={};p.ranges[1]={170,1,1};
    for(float gray:{0.f,.01f,.5f,1.f}){out=evaluateAdjustments(compileAdjustmentStack(s),{gray,gray,gray,1},{});for(std::size_t c=0;c<3;++c)near(out[c],gray,3e-7,"Named hue ranges leave exact gray stable");}
    for(int i=0;i<3600;++i){const double h=double(i)/10;double sum=0;for(int range=0;range<6;++range)sum+=rangeWeight(h,range);near(sum,1,1e-12,"Smooth hue ranges form a partition of unity");}
    p.colorize=true;p.colorizeHue=240;p.colorizeSaturation=1;out=evaluateAdjustments(compileAdjustmentStack(s),{.21404114f,.21404114f,.21404114f,1},{});near(out[2],1,2e-6,"Colorize can introduce hue into gray");
    auto vibrance=std::make_shared<AdjustmentStack>();vibrance->items[5].enabled=true;std::get<VibranceParameters>(vibrance->items[5].parameters).amount=1;
    double gains[2]{};for(int i=0;i<2;++i){const double sat=i==0?.25:.9;auto color=decoded(fromHsl({40,sat,.5}));auto adjusted=evaluateAdjustments(compileAdjustmentStack(vibrance),{float(color[0]),float(color[1]),float(color[2]),1},{});const auto h=hsl(encoded({adjusted[0],adjusted[1],adjusted[2]}));gains[i]=h[1]/sat;}
    check(gains[0]>gains[1],"Vibrance enhances muted saturation proportionally more than already saturated");
}
void monochromeAndTonalWeights()
{
    auto balance=std::make_shared<AdjustmentStack>();balance->items[6].enabled=true;
    auto& p=std::get<ColorBalanceParameters>(balance->items[6].parameters);
    p.tones={{{1,-1,.75},{-.8,1,.1},{.8,-.3,-1}}};
    for(int i=0;i<1000;++i){
        const double v=double(i)/999;RGB input{v,std::fmod(v*.37,.999),1-v};
        const float alpha=.37f;
        PremultipliedColor associated{float(input[0])*alpha,float(input[1])*alpha,float(input[2])*alpha,alpha};
        auto out=evaluateAdjustments(compileAdjustmentStack(balance),associated,{});
        const double before=.2126*associated[0]+.7152*associated[1]+.0722*associated[2];
        const double after=.2126*out[0]+.7152*out[1]+.0722*out[2];
        near(after,before,1e-7,"Color balance preserves linear luminance under strong gamut clipping");
        for(std::size_t channel=0;channel<3;++channel)check(out[channel]>=0&&out[channel]<=alpha,"Preserve luminosity contracts into gamut");
    }
    p.preserveLuminosity=false;
    const auto noPreserve=evaluateAdjustments(compileAdjustmentStack(balance),{.2f,.4f,.6f,1},{});
    const auto expected=reference(balance->items[6],{.2f,.4f,.6f});
    for(std::size_t i=0;i<3;++i)near(noPreserve[i],expected[i],2e-6,"Color balance explicit no-preserve variant");
    auto bw=std::make_shared<AdjustmentStack>();bw->items[8].enabled=true;
    auto& b=std::get<BlackWhiteParameters>(bw->items[8].parameters);
    b.contributions={1,-1,1,-1,1,-1};
    for(float value:{0.f,.01f,.18f,.6f,1.f}){
        const auto out=evaluateAdjustments(compileAdjustmentStack(bw),{value,value,value,1},{});
        for(std::size_t c=0;c<3;++c)near(out[c],value,2e-7,"B&W hue controls never shift neutral gray");
    }
    b.contributions={};b.tintColor={210,68,12,0};b.tintStrength=1;
    const auto transparentTint=evaluateAdjustments(compileAdjustmentStack(bw),{.1f,.2f,.3f,.6f},{});
    b.tintColor.alpha=255;const auto opaqueTint=evaluateAdjustments(compileAdjustmentStack(bw),{.1f,.2f,.3f,.6f},{});
    check(transparentTint==opaqueTint,"Tint color alpha has no hidden effect on content alpha");
    near(.2126*opaqueTint[0]+.7152*opaqueTint[1]+.0722*opaqueTint[2],.2126*.1+.7152*.2+.0722*.3,1e-7,"Tint preserves gray luminance");
    check(opaqueTint[3]==.6f,"B&W tint preserves fractional source alpha");
}
void validationAndIdentity()
{
    AdjustmentStack stack;check(validAdjustments(stack),"Default valid");
    check(equivalentAdjustments({},std::make_shared<AdjustmentStack>()),"Null and pristine stack have same persistent meaning");
    for(std::size_t i=0;i<adjustmentCount;++i){check(std::size_t(allAdjustmentTypes[i])==i,"Frozen adjustment IDs/order");check(adjustmentFromIdentifier(adjustmentIdentifier(allAdjustmentTypes[i]))==allAdjustmentTypes[i],"Stable identifiers round trip");}
    check(!adjustmentFromIdentifier("future-unimplemented-effect"),"Unknown identifier rejected");
    stack.algorithmVersion=2;check(!validAdjustments(stack),"Unknown algorithm rejected");stack.algorithmVersion=1;
    stack.items[0].type=AdjustmentType::Invert;check(!validAdjustments(stack),"Reorder/type mismatches rejected");stack=AdjustmentStack{};
    auto& exposure=std::get<ExposureParameters>(stack.items[0].parameters);exposure.stops=NAN;check(!validAdjustments(stack),"NaN rejected");exposure.stops=21;check(!validAdjustments(stack),"Unbounded exposure rejected");stack=AdjustmentStack{};
    auto& levels=std::get<LevelsParameters>(stack.items[2].parameters);levels.channels[0].inputBlack=.8;levels.channels[0].inputWhite=.2;check(!validAdjustments(stack),"Reversed input bounds rejected predictably");stack=AdjustmentStack{};
    auto& curve=std::get<CurvesParameters>(stack.items[3].parameters).channels[0];curve.points={{0,0},{.5,.3},{.5,.7},{1,1}};check(!validAdjustments(stack),"Duplicate curve inputs rejected rather than ambiguous interpolation");curve.points={{0,0},{.5,.3},{.50000001,.7},{1,1}};check(!validAdjustments(stack),"Unresolvable float curve interval rejected");
    stack=AdjustmentStack{};stack.items[0].mask=AdjustmentMask{};check(!validAdjustments(stack),"Mask cannot mean both null unrestricted and active empty");
}
void masksAndHistory()
{
    Document d(CanvasSpec{{32,24},96});History h;
    auto pixels=std::make_shared<ContiguousRasterSurface>(Extent2u{8,8},Rgba8{87,109,163,191});auto layer=Layer::raster("A",pixels);const LayerId id=layer.id;
    check(d.insertLayer(0,std::move(layer)),"Insert raster fixture");
    const auto originalRevision=pixels->revision();const auto initial=d.layer(id)->adjustmentRevision;
    auto selection=SelectionMask::rectangle({32,24},{3,4,5,6},128);
    check(d.setSelection(selection),"Selection fixture");
    auto state=std::make_shared<AdjustmentStack>();state->items[0].enabled=true;std::get<ExposureParameters>(state->items[0].parameters).stops=1;
    const AffineTransform captured{2,0,3,0,2,4};state->items[0].mask=captureAdjustmentMask(d.selection(),captured);
    check(d.setSelection({}),"Deselect after capture");
    near(adjustmentMaskCoverage(*state->items[0].mask,{.25,.25}),128.0/255,1e-7,"Frozen local mapping retains captured partial coverage");
    const auto compiled=compileAdjustmentStack(state);const auto out=evaluateAdjustments(compiled,{.1f,.1f,.1f,.5f},{.25,.25});
    near(out[0],.1*(1+128.0/255),2e-8,"Mask mixes once in linear RGB and preserves alpha");check(out[3]==.5f,"Mask does not alter alpha");
    near(adjustmentMaskCoverage(*state->items[0].mask,{20,20}),0,0,"Mask zero exterior");
    auto empty=std::make_shared<AdjustmentStack>(*state);empty->items[0].mask=AdjustmentMask{SelectionMask::filled({32,24},0),{}};
    check(!compileAdjustmentStack(empty).active,"Captured active empty selection never becomes unrestricted");
    {
        AdjustmentEditTransaction edit(d,id);check(edit.active(),"Live edit pins target");
        check(edit.update(state),"Live adjustment preview");check(d.layer(id)->adjustmentRevision==initial+1,"Only adjustment revision advances");
        check(!d.isModified(),"Preview doesn't alter saved content token before command");
        check(edit.commit(h),"Commit one adopted adjustment action");check(h.undoDepth()==1,"Single history command");
    }
    check(pixels->revision()==originalRevision&&pixels->dirtySince(originalRevision).empty(),"Adjustments never mutate source pixels or source upload journal");
    check(d.snapshot().layersBottomToTop[0].adjustments==d.layer(id)->adjustments,"Snapshot carries same immutable state");
    check(d.isModified(),"Committed adjustment marks content modified");d.markSaved();
    check(h.undo(d)&&!d.layer(id)->adjustments,"Undo returns original adjustment state");check(d.isModified(),"Undo away from saved marks dirty");
    check(h.redo(d)&&!d.isModified(),"Redo saved checkpoint becomes clean");check(h.undo(d),"Prepare existing redo");
    const auto redo=h.redoDepth();{
        AdjustmentEditTransaction edit(d,id);check(edit.update(state),"Preview before cancel");check(edit.cancel(),"Cancel restores original");
    }check(h.redoDepth()==redo&&!d.layer(id)->adjustments,"Cancellation preserves existing redo branch");
    {AdjustmentEditTransaction edit(d,id);check(edit.update(state),"Round trip preview begin");check(edit.update({}),"Round trip to original");check(!edit.commit(h),"No-op interaction emits no command");}
    check(h.redoDepth()==redo,"No-op preserves redo");
    {AdjustmentEditTransaction edit(d,id);check(edit.update(state),"New divergent preview");check(edit.commit(h),"New divergent commit");}check(!h.canRedo(),"Divergent committed edit clears redo");
    auto shared=std::make_shared<AdjustmentStack>(*state);shared->items[1].mask=state->items[0].mask;
    const auto maskCost=selection->memoryCost();std::size_t curveBytes=0;for(const auto& c:std::get<CurvesParameters>(state->items[3].parameters).channels)curveBytes+=c.points.capacity()*sizeof(CurvePoint);
    check(adjustmentMemoryCost(state)==sizeof(AdjustmentStack)+curveBytes+maskCost,"Exact stack vector and mask accounting");
    check(adjustmentMemoryCost(shared)==adjustmentMemoryCost(state),"Same captured mask retained by multiple effects counted once");
    check(adjustmentMemoryCost(state,shared)==2*(sizeof(AdjustmentStack)+curveBytes)+maskCost,"History before/after share masks without double charging");
    SetLayerAdjustmentsCommand command(id,{},state);check(command.memoryCost()==sizeof(command)+adjustmentMemoryCost(state),"Command accounts full retained state");
    check(d.setLayerTransform(id,{0,-2,11,2,0,9}),"Later transformed layer");
    check(d.layer(id)->adjustments->items[0].mask->localToMask==captured,"Layer transform doesn't mutate or stretch captured mask map");
    auto second=Layer::raster("B",pixels);const auto other=second.id;check(d.insertLayer(1,std::move(second)),"Second stable-ID target");
    auto otherState=std::make_shared<AdjustmentStack>();otherState->items[9].enabled=true;
    {AdjustmentEditTransaction edit(d,id);check(edit.update(otherState),"Pin doomed target preview");check(bool(d.takeLayer(id)),"Remove target during interaction");check(!edit.update(state)&&!edit.commit(h),"Removed target rejects stale callbacks");}
    check(!d.layer(other)->adjustments,"Stale callback never retargets by row index");
    {AdjustmentEditTransaction edit(d,other);check(edit.update(otherState),"Preview existing target");check(h.undo(d)==false,"Unrelated unavailable removed-target undo rejected atomically");check(edit.cancel(),"Still-owned preview can cancel");}
    const auto revision=d.layer(other)->adjustmentRevision;check(!d.setLayerAdjustments(other,std::make_shared<AdjustmentStack>()),"Pristine state setter no-op");check(d.layer(other)->adjustmentRevision==revision,"No-op avoids invalidation");
}
void foreignHistoryRollback()
{
    for (int completion=0;completion<3;++completion) {
        Document d({{16,16},96}); History h;
        auto layer=Layer::raster("Original",std::make_shared<ContiguousRasterSurface>(Extent2u{16,16}));
        const auto id=layer.id; check(d.insertLayer(0,std::move(layer)),"Foreign-history fixture"); d.markSaved();
        auto state=std::make_shared<AdjustmentStack>(); state->items[0].enabled=true;
        std::get<ExposureParameters>(state->items[0].parameters).stops=2;
        AdjustmentEditTransaction edit(d,id); check(edit.update(state),"Foreign-history live preview");
        auto other=Layer::raster("Unrelated",std::make_shared<ContiguousRasterSurface>(Extent2u{16,16}));
        check(h.execute(d,std::make_unique<AddLayerCommand>(std::move(other),1)),"Foreign add invalidates admission");
        if(completion==0) check(edit.cancel(),"Cancel rolls back own metadata across foreign history");
        if(completion==1) check(!edit.update({}),"Stale update rejected and rolled back");
        if(completion==2) check(!edit.commit(h),"Stale commit rejected and rolled back");
        check(!d.layer(id)->adjustments && h.undoDepth()==1,"No unrecorded preview remains on original target");
        check(h.undo(d)&&!d.isModified()&&!d.layer(id)->adjustments,"Undo unrelated command returns exact clean content");
        AdjustmentEditTransaction newer(d,id);check(newer.update(state),"Preview before replacement metadata");
        auto replacement=std::make_shared<AdjustmentStack>();replacement->items[9].enabled=true;
        check(d.setLayerAdjustments(id,replacement),"Foreign adjustment replaces live preview");
        check(!newer.cancel()&&d.layer(id)->adjustments==replacement,"Rollback never overwrites newer target adjustments");
    }
}
}
int main()
{
    independentMath();neutralEndpointsAndLimits();curvesAndHue();monochromeAndTonalWeights();validationAndIdentity();masksAndHistory();foreignHistoryRollback();
    if(failures){std::cerr<<failures<<" adjustment checks failed\n";return EXIT_FAILURE;}
    std::cout<<"Adjustment core: independent double math, endpoints, PCHIP, masks, history, memory and revisions passed\n";
    return EXIT_SUCCESS;
}
