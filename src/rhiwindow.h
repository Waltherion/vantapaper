#pragma once

#include <QWindow>
#include <QElapsedTimer>
#include <rhi/qrhi.h>
#include <memory>

#include "hdr_image.h"
#include "transition.h"

class QScreen;

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
    // HdrImage is shared so one decode can feed every output.
    void setImage(std::shared_ptr<const HdrImage> image, const Transition &trans);

    QString screenName() const;

protected:
    void exposeEvent(QExposeEvent *) override;
    bool event(QEvent *) override;

private:
    void init();
    void resizeSwapChain();
    void releaseSwapChain();
    void render();
    void resizeTexture(int idx, const HdrImage &img);

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

    std::shared_ptr<const HdrImage> m_incoming;
    int m_curIndex = 0;          // which texture holds the incoming image
    bool m_incomingDirty = false; // m_incoming needs uploading to m_tex[m_curIndex]
    bool m_firstShown = false;

    Transition m_trans;
    bool m_transActive = false;
    QElapsedTimer m_transClock;

    float m_scale = 2.5375f; // 203/80

    bool m_initialized = false;
    bool m_hasSwapChain = false;
    bool m_hdrActive = false;
    bool m_tagged = false;
};
