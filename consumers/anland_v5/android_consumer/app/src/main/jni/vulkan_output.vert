#version 450

layout(location = 0) out vec2 uv;

void main()
{
    // A positive-height Vulkan viewport maps clip y=-1 to the top, so
    // staging row 0 (v=0) is at the top without a texture-coordinate flip.
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
