// Canonical blend equations, compiled as both C++ and GLSL. Do not fork these
// formulas into a renderer-specific implementation. BVec3/BVec4 and b* scalar
// intrinsics are provided by each language wrapper. Mode IDs are BlendMode's
// stable numeric values; persistent projects use names, not these numbers.
// References: W3C Compositing and Blending Level 1, sections 6, 10.1 and 10.2.
B_INLINE float bDecode(float c)
{
    c = bClamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : bPow((c + 0.055f) / 1.055f, 2.4f);
}
B_INLINE float bEncode(float c)
{
    c = bClamp(c, 0.0f, 1.0f);
    // The transfer function maps its endpoints exactly. Preserve white rather
    // than losing one ulp in 1.055f - .055f: endpoint-sensitive blend branches
    // must receive 1 for an exactly white unassociated input. No fuzzy snapping.
    if (c == 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * bPow(c, 1.0f / 2.4f) - 0.055f;
}
B_INLINE float bLum(BVec3 c) { return 0.30f*c.x + 0.59f*c.y + 0.11f*c.z; }
B_INLINE float bWholeColorMetric(BVec3 c)
{
    int r=int(bFloor(c.x*255.0f+0.5f)),g=int(bFloor(c.y*255.0f+0.5f)),b=int(bFloor(c.z*255.0f+0.5f));
    if(c.x==bEncodeByte(r)&&c.y==bEncodeByte(g)&&c.z==bEncodeByte(b))
        return float(30*r+59*g+11*b)/25500.0f;
    return bLum(c);
}
B_INLINE float bMin3(BVec3 c) { return bMin(c.x, bMin(c.y, c.z)); }
B_INLINE float bMax3(BVec3 c) { return bMax(c.x, bMax(c.y, c.z)); }
B_INLINE BVec3 bClipColor(BVec3 c)
{
    float l = bLum(c), n = bMin3(c), x = bMax3(c);
    if (n < 0.0f) c = BVec3(l,l,l) + (c - BVec3(l,l,l)) * (l / (l - n));
    if (x > 1.0f) c = BVec3(l,l,l) + (c - BVec3(l,l,l)) * ((1.0f-l) / (x-l));
    return c;
}
B_INLINE BVec3 bSetLum(BVec3 c, float l)
{
    float d = l - bLum(c);
    return bClipColor(c + BVec3(d,d,d));
}
B_INLINE BVec3 bSetSat(BVec3 c, float s)
{
    float n = bMin3(c), x = bMax3(c);
    // Algebraically identical to the spec's sorted-channel assignment,
    // including equal-channel ties; no hue conversion or arbitrary epsilon.
    if (x <= n) return BVec3(0.0f,0.0f,0.0f);
    return (c - BVec3(n,n,n)) * (s / (x-n));
}
B_INLINE float bHardLight(float b, float s)
{
    return s <= 0.5f ? 2.0f*b*s : 1.0f-2.0f*(1.0f-b)*(1.0f-s);
}
B_INLINE float bSeparable(float b, float s, int mode)
{
    switch (mode) {
    case 1: return b*s;
    case 2: return b+s-b*s;
    case 3: return bHardLight(s,b);
    case 4: {
        if (s <= 0.5f) return b-(1.0f-2.0f*s)*b*(1.0f-b);
        float d = b <= 0.25f ? ((16.0f*b-12.0f)*b+4.0f)*b : bSqrt(b);
        return b+(2.0f*s-1.0f)*(d-b);
    }
    case 5: return bHardLight(b,s);
    case 6: return bMin(b,s);
    case 7: return bMax(b,s);
    case 8: return bAbs(b-s);
    case 9: return b+s-2.0f*b*s;
    case 14: { // Color Dodge: W3C backdrop-zero branch has priority.
        if (b == 0.0f) return 0.0f;
        if (s == 1.0f) return 1.0f;
        float denominator = 1.0f-s;
        // Compare before division: equivalent to min(1,b/denominator),
        // without overflowing when the denominator is very small.
        return b >= denominator ? 1.0f : b/denominator;
    }
    case 15: return bMin(1.0f,b+s); // Linear Dodge (Add), RGB only.
    case 16: { // Color Burn: W3C backdrop-one branch has priority.
        if (b == 1.0f) return 1.0f;
        if (s == 0.0f) return 0.0f;
        float numerator = 1.0f-b;
        return numerator >= s ? 0.0f : 1.0f-numerator/s;
    }
    case 17: return bMax(0.0f,b+s-1.0f); // Linear Burn, same blend space.
    case 18: return bMax(0.0f,b-s); // Subtract is ordered, not Difference.
    case 19: // Divide: 0/0=0; positive/0=1; never divide alpha or by zero.
        if (b == 0.0f) return 0.0f;
        if (s == 0.0f || b >= s) return 1.0f;
        return b/s;
    case 23: { // Vivid Light: source endpoints take precedence over backdrop.
        if (s == 0.0f) return 0.0f;
        if (s == 1.0f) return 1.0f;
        if (s < 0.5f) {
            float n=1.0f-b, d=2.0f*s;
            return n>=d ? 0.0f : 1.0f-n/d;
        }
        float d=2.0f*(1.0f-s);
        return b>=d ? 1.0f : b/d;
    }
    case 24: return bClamp(b+2.0f*s-1.0f,0.0f,1.0f);
    case 25: return s<0.5f ? bMin(b,2.0f*s) : bMax(b,2.0f*s-1.0f);
    case 26: { // Ordinary Hard Mix; exact complement ties depend on backdrop.
        // Exact canonical 8-bit operands represent rational n/255, not the
        // slightly asymmetric binary approximations of complementary codes.
        float bi=bFloor(b*255.0f+0.5f),si=bFloor(s*255.0f+0.5f);
        if(b==bEncodeByte(int(bi)) && s==bEncodeByte(int(si)))
            return bi+si>255.0f || (bi+si==255.0f && bi>127.5f) ? 1.0f : 0.0f;
        float total=b+s;
        float z=total-b;
        float error=(b-(total-z))+(s-z); // TwoSum: do not round a neighbor into a tie.
        return total>1.0f || (total==1.0f && (error>0.0f || (error==0.0f && b>0.5f))) ? 1.0f : 0.0f;
    }
    default: return s;
    }
}
B_INLINE BVec3 bBlend(BVec3 b, BVec3 s, int mode)
{
    BVec3 c;
    if (mode == 10) c = bSetLum(bSetSat(s,bMax3(b)-bMin3(b)),bLum(b));
    else if (mode == 11) c = bSetLum(bSetSat(b,bMax3(s)-bMin3(s)),bLum(b));
    else if (mode == 12) c = bSetLum(s,bLum(b));
    else if (mode == 13) c = bSetLum(b,bLum(s));
    else if (mode == 21) c = bWholeColorMetric(s)<bWholeColorMetric(b) ? s : b;
    else if (mode == 22) c = bWholeColorMetric(s)>bWholeColorMetric(b) ? s : b;
    else c = BVec3(bSeparable(b.x,s.x,mode),bSeparable(b.y,s.y,mode),bSeparable(b.z,s.z,mode));
    return BVec3(bClamp(c.x,0.0f,1.0f),bClamp(c.y,0.0f,1.0f),bClamp(c.z,0.0f,1.0f));
}

B_INLINE float bGridDivide(float n,float d)
{
    float q=n/d;
    // Correct the native reciprocal/division residual before choosing a cell.
    return q+bFma(-q,d,n)/d;
}
B_INLINE BVec3 bGridPoint(float x,float y,BVec3 a,BVec3 b,BVec3 w)
{
    float d=bFma(w.x,x,bFma(w.y,y,w.z));
    return BVec3(bGridDivide(bFma(a.x,x,bFma(a.y,y,a.z)),d),
        bGridDivide(bFma(b.x,x,bFma(b.y,y,b.z)),d),1.0f);
}
// Version 1 stochastic coverage: unsigned 32-bit wraparound, top 24 bits,
// integer layer-local unit cells. Negative cells wrap modulo 2^32. Coordinates
// are bounded before conversion (also protecting malformed projective input).
B_INLINE BUint bHash(BUint v)
{
    v ^= v >> 16; v *= 0x7feb352du;
    v ^= v >> 15; v *= 0x846ca68bu;
    return v ^ (v >> 16);
}
B_INLINE BUint bCell(float v)
{ return BUint(int(bClamp(bFloor(v),-2147483648.0f,2147483520.0f))); }
B_INLINE bool bDissolve(float probability,float x,float y,BUint seed)
{
    if (probability<=0.0f) return false;
    if (probability>=1.0f) return true;
    BUint h=bHash(bCell(x)^bHash(bCell(y)^seed));
    return (h>>8)<BUint(probability*16777216.0f);
}
B_INLINE float bUnassociatedLinear(float channel,float alpha)
{ return channel==alpha?1.0f:channel/alpha; }
B_INLINE float bUnassociatedEncoded(float channel, float alpha)
{
    // Callers guarantee positive alpha. GLSL division may round even x/x
    // slightly below one; preserve the exact associated-white identity before
    // division so the named-mode endpoint branches agree across CPU and GPU.
    return channel == alpha ? 1.0f : bEncode(channel/alpha);
}
B_INLINE float bCanonicalEncoded(float channel,float alpha)
{
    float encoded=bUnassociatedEncoded(channel,alpha);
    int code=int(bFloor(encoded*255.0f+0.5f));
    // Recover only an EXACT known associated canonical sample. No epsilon,
    // color snapping or quantization of filtered/continuous intermediate data.
    if(channel==bDecodeByte(code)*alpha)return bEncodeByte(code);
    return encoded;
}
B_INLINE BVec4 bComposite(BVec4 backdrop, BVec4 source, float opacity, int mode)
{
    // Inputs and output are LINEAR-light PREMULTIPLIED. Layer opacity scales
    // coverage once. Only the overlap blend function operates in straight sRGB.
    opacity = bClamp(opacity,0.0f,1.0f);
    float a = source.w * opacity, ab = backdrop.w;
    if (a <= 0.0f) return backdrop;
    float remain = 1.0f-a;
    float alpha = a+ab*remain;
    BVec3 s = BVec3(source.x,source.y,source.z) * opacity;
    BVec3 b = BVec3(backdrop.x,backdrop.y,backdrop.z);
    BVec3 c;
    if (mode == 0 || ab <= 0.0f) c = s + b*remain;
    else {
        BVec3 cs = BVec3(bUnassociatedEncoded(source.x,source.w),bUnassociatedEncoded(source.y,source.w),bUnassociatedEncoded(source.z,source.w));
        BVec3 cb = BVec3(bUnassociatedEncoded(backdrop.x,ab),bUnassociatedEncoded(backdrop.y,ab),bUnassociatedEncoded(backdrop.z,ab));
        if(mode==21 || mode==22 || mode==26) {
            cb=BVec3(bCanonicalEncoded(backdrop.x,ab),bCanonicalEncoded(backdrop.y,ab),bCanonicalEncoded(backdrop.z,ab));
            cs=BVec3(bCanonicalEncoded(source.x,source.w),bCanonicalEncoded(source.y,source.w),bCanonicalEncoded(source.z,source.w));
        }
        BVec3 blended = bBlend(cb,cs,mode);
        BVec3 linear = BVec3(bDecode(blended.x),bDecode(blended.y),bDecode(blended.z));
        c = s*(1.0f-ab) + b*remain + linear*(a*ab);
        // When every nonzero-weight contribution is exactly white, RGB must
        // equal alpha exactly. Summing three overlap terms separately from the
        // two alpha terms can otherwise lose an ulp and make a subsequent Burn
        // misclassify white as near-white. These are exact algebraic identities,
        // not epsilon snapping: deliberately near-white content stays distinct.
        if (blended.x == 1.0f && (ab == 1.0f || source.x == source.w)
            && (a == 1.0f || backdrop.x == ab)) c.x = alpha;
        if (blended.y == 1.0f && (ab == 1.0f || source.y == source.w)
            && (a == 1.0f || backdrop.y == ab)) c.y = alpha;
        if (blended.z == 1.0f && (ab == 1.0f || source.z == source.w)
            && (a == 1.0f || backdrop.z == ab)) c.z = alpha;
    }
    return BVec4(c.x,c.y,c.z,alpha);
}

B_INLINE BVec4 bCompositeAt(BVec4 backdrop,BVec4 source,float opacity,int mode,
    float x,float y,BUint seed,bool protectedCoverage)
{
    if(mode!=20) return bComposite(backdrop,source,opacity,mode);
    float probability=bClamp(opacity,0.0f,1.0f)*(protectedCoverage?1.0f:source.w);
    if(source.w<=0.0f || !bDissolve(probability,x,y,seed))return backdrop;
    if(protectedCoverage)return bComposite(backdrop,source,1.0f,0);
    return BVec4(source.x==source.w?1.0f:source.x/source.w,source.y==source.w?1.0f:source.y/source.w,source.z==source.w?1.0f:source.z/source.w,1.0f);
}

// Accumulate emitted color separately from the surviving external backdrop.
// This is the same overlap blend, followed by Normal accumulation of its
// effective source. It avoids subtracting nearly equal finished composites
// when stochastic coverage needs the conditional (unassociated) source color.
B_INLINE BVec4 bContribution(BVec4 backdrop,BVec4 source,float opacity,int mode,
    BVec4 emitted,float x,float y,BUint seed)
{
    if(mode==20) {
        if(source.w<=0.0f || !bDissolve(source.w*bClamp(opacity,0.0f,1.0f),x,y,seed))return emitted;
        return bCompositeAt(emitted,source,opacity,mode,x,y,seed,false);
    }
    if(source.w<=0.0f || opacity<=0.0f)return emitted;
    if(mode==0 || backdrop.w<=0.0f)return bComposite(emitted,source,opacity,0);
    BVec3 b=BVec3(bUnassociatedEncoded(backdrop.x,backdrop.w),bUnassociatedEncoded(backdrop.y,backdrop.w),bUnassociatedEncoded(backdrop.z,backdrop.w));
    BVec3 s=BVec3(bUnassociatedEncoded(source.x,source.w),bUnassociatedEncoded(source.y,source.w),bUnassociatedEncoded(source.z,source.w));
    if(mode==21 || mode==22 || mode==26) {
        b=BVec3(bCanonicalEncoded(backdrop.x,backdrop.w),bCanonicalEncoded(backdrop.y,backdrop.w),bCanonicalEncoded(backdrop.z,backdrop.w));
        s=BVec3(bCanonicalEncoded(source.x,source.w),bCanonicalEncoded(source.y,source.w),bCanonicalEncoded(source.z,source.w));
    }
    BVec3 mixed=bBlend(b,s,mode);
    float a=source.w,ab=backdrop.w;
    BVec4 effective=BVec4(source.x*(1-ab)+bDecode(mixed.x)*(a*ab),
        source.y*(1-ab)+bDecode(mixed.y)*(a*ab),source.z*(1-ab)+bDecode(mixed.z)*(a*ab),a);
    if(mixed.x==1.0f && (ab==1.0f || source.x==a))effective.x=a;
    if(mixed.y==1.0f && (ab==1.0f || source.y==a))effective.y=a;
    if(mixed.z==1.0f && (ab==1.0f || source.z==a))effective.z=a;
    return bComposite(emitted,effective,opacity,0);
}

B_INLINE BVec4 bAdjustmentBlend(BVec4 original,BVec4 corrected,float strength,int mode,
    float x,float y,BUint seed)
{
    if(original.w<=0.0f || strength<=0.0f || (original.x==corrected.x&&original.y==corrected.y&&original.z==corrected.z))return original;
    strength=bClamp(strength,0.0f,1.0f);
    if(mode==20)return bDissolve(strength,x,y,seed)?corrected:original;
    if(mode==0)return BVec4(original.x+(corrected.x-original.x)*strength,
        original.y+(corrected.y-original.y)*strength,original.z+(corrected.z-original.z)*strength,original.w);
    BVec3 b=BVec3(bUnassociatedEncoded(original.x,original.w),bUnassociatedEncoded(original.y,original.w),bUnassociatedEncoded(original.z,original.w));
    BVec3 s=BVec3(bUnassociatedEncoded(corrected.x,original.w),bUnassociatedEncoded(corrected.y,original.w),bUnassociatedEncoded(corrected.z,original.w));
    BVec3 c=bBlend(b,s,mode);
    if(mode==26) {
        // Preserve a proven exact inverse-color relation through the decode /
        // association round trip; do not mistake roundoff for a side of a tie.
        if(corrected.x==bDecode(1.0f-b.x)*original.w)c.x=b.x>0.5f?1.0f:0.0f;
        if(corrected.y==bDecode(1.0f-b.y)*original.w)c.y=b.y>0.5f?1.0f:0.0f;
        if(corrected.z==bDecode(1.0f-b.z)*original.w)c.z=b.z>0.5f?1.0f:0.0f;
    }
    return BVec4(original.x+(bDecode(c.x)*original.w-original.x)*strength,
        original.y+(bDecode(c.y)*original.w-original.y)*strength,
        original.z+(bDecode(c.z)*original.w-original.z)*strength,original.w);
}
