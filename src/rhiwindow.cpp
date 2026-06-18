#include "rhiwindow.h"
#include "cm_tagging.h"

#include <QFile>
#include <QScreen>
#include <QExposeEvent>
#include <QPlatformSurfaceEvent>
#include <rhi/qshader.h>

static QShader loadShader(const QString &name)
{
    QFile f(name);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());
    qWarning("Failed to load shader %s", qPrintable(name));
    return QShader();
}

WallpaperOutput::WallpaperOutput(QVulkanInstance *inst, QScreen *screen)
    : m_inst(inst), m_screen(screen)
{
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(inst);
    if (screen)
        setScreen(screen);
}

WallpaperOutput::~WallpaperOutput() = default;

QString WallpaperOutput::screenName() const
{
    return m_screen ? m_screen->name() : QString();
}

void WallpaperOutput::setHdrMode(bool hdr)
{
    if (m_hdrModeKnown && hdr == m_hdrMode)
        return;
    m_hdrModeKnown = true;
    m_hdrMode = hdr;
    qInfo("vantapaper[%s]: monitor is %s mode",
          qPrintable(m_screen ? m_screen->name() : QStringLiteral("?")), hdr ? "HDR" : "SDR");

    // Re-tag the surface (HDR scRGB vs SDR sRGB); the shader's SDR branch follows
    // the same flag. The new image description applies on the next committed frame.
    if (m_color && m_color->valid()) {
        if (m_hdrMode)
            m_color->setWindowsScrgb();
        else
            m_color->setSrgb();
        requestUpdate();
    }
}

void WallpaperOutput::resizeTexture(int idx, const HdrImage &img)
{
    const QSize want(img.w, img.h);
    if (m_tex[idx] && m_tex[idx]->pixelSize() != want) {
        m_tex[idx]->destroy();
        m_tex[idx]->setPixelSize(want);
        m_tex[idx]->create();
    }
}

void WallpaperOutput::setImage(std::shared_ptr<const HdrImage> image, const Transition &trans)
{
    if (!image || !image->valid())
        return;
    m_incoming = std::move(image);

    if (!m_initialized) {
        // First image arrives before the surface exists; shown in init/render.
        m_incomingDirty = true;
        m_transActive = false;
        return;
    }

    if (!m_firstShown) {
        resizeTexture(m_curIndex, *m_incoming);
        m_incomingDirty = true;
        m_transActive = false;
        requestUpdate();
        return;
    }

    // Cross-transition: upload the new image into the other texture, make it current.
    m_curIndex = 1 - m_curIndex;
    resizeTexture(m_curIndex, *m_incoming);
    m_incomingDirty = true;

    m_trans = trans;
    if (trans.type < 0 || trans.durationMs <= 0) {
        m_transActive = false;
    } else {
        m_transActive = true;
        m_transClock.restart();
    }
    requestUpdate();
}

void WallpaperOutput::init()
{
    QRhiVulkanInitParams params;
    params.inst = m_inst;
    params.window = this;

    m_rhi.reset(QRhi::create(QRhi::Vulkan, &params));
    if (!m_rhi)
        qFatal("vantapaper: failed to create QRhi (Vulkan backend)");

    m_sc.reset(m_rhi->newSwapChain());
    m_sc->setWindow(this);

    const QString screenName = m_screen ? m_screen->name() : QStringLiteral("?");

    const QByteArray want = qgetenv("VANTAPAPER_FORMAT").toLower();
    auto trySet = [&](QRhiSwapChain::Format f, const char *name) -> bool {
        if (m_sc->isFormatSupported(f)) {
            m_sc->setFormat(f);
            m_hdrActive = (f != QRhiSwapChain::SDR);
            qInfo("vantapaper[%s]: swapchain format = %s%s", qPrintable(screenName),
                  name, m_hdrActive ? " (HDR)" : "");
            return true;
        }
        return false;
    };

    bool ok = false;
    if (want == "hdr10")
        ok = trySet(QRhiSwapChain::HDR10, "HDR10/PQ");
    else if (want == "sdr")
        ok = trySet(QRhiSwapChain::SDR, "SDR");
    else if (want == "p3")
        ok = trySet(QRhiSwapChain::HDRExtendedDisplayP3Linear, "HDRExtendedDisplayP3Linear");
    else
        ok = trySet(QRhiSwapChain::HDRExtendedSrgbLinear, "HDRExtendedSrgbLinear/scRGB");

    if (!ok) {
        m_sc->setFormat(QRhiSwapChain::SDR);
        m_hdrActive = false;
        qWarning("vantapaper[%s]: HDR format unsupported -> SDR", qPrintable(screenName));
    }

    m_rp.reset(m_sc->newCompatibleRenderPassDescriptor());
    m_sc->setRenderPassDescriptor(m_rp.get());

    bool okScale = false;
    const float s = qEnvironmentVariable("VANTAPAPER_SCALE").toFloat(&okScale);
    if (okScale) m_scale = s;

    // Two textures: tex[0] sized to the current image (or 1x1 placeholder), tex[1] 1x1.
    const QSize firstSize = (m_incoming && m_incoming->valid()) ? QSize(m_incoming->w, m_incoming->h)
                                                                : QSize(1, 1);
    m_tex[0].reset(m_rhi->newTexture(QRhiTexture::RGBA16F, firstSize));
    m_tex[0]->create();
    m_tex[1].reset(m_rhi->newTexture(QRhiTexture::RGBA16F, QSize(1, 1)));
    m_tex[1]->create();

    m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_sampler->create();
    m_ubo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 12 * sizeof(float)));
    m_ubo->create();

    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, m_ubo.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_tex[0].get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  m_tex[1].get(), m_sampler.get())
    });
    m_srb->create();

    QRhiVertexInputLayout inputLayout; // fullscreen triangle from gl_VertexIndex
    m_ps.reset(m_rhi->newGraphicsPipeline());
    m_ps->setShaderStages({
        { QRhiShaderStage::Vertex,   loadShader(QStringLiteral(":/shaders/fullscreen.vert.qsb")) },
        { QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/image.frag.qsb")) }
    });
    m_ps->setVertexInputLayout(inputLayout);
    m_ps->setShaderResourceBindings(m_srb.get());
    m_ps->setRenderPassDescriptor(m_rp.get());
    if (!m_ps->create())
        qFatal("vantapaper: failed to create image pipeline");

    m_curIndex = 0;
    m_initialized = true;
}

void WallpaperOutput::resizeSwapChain()
{
    m_hasSwapChain = m_sc->createOrResize();
}

void WallpaperOutput::releaseSwapChain()
{
    if (m_hasSwapChain) {
        m_hasSwapChain = false;
        m_sc->destroy();
    }
}

void WallpaperOutput::render()
{
    if (!m_hasSwapChain || !m_initialized)
        return;

    if (m_sc->currentPixelSize() != m_sc->surfacePixelSize()) {
        resizeSwapChain();
        if (!m_hasSwapChain)
            return;
    }

    QRhi::FrameOpResult r = m_rhi->beginFrame(m_sc.get());
    if (r == QRhi::FrameOpSwapChainOutOfDate) {
        resizeSwapChain();
        if (!m_hasSwapChain)
            return;
        r = m_rhi->beginFrame(m_sc.get());
    }
    if (r != QRhi::FrameOpSuccess) {
        requestUpdate();
        return;
    }

    QRhiCommandBuffer *cb = m_sc->currentFrameCommandBuffer();
    const QSize outputSize = m_sc->currentPixelSize();

    QRhiResourceUpdateBatch *rub = m_rhi->nextResourceUpdateBatch();

    if (m_incomingDirty && m_incoming && m_incoming->valid()) {
        QRhiTextureSubresourceUploadDescription sub(
            m_incoming->rgba16f.data(), quint32(m_incoming->rgba16f.size() * sizeof(uint16_t)));
        QRhiTextureUploadEntry entry(0, 0, sub);
        QRhiTextureUploadDescription desc(entry);
        rub->uploadTexture(m_tex[m_curIndex].get(), desc);
        m_incomingDirty = false;
        m_firstShown = true;
    }

    float progress = 1.0f;
    if (m_transActive) {
        progress = float(m_transClock.elapsed()) / float(m_trans.durationMs);
        if (progress >= 1.0f) {
            progress = 1.0f;
            m_transActive = false;
        }
    }

    const float aspect = outputSize.height() > 0
        ? float(outputSize.width()) / float(outputSize.height()) : 1.0f;
    const float imageHdr = (m_incoming && m_incoming->hdr) ? 1.0f : 0.0f;
    const float ubo[12] = {
        m_scale, progress, float(m_trans.type), float(m_curIndex),
        m_trans.cx, m_trans.cy, m_trans.angle, aspect,
        m_hdrMode ? 0.0f : 1.0f, imageHdr, 0.0f, 0.0f
    };
    rub->updateDynamicBuffer(m_ubo.get(), 0, sizeof(ubo), ubo);

    cb->beginPass(m_sc->currentFrameRenderTarget(), QColor(0, 0, 0, 255), { 1.0f, 0 }, rub);
    if (m_firstShown) {
        cb->setGraphicsPipeline(m_ps.get());
        cb->setViewport({ 0, 0, float(outputSize.width()), float(outputSize.height()) });
        cb->setShaderResources();
        cb->draw(3);
    }
    cb->endPass();

    m_rhi->endFrame(m_sc.get());

    if (m_transActive)
        requestUpdate(); // keep animating
}

void WallpaperOutput::exposeEvent(QExposeEvent *)
{
    if (isExposed() && !m_initialized) {
        init();
        resizeSwapChain();
    }

    const QSize surfaceSize = m_hasSwapChain ? m_sc->surfacePixelSize() : QSize();

    if (isExposed() && m_initialized && m_hasSwapChain && !surfaceSize.isEmpty()) {
        render();

        if (!m_colorApplied) {
            m_colorApplied = true;
            m_color = std::make_unique<cm::SurfaceColor>(this);
            if (m_color->valid()) {
                if (m_hdrMode)
                    m_color->setWindowsScrgb();
                else
                    m_color->setSrgb();
            }
            render(); // commit applies the image description
        }
    }

    if (!isExposed() && m_initialized && surfaceSize.isEmpty())
        releaseSwapChain();
}

bool WallpaperOutput::event(QEvent *e)
{
    switch (e->type()) {
    case QEvent::UpdateRequest:
        render();
        break;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent *>(e)->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            releaseSwapChain();
        break;
    default:
        break;
    }
    return QWindow::event(e);
}
