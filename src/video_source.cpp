#include "video_source.h"
#include "hdr_image.h"
#include "color_math.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <QFileInfo>
#include <QHash>
#include <QtCore/qfloat16.h>

#include <cstdio>

// ---- shared-instance registry (one decoder per file across outputs) -----------

static std::mutex s_registryMutex;
static QHash<QString, std::weak_ptr<VideoSource>> *s_registry = nullptr;

std::shared_ptr<VideoSource> VideoSource::acquire(const QString &path)
{
    const QFileInfo fi(path);
    const QString key = fi.canonicalFilePath().isEmpty() ? path : fi.canonicalFilePath();

    std::lock_guard<std::mutex> lk(s_registryMutex);
    if (!s_registry)
        s_registry = new QHash<QString, std::weak_ptr<VideoSource>>();
    if (auto existing = s_registry->value(key).lock())
        return existing;
    auto src = std::shared_ptr<VideoSource>(new VideoSource());
    src->m_path = key;
    s_registry->insert(key, src);
    return src;
}

// ---- YUV -> RGB matrix ---------------------------------------------------------

// Build rgb = M * yuv + bias for raw [0,1] texture samples. Bit-depth scale, range
// expansion and the matrix coefficients are all folded in, so the shader (and the
// CPU first-frame path) apply one mat3 + bias regardless of source flavour.
void yuvToRgbMatrix(const VideoColorInfo &ci, float m[9], float bias[3])
{
    // Matrix coefficients.
    float kr = 0.2126f, kb = 0.0722f;           // BT.709
    if (ci.bt2020Matrix) { kr = 0.2627f; kb = 0.0593f; }
    else if (ci.bt601Matrix) { kr = 0.299f; kb = 0.114f; }
    const float kg = 1.0f - kr - kb;

    // Normalisation of the raw sample to a full-scale value in [0,1]:
    //   P010: 10 bits in the HIGH bits of 16 -> *65535/64/1023
    //   yuv420p10le: 10 bits in the LOW bits  -> *65535/1023 (sampled as 16-bit unorm)
    //   8-bit: already full scale.
    float depthScale = 1.0f;
    if (ci.bitDepth == 10)
        depthScale = ci.highBits ? (65535.0f / (1023.0f * 64.0f)) : (65535.0f / 1023.0f);

    // Range expansion (in n-bit code values, normalised by maxVal).
    const float maxVal = float((1 << ci.bitDepth) - 1);
    const float yOff = ci.fullRange ? 0.0f : float(16 << (ci.bitDepth - 8)) / maxVal;
    const float cOff = float(128 << (ci.bitDepth - 8)) / maxVal;
    const float yScale = ci.fullRange ? 1.0f : maxVal / float(219 << (ci.bitDepth - 8));
    const float cScale = ci.fullRange ? 1.0f : maxVal / float(224 << (ci.bitDepth - 8));

    // y' = (s*depthScale - yOff) * yScale ; u'/v' likewise with cOff/cScale.
    // R = y' + 2(1-kr) v' ; B = y' + 2(1-kb) u' ; G = (y' - kr R - kb B) / kg.
    const float rv = 2.0f * (1.0f - kr);
    const float bu = 2.0f * (1.0f - kb);
    const float gu = -bu * kb / kg;
    const float gv = -rv * kr / kg;

    const float ys = yScale * depthScale;
    const float cs = cScale * depthScale;
    // Row-major 3x3: rgb = M * (rawY, rawU, rawV)
    m[0] = ys; m[1] = 0.0f;    m[2] = rv * cs;
    m[3] = ys; m[4] = gu * cs; m[5] = gv * cs;
    m[6] = ys; m[7] = bu * cs; m[8] = 0.0f;
    const float yB = -yOff * yScale;
    const float cB = -cOff * cScale;
    bias[0] = yB + rv * cB;
    bias[1] = yB + (gu + gv) * cB;
    bias[2] = yB + bu * cB;
}

// ---- VideoFrame ----------------------------------------------------------------

VideoFrame::~VideoFrame()
{
    if (frame)
        av_frame_free(&frame);
}

// ---- decoder plumbing ----------------------------------------------------------

// get_format callback for the hw attempt: pick CUDA when offered, else the first
// software format (which makes the trial decode fail the hw check -> sw fallback).
static AVPixelFormat pickCudaFormat(AVCodecContext *, const AVPixelFormat *fmts)
{
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_CUDA)
            return AV_PIX_FMT_CUDA;
    return fmts[0];
}

VideoSource::~VideoSource()
{
    stopThread();
    close();
}

const char *VideoSource::decoderName() const
{
    return (m_ctx && m_ctx->codec) ? m_ctx->codec->name : "?";
}

void VideoSource::closeDecoder()
{
    if (m_ctx)
        avcodec_free_context(&m_ctx);
    if (m_hwDevice)
        av_buffer_unref(&m_hwDevice);
}

void VideoSource::close()
{
    closeDecoder();
    if (m_hwFrame)
        av_frame_free(&m_hwFrame);
    if (m_pkt)
        av_packet_free(&m_pkt);
    if (m_fmt)
        avformat_close_input(&m_fmt);
    m_stream = -1;
}

// Open the decoder half. NVDEC subtlety (verified on this ffmpeg): the DEFAULT AV1
// decoder is libdav1d, which does NOT support hwaccel -- the native "av1" decoder is
// the hwaccel host. So the hw attempt must pick the decoder BY NAME for AV1, and the
// sw fallback goes back to libdav1d. H.264/HEVC use one native decoder for both paths.
bool VideoSource::openDecoder(bool hwdec)
{
    const AVStream *st = m_fmt->streams[m_stream];
    const AVCodecID id = st->codecpar->codec_id;

    const AVCodec *dec = nullptr;
    if (hwdec) {
        if (id == AV_CODEC_ID_AV1)
            dec = avcodec_find_decoder_by_name("av1"); // hwaccel-capable host
        else
            dec = avcodec_find_decoder(id);
    } else {
        if (id == AV_CODEC_ID_AV1)
            dec = avcodec_find_decoder_by_name("libdav1d");
        if (!dec)
            dec = avcodec_find_decoder(id);
    }
    if (!dec)
        return false;

    m_ctx = avcodec_alloc_context3(dec);
    if (!m_ctx)
        return false;
    if (avcodec_parameters_to_context(m_ctx, st->codecpar) < 0) {
        closeDecoder();
        return false;
    }

    if (hwdec) {
        if (av_hwdevice_ctx_create(&m_hwDevice, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
            closeDecoder();
            return false;
        }
        m_ctx->hw_device_ctx = av_buffer_ref(m_hwDevice);
        m_ctx->get_format = pickCudaFormat;
    } else {
        m_ctx->thread_count = 0; // auto (all cores) for software decode
    }

    if (avcodec_open2(m_ctx, dec, nullptr) < 0) {
        closeDecoder();
        return false;
    }
    return true;
}

bool VideoSource::open(const QString &path, bool hwdec)
{
    close();

    const QByteArray p8 = path.toUtf8();
    if (avformat_open_input(&m_fmt, p8.constData(), nullptr, nullptr) < 0) {
        std::fprintf(stderr, "vantapaper: video open failed: %s\n", p8.constData());
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        close();
        return false;
    }
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_stream < 0) {
        close();
        return false;
    }

    m_pkt = av_packet_alloc();
    m_hwFrame = av_frame_alloc();

    // Try hw first (unless disabled), validated by a real trial decode of the first
    // frame -- av_hwdevice_ctx_create succeeding does not guarantee the codec/profile
    // actually decodes on NVDEC. Any failure tears down and reopens in software.
    AVFrame *trial = av_frame_alloc();
    bool ok = false;
    for (int attempt = hwdec ? 0 : 1; attempt < 2 && !ok; ++attempt) {
        const bool hw = (attempt == 0);
        closeDecoder();
        seekToStart();
        if (!openDecoder(hw))
            continue;
        if (decodeNext(trial)) {
            ok = true;
        } else if (hw) {
            std::fprintf(stderr, "vantapaper: hw trial decode failed for %s -> software\n",
                         p8.constData());
        }
    }
    if (!ok) {
        av_frame_free(&trial);
        close();
        std::fprintf(stderr, "vantapaper: no working decoder for %s\n", p8.constData());
        return false;
    }

    // Resolve size, fps and colour metadata from the real first frame (more reliable
    // than container tags, and reflects what the decoder actually outputs).
    m_size = QSize(trial->width, trial->height);
    const AVStream *st = m_fmt->streams[m_stream];
    m_fps = av_q2d(st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate);
    m_streamStartPts = (st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;

    switch (trial->color_trc) {
    case AVCOL_TRC_SMPTE2084:    m_color.transfer = VideoColorInfo::PQ; break;
    case AVCOL_TRC_ARIB_STD_B67: m_color.transfer = VideoColorInfo::HLG; break;
    default:                     m_color.transfer = VideoColorInfo::SRGB; break;
    }
    m_color.bt2020 = (trial->color_primaries == AVCOL_PRI_BT2020);
    // YUV matrix: honour the tag; untagged -> BT.709 for HD, BT.601 for SD.
    if (trial->colorspace == AVCOL_SPC_BT2020_NCL || trial->colorspace == AVCOL_SPC_BT2020_CL) {
        m_color.bt2020Matrix = true;
    } else if (trial->colorspace == AVCOL_SPC_BT470BG || trial->colorspace == AVCOL_SPC_SMPTE170M) {
        m_color.bt601Matrix = true;
    } else if (trial->colorspace == AVCOL_SPC_UNSPECIFIED && trial->height < 720) {
        m_color.bt601Matrix = true;
    }
    m_color.fullRange = (trial->color_range == AVCOL_RANGE_JPEG);

    const AVPixelFormat swFmt = AVPixelFormat(trial->format);
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(swFmt);
    m_color.bitDepth = desc ? desc->comp[0].depth : 8;
    // Semi-planar (NV12/P010: chroma planes share one interleaved plane) vs planar.
    m_color.semiPlanar = desc && desc->nb_components >= 3
        && desc->comp[1].plane == desc->comp[2].plane;
    // P010 keeps its 10 bits in the high bits of 16; yuv420p10le in the low bits.
    m_color.highBits = (swFmt == AV_PIX_FMT_P010LE || swFmt == AV_PIX_FMT_P010BE);

    av_frame_free(&trial);
    seekToStart(); // playback starts from frame 0
    return true;
}

bool VideoSource::ensureOpen(bool hwdec)
{
    std::lock_guard<std::mutex> lk(m_openMutex);
    if (m_openTried)
        return m_openOk;
    m_openTried = true;
    m_openOk = open(m_path, hwdec);
    return m_openOk;
}

bool VideoSource::decodeNext(AVFrame *out)
{
    if (!m_fmt || !m_ctx)
        return false;

    av_frame_unref(out);
    while (true) {
        // Drain any frame already buffered in the decoder first.
        AVFrame *dst = m_hwDevice ? m_hwFrame : out;
        av_frame_unref(dst);
        int r = avcodec_receive_frame(m_ctx, dst);
        if (r == 0) {
            if (m_hwDevice) {
                if (dst->format == AV_PIX_FMT_CUDA) {
                    // GPU frame -> system RAM (P010/NV12); pacing metadata follows.
                    if (av_hwframe_transfer_data(out, dst, 0) < 0)
                        return false;
                    av_frame_copy_props(out, dst);
                } else {
                    // Decoder gave a software frame despite the hw setup: treat the
                    // hw trial as failed so open() falls back to software cleanly.
                    return false;
                }
            }
            return true;
        }
        if (r != AVERROR(EAGAIN))
            return false; // EOF or error

        // Feed the next packet from our stream.
        while (true) {
            av_packet_unref(m_pkt);
            if (av_read_frame(m_fmt, m_pkt) < 0) {
                avcodec_send_packet(m_ctx, nullptr); // EOF: start draining
                break;
            }
            if (m_pkt->stream_index != m_stream)
                continue;
            if (avcodec_send_packet(m_ctx, m_pkt) < 0)
                return false;
            break;
        }
    }
}

void VideoSource::seekToStart()
{
    if (!m_fmt)
        return;
    av_seek_frame(m_fmt, m_stream, 0, AVSEEK_FLAG_BACKWARD);
    if (m_ctx)
        avcodec_flush_buffers(m_ctx);
}

// ---- first frame -> HdrImage (fp16 linear 709, for the transition still) --------

std::shared_ptr<const HdrImage> VideoSource::firstFrame()
{
    std::lock_guard<std::mutex> lk(m_openMutex);
    if (!m_openOk)
        return nullptr;
    if (m_firstFrame)
        return m_firstFrame;

    AVFrame *f = av_frame_alloc();
    if (!decodeNext(f)) {
        av_frame_free(&f);
        return nullptr;
    }

    const VideoColorInfo &ci = m_color;
    float M[9], bias[3];
    yuvToRgbMatrix(ci, M, bias);
    const colormath::Tf tf = ci.transfer == VideoColorInfo::PQ  ? colormath::Tf::PQ
                           : ci.transfer == VideoColorInfo::HLG ? colormath::Tf::HLG
                                                                : colormath::Tf::SRGB;

    auto img = std::make_shared<HdrImage>();
    img->w = f->width;
    img->h = f->height;
    img->hdr = ci.hdr();
    img->rgba16f.resize(size_t(img->w) * img->h * 4);
    qfloat16 *dst = reinterpret_cast<qfloat16 *>(img->rgba16f.data());

    const bool tenBit = ci.bitDepth == 10;
    const int w = img->w;
    // Raw sample -> the [0,1] scale yuvToRgbMatrix expects (16-bit unorm semantics
    // for 10-bit data, matching what the GPU will sample from R16/RG16 textures).
    const float norm = tenBit ? (1.0f / 65535.0f) : (1.0f / 255.0f);
    const float lowShift = 64.0f; // p10le low-bits -> 16-bit unorm scale (v*64/65535)

    colormath::parallelRows(img->h, [&](int y) {
        const uint8_t *yRow = f->data[0] + size_t(y) * f->linesize[0];
        const int cy = y >> 1;
        const uint8_t *uRow, *vRow;
        int uStep, vStep; // element stride within the chroma row
        if (ci.semiPlanar) {
            uRow = f->data[1] + size_t(cy) * f->linesize[1];
            vRow = uRow + (tenBit ? 2 : 1);
            uStep = vStep = 2;
        } else {
            uRow = f->data[1] + size_t(cy) * f->linesize[1];
            vRow = f->data[2] + size_t(cy) * f->linesize[2];
            uStep = vStep = 1;
        }
        for (int x = 0; x < w; ++x) {
            float sy, su, sv;
            const int cx = x >> 1;
            if (tenBit) {
                const uint16_t *y16 = reinterpret_cast<const uint16_t *>(yRow);
                const uint16_t *u16 = reinterpret_cast<const uint16_t *>(uRow);
                const uint16_t *v16 = reinterpret_cast<const uint16_t *>(vRow);
                float vy = float(y16[x]);
                float vu = float(u16[cx * uStep]);
                float vv = float(v16[cx * vStep]);
                if (!ci.highBits) { vy *= lowShift; vu *= lowShift; vv *= lowShift; }
                sy = vy * norm; su = vu * norm; sv = vv * norm;
            } else {
                sy = float(yRow[x]) * norm;
                su = float(uRow[cx * uStep]) * norm;
                sv = float(vRow[cx * vStep]) * norm;
            }
            float r = M[0] * sy + M[1] * su + M[2] * sv + bias[0];
            float g = M[3] * sy + M[4] * su + M[5] * sv + bias[1];
            float b = M[6] * sy + M[7] * su + M[8] * sv + bias[2];
            r = colormath::linChan(std::clamp(r, 0.0f, 1.0f), tf);
            g = colormath::linChan(std::clamp(g, 0.0f, 1.0f), tf);
            b = colormath::linChan(std::clamp(b, 0.0f, 1.0f), tf);
            if (ci.bt2020)
                colormath::bt2020ToBt709(r, g, b);
            const size_t o = (size_t(y) * w + x) * 4;
            dst[o + 0] = qfloat16(std::max(r, 0.0f));
            dst[o + 1] = qfloat16(std::max(g, 0.0f));
            dst[o + 2] = qfloat16(std::max(b, 0.0f));
            dst[o + 3] = qfloat16(1.0f);
        }
    });

    av_frame_free(&f);
    seekToStart(); // playback starts from frame 0
    m_firstFrame = img;
    return m_firstFrame;
}

// ---- decode thread + pacing ------------------------------------------------------

void VideoSource::start()
{
    std::lock_guard<std::mutex> lk(m_openMutex);
    if (m_started || !m_openOk)
        return;
    m_started = true;
    m_stop.store(false);
    m_clock.start();
    m_thread = std::thread(&VideoSource::decodeLoop, this);
}

void VideoSource::stopThread()
{
    m_stop.store(true);
    m_ringCv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void VideoSource::decodeLoop()
{
    const AVRational tb = m_fmt->streams[m_stream]->time_base;
    const qint64 frameDurNs = m_fps > 0 ? qint64(1e9 / m_fps) : 8333333;
    qint64 lastEndNs = 0;

    while (!m_stop.load()) {
        AVFrame *f = av_frame_alloc();
        if (!decodeNext(f)) {
            av_frame_free(&f);
            if (m_stop.load())
                break;
            // EOF -> hard-cut loop: restart the timeline where this loop ended.
            m_loopBaseNs += lastEndNs > 0 ? lastEndNs : frameDurNs;
            lastEndNs = 0;
            seekToStart();
            // A decode error would loop forever here; detect "seek didn't help".
            AVFrame *retry = av_frame_alloc();
            if (!decodeNext(retry)) {
                av_frame_free(&retry);
                std::fprintf(stderr, "vantapaper: video decode failed mid-play (%s)\n",
                             qPrintable(m_path));
                m_error.store(true);
                return;
            }
            f = retry;
        }

        const qint64 pts = f->best_effort_timestamp != AV_NOPTS_VALUE
            ? f->best_effort_timestamp : f->pts;
        const qint64 relPts = (pts != AV_NOPTS_VALUE) ? (pts - m_streamStartPts) : 0;
        const qint64 ptsNs = av_rescale_q(relPts, tb, AVRational{ 1, 1000000000 });
        const qint64 durNs = f->duration > 0
            ? av_rescale_q(f->duration, tb, AVRational{ 1, 1000000000 }) : frameDurNs;
        lastEndNs = ptsNs + durNs;

        auto vf = std::make_shared<VideoFrame>();
        vf->frame = f;
        vf->timelineNs = m_loopBaseNs + ptsNs;

        std::unique_lock<std::mutex> lk(m_ringMutex);
        m_ringCv.wait(lk, [this]() { return m_ring.size() < 4 || m_stop.load(); });
        if (m_stop.load())
            break;
        m_ring.push_back(std::move(vf));
    }
}

std::shared_ptr<VideoFrame> VideoSource::frameFor(qint64 *epochAdjustNs)
{
    if (!m_started)
        return nullptr;
    const qint64 now = m_clock.nsecsElapsed() - *epochAdjustNs;

    std::lock_guard<std::mutex> lk(m_ringMutex);
    if (m_ring.empty())
        return nullptr;

    // Newest due frame; drop frames BEHIND it but keep it in the ring so a second
    // output sharing this source can display the same frame.
    while (m_ring.size() > 1 && m_ring[1]->timelineNs <= now) {
        m_ring.pop_front();
        m_ringCv.notify_one();
    }
    if (m_ring.front()->timelineNs > now)
        return nullptr; // decoder ahead of the clock; keep showing the current frame
    std::shared_ptr<VideoFrame> pick = m_ring.front();

    // Long gap (occlusion / compositor throttling): rebase instead of fast-forwarding.
    const qint64 gap = now - pick->timelineNs;
    if (gap > 250000000LL)
        *epochAdjustNs += gap;
    return pick;
}

// ---- Step-0 diagnostic -----------------------------------------------------------

int VideoSource::probeBench(const QString &path)
{
    VideoSource src;
    src.m_path = path;
    if (!src.ensureOpen(/*hwdec=*/true)) {
        std::printf("FAILED: could not open/decode %s\n", qPrintable(path));
        return 1;
    }

    const VideoColorInfo &ci = src.colorInfo();
    std::printf("decoder: %s (%s)\n", src.decoderName(), src.usingHwdec() ? "NVDEC/cuda" : "software");
    std::printf("size: %dx%d @ %.1f fps\n", src.size().width(), src.size().height(), src.avgFrameRate());
    std::printf("color: transfer=%s primaries=%s matrix=%s range=%s depth=%d layout=%s\n",
                ci.transfer == VideoColorInfo::PQ ? "PQ" : ci.transfer == VideoColorInfo::HLG ? "HLG" : "sRGB",
                ci.bt2020 ? "BT.2020" : "BT.709",
                ci.bt2020Matrix ? "BT.2020" : ci.bt601Matrix ? "BT.601" : "BT.709",
                ci.fullRange ? "full" : "limited",
                ci.bitDepth, ci.semiPlanar ? "semi-planar" : "planar");

    // Sustained throughput: decode (+ GPU->RAM transfer) for ~5s of media, looping.
    AVFrame *f = av_frame_alloc();
    const int target = int(src.avgFrameRate() > 0 ? src.avgFrameRate() * 5 : 600);
    QElapsedTimer clock;
    clock.start();
    int frames = 0;
    while (frames < target) {
        if (!src.decodeNext(f)) {
            src.seekToStart(); // loop exactly like playback will
            continue;
        }
        ++frames;
    }
    const double secs = clock.nsecsElapsed() / 1e9;
    av_frame_free(&f);

    const double fps = frames / secs;
    std::printf("sustained: %d frames in %.2fs = %.0f fps\n", frames, secs, fps);
    const bool pass = fps >= src.avgFrameRate();
    std::printf("%s: %.0f fps %s the file's %.1f fps\n", pass ? "PASS" : "FAIL",
                fps, pass ? ">=" : "<", src.avgFrameRate());
    return pass ? 0 : 1;
}
