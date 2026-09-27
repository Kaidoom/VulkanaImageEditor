// Shared C++/GLSL convex clipping. Coordinates are relative to the sample
// center to avoid cancellation in area sums for far-off-origin crop frames.
// A convex quad clipped against eight planes has at most twelve vertices.
C_INLINE float cCoverage(CVec2 dx,CVec2 dy,CVec4 rect,CVec4 cuts)
{
    CVec2 radius=(cAbs(dx)+cAbs(dy))*.5F;
    if(rect.z<=rect.x || rect.w<=rect.y || rect.z<=-radius.x || rect.w<=-radius.y
        || rect.x>=radius.x || rect.y>=radius.y)return 0.0F;
    bool bevel=cuts.x>0.0F||cuts.y>0.0F||cuts.z>0.0F||cuts.w>0.0F;
    bool fullyInside=rect.x<=-radius.x && rect.y<=-radius.y && rect.z>=radius.x && rect.w>=radius.y;
    if(bevel){
        // Exact projected half-widths along the two diagonal plane normals.
        // Interior pixels stay constant-work; only edge pixels need clipping.
        float sumRadius=(cAbs(dx.x+dx.y)+cAbs(dy.x+dy.y))*.5F;
        float differenceRadius=(cAbs(dx.x-dx.y)+cAbs(dy.x-dy.y))*.5F;
        CVec4 b=CVec4(rect.x+rect.y+cuts.x,-rect.z+rect.y+cuts.y,
            -rect.z-rect.w+cuts.z,rect.x-rect.w+cuts.w);
        if(b.x>=sumRadius||b.y>=differenceRadius||b.z>=sumRadius||b.w>=differenceRadius)return 0.0F;
        fullyInside=fullyInside && b.x<=-sumRadius && b.y<=-differenceRadius
            && b.z<=-sumRadius && b.w<=-differenceRadius;
    }
    float fullArea=cAbs(dx.x*dy.y-dx.y*dy.x);
    if(fullArea<=0.0F)return 0.0F;
    if(fullyInside)return 1.0F;
    CVec2 vertices[12];
    vertices[0]=(dx+dy)*-.5F; vertices[1]=(dx-dy)*.5F;
    vertices[2]=(dx+dy)*.5F; vertices[3]=(dy-dx)*.5F;
    int count=4;
    for(int side=0;side<(bevel?8:4);++side){
        CVec2 next[12]; int n=0;
        CVec2 normal;
        float boundary;
        if(side==0){normal=CVec2(1.0F,0.0F);boundary=rect.x;}
        else if(side==1){normal=CVec2(-1.0F,0.0F);boundary=-rect.z;}
        else if(side==2){normal=CVec2(0.0F,1.0F);boundary=rect.y;}
        else if(side==3){normal=CVec2(0.0F,-1.0F);boundary=-rect.w;}
        else if(side==4){normal=CVec2(1.0F,1.0F);boundary=rect.x+rect.y+cuts.x;}
        else if(side==5){normal=CVec2(-1.0F,1.0F);boundary=-rect.z+rect.y+cuts.y;}
        else if(side==6){normal=CVec2(-1.0F,-1.0F);boundary=-rect.z-rect.w+cuts.z;}
        else {normal=CVec2(1.0F,-1.0F);boundary=rect.x-rect.w+cuts.w;}
        for(int i=0;i<count;++i){
            CVec2 a=vertices[i],b=vertices[(i+1)%count];
            float av=normal.x*a.x+normal.y*a.y,bv=normal.x*b.x+normal.y*b.y;
            bool insideA=av>=boundary,insideB=bv>=boundary;
            if(insideA)next[n++]=a;
            if(insideA!=insideB)next[n++]=a+(b-a)*((boundary-av)/(bv-av));
        }
        count=n;
        if(count<3)return 0.0F;
        for(int i=0;i<count;++i)vertices[i]=next[i];
    }
    float area=0.0F;
    for(int i=0;i<count;++i){CVec2 a=vertices[i],b=vertices[(i+1)%count];area+=a.x*b.y-a.y*b.x;}
    return cClamp(cAbs(area)*.5F/fullArea,0.0F,1.0F);
}
