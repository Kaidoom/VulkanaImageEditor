#version 450
layout(location=0) in vec4 edge;
layout(push_constant) uniform SelectionPush { vec4 originScale; vec4 viewportStyle; vec4 colorStyle; vec4 canvasRect; vec4 outsideColor; } pc;
layout(location=0) flat out vec4 screenEdge;
void main() {
    vec2 a = pc.originScale.xy + edge.xy * pc.originScale.zw;
    vec2 b = pc.originScale.xy + edge.zw * pc.originScale.zw;
    vec2 delta = b-a;
    vec2 tangent = length(delta) > 0.0001 ? normalize(delta) : vec2(1,0);
    vec2 normal = vec2(-tangent.y, tangent.x);
    const vec2 corners[6] = vec2[6](vec2(0,-1),vec2(1,-1),vec2(0,1),vec2(0,1),vec2(1,-1),vec2(1,1));
    vec2 corner = corners[gl_VertexIndex];
    float radius = (pc.colorStyle.w>2.5 && pc.colorStyle.w<3.5 ? 4.5 : 1.6) * pc.viewportStyle.z;
    vec2 point = mix(a,b,corner.x) + normal * corner.y * radius + tangent * (corner.x*2-1) * radius;
    gl_Position = vec4(point / pc.viewportStyle.xy * 2.0 - 1.0, 0, 1);
    screenEdge = vec4(a,b);
}
