#include "rhiwindow.h"
#include "cm_tagging.h"
#include "video_source.h"

extern "C" {
#include <libavutil/frame.h>
}

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

void WallpaperOutput::resizeTexture(int idx, int w, int h)
{
    const QSize want(w, h);
    if (m_tex[idx] && m_tex[idx]->pixelSize() != want) {
        m_tex[idx]->destroy();
        m_tex[idx]->setPixelSize(want);
        m_tex[idx]->create();
        // The native texture was recreated; a render target referencing it must be
        // rebuilt before the next video convert pass targets it.
        m_videoRt[idx].reset();
    }
}

void WallpaperOutput::setImage(std::shared_ptr<const HdrImage> image, const Transition &trans)
{
    if (!image || !image->valid())
        return;
    // Leaving a video: stop playback -- its last shown frame stays resident in the
    // outgoing texture, so the transition naturally freezes on it.
    stopVideo();
    m_incoming = std::move(image);

    if (!m_initialized) {
        // First image arrives before the surface exists; shown in init/render.
        m_incomingDirty = true;
        m_transActive = false;
        return;
    }

    if (!m_firstShown) {
        resizeTexture(m_curIndex, m_incoming->w, m_incoming->h);
        m_incomingDirty = true;
        m_transActive = false;
        requestUpdate();
        return;
    }

    // Cross-transition: upload the new image into the other texture, make it current.
    m_curIndex = 1 - m_curIndex;
    resizeTexture(m_curIndex, m_incoming->w, m_incoming->h);
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

void WallpaperOutput::setVideo(std::shared_ptr<VideoSource> src,
                               std::shared_ptr<const HdrImage> firstFrame,
                               const Transition &trans)
{
    if (!src || !firstFrame || !firstFrame->valid())
        return;
    // The first frame transitions in as a still (setImage also stops any previous
    // video); live playback starts in render() once the transition completes.
    setImage(std::move(firstFrame), trans);
    m_pendingVideo = std::move(src);
    requestUpdate();
}

void WallpaperOutput::stopVideo()
{
    m_video.reset();
    m_pendingVideo.reset();
    m_curFrame.reset();
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
    // RenderTarget: the video convert pass renders decoded frames INTO these.
    const QSize firstSize = (m_incoming && m_incoming->valid()) ? QSize(m_incoming->w, m_incoming->h)
                                                                : QSize(1, 1);
    m_tex[0].reset(m_rhi->newTexture(QRhiTexture::RGBA16F, firstSize, 1, QRhiTexture::RenderTarget));
    m_tex[0]->create();
    m_tex[1].reset(m_rhi->newTexture(QRhiTexture::RGBA16F, QSize(1, 1), 1, QRhiTexture::RenderTarget));
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

// (Re)create the video convert pass resources for this frame's size/layout: plane
// textures (Y + chroma [+ V when planar]), the UBO (matrix/bias/params -- rewritten
// whenever anything was rebuilt), the SRB, the render target into m_tex[m_curIndex]
// and the pipeline. Cheap when nothing changed. False = video unusable.
bool WallpaperOutput::ensureVideoResources(const VideoFrame &vf, QRhiResourceUpdateBatch *rub)
{
    const VideoColorInfo &ci = m_video->colorInfo();
    const AVFrame *f = vf.frame;
    const int w = f->width, h = f->height;
    const QSize chromaSize((w + 1) / 2, (h + 1) / 2);
    const bool tenBit = ci.bitDepth == 10;

    const QRhiTexture::Format yFmt = tenBit ? QRhiTexture::R16 : QRhiTexture::R8;
    const QRhiTexture::Format cFmt = ci.semiPlanar
        ? (tenBit ? QRhiTexture::RG16 : QRhiTexture::RG8) : yFmt;

    auto need = [&](std::unique_ptr<QRhiTexture> &t, QRhiTexture::Format fmt, QSize size) {
        if (t && t->format() == fmt && t->pixelSize() == size)
            return false;
        t.reset(m_rhi->newTexture(fmt, size));
        if (!t->create())
            t.reset();
        return true;
    };
    bool rebuilt = false;
    rebuilt |= need(m_planeTex[0], yFmt, QSize(w, h));
    rebuilt |= need(m_planeTex[1], cFmt, chromaSize);
    if (!ci.semiPlanar) {
        rebuilt |= need(m_planeTex[2], yFmt, chromaSize);
    } else if (m_planeTex[2]) {
        m_planeTex[2].reset(); // semi-planar binds the interleaved chroma twice
        rebuilt = true;
    }
    if (!m_planeTex[0] || !m_planeTex[1] || (!ci.semiPlanar && !m_planeTex[2]))
        return false;

    if (!m_videoUbo) {
        // mat4 (64) + vec4 bias (16) + vec4 params (16), std140.
        m_videoUbo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 96));
        if (!m_videoUbo->create())
            return false;
        rebuilt = true;
    }
    if (rebuilt) {
        float M[9], bias[3];
        yuvToRgbMatrix(ci, M, bias);
        const float u[24] = {
            // column-major mat4 from the row-major 3x3 (rgb = M * yuv)
            M[0], M[3], M[6], 0.0f,
            M[1], M[4], M[7], 0.0f,
            M[2], M[5], M[8], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            bias[0], bias[1], bias[2], 0.0f,
            float(ci.transfer == VideoColorInfo::PQ ? 1 : ci.transfer == VideoColorInfo::HLG ? 2 : 0),
            ci.bt2020 ? 1.0f : 0.0f,
            ci.semiPlanar ? 0.0f : 1.0f,
            0.0f,
        };
        rub->updateDynamicBuffer(m_videoUbo.get(), 0, sizeof(u), u);

        QRhiTexture *texC2 = ci.semiPlanar ? m_planeTex[1].get() : m_planeTex[2].get();
        m_videoSrb.reset(m_rhi->newShaderResourceBindings());
        m_videoSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage,
                                                     m_videoUbo.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      m_planeTex[0].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                      m_planeTex[1].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                      texC2, m_sampler.get()),
        });
        if (!m_videoSrb->create())
            return false;
    }

    // Render target into the current transition texture (reset on texture resize).
    if (!m_videoRt[m_curIndex]) {
        QRhiColorAttachment att(m_tex[m_curIndex].get());
        QRhiTextureRenderTargetDescription desc(att);
        m_videoRt[m_curIndex].reset(m_rhi->newTextureRenderTarget(desc));
        if (!m_videoRp)
            m_videoRp.reset(m_videoRt[m_curIndex]->newCompatibleRenderPassDescriptor());
        m_videoRt[m_curIndex]->setRenderPassDescriptor(m_videoRp.get());
        if (!m_videoRt[m_curIndex]->create())
            return false;
    }

    if (!m_videoPs) {
        m_videoPs.reset(m_rhi->newGraphicsPipeline());
        m_videoPs->setShaderStages({
            { QRhiShaderStage::Vertex,   loadShader(QStringLiteral(":/shaders/fullscreen.vert.qsb")) },
            { QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/video_convert.frag.qsb")) }
        });
        QRhiVertexInputLayout inputLayout;
        m_videoPs->setVertexInputLayout(inputLayout);
        m_videoPs->setShaderResourceBindings(m_videoSrb.get());
        m_videoPs->setRenderPassDescriptor(m_videoRp.get());
        if (!m_videoPs->create())
            return false;
    }
    return true;
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

    // Video: promote the pending source once the in-transition is done, then each
    // render pull the newest due frame, upload its planes and convert into
    // m_tex[m_curIndex] in an offscreen pass BEFORE the swapchain pass. Skipped
    // while a still upload is pending (the transition's first frame lands first).
    if (m_pendingVideo && !m_transActive && !m_incomingDirty) {
        m_video = std::move(m_pendingVideo);
        m_video->start(); // idempotent (a second output may share this source)
    }
    if (m_video && m_video->failed())
        stopVideo(); // last shown frame stays as a still until the next rotation
    if (m_video && !m_incomingDirty) {
        std::shared_ptr<VideoFrame> vf = m_video->frameFor();
        if (vf && vf != m_curFrame) {
            m_curFrame = std::move(vf); // held so the planes stay valid for the GPU
            QRhiResourceUpdateBatch *vub = m_rhi->nextResourceUpdateBatch();
            if (!ensureVideoResources(*m_curFrame, vub)) {
                cb->resourceUpdate(vub); // consume the batch harmlessly
                qWarning("vantapaper[%s]: video resources failed -- stopping video",
                         qPrintable(screenName()));
                stopVideo();
            } else {
                const AVFrame *f = m_curFrame->frame;
                const int ch = (f->height + 1) / 2;
                auto uploadPlane = [&](QRhiTexture *tex, const uint8_t *data,
                                       int linesize, int rows) {
                    QRhiTextureSubresourceUploadDescription sub(
                        data, quint32(linesize) * quint32(rows));
                    sub.setDataStride(quint32(linesize));
                    vub->uploadTexture(tex, QRhiTextureUploadDescription(
                                                QRhiTextureUploadEntry(0, 0, sub)));
                };
                uploadPlane(m_planeTex[0].get(), f->data[0], f->linesize[0], f->height);
                uploadPlane(m_planeTex[1].get(), f->data[1], f->linesize[1], ch);
                if (m_planeTex[2])
                    uploadPlane(m_planeTex[2].get(), f->data[2], f->linesize[2], ch);

                cb->beginPass(m_videoRt[m_curIndex].get(), QColor(0, 0, 0, 255),
                              { 1.0f, 0 }, vub);
                cb->setGraphicsPipeline(m_videoPs.get());
                cb->setViewport({ 0, 0, float(f->width), float(f->height) });
                cb->setShaderResources(m_videoSrb.get());
                cb->draw(3);
                cb->endPass();
            }
        }
    }

    const float aspect = outputSize.height() > 0
        ? float(outputSize.width()) / float(outputSize.height()) : 1.0f;
    const float imageHdr = m_video ? (m_video->colorInfo().hdr() ? 1.0f : 0.0f)
                                   : ((m_incoming && m_incoming->hdr) ? 1.0f : 0.0f);
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

    if (m_transActive || m_video || m_pendingVideo)
        requestUpdate(); // keep animating (transition) / playing (video)
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
