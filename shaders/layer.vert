#version 450

layout(push_constant) uniform LayerPush {
    mat4 clipFromUnit;
    float opacity;
} pc;

layout(location = 0) out vec2 uv;

const vec2 positions[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main()
{
    vec2 position = positions[gl_VertexIndex];
    uv = position;
    gl_Position = pc.clipFromUnit * vec4(position, 0.0, 1.0);
}

