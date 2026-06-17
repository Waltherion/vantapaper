#pragma once

#include <QWindow>
#include <rhi/qrhi.h>
#include <memory>

#include "hdr_image.h"

// Minimal QRhi-driven QWindow that owns its own swapchain so we can request an
// HDR (scRGB extended-linear) format -- something a plain QQuickWindow does not
// expose. This is the foundation the rest of vantapaper will build on.
class RhiWindow : public QWindow
{
    Q_OBJECT
public:
    explicit RhiWindow(QVulkanInstance *inst);
    ~RhiWindow() override;

protected:
    void exposeEvent(QExposeEvent *) override;
    bool event(QEvent *) override;

private:
    void init();
    void resizeSwapChain();
    void releaseSwapChain();
    void render();

    QVulkanInstance *m_inst = nullptr;

    // Declaration order matters: m_rhi must outlive the resources below, so it
    // is declared first and therefore destroyed last.
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiSwapChain> m_sc;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_ps;

    // Image mode (Fase 0b): decoded HDR image displayed via a textured quad.
    bool m_imageMode = false;
    HdrImage m_image;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_ubo;
    bool m_texUploaded = false;
    float m_scale = 2.5375f; // 203/80
    float m_splitX = 0.5f;   // <=0 disables the SDR/HDR split

    bool m_initialized = false;
    bool m_hasSwapChain = false;
    bool m_hdrActive = false;
    bool m_tagged = false;
};
