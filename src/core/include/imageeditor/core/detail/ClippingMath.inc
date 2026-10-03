// Shared associated-linear clipping math. Restore shared coverage once.
B_INLINE BVec4 bClippingOpaque(BVec4 c)
{
    if(c.w<=0.0F)return BVec4(0,0,0,0);
    return BVec4(c.x/c.w,c.y/c.w,c.z/c.w,1);
}
B_INLINE BVec4 bClippingContainer(BVec4 styled,BVec4 content,BVec4 working)
{
    BVec4 opaque=bClippingOpaque(styled);
    float coverage=bMin(content.w,styled.w);
    return BVec4(styled.x+(working.x-opaque.x)*coverage,
        styled.y+(working.y-opaque.y)*coverage,
        styled.z+(working.z-opaque.z)*coverage,styled.w);
}
