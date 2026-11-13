#version 450

// Content is already blended in a transparent linear-float image. Place it
// over the checkerboard before the final sRGB attachment conversion. The
// fixed-function sRGB blend path can quantize translucent coverage differently
// across GPUs; it must not choose the precision of the document preview.
layout(set = 0, binding = 0) uniform sampler2D composition;
layout(push_constant) uniform CanvasBackground {
    vec4 canvasRect;
    vec4 background;
    vec4 light;
    vec4 dark;
} pc;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 point = gl_FragCoord.xy;
    bool inside = all(greaterThanEqual(point, pc.canvasRect.xy))
        && all(lessThan(point, pc.canvasRect.xy + pc.canvasRect.zw));
    if (!inside || pc.canvasRect.z <= 0.0 || pc.canvasRect.w <= 0.0) {
        outColor = pc.background;
        return;
    }
    ivec2 cell = ivec2(floor((point - pc.canvasRect.xy) / 12.0));
    vec3 checker = ((cell.x + cell.y) & 1) != 0 ? pc.dark.rgb : pc.light.rgb;
    vec4 content = texelFetch(composition, ivec2(point), 0);
    outColor = vec4(content.rgb + checker * (1.0 - content.a), 1.0);
}
