// Shared CPU/GLSL style construction. Input/output: premultiplied linear RGBA.
// E_PARAM is the compiled effect program, E_MASK(i) is sampled R8 coverage.
E_INLINE BVec4 eColor(int effect)
{
    int i=effect*32;
    return BVec4(E_PARAM(i+4),E_PARAM(i+5),E_PARAM(i+6),E_PARAM(i+7));
}
E_INLINE BVec4 eGradientStop(int stop)
{
    int i=int(E_PARAM(6*32+18))+stop*5;
    return BVec4(E_PARAM(i+1),E_PARAM(i+2),E_PARAM(i+3),E_PARAM(i+4));
}
E_INLINE BVec4 eGradientSample(float t)
{
    int i=6*32;
    int count=int(E_PARAM(i+17));
    // Preserve the original arithmetic for existing two-color gradients.
    if(count==0)return BVec4(E_PARAM(i+4)*(1-t)+E_PARAM(i+8)*t,E_PARAM(i+5)*(1-t)+E_PARAM(i+9)*t,
        E_PARAM(i+6)*(1-t)+E_PARAM(i+10)*t,E_PARAM(i+7)*(1-t)+E_PARAM(i+11)*t);
    int offset=int(E_PARAM(i+18));
    int lo=0,hi=count;
    // First stop strictly after t. Duplicate positions are intentional hard
    // transitions: the final stop at that position owns the transition edge.
    while(lo<hi) {
        int middle=(lo+hi)/2;
        if(E_PARAM(offset+middle*5)<=t)lo=middle+1;else hi=middle;
    }
    if(lo==0)return eGradientStop(0);
    if(lo==count)return eGradientStop(count-1);
    float left=E_PARAM(offset+(lo-1)*5),right=E_PARAM(offset+lo*5);
    float amount=bClamp((t-left)/(right-left),0.0F,1.0F);
    BVec4 a=eGradientStop(lo-1),b=eGradientStop(lo);
    return BVec4(a.x*(1-amount)+b.x*amount,a.y*(1-amount)+b.y*amount,
        a.z*(1-amount)+b.z*amount,a.w*(1-amount)+b.w*amount);
}
E_INLINE BVec4 eGradient(float x,float y)
{
    int i=6*32;
    float w=bMax(E_PARAM(258),0.000001F), h=bMax(E_PARAM(259),0.000001F);
    x-=E_PARAM(256)+w*0.5F;y-=E_PARAM(257)+h*0.5F;
    float t;
    float scale=bMax(E_PARAM(i+15),0.01F);
    if(E_PARAM(i+12)>0.5F) t=2.0F*bSqrt((x/w)*(x/w)+(y/h)*(y/h))/scale;
    else t=0.5F+(x*E_PARAM(i+13)+y*E_PARAM(i+14))
        /(bMax(bAbs(E_PARAM(i+13))*w+bAbs(E_PARAM(i+14))*h,0.000001F)*scale);
    t=bClamp(t,0.0F,1.0F);if(E_PARAM(i+16)>0.5F)t=1.0F-t;
    // Interpolate associated linear colors, including their alpha. A zero-alpha
    // stop cannot contribute hidden RGB; blend functions handle unassociation.
    return eGradientSample(t);
}
E_INLINE BUint eSeed(int contribution)
{ return (BUint(E_PARAM(3)) | (BUint(E_PARAM(19))<<16)) ^ (BUint(contribution)*0x9e3779b9u); }
E_INLINE BVec4 eInside(BVec4 base,BVec4 paint,int effect,float coverage,float x,float y)
{
    int i=effect*32;
    if(E_PARAM(i)==0.0F || base.w<=0.0F)return base;
    BVec4 opaque=BVec4(bUnassociatedLinear(base.x,base.w),bUnassociatedLinear(base.y,base.w),bUnassociatedLinear(base.z,base.w),1.0F);
    BVec4 result=bCompositeAt(opaque,paint,E_PARAM(i+2)*bClamp(coverage,0.0F,1.0F),int(E_PARAM(i+1)),x,y,eSeed(effect+1),false);
    return BVec4(result.x*base.w,result.y*base.w,result.z*base.w,base.w);
}
E_INLINE BVec4 eOutside(BVec4 backdrop,int effect,float coverage,float x,float y)
{
    int i=effect*32;
    if(E_PARAM(i)==0.0F)return backdrop;
    return bCompositeAt(backdrop,eColor(effect),E_PARAM(i+2)*bClamp(coverage,0.0F,1.0F),int(E_PARAM(i+1)),x,y,eSeed(effect+1),false);
}
E_INLINE BVec4 eEmission(BVec4 backdrop,BVec4 emitted,int effect,float coverage,float x,float y)
{
    int i=effect*32;
    if(E_PARAM(i)==0.0F)return emitted;
    return bContribution(backdrop,eColor(effect),E_PARAM(i+2)*bClamp(coverage,0.0F,1.0F),int(E_PARAM(i+1)),emitted,x,y,eSeed(effect+1));
}
E_INLINE BVec4 ePaintInside(BVec4 paint,BVec4 color,int effect,float coverage,float x,float y)
{
    int i=effect*32;
    if(E_PARAM(i)==0.0F)return paint;
    return bCompositeAt(paint,color,E_PARAM(i+2)*bClamp(coverage,0.0F,1.0F),int(E_PARAM(i+1)),x,y,eSeed(effect+1),false);
}
E_INLINE BVec4 eInitialPaint(BVec4 base,float fill,float shapeAlpha)
{
    if(shapeAlpha<=0.0F)return BVec4(0,0,0,0);
    if(E_PARAM(22)>0.5F)return BVec4(bUnassociatedLinear(base.x,base.w)*fill,bUnassociatedLinear(base.y,base.w)*fill,
        bUnassociatedLinear(base.z,base.w)*fill,fill);
    return BVec4(base.x*fill/shapeAlpha,base.y*fill/shapeAlpha,base.z*fill/shapeAlpha,base.w*fill/shapeAlpha);
}
E_INLINE BVec4 eFillPaint(BVec4 base,float fill,float x,float y,float shapeAlpha)
{
    if(shapeAlpha<=0.0F)return BVec4(0,0,0,0);
    // Work inside the original silhouette with an independent paint alpha.
    // Interior effects can add paint even when the content Fill is zero.
    BVec4 paint=eInitialPaint(base,fill,shapeAlpha);
    paint=ePaintInside(paint,eColor(5),5,1.0F,x,y);
    paint=ePaintInside(paint,eGradient(x,y),6,1.0F,x,y);
    paint=ePaintInside(paint,eColor(2),2,E_MASK(3),x,y);
    paint=ePaintInside(paint,eColor(4),4,E_MASK(5),x,y);
    if(E_PARAM(224)!=0.0F) {
        paint=bCompositeAt(paint,BVec4(E_PARAM(232),E_PARAM(233),E_PARAM(234),E_PARAM(235)),
            E_PARAM(242)*E_MASK(7),int(E_PARAM(241)),x,y,eSeed(9),false);
        paint=bCompositeAt(paint,eColor(7),E_PARAM(226)*E_MASK(6),int(E_PARAM(225)),x,y,eSeed(8),false);
    }
    return ePaintInside(paint,eColor(0),0,E_MASK(1),x,y);
}
E_INLINE BVec4 eCompositePass(BVec4 backdrop,BVec4 base,float x,float y,int mode,E_OUTPUT emitted,bool track,float fill,float shapeAlpha)
{
    emitted=BVec4(0,0,0,0);
    BVec4 styled=base;
    if(fill>=1.0F && E_PARAM(22)>0.5F) {
        styled=eInside(styled,eColor(5),5,1.0F,x,y);
        styled=eInside(styled,eGradient(x,y),6,1.0F,x,y);
        styled=eInside(styled,eColor(2),2,E_MASK(3),x,y);
        styled=eInside(styled,eColor(4),4,E_MASK(5),x,y);
        if(E_PARAM(224)!=0.0F && styled.w>0.0F) {
            float alpha=styled.w;
            BVec4 opaque=BVec4(bUnassociatedLinear(styled.x,alpha),bUnassociatedLinear(styled.y,alpha),bUnassociatedLinear(styled.z,alpha),1.0F);
            opaque=bCompositeAt(opaque,BVec4(E_PARAM(232),E_PARAM(233),E_PARAM(234),E_PARAM(235)),
                E_PARAM(242)*E_MASK(7),int(E_PARAM(241)),x,y,eSeed(9),false);
            opaque=bCompositeAt(opaque,eColor(7),E_PARAM(226)*E_MASK(6),int(E_PARAM(225)),x,y,eSeed(8),false);
            styled=BVec4(opaque.x*alpha,opaque.y*alpha,opaque.z*alpha,alpha);
        }
        styled=eInside(styled,eColor(0),0,E_MASK(1),x,y);
    }
    if(track)emitted=eEmission(backdrop,emitted,1,E_MASK(2),x,y);
    BVec4 result=eOutside(backdrop,1,E_MASK(2),x,y);
    if(track)emitted=eEmission(result,emitted,3,E_MASK(4),x,y);
    result=eOutside(result,3,E_MASK(4),x,y);
    if(fill>=1.0F && E_PARAM(22)>0.5F) {
        if(track)emitted=bContribution(result,styled,1.0F,mode,emitted,x,y,eSeed(0));
        result=bComposite(result,styled,1.0F,mode);
    } else {
        BVec4 initial=eInitialPaint(base,fill,shapeAlpha);
        BVec4 paint=eFillPaint(base,fill,x,y,shapeAlpha);
        if(E_PARAM(22)<0.5F && paint.x==initial.x && paint.y==initial.y && paint.z==initial.z && paint.w==initial.w) {
            // Merely ignoring alpha for effect geometry cannot turn source alpha
            // into Fill (notably for Color Dodge/Burn and other special modes).
            if(track)emitted=bContribution(result,base,fill,mode,emitted,x,y,eSeed(0));
            result=bCompositeFillAtSpace(result,base,1.0F,mode,x,y,eSeed(0),false,E_SPACE,fill);
        } else if(paint.w>0.0F) {
            BVec4 shape=BVec4(bUnassociatedLinear(paint.x,paint.w)*shapeAlpha,bUnassociatedLinear(paint.y,paint.w)*shapeAlpha,
                bUnassociatedLinear(paint.z,paint.w)*shapeAlpha,shapeAlpha);
            if(track)emitted=bContribution(result,shape,paint.w,mode,emitted,x,y,eSeed(0));
            result=bCompositeFillAtSpace(result,shape,1.0F,mode,x,y,eSeed(0),false,E_SPACE,paint.w);
        }
    }
    if(E_PARAM(224)!=0.0F) {
        if(track)emitted=bContribution(result,BVec4(E_PARAM(232),E_PARAM(233),E_PARAM(234),E_PARAM(235)),
            E_PARAM(242)*E_MASK(9),int(E_PARAM(241)),emitted,x,y,eSeed(9));
        result=bCompositeAt(result,BVec4(E_PARAM(232),E_PARAM(233),E_PARAM(234),E_PARAM(235)),
            E_PARAM(242)*E_MASK(9),int(E_PARAM(241)),x,y,eSeed(9),false);
        if(track)emitted=bContribution(result,eColor(7),E_PARAM(226)*E_MASK(8),int(E_PARAM(225)),emitted,x,y,eSeed(8));
        result=bCompositeAt(result,eColor(7),E_PARAM(226)*E_MASK(8),int(E_PARAM(225)),x,y,eSeed(8),false);
    }
    if(track)emitted=eEmission(result,emitted,0,E_MASK(0),x,y);
    result=eOutside(result,0,E_MASK(0),x,y);
    return result;
}
E_INLINE BVec4 eComposite(BVec4 backdrop,BVec4 base,float x,float y,float opacity,int mode,
    float coverage,bool protectedCoverage,float fill,float maskCoverage)
{
    if(E_PARAM(21)>0.5F)coverage*=maskCoverage;
    else base=BVec4(base.x*maskCoverage,base.y*maskCoverage,base.z*maskCoverage,base.w*maskCoverage);
    float shapeAlpha=base.w;
    if(E_PARAM(22)<0.5F) {
        // Interior paint may fill transparent source pixels, but never the
        // arbitrary padding allocated for a shadow or blur.
        float frame=bClamp(x-E_PARAM(256)+0.5F,0.0F,1.0F)*bClamp(E_PARAM(256)+E_PARAM(258)-x+0.5F,0.0F,1.0F)
            *bClamp(y-E_PARAM(257)+0.5F,0.0F,1.0F)*bClamp(E_PARAM(257)+E_PARAM(259)-y+0.5F,0.0F,1.0F);
        shapeAlpha=bMax(base.w,frame*(E_PARAM(21)>0.5F?1.0F:maskCoverage));
    }
    BVec4 emitted;
    BVec4 result=eCompositePass(backdrop,base,x,y,mode==20?0:mode,emitted,mode==20&&!protectedCoverage,fill,shapeAlpha);
    if(mode==20) {
        // Resolve the complete styled contribution against the actual backdrop.
        // Track emitted color without subtracting the surviving backdrop.
        if(protectedCoverage) {
            if(!bDissolve(opacity,x,y,eSeed(0)))return backdrop;
            opacity=1.0F;
        } else {
            float alpha=emitted.w;
            if(alpha<=0.0F || !bDissolve(alpha*opacity*coverage,x,y,eSeed(0)))return backdrop;
            return BVec4(emitted.x==alpha?1.0F:emitted.x/alpha,
                emitted.y==alpha?1.0F:emitted.y/alpha,emitted.z==alpha?1.0F:emitted.z/alpha,1.0F);
        }
    }
    // One group-contribution opacity, not opacity applied repeatedly to each
    // generated pass. Final crop coverage uses this same contribution boundary.
    float a=bClamp(opacity*coverage,0.0F,1.0F);
    // Preserve the historical arithmetic for linear documents, including its
    // endpoint rounding. Encoded documents mix group contribution in sRGB.
    if(E_SPACE==0)return BVec4(backdrop.x+(result.x-backdrop.x)*a,backdrop.y+(result.y-backdrop.y)*a,
        backdrop.z+(result.z-backdrop.z)*a,backdrop.w+(result.w-backdrop.w)*a);
    return bMixSpace(backdrop,result,a,E_SPACE);
}
