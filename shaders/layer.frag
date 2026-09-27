#version 450

// Content is already blended in a transparent linear-float image. Only now
// place it over the checkerboard; no content layer can blend with the UI.
layout(set = 0, binding = 0) uniform sampler2D composition;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texelFetch(composition,ivec2(gl_FragCoord.xy),0);
}
