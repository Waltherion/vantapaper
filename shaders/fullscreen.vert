#version 440

// Fullscreen triangle generated from gl_VertexIndex (no vertex buffer).
// Vertices: (0,0) (2,0) (0,2) -> NDC (-1,-1) (3,-1) (-1,3) covers the screen.
// v_uv spans 0..1 across the visible area.

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
