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

    bool valid() const { return w > 0 && h > 0 && rgba16f.size() == size_t(w) * h * 4; }
};

// Decode an UltraHDR (gain-map) JPEG via libultrahdr. Returns an invalid HdrImage
// on failure (diagnostics go to stderr). This is the path mpv-based wallpapers
// lack: it understands the HDR gain-map metadata baked into the JPEG.
HdrImage decodeUltraHdr(const QString &path);
