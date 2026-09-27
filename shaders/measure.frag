#version 450
layout(push_constant) uniform MeasurePush {
    vec4 endpoints;
    vec4 style;
    vec4 accent;
} pc;
layout(location=0) out vec4 outColor;
float segmentDistance(vec2 p, vec2 a, vec2 b) {
    vec2 d=b-a;
    return length(p-a-clamp(dot(p-a,d)/max(dot(d,d),1e-12),0.0,1.0)*d);
}
void main() {
    vec2 scale=max(pc.style.xy,vec2(.01));
    vec2 p=gl_FragCoord.xy/scale, a=pc.endpoints.xy/scale, b=pc.endpoints.zw/scale;
    float d=segmentDistance(p,a,b);
    for(int i=0;i<(pc.style.z>0.5?0:2);++i) {
        vec2 q=i==0?a:b;
        d=min(d,min(segmentDistance(p,q-vec2(4,0),q+vec2(4,0)),
                    segmentDistance(p,q-vec2(0,4),q+vec2(0,4))));
    }
    bool alignment=pc.style.z>0.5;
    float alpha=1.0-smoothstep(alignment?1.0:1.25,alignment?1.6:1.9,d);
    if(alpha<=0.0) discard;
    vec3 rgb=mix(vec3(.008),pc.accent.rgb,1.0-smoothstep(.35,.85,d));
    outColor=vec4(rgb,alpha*pc.accent.a);
}
