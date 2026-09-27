#version 450
layout(push_constant) uniform SelectionPush { vec4 originScale; vec4 viewportStyle; vec4 colorStyle; vec4 canvasRect; vec4 outsideColor; } pc;
layout(location=0) flat in vec4 screenEdge;
layout(location=0) out vec4 outColor;
void main() {
    vec2 a = screenEdge.xy, b = screenEdge.zw, delta = b-a;
    if (pc.colorStyle.w>2.5 && pc.colorStyle.w<3.5) {
        vec2 offset=abs(gl_FragCoord.xy-a)/pc.viewportStyle.z;
        float d=offset.x+offset.y;
        if (d>4.2) discard;
        float alpha=1.0-smoothstep(3.5,4.2,d);
        vec3 ink=d<1.7 ? vec3(0.015) : pc.colorStyle.rgb;
        outColor=vec4(ink*alpha,alpha); return;
    }
    float t = clamp(dot(gl_FragCoord.xy-a, delta)/max(dot(delta,delta),0.0001),0,1);
    float distance = length(gl_FragCoord.xy - mix(a,b,t)) / pc.viewportStyle.z;
    if (pc.colorStyle.w>5.5) {
        // Provisional repair marks: solid accent with a dark keyline. Static
        // geometry has no animation tick and never uploads the raster image.
        if (distance>1.5 || any(lessThan(gl_FragCoord.xy,pc.canvasRect.xy))
            || any(greaterThanEqual(gl_FragCoord.xy,pc.canvasRect.zw))) discard;
        float inner=1.0-smoothstep(.5,1.0,distance);
        float alpha=1.0-smoothstep(1.0,1.5,distance);
        outColor=vec4(mix(vec3(.01),pc.colorStyle.rgb,inner)*alpha,alpha);return;
    }
    if (pc.colorStyle.w>4.5) {
        // Captured adjustment region: static, translucent accent dashes, not
        // moving selection ants. The artwork stays unobscured inside the mask.
        if (distance>1.35 || any(lessThan(gl_FragCoord.xy,pc.canvasRect.xy))
            || any(greaterThanEqual(gl_FragCoord.xy,pc.canvasRect.zw))) discard;
        float along=length(mix(a,b,t)-a)/pc.viewportStyle.z;
        if (mod(along,9.0)>6.0) discard;
        float inner=1.0-smoothstep(0.35,0.8,distance);
        float alpha=.72*(1.0-smoothstep(0.9,1.35,distance));
        outColor=vec4(mix(vec3(0.01),pc.colorStyle.rgb,inner)*alpha,alpha); return;
    }
    if (pc.colorStyle.w>3.5) {
        // Thin static layer frame. The document clip changes only its color,
        // never hides the portion extending into the surrounding workspace.
        if (distance>1.35) discard;
        bool inside=all(greaterThanEqual(gl_FragCoord.xy,pc.canvasRect.xy))
            && all(lessThan(gl_FragCoord.xy,pc.canvasRect.zw));
        vec3 color=inside ? pc.colorStyle.rgb : pc.outsideColor.rgb;
        float inner=1.0-smoothstep(0.4,0.85,distance);
        float alpha=1.0-smoothstep(0.9,1.35,distance);
        outColor=vec4(mix(vec3(0.01),color,inner)*alpha,alpha); return;
    }
    if (distance > 1.5) discard;
    // Logical-pixel ants, not texture texels: crisp at any zoom. The dark
    // under-stroke maintains contrast where the light dashes cross white art.
    float phase = (gl_FragCoord.x + gl_FragCoord.y) / pc.viewportStyle.z - pc.viewportStyle.w;
    float dash = mod(floor(phase),8.0) < 4.0 ? 1.0 : 0.0;
    if (pc.colorStyle.w>0.5) {
        // Solid accent trace, static dashed closing edge, dark contrast keyline.
        float along=length(mix(a,b,t)-a)/pc.viewportStyle.z;
        float pattern=pc.colorStyle.w>1.5 ? (mod(along,8.0)<4.0 ? 1.0 : 0.15) : 1.0;
        float inner=1.0-smoothstep(0.5,0.95,distance);
        float alpha=1.0-smoothstep(1.0,1.5,distance);
        outColor=vec4(mix(vec3(0.015),pc.colorStyle.rgb,inner*pattern)*alpha,alpha); return;
    }
    float inner = 1.0-smoothstep(0.45,0.85,distance);
    float alpha = 1.0-smoothstep(1.0,1.5,distance);
    outColor = vec4(vec3(mix(0.015,0.96,dash*inner)) * alpha, alpha);
}
