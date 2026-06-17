#include "hdr_image.h"

#include <QFile>
#include <QString>
#include <QImage>
#include <QtCore/qfloat16.h>

#include <ultrahdr_api.h>
#include <avif/avif.h>
#include <lcms2.h>
#include <jxl/decode.h>
#include <libheif/heif.h>

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

// Shared transfer model used by all HDR decoders.
enum class Tf { SRGB, PQ, HLG, Linear };

// Linearise one encoded channel to the fp16 convention (1.0 = 203 cd/m^2).
float linChan(float c, Tf t)
{
    switch (t) {
    case Tf::PQ:     return pqEotf(c) * 10000.0f / 203.0f;
    case Tf::HLG:    return hlgSceneLinear(c) * 1000.0f / 203.0f;
    case Tf::Linear: return c;
    default:         return srgbEotf(c);
    }
}

// Infer transfer + BT.2020-ness from an embedded ICC profile's description.
// Real-world HDR (Lightroom etc.) tags colour this way rather than via CICP/nclx.
Tf tfFromIcc(const void *icc, size_t size, bool &bt2020)
{
    Tf t = Tf::SRGB;
    if (cmsHPROFILE p = cmsOpenProfileFromMem(icc, cmsUInt32Number(size))) {
        char desc[256] = { 0 };
        cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", desc, sizeof(desc));
        cmsCloseProfile(p);
        const QString d = QString::fromLatin1(desc).toLower();
        if (d.contains("pq") || d.contains("2100") || d.contains("2084")) t = Tf::PQ;
        else if (d.contains("hlg")) t = Tf::HLG;
        else if (d.contains("linear")) t = Tf::Linear;
        if (d.contains("2020") || d.contains("2100")) bt2020 = true;
        std::fprintf(stderr, "vantapaper: ICC profile = '%s'\n", desc);
    }
    return t;
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
    bool bt2020 = (img->colorPrimaries == AVIF_COLOR_PRIMARIES_BT2020);
    Tf tf = Tf::SRGB;
    switch (img->transferCharacteristics) {
    case AVIF_TRANSFER_CHARACTERISTICS_PQ:     tf = Tf::PQ; break;
    case AVIF_TRANSFER_CHARACTERISTICS_HLG:    tf = Tf::HLG; break;
    case AVIF_TRANSFER_CHARACTERISTICS_LINEAR: tf = Tf::Linear; break;
    case AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED:
    case AVIF_TRANSFER_CHARACTERISTICS_UNKNOWN:
        // CICP unspecified: define the colour space from the embedded ICC profile.
        if (img->icc.size > 0)
            tf = tfFromIcc(img->icc.data, img->icc.size, bt2020);
        break;
    default: break;
    }

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

    qfloat16 *dst = reinterpret_cast<qfloat16 *>(result.rgba16f.data());
    for (int y = 0; y < h; ++y) {
        const qfloat16 *src = reinterpret_cast<const qfloat16 *>(rgb.pixels + size_t(y) * rgb.rowBytes);
        for (int x = 0; x < w; ++x) {
            float rr = linChan(float(src[x * 4 + 0]), tf);
            float gg = linChan(float(src[x * 4 + 1]), tf);
            float bb = linChan(float(src[x * 4 + 2]), tf);
            const float aa = float(src[x * 4 + 3]);
            if (bt2020)
                bt2020ToBt709(rr, gg, bb);
            const size_t o = (size_t(y) * w + x) * 4;
            dst[o + 0] = qfloat16(std::max(rr, 0.0f));
            dst[o + 1] = qfloat16(std::max(gg, 0.0f));
            dst[o + 2] = qfloat16(std::max(bb, 0.0f));
            dst[o + 3] = qfloat16(aa);
        }
    }

    std::fprintf(stderr, "vantapaper: decoded AVIF %dx%d depth=%d transfer=%d bt2020=%d\n",
                 w, h, depth, int(tf), int(bt2020));

    avifRGBImageFreePixels(&rgb);
    avifImageDestroy(img);
    avifDecoderDestroy(dec);
    return result;
}

// --- JPEG-XL ----------------------------------------------------------------

HdrImage decodeJxl(const QString &path)
{
    HdrImage result;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "vantapaper: cannot open %s\n", qPrintable(path));
        return result;
    }
    const QByteArray bytes = f.readAll();

    JxlDecoder *dec = JxlDecoderCreate(nullptr);
    if (!dec)
        return result;
    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()));
    JxlDecoderCloseInput(dec);

    const JxlPixelFormat fmt = { 4, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0 };
    Tf tf = Tf::SRGB;
    bool bt2020 = false;
    std::vector<float> pixels;
    int w = 0, h = 0;
    bool ok = false;

    for (bool run = true; run; ) {
        switch (JxlDecoderProcessInput(dec)) {
        case JXL_DEC_ERROR:
        case JXL_DEC_NEED_MORE_INPUT:
            std::fprintf(stderr, "vantapaper: JXL decode error\n");
            run = false;
            break;
        case JXL_DEC_SUCCESS:
            ok = true;
            run = false;
            break;
        case JXL_DEC_BASIC_INFO: {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(dec, &info) == JXL_DEC_SUCCESS) {
                w = int(info.xsize);
                h = int(info.ysize);
            }
            break;
        }
        case JXL_DEC_COLOR_ENCODING: {
            JxlColorEncoding enc;
            if (JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, &enc) == JXL_DEC_SUCCESS) {
                switch (enc.transfer_function) {
                case JXL_TRANSFER_FUNCTION_PQ:     tf = Tf::PQ; break;
                case JXL_TRANSFER_FUNCTION_HLG:    tf = Tf::HLG; break;
                case JXL_TRANSFER_FUNCTION_LINEAR: tf = Tf::Linear; break;
                default:                           tf = Tf::SRGB; break;
                }
                bt2020 = (enc.primaries == JXL_PRIMARIES_2100);
            } else {
                size_t iccSize = 0;
                if (JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &iccSize) == JXL_DEC_SUCCESS
                    && iccSize > 0) {
                    std::vector<uint8_t> icc(iccSize);
                    if (JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, icc.data(), iccSize)
                        == JXL_DEC_SUCCESS)
                        tf = tfFromIcc(icc.data(), iccSize, bt2020);
                }
            }
            break;
        }
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            size_t need = 0;
            JxlDecoderImageOutBufferSize(dec, &fmt, &need);
            pixels.resize(need / sizeof(float));
            JxlDecoderSetImageOutBuffer(dec, &fmt, pixels.data(), need);
            break;
        }
        default:
            break; // JXL_DEC_FULL_IMAGE etc. -> keep processing
        }
    }
    JxlDecoderDestroy(dec);

    if (!ok || w <= 0 || h <= 0 || pixels.size() < size_t(w) * h * 4) {
        std::fprintf(stderr, "vantapaper: JXL decode produced no usable image\n");
        return result;
    }

    result.w = w;
    result.h = h;
    result.rgba16f.resize(size_t(w) * h * 4);
    qfloat16 *dst = reinterpret_cast<qfloat16 *>(result.rgba16f.data());
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        float rr = linChan(pixels[i * 4 + 0], tf);
        float gg = linChan(pixels[i * 4 + 1], tf);
        float bb = linChan(pixels[i * 4 + 2], tf);
        if (bt2020)
            bt2020ToBt709(rr, gg, bb);
        dst[i * 4 + 0] = qfloat16(std::max(rr, 0.0f));
        dst[i * 4 + 1] = qfloat16(std::max(gg, 0.0f));
        dst[i * 4 + 2] = qfloat16(std::max(bb, 0.0f));
        dst[i * 4 + 3] = qfloat16(pixels[i * 4 + 3]);
    }
    std::fprintf(stderr, "vantapaper: decoded JXL %dx%d transfer=%d bt2020=%d\n", w, h, int(tf), int(bt2020));
    return result;
}

// --- HEIC / HEIF ------------------------------------------------------------

HdrImage decodeHeic(const QString &path)
{
    HdrImage result;

    heif_context *ctx = heif_context_alloc();
    if (!ctx)
        return result;

    const QByteArray pathBytes = path.toLocal8Bit();
    heif_error err = heif_context_read_from_file(ctx, pathBytes.constData(), nullptr);
    if (err.code != heif_error_Ok) {
        std::fprintf(stderr, "vantapaper: HEIC read failed: %s\n", err.message);
        heif_context_free(ctx);
        return result;
    }

    heif_image_handle *handle = nullptr;
    if (heif_context_get_primary_image_handle(ctx, &handle).code != heif_error_Ok || !handle) {
        heif_context_free(ctx);
        return result;
    }

    int bpp = heif_image_handle_get_luma_bits_per_pixel(handle);
    if (bpp <= 0 || bpp > 16)
        bpp = 16;

    // Colour space from nclx (CICP), else from the embedded ICC profile.
    Tf tf = Tf::SRGB;
    bool bt2020 = false;
    heif_color_profile_nclx *nclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code == heif_error_Ok && nclx) {
        if (nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_PQ)
            tf = Tf::PQ;
        else if (nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_HLG)
            tf = Tf::HLG;
        bt2020 = (nclx->color_primaries == heif_color_primaries_ITU_R_BT_2020_2_and_2100_0);
        heif_nclx_color_profile_free(nclx);
    } else if (size_t iccSize = heif_image_handle_get_raw_color_profile_size(handle)) {
        std::vector<uint8_t> icc(iccSize);
        if (heif_image_handle_get_raw_color_profile(handle, icc.data()).code == heif_error_Ok)
            tf = tfFromIcc(icc.data(), iccSize, bt2020);
    }

    heif_image *img = nullptr;
    err = heif_decode_image(handle, &img, heif_colorspace_RGB,
                            heif_chroma_interleaved_RRGGBBAA_LE, nullptr);
    if (err.code != heif_error_Ok || !img) {
        std::fprintf(stderr, "vantapaper: HEIC decode failed: %s\n", err.message);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return result;
    }

    const int w = heif_image_get_width(img, heif_channel_interleaved);
    const int h = heif_image_get_height(img, heif_channel_interleaved);
    int stride = 0;
    const uint8_t *plane = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);

    if (w > 0 && h > 0 && plane) {
        const float maxv = float((1u << bpp) - 1u); // samples are right-aligned in 16-bit LE
        result.w = w;
        result.h = h;
        result.rgba16f.resize(size_t(w) * h * 4);
        qfloat16 *dst = reinterpret_cast<qfloat16 *>(result.rgba16f.data());
        for (int y = 0; y < h; ++y) {
            const uint16_t *row = reinterpret_cast<const uint16_t *>(plane + size_t(y) * stride);
            for (int x = 0; x < w; ++x) {
                float rr = linChan(row[x * 4 + 0] / maxv, tf);
                float gg = linChan(row[x * 4 + 1] / maxv, tf);
                float bb = linChan(row[x * 4 + 2] / maxv, tf);
                const float aa = row[x * 4 + 3] / maxv;
                if (bt2020)
                    bt2020ToBt709(rr, gg, bb);
                const size_t o = (size_t(y) * w + x) * 4;
                dst[o + 0] = qfloat16(std::max(rr, 0.0f));
                dst[o + 1] = qfloat16(std::max(gg, 0.0f));
                dst[o + 2] = qfloat16(std::max(bb, 0.0f));
                dst[o + 3] = qfloat16(aa);
            }
        }
        std::fprintf(stderr, "vantapaper: decoded HEIC %dx%d bpp=%d transfer=%d bt2020=%d\n",
                     w, h, bpp, int(tf), int(bt2020));
    }

    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
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

    if (path.endsWith(QStringLiteral(".jxl"), Qt::CaseInsensitive))
        return decodeJxl(path);

    if (path.endsWith(QStringLiteral(".heic"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".heif"), Qt::CaseInsensitive))
        return decodeHeic(path);

    if (path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive)) {
        HdrImage uhdr = decodeUltraHdr(path); // UltraHDR gain-map JPEG, if it is one
        if (uhdr.valid())
            return uhdr;
        // Otherwise fall through: a plain SDR JPEG.
    }

    return decodeSdrImage(path); // PNG/JPEG/WebP/BMP/... via Qt
}
