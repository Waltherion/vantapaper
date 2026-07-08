#pragma once

// Shared colour math for the fp16-linear pipeline (1.0 = 203 cd/m^2, BT.709).
// Used by the image decoders (hdr_image.cpp) and the video decoder's CPU
// first-frame conversion (video_source.cpp). The GPU video-convert shader
// (shaders/video_convert.frag) replicates these EXACT formulas -- keep in sync.

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace colormath {

// PQ (SMPTE ST 2084) EOTF: encoded [0,1] -> linear fraction of 10000 cd/m^2.
inline float pqEotf(float e)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    e = std::clamp(e, 0.0f, 1.0f);
    const float ep = std::pow(e, 1.0f / m2);
    const float num = std::max(ep - c1, 0.0f);
    const float den = c2 - c3 * ep;
    return std::pow(num / den, 1.0f / m1);
}

// HLG inverse OETF (scene linear [0,1]); display OOTF approximated by peak scale.
inline float hlgSceneLinear(float x)
{
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.5f ? (x * x) / 3.0f : (std::exp((x - c) / a) + b) / 12.0f;
}

inline float srgbEotf(float c)
{
    c = std::max(c, 0.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Linear BT.2020 -> linear BT.709 primaries.
inline void bt2020ToBt709(float &r, float &g, float &b)
{
    const float R =  1.660491f * r - 0.587641f * g - 0.072850f * b;
    const float G = -0.124550f * r + 1.132900f * g - 0.008349f * b;
    const float B = -0.018151f * r - 0.100579f * g + 1.118730f * b;
    r = R; g = G; b = B;
}

// Shared transfer model used by all decoders.
enum class Tf { SRGB, PQ, HLG, Linear };

// Linearise one encoded channel to the fp16 convention (1.0 = 203 cd/m^2).
inline float linChan(float c, Tf t)
{
    switch (t) {
    case Tf::PQ:     return pqEotf(c) * 10000.0f / 203.0f;
    case Tf::HLG:    return hlgSceneLinear(c) * 1000.0f / 203.0f;
    case Tf::Linear: return c;
    default:         return srgbEotf(c);
    }
}

// Run fn(y) for every image row, split across all CPU cores. The per-pixel
// linearisation is the decode bottleneck on large images; each row writes a
// distinct output region, so this is race-free.
template <typename F>
inline void parallelRows(int height, F &&fn)
{
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0)
        n = 1;
    if (height < 128 || n <= 1) {
        for (int y = 0; y < height; ++y)
            fn(y);
        return;
    }
    n = std::min<unsigned>(n, unsigned(height));
    const int per = (height + int(n) - 1) / int(n);
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (unsigned t = 0; t < n; ++t) {
        const int y0 = int(t) * per;
        const int y1 = std::min(height, y0 + per);
        if (y0 >= y1)
            break;
        threads.emplace_back([&fn, y0, y1]() {
            for (int y = y0; y < y1; ++y)
                fn(y);
        });
    }
    for (auto &th : threads)
        th.join();
}

} // namespace colormath
