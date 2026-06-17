#pragma once

#include <QWindow>
#include <rhi/qrhi.h>
#include <memory>

#include "hdr_image.h"

class QScreen;

// One wallpaper surface for a single output (QScreen). Owns its own QRhi + Vulkan
// swapchain so it can request an HDR (scRGB extended-linear) format -- something a
// plain QQuickWindow does not expose -- and tag the surface via wp-color-management.
//
// The image to display is pushed in via setImage(); decoding happens elsewhere so
// one decode can be shared across all outputs.
class WallpaperOutput : public QWindow
{
    Q_OBJECT
public:
    WallpaperOutput(QVulkanInstance *inst, QScreen *screen);
    ~WallpaperOutput() override;

    // Show this image (shared across outputs). Safe to call before or after the
    // surface is initialized; the texture is (re)uploaded on the next frame.
    void setImage(std::shared_ptr<const HdrImage> image);

    QString screenName() const;

protected:
    void exposeEvent(QExposeEvent *) override;
    bool event(QEvent *) override;

private:
    void init();
    void resizeSwapChain();
    void releaseSwapChain();
    void render();

    QVulkanInstance *m_inst = nullptr;
    QScreen *m_screen = nullptr;

    // Declaration order matters: m_rhi must outlive the resources below, so it
    // is declared first and therefore destroyed last.
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiSwapChain> m_sc;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_ps;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_ubo;

    std::shared_ptr<const HdrImage> m_image;
    bool m_texUploaded = false;

    float m_scale = 2.5375f; // 203/80
    float m_splitX = 0.0f;   // >0 enables the SDR/HDR debug split

    bool m_initialized = false;
    bool m_hasSwapChain = false;
    bool m_hdrActive = false;
    bool m_tagged = false;
};
