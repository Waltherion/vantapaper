#version 440

// Fase 0a HDR true-black test pattern.
//
// Swapchain is QRhiSwapChain::HDRExtendedSrgbLinear (scRGB-style extended-linear):
//   value 1.0  = reference SDR white
//   value > 1.0 = HDR headroom (brighter than SDR white)
//   value 0.0  = nothing emitted -> on an OLED in HDR mode this must be a fully OFF pixel.
//
// Layout (across the screen, left -> right):
//   [0.00 .. 0.45]  pure black (0,0,0)          <- must be indistinguishable from the bezel
//   [0.45 .. 0.55]  SDR-white reference (1.0)   <- "normal white" anchor
//   [0.55 .. 1.00]  HDR gradient 1.0 -> 8.0      <- must visibly glow brighter than the anchor

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

void main()
{
    float x = v_uv.x;
    vec3 c;
    if (x < 0.45) {
        c = vec3(0.0);                       // true black
    } else if (x < 0.55) {
        c = vec3(1.0);                       // SDR white reference
    } else {
        float t = clamp((x - 0.55) / 0.45, 0.0, 1.0);
        c = vec3(mix(1.0, 8.0, t));          // HDR headroom gradient
    }
    fragColor = vec4(c, 1.0);
}
