#version 450
layout(push_constant) uniform Push { vec4 top; vec4 bottom; vec4 color; vec4 clip; } p;
void main() {
    const int indices[6]=int[6](0,1,2,0,2,3);
    vec2 points[4]=vec2[4](p.top.xy,p.top.zw,p.bottom.xy,p.bottom.zw);
    gl_Position=vec4(points[indices[gl_VertexIndex]],0,1);
}
