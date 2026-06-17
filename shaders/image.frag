#version 440

// Display a decoded HDR image (fp16 linear, libultrahdr convention 1.0 = 203 nits)
// on the Windows-scRGB surface (1.0 = 80 nits), so multiply by 203/80 = 2.5375.
//
// Optional A/B compare: left of `splitX` simulates SDR by clipping highlights to
// the SDR/reference-white level; right of it shows the full HDR rendition.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Ubo {
    float scale;   // 203/80 by default
    float splitX;  // <= 0 disables the SDR/HDR split
    float pad0;
    float pad1;
} u;

layout(binding = 1) uniform sampler2D tex;

void main()
{
    // Texture row 0 is the top of the image, and v_uv.y = 0 is the top of the
    // screen here, so sample directly -- no Y flip (that turned images upside down).
    vec3 c = texture(tex, v_uv).rgb;

    if (u.splitX > 0.0) {
        // thin black divider
        if (abs(v_uv.x - u.splitX) < 0.0012) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
        // left half: SDR simulation -> clip highlights above reference white
        if (v_uv.x < u.splitX)
            c = min(c, vec3(1.0));
    }

    fragColor = vec4(c * u.scale, 1.0);
}
