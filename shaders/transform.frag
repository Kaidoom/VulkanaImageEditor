#version 450
layout(push_constant) uniform TransformPush {
    vec4 topCorners;
    vec4 bottomCorners;
    vec4 style;
    vec4 accent;
    vec4 cutX;
    vec4 cutY;
} pc;
layout(location=0) out vec4 outColor;

float segmentDistance(vec2 p, vec2 a, vec2 b) {
    vec2 axis = b-a;
    float t = clamp(dot(p-a,axis)/max(dot(axis,axis),1e-12),0.0,1.0);
    return length(p-a-t*axis);
}
void main() {
    // Work in logical screen pixels: hit targets, borders and handles stay
    // the same physical UI size at any zoom and fractional display scale.
    vec2 scale = max(pc.style.xy, vec2(0.01));
    vec2 p = gl_FragCoord.xy/scale;
    vec2 corners[4] = vec2[4](pc.topCorners.xy/scale, pc.topCorners.zw/scale,
        pc.bottomCorners.xy/scale, pc.bottomCorners.zw/scale);
    vec2 u=corners[1]-corners[0],v=corners[3]-corners[0];
    vec2 before[4]=vec2[4](corners[0]+v*pc.cutY.x,corners[1]-u*pc.cutX.y,
        corners[2]-v*pc.cutY.z,corners[3]+u*pc.cutX.w);
    vec2 after[4]=vec2[4](corners[0]+u*pc.cutX.x,corners[1]+v*pc.cutY.y,
        corners[2]-u*pc.cutX.z,corners[3]-v*pc.cutY.w);
    vec2 handles[8];
    float line = 1e30, handleDistance=1e30;
    for (int i=0;i<4;++i){
        line=min(line,segmentDistance(p,before[i],after[i]));
        line=min(line,segmentDistance(p,after[i],before[(i+1)%4]));
        handles[2*i]=pc.style.w>0.5?(before[i]+after[i])*0.5:corners[i];
        // Shared geometry contract: core::geometryTransformHandles uses these
        // projected-edge midpoints too, not projected local midpoints.
        handles[2*i+1]=(corners[i]+corners[(i+1)%4])*0.5;
        handleDistance=min(handleDistance,length(p-handles[2*i]));
        handleDistance=min(handleDistance,length(p-handles[2*i+1]));
    }
    vec2 center=(corners[0]+corners[2])*0.5;
    if (line>7.6 && handleDistance>7.6 && length(p-center)>6.0) discard;
    vec4 color = vec4(0.01,0.012,0.02,1.0-smoothstep(1.7,2.4,line));
    color.rgb = mix(color.rgb,pc.accent.rgb,1.0-smoothstep(0.55,1.15,line));
    // Corners paint last when the box is sub-pixel/flattened; no division by
    // layer scale, no disappearing handles while dragging through zero.
    for (int order=0;order<9;++order) {
        int i = order < 4 ? order*2+1 : (order-4)*2;
        if (order==8) {
            i=int(pc.style.z);
            if (i<0 || i>7) break;
        }
        vec2 h = handles[i];
        vec2 d = abs(p-h);
        float box = max(d.x,d.y);
        float alpha = 1.0-smoothstep(4.6,5.3,box);
        vec3 fill = abs(pc.style.z-float(i))<0.1 ? mix(pc.accent.rgb,vec3(1.0),0.58) : pc.accent.rgb;
        fill = mix(fill,vec3(0.01,0.012,0.02),smoothstep(3.0,3.8,box));
        color = mix(color,vec4(fill,1.0),alpha);
    }
    float pivot=min(segmentDistance(p,center-vec2(4,0),center+vec2(4,0)),
        segmentDistance(p,center-vec2(0,4),center+vec2(0,4)));
    color=mix(color,vec4(0.01,0.012,0.02,1.0),1.0-smoothstep(1.0,1.8,pivot));
    color=mix(color,vec4(pc.accent.rgb,1.0),1.0-smoothstep(0.3,0.8,pivot));
    if (color.a<=0.0) discard;
    outColor=color;
}
