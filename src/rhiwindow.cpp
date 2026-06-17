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

void WallpaperOutput::setImage(std::shared_ptr<const HdrImage> image)
{
    m_image = std::move(image);
    m_texUploaded = false;

    // Resize the texture in place if the new image has different dimensions
    // (keeps the shader resource bindings valid since the QRhiTexture* is reused).
    if (m_initialized && m_tex && m_image && m_image->valid()) {
        const QSize want(m_image->w, m_image->h);
        if (m_tex->pixelSize() != want) {
            m_tex->destroy();
            m_tex->setPixelSize(want);
            m_tex->create();
        }
    }
    if (m_initialized)
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

    // Swapchain colorspace; VANTAPAPER_FORMAT overrides for debugging.
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
    if (qEnvironmentVariableIsSet("VANTAPAPER_SPLIT"))
        m_splitX = qEnvironmentVariable("VANTAPAPER_SPLIT").toFloat();

    // Texture sized to the current image, or a 1x1 placeholder until one arrives.
    const QSize texSize = (m_image && m_image->valid()) ? QSize(m_image->w, m_image->h) : QSize(1, 1);
    m_tex.reset(m_rhi->newTexture(QRhiTexture::RGBA16F, texSize));
    m_tex->create();
    m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_sampler->create();
    m_ubo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 4 * sizeof(float)));
    m_ubo->create();

    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, m_ubo.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_tex.get(), m_sampler.get())
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
    const bool haveImage = m_image && m_image->valid();

    QRhiResourceUpdateBatch *rub = nullptr;
    if (haveImage && !m_texUploaded) {
        rub = m_rhi->nextResourceUpdateBatch();
        QRhiTextureSubresourceUploadDescription sub(
            m_image->rgba16f.data(), quint32(m_image->rgba16f.size() * sizeof(uint16_t)));
        QRhiTextureUploadEntry entry(0, 0, sub);
        QRhiTextureUploadDescription desc(entry);
        rub->uploadTexture(m_tex.get(), desc);

        const float ubo[4] = { m_scale, m_splitX, 0.0f, 0.0f };
        rub->updateDynamicBuffer(m_ubo.get(), 0, sizeof(ubo), ubo);
        m_texUploaded = true;
    }

    cb->beginPass(m_sc->currentFrameRenderTarget(), QColor(0, 0, 0, 255), { 1.0f, 0 }, rub);
    if (haveImage) {
        cb->setGraphicsPipeline(m_ps.get());
        cb->setViewport({ 0, 0, float(outputSize.width()), float(outputSize.height()) });
        cb->setShaderResources();
        cb->draw(3);
    }
    cb->endPass();

    m_rhi->endFrame(m_sc.get());
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

        // After the first frame the native wl_surface exists; tag it Windows-scRGB
        // (HDR outputs only), then render once more so the next commit applies the
        // double-buffered color-management image description.
        if (!m_tagged) {
            m_tagged = true;
            if (m_hdrActive) {
                const bool tagOk = cm::tagWindowWindowsScrgb(this);
                qInfo("vantapaper[%s]: windows-scRGB tag %s",
                      qPrintable(m_screen ? m_screen->name() : QStringLiteral("?")),
                      tagOk ? "ok" : "FAILED");
            }
            render();
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
