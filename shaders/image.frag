#version 440

// Display + transition between two wallpaper textures (tex0, tex1).
// u.curIndex picks the incoming (new) image; the other is outgoing. During a
// switch u.progress animates 0->1; type/params select the shape.
//
// Output depends on the monitor's current mode (u.sdr):
//   HDR: linear scRGB * scale (203/80) for the Windows-scRGB surface.
//   SDR: highlights rolled off into range, then sRGB-encoded, for an sRGB surface.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform U {
    float scale;
    float progress;  // 0..1
    float ttype;     // 0 fade, 1 wipe, 2 grow, <0 instant
    float curIndex;  // 0 or 1: which sampler holds the incoming image
    float cx;        // grow centre x (0..1)
    float cy;        // grow centre y (0..1)
    float angle;     // wipe direction (radians)
    float aspect;    // output width / height
    float sdr;       // >0.5 -> SDR output (tone-map + sRGB encode)
    float imageHdr;  // >0.5 -> the image is true HDR (needs range compression on SDR)
} u;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;

// Soft highlight roll-off: linear below the knee, asymptotes to 1.0 above it.
float rolloff(float v)
{
    const float k = 0.8;
    return v <= k ? v : k + (1.0 - k) * (1.0 - exp(-(v - k) / (1.0 - k)));
}

vec3 srgbEncode(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main()
{
    bool curIs1 = u.curIndex > 0.5;
    float p = clamp(u.progress, 0.0, 1.0);
    int t = int(floor(u.ttype + 0.5));

    // Per-transition: incoming/outgoing sample coords (default in place) and the
    // incoming coverage weight f (0 = show outgoing, 1 = show incoming).
    vec2 iuv = v_uv;
    vec2 ouv = v_uv;
    float f;
    if (u.ttype < 0.0 || p >= 1.0) {
        f = 1.0;
    } else if (t == 1) {
        vec2 dir = vec2(cos(u.angle), sin(u.angle));
        vec2 c = (v_uv - 0.5) * vec2(u.aspect, 1.0);
        float d = dot(c, dir) * 0.5 + 0.5;
        float front = p * 1.3 - 0.15;
        f = 1.0 - smoothstep(front - 0.06, front + 0.06, d);
    } else if (t == 2) {
        vec2 c = (v_uv - vec2(u.cx, u.cy)) * vec2(u.aspect, 1.0);
        float dist = length(c);
        float r = p * 1.6;
        f = 1.0 - smoothstep(r - 0.05, r + 0.05, dist);
    } else if (t == 3) {
        // slide/push: the incoming image shoves the outgoing one off-screen along a
        // cardinal axis (angle is snapped to a cardinal, so the push stays in range).
        vec2 dir = vec2(cos(u.angle), sin(u.angle));
        float s = dot(v_uv - 0.5, dir) + 0.5; // 0..1 along the push axis
        f = 1.0 - smoothstep(p - 0.003, p + 0.003, s);
        iuv = v_uv + dir * (1.0 - p); // incoming slides in from the trailing edge
        ouv = v_uv - dir * p;         // outgoing is pushed toward the leading edge
    } else {
        f = p;
    }

    vec3 incoming = curIs1 ? texture(tex1, iuv).rgb : texture(tex0, iuv).rgb;
    vec3 outgoing = curIs1 ? texture(tex0, ouv).rgb : texture(tex1, ouv).rgb;
    vec3 color = mix(outgoing, incoming, f);

    if (u.sdr > 0.5) {
        if (u.imageHdr > 0.5) {
            // True HDR content on an SDR monitor: compress the whole range (Reinhard
            // with a white point) so it reads like a normal photo, not a bright wash.
            float L = dot(color, vec3(0.2126, 0.7152, 0.0722));
            const float Lw = 6.0;
            float Lt = L * (1.0 + L / (Lw * Lw)) / (1.0 + L);
            color *= (L > 1e-4) ? (Lt / L) : 1.0;
        } else {
            // SDR content: roll highlights off (near-identity), keep white.
            color = vec3(rolloff(color.r), rolloff(color.g), rolloff(color.b));
        }
        fragColor = vec4(srgbEncode(color), 1.0);
    } else {
        // HDR monitor: linear scRGB scaled for the Windows-scRGB surface.
        fragColor = vec4(color * u.scale, 1.0);
    }
}
