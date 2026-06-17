#include "rhiwindow.h"
#include "cm_tagging.h"

#include <QFile>
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

RhiWindow::RhiWindow(QVulkanInstance *inst)
    : m_inst(inst)
{
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(inst);
}

RhiWindow::~RhiWindow() = default;

void RhiWindow::init()
{
    QRhiVulkanInitParams params;
    params.inst = m_inst;
    params.window = this;

    m_rhi.reset(QRhi::create(QRhi::Vulkan, &params));
    if (!m_rhi)
        qFatal("vantapaper: failed to create QRhi (Vulkan backend)");

    qInfo("vantapaper: QRhi backend = %s", m_rhi->backendName());

    m_sc.reset(m_rhi->newSwapChain());
    m_sc->setWindow(this);

    // Swapchain colorspace is selectable via VANTAPAPER_FORMAT for A/B testing:
    //   scrgb (default) = HDRExtendedSrgbLinear  (linear, 1.0 = SDR white)
    //   hdr10           = HDR10 / Rec.2100 PQ    (absolute, code 0 = 0 nits)
    //   sdr             = plain SDR
    // NOTE: the test-pattern shader currently writes *linear* values. Under
    // hdr10 those values are interpreted as PQ, so only the BLACK (left) area is
    // meaningful there -- judge whether black goes truly off under PQ.
    const QByteArray want = qgetenv("VANTAPAPER_FORMAT").toLower();
    auto trySet = [&](QRhiSwapChain::Format f, const char *name) -> bool {
        if (m_sc->isFormatSupported(f)) {
            m_sc->setFormat(f);
            m_hdrActive = (f != QRhiSwapChain::SDR);
            qInfo("vantapaper: swapchain format = %s%s", name, m_hdrActive ? " (HDR ACTIVE)" : "");
            return true;
        }
        qWarning("vantapaper: format %s NOT supported", name);
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
        qWarning("vantapaper: requested format unsupported -> SDR fallback");
    }

    m_rp.reset(m_sc->newCompatibleRenderPassDescriptor());
    m_sc->setRenderPassDescriptor(m_rp.get());

    // If VANTAPAPER_IMAGE points at an UltraHDR JPEG, decode it and switch to
    // image mode; otherwise show a built-in test pattern.
    const QByteArray imagePath = qgetenv("VANTAPAPER_IMAGE");
    if (!imagePath.isEmpty()) {
        m_image = decodeUltraHdr(QString::fromLocal8Bit(imagePath));
        m_imageMode = m_image.valid();
        if (!m_imageMode)
            qWarning("vantapaper: image decode failed -> falling back to test pattern");
    }

    QRhiVertexInputLayout inputLayout; // no vertex buffers; positions come from gl_VertexIndex

    if (m_imageMode) {
        bool okScale = false;
        const float s = qEnvironmentVariable("VANTAPAPER_SCALE").toFloat(&okScale);
        if (okScale) m_scale = s;
        if (qEnvironmentVariableIsSet("VANTAPAPER_SPLIT"))
            m_splitX = qEnvironmentVariable("VANTAPAPER_SPLIT").toFloat();

        m_tex.reset(m_rhi->newTexture(QRhiTexture::RGBA16F, QSize(m_image.w, m_image.h)));
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
        qInfo("vantapaper: image mode %dx%d, scale=%.3f, splitX=%.3f",
              m_image.w, m_image.h, m_scale, m_splitX);
    } else {
        m_srb.reset(m_rhi->newShaderResourceBindings());
        m_srb->create();

        // VANTAPAPER_PATTERN: "split" (true-black vs HDR) or "nits" (staircase). Default nits.
        const QByteArray pattern = qgetenv("VANTAPAPER_PATTERN").toLower();
        const QString fragQsb = (pattern == "split")
            ? QStringLiteral(":/shaders/testpattern.frag.qsb")
            : QStringLiteral(":/shaders/nitstest.frag.qsb");
        qInfo("vantapaper: pattern = %s", pattern == "split" ? "split" : "nits");

        m_ps.reset(m_rhi->newGraphicsPipeline());
        m_ps->setShaderStages({
            { QRhiShaderStage::Vertex,   loadShader(QStringLiteral(":/shaders/fullscreen.vert.qsb")) },
            { QRhiShaderStage::Fragment, loadShader(fragQsb) }
        });
        m_ps->setVertexInputLayout(inputLayout);
        m_ps->setShaderResourceBindings(m_srb.get());
        m_ps->setRenderPassDescriptor(m_rp.get());
        if (!m_ps->create())
            qFatal("vantapaper: failed to create graphics pipeline");
    }

    m_initialized = true;
}

void RhiWindow::resizeSwapChain()
{
    m_hasSwapChain = m_sc->createOrResize();
    if (m_hasSwapChain) {
        const QRhiSwapChainHdrInfo hdr = m_sc->hdrInfo();
        qInfo("vantapaper: swapchain %dx%d, HDR active=%d, reported max luminance info available",
              m_sc->currentPixelSize().width(), m_sc->currentPixelSize().height(),
              int(m_hdrActive));
        qDebug() << "vantapaper: hdrInfo =" << hdr;
    }
}

void RhiWindow::releaseSwapChain()
{
    if (m_hasSwapChain) {
        m_hasSwapChain = false;
        m_sc->destroy();
    }
}

void RhiWindow::render()
{
    if (!m_hasSwapChain || !m_initialized)
        return;

    // React to surface size changes (compositor decides our size on a layer surface).
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

    // Upload the decoded image texture and uniforms once.
    QRhiResourceUpdateBatch *rub = nullptr;
    if (m_imageMode && !m_texUploaded) {
        rub = m_rhi->nextResourceUpdateBatch();
        QRhiTextureSubresourceUploadDescription sub(
            m_image.rgba16f.data(), quint32(m_image.rgba16f.size() * sizeof(uint16_t)));
        QRhiTextureUploadEntry entry(0, 0, sub);
        QRhiTextureUploadDescription desc(entry);
        rub->uploadTexture(m_tex.get(), desc);

        const float ubo[4] = { m_scale, m_splitX, 0.0f, 0.0f };
        rub->updateDynamicBuffer(m_ubo.get(), 0, sizeof(ubo), ubo);
        m_texUploaded = true;
    }

    cb->beginPass(m_sc->currentFrameRenderTarget(), QColor(0, 0, 0, 255), { 1.0f, 0 }, rub);
    cb->setGraphicsPipeline(m_ps.get());
    cb->setViewport({ 0, 0, float(outputSize.width()), float(outputSize.height()) });
    cb->setShaderResources();
    cb->draw(3);
    cb->endPass();

    m_rhi->endFrame(m_sc.get());
}

void RhiWindow::exposeEvent(QExposeEvent *)
{
    if (isExposed() && !m_initialized) {
        init();
        resizeSwapChain();
    }

    const QSize surfaceSize = m_hasSwapChain ? m_sc->surfacePixelSize() : QSize();

    if (isExposed() && m_initialized && m_hasSwapChain && !surfaceSize.isEmpty()) {
        render();

        // After the first frame the native wl_surface exists and has a buffer;
        // tag it as Windows-scRGB, then render once more so the next commit
        // applies the (double-buffered) color-management image description.
        if (!m_tagged) {
            m_tagged = true;
            if (m_hdrActive)
                cm::tagWindowWindowsScrgb(this);
            render();
        }
    }

    // Surface went away (e.g. output removed): drop the swapchain.
    if (!isExposed() && m_initialized && surfaceSize.isEmpty())
        releaseSwapChain();
}

bool RhiWindow::event(QEvent *e)
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
