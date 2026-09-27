// Version-1 authoritative adjustment equations, shared by C++ and GLSL.
// Requires BlendMath.inc, A_INLINE, A_PARAM(absoluteFloatIndex), and aFloor.
// aEvaluateStage accepts/returns unassociated linear RGB. Alpha and mask mixing
// are owned by the wrapper. See docs/ADJUSTMENT_ALGORITHMS.md for exact semantics.
A_INLINE BVec3 aClamp(BVec3 c)
{ return BVec3(bClamp(c.x,0.0f,1.0f),bClamp(c.y,0.0f,1.0f),bClamp(c.z,0.0f,1.0f)); }
A_INLINE BVec3 aEncode(BVec3 c) { return BVec3(bEncode(c.x),bEncode(c.y),bEncode(c.z)); }
A_INLINE BVec3 aDecode(BVec3 c) { return BVec3(bDecode(c.x),bDecode(c.y),bDecode(c.z)); }
A_INLINE float aLuminance(BVec3 c) { return .2126f*c.x+.7152f*c.y+.0722f*c.z; }
A_INLINE float aSmooth(float x) { x=bClamp(x,0.0f,1.0f); return x*x*(3.0f-2.0f*x); }
A_INLINE float aWrapHue(float h) { return h-360.0f*aFloor(h/360.0f); }
A_INLINE BVec3 aHsl(BVec3 c)
{
    float lo=bMin3(c), hi=bMax3(c), d=hi-lo, l=(hi+lo)*.5f;
    if (d==0.0f) return BVec3(0.0f,0.0f,l);
    // Stable form of 1-|2L-1|: averaging adjacent floats near white can
    // round L to one despite nonzero chroma. No epsilon changes the hue math.
    float sum=hi+lo;
    float denominator=sum<=1.0f ? sum : (1.0f-hi)+(1.0f-lo);
    float s=d/denominator;
    float h=hi==c.x ? (c.y-c.z)/d : hi==c.y ? (c.z-c.x)/d+2.0f : (c.x-c.y)/d+4.0f;
    return BVec3(aWrapHue(h*60.0f),bClamp(s,0.0f,1.0f),l);
}
A_INLINE float aHueChannel(float h,float s,float l,float n)
{
    float k=n+h/30.0f;
    k=k-12.0f*aFloor(k/12.0f);
    float a=s*bMin(l,1.0f-l);
    return l-a*bMax(-1.0f,bMin(bMin(k-3.0f,9.0f-k),1.0f));
}
A_INLINE BVec3 aFromHsl(BVec3 hsl)
{
    return aClamp(BVec3(aHueChannel(hsl.x,hsl.y,hsl.z,0.0f),
        aHueChannel(hsl.x,hsl.y,hsl.z,8.0f),aHueChannel(hsl.x,hsl.y,hsl.z,4.0f)));
}
A_INLINE float aRangeWeight(float hue,int range)
{
    float d=bAbs(aWrapHue(hue-float(range)*60.0f+180.0f)-180.0f);
    return 1.0f-aSmooth(d/60.0f);
}
A_INLINE BVec3 aPreserveLum(BVec3 c,float y)
{
    float d=y-aLuminance(c);
    c=c+BVec3(d,d,d);
    float lo=bMin3(c),hi=bMax3(c),scale=1.0f;
    if(lo<0.0f) scale=bMin(scale,y/(y-lo));
    if(hi>1.0f) scale=bMin(scale,(1.0f-y)/(hi-y));
    return aClamp(BVec3(y,y,y)+(c-BVec3(y,y,y))*scale);
}
A_INLINE float aLevels(float c,int p)
{
    float black=A_PARAM(p),gamma=A_PARAM(p+1),white=A_PARAM(p+2);
    // A coincident range is an exact threshold: <= black maps to output black.
    float t=white==black ? (c<=black ? 0.0f : 1.0f) : bClamp((c-black)/(white-black),0.0f,1.0f);
    if(gamma!=1.0f && t>0.0f && t<1.0f) t=bPow(t,1.0f/gamma);
    return A_PARAM(p+3)+(A_PARAM(p+4)-A_PARAM(p+3))*t;
}
A_INLINE float aCurve(float c,int p)
{
    int count=int(A_PARAM(p));
    if(count==0) return c; // Compiled identity; avoid Hermite-roundoff drift.
    if(c<=A_PARAM(p+1)) return A_PARAM(p+2);
    if(c>=A_PARAM(p+1+(count-1)*3)) return A_PARAM(p+2+(count-1)*3);
    for(int i=0;i<15;++i) {
        if(i+1>=count) break;
        int q=p+1+i*3;
        float x=A_PARAM(q),x1=A_PARAM(q+3);
        if(c<=x1) {
            float y=A_PARAM(q+1),y1=A_PARAM(q+4),h=x1-x,t=(c-x)/h;
            float t2=t*t,t3=t2*t;
            float v=(2.0f*t3-3.0f*t2+1.0f)*y+(t3-2.0f*t2+t)*h*A_PARAM(q+2)
                +(-2.0f*t3+3.0f*t2)*y1+(t3-t2)*h*A_PARAM(q+5);
            // PCHIP analytically remains in the segment range; clamp only
            // floating roundoff, never force global monotonicity.
            return bClamp(v,bMin(y,y1),bMax(y,y1));
        }
    }
    return c;
}
A_INLINE BVec3 aEvaluateStage(BVec3 linear,int stage)
{
    int base=stage*256,p=base+8,type=int(A_PARAM(base+1));
    if(A_PARAM(base)==0.0f) return linear;
    if(type==0) return aClamp(linear*A_PARAM(p)); // compiled 2^EV, linear exposure
    if(type==7) return aClamp(BVec3(linear.x*A_PARAM(p),linear.y*A_PARAM(p+1),linear.z*A_PARAM(p+2)));
    BVec3 c=aEncode(linear);
    if(type==1) {
        float gain=A_PARAM(p+1),offset=A_PARAM(p);
        return aDecode(aClamp((c-BVec3(.5f,.5f,.5f))*gain+BVec3(.5f+offset,.5f+offset,.5f+offset)));
    }
    if(type==2) {
        c=BVec3(aLevels(c.x,p),aLevels(c.y,p),aLevels(c.z,p));
        return aDecode(BVec3(aLevels(c.x,p+5),aLevels(c.y,p+10),aLevels(c.z,p+15)));
    }
    if(type==3) {
        c=BVec3(aCurve(c.x,p),aCurve(c.y,p),aCurve(c.z,p));
        return aDecode(BVec3(aCurve(c.x,p+49),aCurve(c.y,p+98),aCurve(c.z,p+147)));
    }
    if(type==4) {
        BVec3 hsl=aHsl(c);
        float hue=A_PARAM(p),sat=A_PARAM(p+1),light=A_PARAM(p+2);
        // Hue-range controls fade out below one 8-bit code of chroma; neutral
        // gray has no arbitrary hue-range allegiance. Master remains effective.
        float confidence=aSmooth((bMax3(c)-bMin3(c))*255.0f);
        for(int i=0;i<6;++i) {
            float w=aRangeWeight(hsl.x,i)*confidence;
            int q=p+3+i*3;
            hue+=A_PARAM(q)*w; sat+=A_PARAM(q+1)*w; light+=A_PARAM(q+2)*w;
        }
        hsl.x=aWrapHue(hsl.x+hue);
        hsl.y=bClamp(hsl.y*(1.0f+sat),0.0f,1.0f);
        light=bClamp(light,-1.0f,1.0f);
        hsl.z=light<0.0f ? hsl.z*(1.0f+light) : hsl.z+(1.0f-hsl.z)*light;
        if(A_PARAM(p+21)>0.0f) { hsl.x=A_PARAM(p+22); hsl.y=A_PARAM(p+23); }
        return aDecode(aFromHsl(hsl));
    }
    if(type==5) {
        BVec3 hsl=aHsl(c);
        hsl.y=bClamp(hsl.y+A_PARAM(p)*hsl.y*(1.0f-hsl.y),0.0f,1.0f);
        return aDecode(aFromHsl(hsl));
    }
    if(type==6) {
        float y=aLuminance(linear),q=bEncode(y);
        float shadows=(1.0f-q)*(1.0f-q),mid=2.0f*q*(1.0f-q),high=q*q;
        c=aClamp(c+BVec3(
            .5f*(A_PARAM(p)*shadows+A_PARAM(p+3)*mid+A_PARAM(p+6)*high),
            .5f*(A_PARAM(p+1)*shadows+A_PARAM(p+4)*mid+A_PARAM(p+7)*high),
            .5f*(A_PARAM(p+2)*shadows+A_PARAM(p+5)*mid+A_PARAM(p+8)*high)));
        linear=aDecode(c);
        return A_PARAM(p+9)>0.0f ? aPreserveLum(linear,y) : linear;
    }
    if(type==8) {
        BVec3 hsl=aHsl(c);
        float offset=0.0f;
        for(int i=0;i<6;++i) offset+=A_PARAM(p+i)*aRangeWeight(hsl.x,i);
        float g=bClamp(bEncode(aLuminance(linear))+(bMax3(c)-bMin3(c))*offset,0.0f,1.0f);
        float y=bDecode(g),strength=A_PARAM(p+9);
        BVec3 gray=BVec3(y,y,y);
        if(strength==0.0f) return gray;
        BVec3 tinted=aPreserveLum(BVec3(A_PARAM(p+6),A_PARAM(p+7),A_PARAM(p+8)),y);
        return gray+(tinted-gray)*strength;
    }
    if(type==9) return aDecode(BVec3(1.0f-c.x,1.0f-c.y,1.0f-c.z));
    return linear;
}
