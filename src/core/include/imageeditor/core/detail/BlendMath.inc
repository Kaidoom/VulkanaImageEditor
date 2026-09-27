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
    else c = BVec3(bSeparable(b.x,s.x,mode),bSeparable(b.y,s.y,mode),bSeparable(b.z,s.z,mode));
    return BVec3(bClamp(c.x,0.0f,1.0f),bClamp(c.y,0.0f,1.0f),bClamp(c.z,0.0f,1.0f));
}
B_INLINE float bUnassociatedEncoded(float channel, float alpha)
{
    // Callers guarantee positive alpha. GLSL division may round even x/x
    // slightly below one; preserve the exact associated-white identity before
    // division so the named-mode endpoint branches agree across CPU and GPU.
    return channel == alpha ? 1.0f : bEncode(channel/alpha);
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
