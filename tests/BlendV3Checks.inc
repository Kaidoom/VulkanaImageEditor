// Behavioral contract v3. Independent double equations, integer hash oracle,
// and explicit boundary cases; old 20-mode fixtures above remain unchanged.
void extendedRgbAndCoverage()
{
    using namespace blend_detail;
    const auto reference=[](double b,double s,int mode) {
        if(mode==23) return s==0?0.:s==1?1.:s<.5?1-std::min(1.,(1-b)/(2*s)):std::min(1.,b/(2*(1-s)));
        if(mode==24) return std::clamp(b+2*s-1,0.,1.);
        if(mode==25) return s<.5?std::min(b,2*s):std::max(b,2*s-1);
        const auto bi=std::lround(b*255),si=std::lround(s*255);
        if(b==double(float(bi)/255.F)&&s==double(float(si)/255.F))
            return bi+si>255 || (bi+si==255&&bi>127) ? 1.:0.;
        // Subtract the larger operand from one first, keeping tiny endpoint
        // neighbors significant without demanding a wider floating type.
        const double distance=s>=.5 ? b-(1-s) : s-(1-b);
        return distance>0 || (distance==0&&b>.5) ? 1.:0.;
    };
    for(int mode=23;mode<=26;++mode) {
        for(int b=0;b<256;++b)for(int s=0;s<256;++s) {
            const float cb=float(b)/255,cs=float(s)/255;
            near(bSeparable(cb,cs,mode),reference(cb,cs,mode),3e-6,"v3 exhaustive channel-pair reference");
        }
        const std::array points{0.F,std::numeric_limits<float>::denorm_min(),
            127.F/255,std::nextafter(.5F,0.F),.5F,std::nextafter(.5F,1.F),128.F/255,std::nextafter(1.F,0.F),1.F};
        for(float b:points)for(float s:points)
            near(bSeparable(b,s,mode),reference(b,s,mode),3e-6,"v3 exact/neighbor endpoints");
    }
    // Red has greater weighted luminance, but a smaller channel sum than blue
    // plus half-green. These probes distinguish both alternatives and min/max.
    BVec3 red{1,0,0},cyan{0,.25F,1};
    auto dark=bBlend(red,cyan,21),light=bBlend(red,cyan,22);
    check(dark.x==0&&dark.y==.25F&&dark.z==1,"Darker Color selects one complete weighted-lightness triplet");
    check(light.x==1&&light.y==0&&light.z==0,"Lighter Color selects one complete triplet");
    BVec3 tieA{.59F,0,0},tieB{0,.30F,0};
    for(int m:{21,22}) {auto c=bBlend(tieA,tieB,m);check(c.x==tieA.x&&c.y==0,"whole-color exact tie retains backdrop");}
    for(int m:{21,22}) {
        BVec3 a{11.F/255,0,0},b{0,0,30.F/255};const auto c=bBlend(a,b,m);
        check(c.x==a.x&&c.y==a.y&&c.z==a.z,"canonical weighted-lightness ties retain the backdrop");
    }
    const auto hash=[](std::uint32_t n) {
        auto multiply=[](std::uint32_t a,std::uint32_t b){return std::uint32_t((std::uint64_t(a)*b)&0xffffffffULL);};
        n=multiply(n^(n>>16),0x7feb352dU);n=multiply(n^(n>>15),0x846ca68bU);return n^(n>>16);
    };
    int accepted=0,other=0;
    for(int y=-128;y<128;++y)for(int x=-128;x<128;++x) {
        const auto expected=(hash(std::uint32_t(x)^hash(std::uint32_t(y)^12345U))>>8)<(1U<<22);
        const bool gate=bDissolve(.25F,float(x)+.5F,float(y)+.5F,12345);
        check(gate==expected,"unsigned hash and negative-coordinate reference");
        check(!gate||bDissolve(.5F,float(x)+.5F,float(y)+.5F,12345),"opacity increases monotonically admit cells");
        accepted+=gate;other+=gate!=bDissolve(.25F,float(x)+.5F,float(y)+.5F,67890);
        const auto b=decodeColor({23,80,190,127}),s=decodeColor({220,40,30,128});
        auto r=compositeLayer(b,s,.5F,BlendMode::Dissolve,{x+.5,y+.5},12345);
        const bool contributes=bDissolve(s[3]*.5F,float(x)+.5F,float(y)+.5F,12345);
        check(contributes?r[3]==1:r==b,"Dissolve rejected coverage leaves backdrop exactly unchanged");
        if(contributes)near(r[0],s[0]/s[3],0,"accepted source consumes probability without RGB dimming");
        auto protectedResult=compositeLayer({},s,.5F,BlendMode::Dissolve,{x+.5,y+.5},12345,true);
        check(protectedResult[3]==0||protectedResult[3]==s[3],"clipping base keeps its shared fractional coverage");
        const auto adjusted=bAdjustmentBlend({b[0],b[1],b[2],b[3]}, {s[0]*b[3]/s[3],s[1]*b[3]/s[3],s[2]*b[3]/s[3],b[3]},.25F,20,float(x),float(y),12345);
        check(adjusted.w==b[3],"Dissolve adjustment retains input alpha");
    }
    near(double(accepted)/65536,.25,.005,"Dissolve statistical occupancy");
    check(other>10000,"independent appearance seeds decorrelate contributions");
    check(!bDissolve(0,0,0,0)&&bDissolve(1,0,0,0),"coverage endpoints are unconditional");
    std::cout<<"Blend v3: checked 262144 channel pairs, boundaries, whole-color selection and coverage/hash/alpha\n";
}
