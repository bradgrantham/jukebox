#version 450

// Output is premultiplied alpha; the pipeline blends with
// (ONE, ONE_MINUS_SRC_ALPHA).  Colours are treated as sRGB-encoded values and
// blended in that space, like classic 2D toolkits.  If the swapchain format is
// an _SRGB one (hardware encodes on write), the colour is decoded to linear
// first so the stored values come out the same.

layout(push_constant) uniform PushConstants {
    vec2 framebuffer_size;
    float encode_srgb;
} pc;

layout(binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 v_local;
layout(location = 1) flat in vec4 v_shape;
layout(location = 2) flat in vec4 v_fill;
layout(location = 3) flat in vec4 v_border;
layout(location = 4) in vec2 v_uv;
layout(location = 5) flat in float v_kind;

layout(location = 0) out vec4 out_color;

// Signed distance (pixels) to a rounded box centred on the origin.
float rounded_box_distance(vec2 p, vec2 half_size, float radius)
{
    vec2 q = abs(p) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

vec3 srgb_to_linear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec4 premultiply(vec4 c)
{
    return vec4(c.rgb * c.a, c.a);
}

void main()
{
    vec4 color;

    if(v_kind > 0.5) {
        float coverage = texture(atlas, v_uv / vec2(textureSize(atlas, 0))).r;
        color = premultiply(vec4(v_fill.rgb, v_fill.a * coverage));
    } else {
        vec2 half_size = v_shape.xy;
        float radius = clamp(v_shape.z, 0.0, min(half_size.x, half_size.y));
        float border_width = v_shape.w;

        float d = rounded_box_distance(v_local, half_size, radius);
        float outer = clamp(0.5 - d, 0.0, 1.0);

        color = premultiply(v_fill);
        if(border_width > 0.0) {
            // Offsetting the distance gives the inner edge of a uniform-width
            // border, with inner corner radius (radius - border_width).
            float inner = clamp(0.5 - (d + border_width), 0.0, 1.0);
            color = mix(premultiply(v_border), color, inner);
        }
        color *= outer;
    }

    if(pc.encode_srgb > 0.5 && color.a > 0.0) {
        vec3 straight = color.rgb / color.a;
        color.rgb = srgb_to_linear(straight) * color.a;
    }

    out_color = color;
}
