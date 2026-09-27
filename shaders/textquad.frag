#version 450
layout(push_constant) uniform Push { vec4 top; vec4 bottom; vec4 color; vec4 clip; } p;
layout(location=0) out vec4 outColor;
void main() {
    if(any(lessThan(gl_FragCoord.xy,p.clip.xy))||any(greaterThanEqual(gl_FragCoord.xy,p.clip.xy+p.clip.zw)))discard;
    outColor=vec4(p.color.rgb*p.color.a,p.color.a);
}
