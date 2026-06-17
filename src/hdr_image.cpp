#include "hdr_image.h"

#include <QFile>
#include <QString>

#include <ultrahdr_api.h>

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
