#version 450

layout(push_constant) uniform CanvasPush {
    vec4 canvasRect;
    vec4 cursorAndFlags;
    vec4 cursorStyle;
    vec4 pickerStyle;
    vec4 pickerReference;
    vec4 pickerCandidate;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 point = gl_FragCoord.xy;
    if (pc.pickerStyle.x > 0.5) {
        // Logical-pixel UI size independent of document zoom; the middle
        // stays unobscured except for a small high-contrast crosshair.
        vec2 p = (point - pc.cursorAndFlags.xy) / max(pc.pickerStyle.z, 0.01);
        vec2 a = abs(p);
        bool crossDark = (a.x <= 1.5 && a.y <= 5.0) || (a.y <= 1.5 && a.x <= 5.0);
        bool crossLight = (a.x <= 0.5 && a.y <= 4.0) || (a.y <= 0.5 && a.x <= 4.0);
        if (crossDark) {
            outColor = vec4(crossLight ? vec3(0.95) : vec3(0.025), 1.0);
            return;
        }
        float radius = length(p);
        if (pc.pickerStyle.y > 0.5 && radius > 31.0 && radius < 42.0) {
            vec4 swatch = p.y < 0.0 ? pc.pickerCandidate : pc.pickerReference;
            ivec2 cell = ivec2(floor(p / 5.0));
            float checker = ((cell.x + cell.y) & 1) == 0 ? 0.22 : 0.45;
            vec3 color = swatch.rgb * swatch.a + vec3(checker) * (1.0 - swatch.a);
            float edge = min(abs(radius - 32.0), abs(radius - 41.0));
            color = mix(vec3(0.02), color, smoothstep(0.4, 1.2, edge));
            float coverage = smoothstep(31.0, 32.0, radius) * (1.0 - smoothstep(41.0, 42.0, radius));
            outColor = vec4(color * coverage, coverage);
            return;
        }
    }
    bool cursorEnabled = pc.cursorAndFlags.w > 0.5;
    float majorRadius = max(0.5, pc.cursorAndFlags.z);
    float minorRadius = max(0.5, pc.cursorStyle.x);
    float cosine = cos(pc.cursorStyle.z);
    float sine = sin(pc.cursorStyle.z);
    vec2 cursorPoint = point - pc.cursorAndFlags.xy;
    // Positive brush angles are clockwise in the canonical Y-down document
    // space. Apply the inverse rotation before evaluating the ellipse.
    vec2 localPoint = vec2(
        cosine * cursorPoint.x + sine * cursorPoint.y,
        -sine * cursorPoint.x + cosine * cursorPoint.y);
    vec2 radii = vec2(majorRadius, minorRadius);
    vec2 normalizedPoint = localPoint / radii;
    float implicitOuter = dot(normalizedPoint, normalizedPoint) - 1.0;
    vec2 outerGradient = 2.0 * localPoint / (radii * radii);
    float cursorDistance = abs(implicitOuter)
        / max(length(outerGradient), 0.0001);
    bool outerWhite = cursorEnabled && cursorDistance < 0.8;
    bool outerDark = cursorEnabled && cursorDistance >= 0.8 && cursorDistance < 1.8;

    float hardness = pc.cursorStyle.y;
    float implicitHardness = dot(normalizedPoint, normalizedPoint)
        - hardness * hardness;
    float hardnessDistance = abs(implicitHardness)
        / max(length(outerGradient), 0.0001);
    bool hardnessRing = cursorEnabled && pc.cursorStyle.w > 0.5
        && minorRadius * hardness > 1.5 && hardnessDistance < 0.75;
    bool cursorRing = outerWhite || outerDark || hardnessRing;

    if (!cursorRing) {
        discard;
    }
    float alpha = outerWhite ? 0.94 : outerDark ? 0.78 : 0.50;
    vec3 color = outerWhite ? vec3(0.96, 0.97, 1.0)
        : outerDark ? vec3(0.03, 0.035, 0.045)
        : vec3(0.58, 0.66, 1.0);
    outColor = vec4(color * alpha, alpha);
}
