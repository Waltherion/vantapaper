#include "hdr_image.h"

#include <QFile>
#include <QString>
#include <QImage>
#include <QtCore/qfloat16.h>

#include <ultrahdr_api.h>
#include <avif/avif.h>
#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

HdrImage decodeUltraHdr(const QString &path)
{
    HdrImage result;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "vantapaper: cannot open image %s\n", qPrintable(path));
        return result;
    }
    QByteArray bytes = f.readAll();

    uhdr_codec_private_t *dec = uhdr_create_decoder();

    uhdr_compressed_image_t in{};
    in.data = bytes.data();
    in.data_sz = size_t(bytes.size());
    in.capacity = size_t(bytes.size());
    in.cg = UHDR_CG_UNSPECIFIED;
    in.ct = UHDR_CT_UNSPECIFIED;
    in.range = UHDR_CR_UNSPECIFIED;

    auto ok = [](uhdr_error_info_t e, const char *what) -> bool {
        if (e.error_code != UHDR_CODEC_OK) {
            std::fprintf(stderr, "vantapaper: %s failed (%d): %s\n",
                         what, int(e.error_code), e.has_detail ? e.detail : "");
            return false;
        }
        return true;
    };

    const bool decoded =
        ok(uhdr_dec_set_image(dec, &in), "uhdr_dec_set_image")
        && ok(uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_64bppRGBAHalfFloat), "uhdr_dec_set_out_img_format")
        && ok(uhdr_dec_set_out_color_transfer(dec, UHDR_CT_LINEAR), "uhdr_dec_set_out_color_transfer")
        && ok(uhdr_decode(dec), "uhdr_decode");

    if (decoded) {
        uhdr_raw_image_t *out = uhdr_get_decoded_image(dec);
        if (out && out->fmt == UHDR_IMG_FMT_64bppRGBAHalfFloat && out->planes[UHDR_PLANE_PACKED]) {
            result.w = int(out->w);
            result.h = int(out->h);
            result.rgba16f.resize(size_t(out->w) * out->h * 4);

            const uint16_t *src = static_cast<const uint16_t *>(out->planes[UHDR_PLANE_PACKED]);
            const size_t srcStridePixels = out->stride[UHDR_PLANE_PACKED]; // stride in pixels
            const size_t rowBytes = size_t(out->w) * 4 * sizeof(uint16_t);
            for (unsigned y = 0; y < out->h; ++y) {
                std::memcpy(&result.rgba16f[size_t(y) * out->w * 4],
                            src + size_t(y) * srcStridePixels * 4,
                            rowBytes);
            }
            std::fprintf(stderr, "vantapaper: decoded UltraHDR %dx%d (fp16 linear)\n", result.w, result.h);
        } else {
            std::fprintf(stderr, "vantapaper: decoded image has unexpected format\n");
        }
    }

    uhdr_release_decoder(dec);
    return result;
}

// --- AVIF -------------------------------------------------------------------

namespace {

// PQ (SMPTE ST 2084) EOTF: encoded [0,1] -> linear fraction of 10000 cd/m^2.
float pqEotf(float e)
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
float hlgSceneLinear(float x)
{
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.5f ? (x * x) / 3.0f : (std::exp((x - c) / a) + b) / 12.0f;
}

float srgbEotf(float c)
{
    c = std::max(c, 0.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Linear BT.2020 -> linear BT.709 primaries.
void bt2020ToBt709(float &r, float &g, float &b)
{
    const float R =  1.660491f * r - 0.587641f * g - 0.072850f * b;
    const float G = -0.124550f * r + 1.132900f * g - 0.008349f * b;
    const float B = -0.018151f * r - 0.100579f * g + 1.118730f * b;
    r = R; g = G; b = B;
}

} // namespace

HdrImage decodeAvif(const QString &path)
{
    HdrImage result;

    avifDecoder *dec = avifDecoderCreate();
    dec->maxThreads = 4;
    avifImage *img = avifImageCreateEmpty();

    const QByteArray pathBytes = path.toLocal8Bit();
    avifResult r = avifDecoderReadFile(dec, img, pathBytes.constData());
    if (r != AVIF_RESULT_OK) {
        std::fprintf(stderr, "vantapaper: AVIF decode failed: %s\n", avifResultToString(r));
        avifImageDestroy(img);
        avifDecoderDestroy(dec);
        return result;
    }

    const int depth = int(img->depth);
    avifTransferCharacteristics tc = img->transferCharacteristics;
    avifColorPrimaries prim = img->colorPrimaries;

    // Real-world HDR (e.g. Lightroom) often leaves CICP "unspecified" and defines
    // the colour space via an embedded ICC profile instead. Infer transfer and
    // primaries from the ICC profile description in that case.
    if ((tc == AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED
         || tc == AVIF_TRANSFER_CHARACTERISTICS_UNKNOWN)
        && img->icc.size > 0) {
        if (cmsHPROFILE p = cmsOpenProfileFromMem(img->icc.data, cmsUInt32Number(img->icc.size))) {
            char desc[256] = { 0 };
            cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", desc, sizeof(desc));
            cmsCloseProfile(p);
            const QString d = QString::fromLatin1(desc).toLower();
            if (d.contains("pq") || d.contains("2100") || d.contains("2084"))
                tc = AVIF_TRANSFER_CHARACTERISTICS_PQ;
            else if (d.contains("hlg"))
                tc = AVIF_TRANSFER_CHARACTERISTICS_HLG;
            else if (d.contains("linear"))
                tc = AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
            else
                tc = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
            if (d.contains("2020") || d.contains("2100"))
                prim = AVIF_COLOR_PRIMARIES_BT2020;
            std::fprintf(stderr, "vantapaper: AVIF CICP unspecified; ICC='%s' -> transfer=%d\n",
                         desc, int(tc));
        }
    }

    const bool isBt2020 = (prim == AVIF_COLOR_PRIMARIES_BT2020);

    // A wallpaper never needs 45 MP. Downscale huge images before the per-pixel
    // linearisation so loading stays fast (these samples are 8256x5504).
    const uint32_t kMaxDim = 3840;
    if (img->width > kMaxDim || img->height > kMaxDim) {
        const float s = float(kMaxDim) / float(std::max(img->width, img->height));
        avifDiagnostics diag;
        if (avifImageScale(img, uint32_t(img->width * s), uint32_t(img->height * s), &diag) != AVIF_RESULT_OK)
            std::fprintf(stderr, "vantapaper: AVIF downscale failed (continuing at full size)\n");
    }

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, img);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 16;
    rgb.isFloat = AVIF_TRUE; // half-float, normalised [0,1] encoded values
    rgb.maxThreads = 4;

    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK
        || avifImageYUVToRGB(img, &rgb) != AVIF_RESULT_OK) {
        std::fprintf(stderr, "vantapaper: AVIF YUV->RGB conversion failed\n");
        avifRGBImageFreePixels(&rgb);
        avifImageDestroy(img);
        avifDecoderDestroy(dec);
        return result;
    }

    const int w = int(rgb.width), h = int(rgb.height);
    result.w = w;
    result.h = h;
    result.rgba16f.resize(size_t(w) * h * 4);

    // Linearise to the fp16 (1.0 = 203 cd/m^2) convention shared with the JPEG path.
    auto linearize = [tc](float c) -> float {
        switch (tc) {
        case AVIF_TRANSFER_CHARACTERISTICS_PQ:  return pqEotf(c) * 10000.0f / 203.0f;
        case AVIF_TRANSFER_CHARACTERISTICS_HLG: return hlgSceneLinear(c) * 1000.0f / 203.0f;
        case AVIF_TRANSFER_CHARACTERISTICS_LINEAR: return c; // already linear (1.0 ~ ref white)
        default:                                return srgbEotf(c); // sRGB/BT.709 SDR
        }
    };

    qfloat16 *dst = reinterpret_cast<qfloat16 *>(result.rgba16f.data());
    for (int y = 0; y < h; ++y) {
        const qfloat16 *src = reinterpret_cast<const qfloat16 *>(rgb.pixels + size_t(y) * rgb.rowBytes);
        for (int x = 0; x < w; ++x) {
            float rr = linearize(float(src[x * 4 + 0]));
            float gg = linearize(float(src[x * 4 + 1]));
            float bb = linearize(float(src[x * 4 + 2]));
            const float aa = float(src[x * 4 + 3]);
            if (isBt2020)
                bt2020ToBt709(rr, gg, bb);
            const size_t o = (size_t(y) * w + x) * 4;
            dst[o + 0] = qfloat16(std::max(rr, 0.0f));
            dst[o + 1] = qfloat16(std::max(gg, 0.0f));
            dst[o + 2] = qfloat16(std::max(bb, 0.0f));
            dst[o + 3] = qfloat16(aa);
        }
    }

    std::fprintf(stderr, "vantapaper: decoded AVIF %dx%d depth=%d transfer=%d primaries=%s\n",
                 w, h, depth, int(tc), isBt2020 ? "BT.2020" : "BT.709/other");

    avifRGBImageFreePixels(&rgb);
    avifImageDestroy(img);
    avifDecoderDestroy(dec);
    return result;
}

// --- General SDR images (PNG/JPEG/WebP/... via Qt's image plugins) ----------

HdrImage decodeSdrImage(const QString &path)
{
    HdrImage result;

    QImage img(path);
    if (img.isNull()) {
        std::fprintf(stderr, "vantapaper: could not load image %s (unsupported format?)\n",
                     qPrintable(path));
        return result;
    }
    img = img.convertToFormat(QImage::Format_RGBA8888);

    result.w = img.width();
    result.h = img.height();
    result.rgba16f.resize(size_t(result.w) * result.h * 4);

    // sRGB -> linear; 1.0 = reference white. Black (0) stays 0 -> true black on
    // the HDR surface, which is exactly what makes SDR wallpapers look right here.
    qfloat16 *dst = reinterpret_cast<qfloat16 *>(result.rgba16f.data());
    for (int y = 0; y < result.h; ++y) {
        const uchar *line = img.constScanLine(y);
        for (int x = 0; x < result.w; ++x) {
            const size_t o = (size_t(y) * result.w + x) * 4;
            dst[o + 0] = qfloat16(srgbEotf(line[x * 4 + 0] / 255.0f));
            dst[o + 1] = qfloat16(srgbEotf(line[x * 4 + 1] / 255.0f));
            dst[o + 2] = qfloat16(srgbEotf(line[x * 4 + 2] / 255.0f));
            dst[o + 3] = qfloat16(line[x * 4 + 3] / 255.0f);
        }
    }
    std::fprintf(stderr, "vantapaper: loaded SDR image %dx%d via QImage\n", result.w, result.h);
    return result;
}

HdrImage decodeImage(const QString &path)
{
    if (path.endsWith(QStringLiteral(".avif"), Qt::CaseInsensitive))
        return decodeAvif(path);

    if (path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive)) {
        HdrImage uhdr = decodeUltraHdr(path); // UltraHDR gain-map JPEG, if it is one
        if (uhdr.valid())
            return uhdr;
        // Otherwise fall through: a plain SDR JPEG.
    }

    return decodeSdrImage(path); // PNG/JPEG/WebP/BMP/... via Qt
}
