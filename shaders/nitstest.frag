#version 440

// Nits staircase to find where the panel clips / rolls off.
//
// Surface is tagged Windows-scRGB: value 1.0 = 80 cd/m^2, so value = nits / 80.
// 8 equal vertical blocks, left -> right:
//   0: BLACK        (0 nits)
//   1: SDR/ref white (203 nits)
//   2: 1500 nits
//   3: 1600 nits
//   4: 1700 nits
//   5: 1800 nits
//   6: 1900 nits
//   7: 2000 nits
// Blocks butt directly against each other (no separators) so edge contrast does
// not trick the eye into seeing brightness steps that aren't there. Where two
// neighbouring high blocks look identical, the panel has maxed out.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

void main()
{
    int n = clamp(int(floor(v_uv.x * 8.0)), 0, 7);

    float nits;
    if      (n == 0) nits = 0.0;
    else if (n == 1) nits = 203.0;
    else if (n == 2) nits = 1500.0;
    else if (n == 3) nits = 1600.0;
    else if (n == 4) nits = 1700.0;
    else if (n == 5) nits = 1800.0;
    else if (n == 6) nits = 1900.0;
    else             nits = 2000.0;

    float v = nits / 80.0; // Windows-scRGB encoding
    fragColor = vec4(v, v, v, 1.0);
}
