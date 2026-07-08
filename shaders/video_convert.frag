#version 440

// Video frame -> the pipeline's texture convention: linear RGB, BT.709 primaries,
// 1.0 = 203 cd/m^2, alpha 1. Renders into the current transition texture
// (m_tex[cur], RGBA16F), so image.frag's transitions + SDR tone-mapping treat a
// video frame exactly like a decoded still.
//
// The YUV -> RGB matrix and bias come from the CPU (yuvToRgbMatrix in
// video_source.cpp) with range expansion + bit-depth scale folded in, so this
// shader is one mat3+bias regardless of source flavour (full/limited, 8/10-bit,
// P010 high-bits / p10le low-bits, BT.709/601/2020 matrix).
//
// The transfer/primaries math REPLICATES src/color_math.h exactly -- keep in sync.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform U {
    mat4 yuvToRgb;  // upper-left 3x3 used; column-major (built transposed on CPU)
    vec4 yuvBias;   // xyz added after the matrix
    vec4 params;    // x: transfer (0 sRGB, 1 PQ, 2 HLG)
                    // y: bt2020 primaries (>0.5 -> convert to BT.709)
                    // z: planar chroma (>0.5 -> sample U and V from separate planes)
                    // w: unused
} u;

layout(binding = 1) uniform sampler2D texY;  // luma: R8 / R16
layout(binding = 2) uniform sampler2D texC;  // semi-planar: RG8/RG16 interleaved UV; planar: U plane
layout(binding = 3) uniform sampler2D texC2; // planar: V plane (texC bound again when semi-planar)

// PQ (SMPTE ST 2084) EOTF -- colormath::pqEotf.
float pqEotf(float e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    e = clamp(e, 0.0, 1.0);
    float ep = pow(e, 1.0 / m2);
    float num = max(ep - c1, 0.0);
    float den = c2 - c3 * ep;
    return pow(num / den, 1.0 / m1);
}

// HLG inverse OETF -- colormath::hlgSceneLinear.
float hlgSceneLinear(float x)
{
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    x = clamp(x, 0.0, 1.0);
    return x <= 0.5 ? (x * x) / 3.0 : (exp((x - c) / a) + b) / 12.0;
}

// sRGB EOTF -- colormath::srgbEotf.
float srgbEotf(float c)
{
    c = max(c, 0.0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// colormath::linChan -- linearise to the 1.0 = 203 cd/m^2 convention.
float linChan(float c, int tf)
{
    if (tf == 1) return pqEotf(c) * 10000.0 / 203.0;
    if (tf == 2) return hlgSceneLinear(c) * 1000.0 / 203.0;
    return srgbEotf(c);
}

// colormath::bt2020ToBt709.
vec3 bt2020ToBt709(vec3 c)
{
    return vec3(
         1.660491 * c.r - 0.587641 * c.g - 0.072850 * c.b,
        -0.124550 * c.r + 1.132900 * c.g - 0.008349 * c.b,
        -0.018151 * c.r - 0.100579 * c.g + 1.118730 * c.b);
}

void main()
{
    vec3 yuv;
    yuv.x = texture(texY, v_uv).r;
    if (u.params.z > 0.5) {
        yuv.y = texture(texC, v_uv).r;   // planar: separate U / V planes
        yuv.z = texture(texC2, v_uv).r;
    } else {
        yuv.yz = texture(texC, v_uv).rg; // semi-planar: interleaved UV
    }

    vec3 rgb = mat3(u.yuvToRgb) * yuv + u.yuvBias.xyz;
    rgb = clamp(rgb, 0.0, 1.0);

    int tf = int(u.params.x + 0.5);
    rgb = vec3(linChan(rgb.r, tf), linChan(rgb.g, tf), linChan(rgb.b, tf));
    if (u.params.y > 0.5)
        rgb = max(bt2020ToBt709(rgb), vec3(0.0));

    fragColor = vec4(rgb, 1.0);
}
