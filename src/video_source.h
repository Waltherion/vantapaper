#pragma once

#include <QString>
#include <QSize>
#include <QElapsedTimer>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
struct HdrImage;

// Colour metadata resolved from the stream/first frame (untagged defaults applied:
// matrix -> BT.709 for >=720p else BT.601, range -> limited, transfer -> sRGB).
struct VideoColorInfo {
    enum Tf { SRGB, PQ, HLG };
    Tf transfer = SRGB;        // BT.709/unspecified -> SRGB (matches the stills pipeline)
    bool bt2020 = false;       // primaries need 2020->709 conversion in the shader
    bool bt2020Matrix = false; // YUV matrix: BT.2020-NCL vs BT.709 (601 for SD)
    bool bt601Matrix = false;  // SD default / explicit 601 tag
    bool fullRange = false;    // AVCOL_RANGE_JPEG (Walther's AV1 files ARE full range)
    int bitDepth = 8;          // 8 or 10
    bool semiPlanar = true;    // NV12/P010 (hwdownload) vs yuv420p/p10le (sw decode)
    bool highBits = false;     // 10-bit stored in the HIGH bits of 16 (P010) vs low (p10le)
    bool hdr() const { return transfer != SRGB; }
};

// The normalized YUV -> RGB transform for a given colour info: rgb = M * yuv + bias,
// where yuv are raw texture samples in [0,1] (bit-depth scale folded into M/bias).
// Shared by the CPU first-frame conversion and the GPU convert pass's UBO.
void yuvToRgbMatrix(const VideoColorInfo &ci, float m[9], float bias[3]);

// One decoded CPU frame on the shared timeline; owns its AVFrame.
struct VideoFrame {
    AVFrame *frame = nullptr;
    qint64 timelineNs = 0; // loop-corrected presentation time (monotonic across loops)
    ~VideoFrame();
};

// Decode engine for one video file: demux + decoder with NVDEC (cuda hwaccel +
// transfer to system RAM) and automatic software fallback. One instance is shared
// by every output showing the same file (refcounted registry) -- a single decode
// thread fills a small ring of CPU frames; outputs pull the newest frame at their
// own pace against the shared master clock.
class VideoSource
{
public:
    // Shared instance per canonical path (two outputs on the same file = one
    // decoder/NVDEC session). Thread-safe; the instance is NOT opened yet.
    static std::shared_ptr<VideoSource> acquire(const QString &path);

    VideoSource() = default;
    ~VideoSource();
    VideoSource(const VideoSource &) = delete;
    VideoSource &operator=(const VideoSource &) = delete;

    // Open + decode the first frame (thread-safe, idempotent; both may be called
    // from concurrent worker tasks when two outputs rotate onto the same file).
    // ensureOpen returns false if the file is unusable (caller skips it).
    bool ensureOpen(bool hwdec);
    std::shared_ptr<const HdrImage> firstFrame(); // fp16 linear 709 still (cached)

    // Start the decode thread + master clock (idempotent; called on the GUI thread
    // when an output's in-transition completes).
    void start();

    // Newest frame with timelineNs <= now (per-output epoch adjust handles pauses:
    // a gap > 250ms rebases instead of fast-forwarding). Null if nothing new is
    // ready -- the caller keeps showing its current frame. GUI thread.
    // Newest due frame on the SHARED timeline. The pause/occlusion epoch adjust is shared per
    // source (not per output) so two outputs on the same file stay in lock-step: independent
    // epochs would desync after an occlusion rebase and the faster output would pop frames out
    // of the small shared ring before the slower one saw them, starving it (permanent freeze).
    std::shared_ptr<VideoFrame> frameFor();

    bool failed() const { return m_error.load(); } // decode thread hit a fatal error

    bool usingHwdec() const { return m_hwDevice != nullptr; }
    const char *decoderName() const;
    const VideoColorInfo &colorInfo() const { return m_color; }
    QSize size() const { return m_size; }
    double avgFrameRate() const { return m_fps; }
    QString path() const { return m_path; }

    // Step-0 spike, kept as a diagnostic: `vantapaper probe-video <file>` decodes
    // ~5s worth of frames through the exact playback path and reports sustained fps.
    static int probeBench(const QString &path);

private:
    bool open(const QString &path, bool hwdec); // demux + decoder + trial + metadata
    void close();
    bool openDecoder(bool hwdec);
    void closeDecoder();
    bool decodeNext(AVFrame *out); // false on EOF/error (synchronous, decode thread)
    void seekToStart();            // hard-cut loop: seek 0 + flush decoder
    void decodeLoop();             // thread body: fill the ring, loop at EOF
    void stopThread();

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_ctx = nullptr;
    AVBufferRef *m_hwDevice = nullptr; // cuda device; null = software path
    AVPacket *m_pkt = nullptr;
    AVFrame *m_hwFrame = nullptr;      // staging for cuda frames before transfer
    int m_stream = -1;
    VideoColorInfo m_color;
    QSize m_size;
    double m_fps = 0.0;
    QString m_path;

    // open/firstFrame serialization + caches (concurrent worker tasks).
    std::mutex m_openMutex;
    bool m_openTried = false;
    bool m_openOk = false;
    std::shared_ptr<const HdrImage> m_firstFrame;

    // Decode thread + bounded ring (producer blocks when full = auto-throttle;
    // consumers pop frames older than the one they display).
    std::thread m_thread;
    std::mutex m_ringMutex;
    std::condition_variable m_ringCv;
    std::deque<std::shared_ptr<VideoFrame>> m_ring;
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_error{ false };
    qint64 m_epochAdjustNs = 0;             // shared pause/occlusion rebase (guarded by m_ringMutex)
    bool m_started = false;
    QElapsedTimer m_clock;   // master playback clock, started in start()
    qint64 m_loopBaseNs = 0; // accumulated duration of completed loops
    qint64 m_streamStartPts = 0;
};
