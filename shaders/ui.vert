#version 450

// One instance per quad, drawn as 6 vertices (two triangles) of a triangle
// list.  (A strip would need only 4, but MoltenVK warns that Metal cannot
// disable primitive restart for strips.)  Positions are
// in framebuffer pixels (origin top left); the push constant converts them to
// normalized device coordinates.

layout(push_constant) uniform PushConstants {
    vec2 framebuffer_size;
    float encode_srgb;  // 1 if the swapchain format is *_SRGB (see ui.frag)
} pc;

layout(location = 0) in vec4 in_rect;    // x, y, w, h in pixels
layout(location = 1) in vec4 in_params;  // corner radius, border width, kind (0 = rounded rect, 1 = glyph), unused
layout(location = 2) in vec4 in_fill;    // straight (non-premultiplied) RGBA
layout(location = 3) in vec4 in_border;  // straight RGBA
layout(location = 4) in vec4 in_uv;      // glyph: u0, v0, u1, v1 in atlas texels

layout(location = 0) out vec2 v_local;          // pixel offset from the rect centre
layout(location = 1) flat out vec4 v_shape;     // half width, half height, radius, border width
layout(location = 2) flat out vec4 v_fill;
layout(location = 3) flat out vec4 v_border;
layout(location = 4) out vec2 v_uv;
layout(location = 5) flat out float v_kind;

void main()
{
    const int corner_index[6] = int[](0, 1, 2, 2, 1, 3);
    int c = corner_index[gl_VertexIndex];
    vec2 corner = vec2(float(c & 1), float(c >> 1));
    float kind = in_params.z;

    // Rounded rects get one pixel of padding so the antialiased edge is not
    // cut off when the rect is not pixel aligned.  Glyph quads map texels 1:1.
    float pad = (kind < 0.5) ? 1.0 : 0.0;

    vec2 half_size = in_rect.zw * 0.5;
    vec2 centre = in_rect.xy + half_size;
    vec2 local = (corner * 2.0 - 1.0) * (half_size + pad);
    vec2 pos = centre + local;

    gl_Position = vec4(pos / pc.framebuffer_size * 2.0 - 1.0, 0.0, 1.0);

    v_local = local;
    v_shape = vec4(half_size, in_params.x, in_params.y);
    v_fill = in_fill;
    v_border = in_border;
    v_uv = mix(in_uv.xy, in_uv.zw, corner);
    v_kind = kind;
}
