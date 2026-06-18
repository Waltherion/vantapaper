#pragma once

#include <cstdint>
#include <vector>

class QString;

// A decoded HDR image: tightly packed RGBA half-float (fp16) pixels, linear,
// using libultrahdr's convention where 1.0 corresponds to 203 cd/m^2.
struct HdrImage {
    int w = 0;
    int h = 0;
    std::vector<uint16_t> rgba16f; // w*h*4 halfs (raw fp16 bits)
    bool hdr = false;              // true HDR content (PQ/HLG/gain-map) vs SDR

    bool valid() const { return w > 0 && h > 0 && rgba16f.size() == size_t(w) * h * 4; }
};

// Decode an UltraHDR (gain-map) JPEG via libultrahdr. Returns an invalid HdrImage
// on failure (diagnostics go to stderr). This is the path mpv-based wallpapers
// lack: it understands the HDR gain-map metadata baked into the JPEG.
HdrImage decodeUltraHdr(const QString &path);

// Decode an AVIF via libavif, linearising PQ/HLG/sRGB and converting BT.2020
// primaries to BT.709, to the same fp16 linear (1.0 = 203 nits) convention.
// AVIF carries true HDR pixels directly -- the right format for graphic/drawn
// content, with none of the gain-map inverse-tonemapping banding.
HdrImage decodeAvif(const QString &path);

// Decode a JPEG-XL via libjxl (HDR PQ/HLG or SDR) into the same fp16 pipeline.
HdrImage decodeJxl(const QString &path);

// Decode a HEIC/HEIF via libheif (HDR PQ/HLG, 10/12-bit, or SDR).
HdrImage decodeHeic(const QString &path);

// Pick the decoder by file extension (.avif, .jxl, .jpg -> HDR-aware; else QImage).
HdrImage decodeImage(const QString &path);
