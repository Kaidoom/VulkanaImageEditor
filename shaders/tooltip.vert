#version 450
layout(push_constant) uniform TooltipPush {
    vec4 rect;
    vec4 viewportAndUv;
} pc;
layout(location = 0) out vec2 uv;
const vec2 corners[6] = vec2[](
    vec2(0,0), vec2(1,0), vec2(1,1), vec2(0,0), vec2(1,1), vec2(0,1));
void main()
{
    vec2 p = corners[gl_VertexIndex];
    uv = p * pc.viewportAndUv.zw;
    gl_Position = vec4((pc.rect.xy + p * pc.rect.zw) / pc.viewportAndUv.xy * 2.0 - 1.0, 0, 1);
}
