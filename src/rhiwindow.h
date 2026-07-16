#pragma once

#include <QWindow>
#include <QElapsedTimer>
#include <rhi/qrhi.h>
#include <memory>

#include "hdr_image.h"
#include "transition.h"
#include "cm_tagging.h"

class QScreen;
class VideoSource;
struct VideoFrame;

// One wallpaper surface for a single output (QScreen). Owns its own QRhi + Vulkan
// swapchain so it can request an HDR (scRGB extended-linear) format and tag the
// surface via wp-color-management. Holds two textures and cross-transitions between
// them; the image to display is pushed in via setImage().
class WallpaperOutput : public QWindow
{
    Q_OBJECT
public:
    WallpaperOutput(QVulkanInstance *inst, QScreen *screen);
    ~WallpaperOutput() override;

    // Show this image, playing `trans` (ignored for the very first image). The
    // HdrImage is shared so one decode can feed every output. Stops any video.
    void setImage(std::shared_ptr<const HdrImage> image, const Transition &trans);

    // Show a video: `firstFrame` transitions in as a still; live playback starts
    // when the transition completes (transition OUT later freezes on the last
    // shown frame, which stays resident in the outgoing texture).
    void setVideo(std::shared_ptr<VideoSource> src,
                  std::shared_ptr<const HdrImage> firstFrame, const Transition &trans);

    // Tell this output whether its monitor is currently in HDR or SDR mode, so it
    // can tag + render appropriately. Driven by the daemon's per-output detection.
    void setHdrMode(bool hdr);

    QString screenName() const;

protected:
    void exposeEvent(QExposeEvent *) override;
    bool event(QEvent *) override;

private:
    void init();
    void resizeSwapChain();
    void releaseSwapChain();
    void render();
    void resizeTexture(int idx, int w, int h);
    void stopVideo();
    // (Re)create plane textures / RT / pipeline for the current video frame's
    // size + layout; false = unusable (video is stopped). Fills m_videoUbo.
    bool ensureVideoResources(const VideoFrame &vf, QRhiResourceUpdateBatch *rub);

    QVulkanInstance *m_inst = nullptr;
    QScreen *m_screen = nullptr;

    // Declaration order matters: m_rhi must outlive the resources below.
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiSwapChain> m_sc;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_ps;
    std::unique_ptr<QRhiTexture> m_tex[2];
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_ubo;

    // Video mode: the pending source is promoted to active when the in-transition
    // completes; each new ring frame is uploaded as planes + converted into
    // m_tex[m_curIndex] by an offscreen pass (video_convert.frag).
    std::shared_ptr<VideoSource> m_video;        // live playback
    std::shared_ptr<VideoSource> m_pendingVideo; // starts after the in-transition
    std::shared_ptr<VideoFrame> m_curFrame;      // last uploaded (kept alive for the GPU)
    std::unique_ptr<QRhiTexture> m_planeTex[3];  // Y + chroma (+ V when planar)
    std::unique_ptr<QRhiTextureRenderTarget> m_videoRt[2]; // one per m_tex
    std::unique_ptr<QRhiRenderPassDescriptor> m_videoRp;
    std::unique_ptr<QRhiShaderResourceBindings> m_videoSrb;
    std::unique_ptr<QRhiGraphicsPipeline> m_videoPs;
    std::unique_ptr<QRhiBuffer> m_videoUbo;

    std::shared_ptr<const HdrImage> m_incoming;
    int m_curIndex = 0;          // which texture holds the incoming image
    bool m_incomingDirty = false; // m_incoming needs uploading to m_tex[m_curIndex]
    bool m_firstShown = false;

    Transition m_trans;
    bool m_transActive = false;
    QElapsedTimer m_transClock;

    float m_scale = 2.5375f; // 203/80

    std::unique_ptr<cm::SurfaceColor> m_color;
    bool m_colorApplied = false;

    bool m_initialized = false;
    bool m_hasSwapChain = false;
    bool m_hdrActive = false;

    bool m_hdrMode = true;       // monitor currently in HDR mode?
    bool m_hdrModeKnown = false; // has the daemon told us yet?
};
