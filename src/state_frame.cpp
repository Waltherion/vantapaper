#include "state_frame.h"
#include "hdr_image.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QString>
#include <QtCore/qfloat16.h>

#include <algorithm>
#include <cmath>

// PNG writer adapted from vantaviewer's image_encoder.cpp (pqInverse, pngCrc,
// injectCicp, encodePngBuf). vantapaper's fp16 buffers are always BT.709 after
// decode-side primaries conversion, so the cICP primaries byte is fixed at 1.

namespace {

// Inverse PQ (SMPTE ST 2084) OETF: linear fraction of 10000 cd/m^2 -> encoded [0,1].
float pqInverse(float l)
{
    l = std::clamp(l, 0.0f, 1.0f);
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    const float lp = std::pow(l, m1);
    return std::pow((c1 + c2 * lp) / (1.0f + c3 * lp), m2);
}

float srgbEncode(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

uint32_t pngCrc(const uint8_t *buf, size_t len)
{
    uint32_t table[256];
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        table[n] = c;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// Insert a cICP chunk (primaries, transfer, matrix=0, full-range=1) right after IHDR
// so the decoder reads the PNG's true HDR transfer/primaries.
QByteArray injectCicp(const QByteArray &png, uint8_t primaries, uint8_t transfer)
{
    if (png.size() < 33 || !png.startsWith(QByteArray::fromRawData("\x89PNG\r\n\x1a\n", 8)))
        return png; // not a PNG we recognise; leave untouched

    const uint8_t data[4] = { primaries, transfer, 0, 1 };
    QByteArray chunk;
    auto be32 = [&chunk](uint32_t v) {
        chunk.append(char(v >> 24)); chunk.append(char(v >> 16));
        chunk.append(char(v >> 8));  chunk.append(char(v));
    };
    be32(4); // length of data
    QByteArray typeAndData("cICP");
    typeAndData.append(reinterpret_cast<const char *>(data), 4);
    chunk.append(typeAndData);
    be32(pngCrc(reinterpret_cast<const uint8_t *>(typeAndData.constData()),
                size_t(typeAndData.size())));

    // IHDR is the first chunk: 8-byte signature + (4 len + 4 type + 13 data + 4 crc) = 33.
    QByteArray out = png.left(33);
    out.append(chunk);
    out.append(png.mid(33));
    return out;
}

} // namespace

bool writeStatePng(const QString &outPath, const HdrImage &img)
{
    if (!img.valid())
        return false;
    const qfloat16 *p = reinterpret_cast<const qfloat16 *>(img.rgba16f.data());
    const int w = img.w, h = img.h;

    QByteArray bytes;
    if (img.hdr) {
        // 16-bit PQ, plus a cICP chunk (BT.709 primaries, PQ transfer) injected below.
        QImage out(w, h, QImage::Format_RGBA64);
        for (int y = 0; y < h; ++y) {
            quint16 *row = reinterpret_cast<quint16 *>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const size_t s = (size_t(y) * w + x) * 4;
                const float r = float(p[s + 0]), g = float(p[s + 1]), b = float(p[s + 2]);
                row[x * 4 + 0] = quint16(std::lround(std::clamp(pqInverse(r * 203.0f / 10000.0f), 0.0f, 1.0f) * 65535.0f));
                row[x * 4 + 1] = quint16(std::lround(std::clamp(pqInverse(g * 203.0f / 10000.0f), 0.0f, 1.0f) * 65535.0f));
                row[x * 4 + 2] = quint16(std::lround(std::clamp(pqInverse(b * 203.0f / 10000.0f), 0.0f, 1.0f) * 65535.0f));
                row[x * 4 + 3] = 65535;
            }
        }
        out.setColorSpace(QColorSpace()); // don't embed an sRGB/iCCP profile
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        if (!out.save(&buf, "PNG"))
            return false;
        buf.close();
        bytes = injectCicp(bytes, /*primaries BT.709*/ 1, /*transfer PQ*/ 16);
    } else {
        QImage out(w, h, QImage::Format_RGB888);
        for (int y = 0; y < h; ++y) {
            uchar *row = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const size_t s = (size_t(y) * w + x) * 4;
                row[x * 3 + 0] = uchar(std::lround(srgbEncode(float(p[s + 0])) * 255.0f));
                row[x * 3 + 1] = uchar(std::lround(srgbEncode(float(p[s + 1])) * 255.0f));
                row[x * 3 + 2] = uchar(std::lround(srgbEncode(float(p[s + 2])) * 255.0f));
            }
        }
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        if (!out.save(&buf, "PNG"))
            return false;
        buf.close();
    }

    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}
