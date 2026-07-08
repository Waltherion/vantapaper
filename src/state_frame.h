#pragma once

class QString;
struct HdrImage;

// Write an fp16-linear frame (1.0 = 203 nits, BT.709) as a PNG the lockscreen can
// decode: HDR -> 16-bit PQ + cICP chunk (primaries BT.709, transfer PQ); SDR ->
// plain 8-bit sRGB. Used for the per-output state link when the wallpaper is a
// video (the link must point at a decodable IMAGE, not the .mp4).
bool writeStatePng(const QString &outPath, const HdrImage &img);
