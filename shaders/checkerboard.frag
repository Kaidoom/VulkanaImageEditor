#version 450

layout(push_constant) uniform CanvasPush {
    vec4 canvasRect;
    vec4 background;
    vec4 light;
    vec4 dark;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 point = gl_FragCoord.xy;
    vec2 canvasMin = pc.canvasRect.xy;
    vec2 canvasMax = canvasMin + pc.canvasRect.zw;
    bool inside = all(greaterThanEqual(point, canvasMin)) && all(lessThan(point, canvasMax));
    if (!inside || pc.canvasRect.z <= 0.0 || pc.canvasRect.w <= 0.0) {
        outColor = pc.background;
        return;
    }

    const float grid = 12.0;
    ivec2 cell = ivec2(floor((point - canvasMin) / grid));
    bool alternate = ((cell.x + cell.y) & 1) != 0;
    outColor = alternate ? pc.dark : pc.light;
}
