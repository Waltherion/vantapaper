#version 440

// Display + transition between two wallpaper textures (tex0, tex1).
// u.curIndex picks which one is the incoming (new) image; the other is outgoing.
// During a switch u.progress animates 0->1; transition type/params select the shape.
// Output is linear scRGB scaled by u.scale (203/80) for the Windows-scRGB surface.

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
} u;

layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;

void main()
{
    vec3 a = texture(tex0, v_uv).rgb;
    vec3 b = texture(tex1, v_uv).rgb;
    bool curIs1 = u.curIndex > 0.5;
    vec3 incoming = curIs1 ? b : a;
    vec3 outgoing = curIs1 ? a : b;

    float p = clamp(u.progress, 0.0, 1.0);
    int t = int(floor(u.ttype + 0.5));

    // f = 1 -> show incoming, f = 0 -> show outgoing
    float f;
    if (u.ttype < 0.0 || p >= 1.0) {
        f = 1.0;
    } else if (t == 1) {
        // angled wipe with a soft edge
        vec2 dir = vec2(cos(u.angle), sin(u.angle));
        vec2 c = (v_uv - 0.5) * vec2(u.aspect, 1.0);
        float d = dot(c, dir) * 0.5 + 0.5;       // ~0..1 along the wipe direction
        float front = p * 1.3 - 0.15;            // sweep fully off->on past the edges
        f = 1.0 - smoothstep(front - 0.06, front + 0.06, d);
    } else if (t == 2) {
        // circle growing from (cx, cy)
        vec2 c = (v_uv - vec2(u.cx, u.cy)) * vec2(u.aspect, 1.0);
        float dist = length(c);
        float r = p * 1.6;                        // 1.6 reaches the far corners
        f = 1.0 - smoothstep(r - 0.05, r + 0.05, dist);
    } else {
        // fade
        f = p;
    }

    vec3 color = mix(outgoing, incoming, f);
    fragColor = vec4(color * u.scale, 1.0);
}
