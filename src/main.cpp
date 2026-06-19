#include <QGuiApplication>
#include <QCoreApplication>
#include <QVulkanInstance>
#include <QLocalSocket>
#include <QStringList>
#include <QDir>
#include <QImage>
#include <QtCore/qfloat16.h>
#include <rhi/qrhi.h>

#include "daemon.h"
#include "hdr_image.h"

#include <cstdio>
#include <cmath>
#include <algorithm>

// `vantapaper ctl <cmd>`: connect to the running daemon, send one command, print
// the reply. Used to wire up keybinds (next/previous/toggle-pause/...).
static int runControlClient(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QStringList parts;
    for (int i = 2; i < argc; ++i)
        parts << QString::fromLocal8Bit(argv[i]);
    const QString cmd = parts.join(QLatin1Char(' '));
    if (cmd.isEmpty()) {
        std::fprintf(stderr,
            "usage: vantapaper ctl <next|previous|toggle-pause|pause|play|status|reload>\n");
        return 2;
    }

    QLocalSocket sock;
    sock.connectToServer(Daemon::socketPath());
    if (!sock.waitForConnected(500)) {
        std::fprintf(stderr, "vantapaper: daemon not running (%s)\n",
                     qPrintable(Daemon::socketPath()));
        return 1;
    }
    sock.write(cmd.toUtf8() + '\n');
    sock.flush();
    sock.waitForBytesWritten(500);
    if (sock.waitForReadyRead(1000))
        std::fputs(sock.readAll().constData(), stdout);
    return 0;
}

// `vantapaper probe <image>`: decode the file and report whether vantapaper can
// display it (and how it was interpreted -- see the diagnostics on stderr).
static int runProbe(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: vantapaper probe <image>\n");
        return 2;
    }
    const HdrImage h = decodeImage(QString::fromLocal8Bit(argv[2]));
    if (h.valid()) {
        std::printf("OK: decoded %dx%d -- vantapaper can display this\n", h.w, h.h);
        return 0;
    }
    std::printf("FAILED: could not decode -- unsupported\n");
    return 1;
}

// `vantapaper thumbnail <in> <out> [maxdim]`: decode any image (HDR-aware),
// tone-map it to SDR exactly like the SDR display path, and write a plain sRGB
// PNG/JPEG -- so other tools (e.g. the theme-switcher preview) can show HDR
// wallpapers correctly instead of as a grey, washed-out mess.
static int runThumbnail(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: vantapaper thumbnail <in> <out> [maxdim]\n");
        return 2;
    }
    const HdrImage h = decodeImage(QString::fromLocal8Bit(argv[2]));
    if (!h.valid()) {
        std::fprintf(stderr, "vantapaper: thumbnail decode failed\n");
        return 1;
    }

    auto rolloff = [](float v) {
        const float k = 0.8f;
        return v <= k ? v : k + (1.0f - k) * (1.0f - std::exp(-(v - k) / (1.0f - k)));
    };
    auto srgb = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    };

    QImage img(h.w, h.h, QImage::Format_RGB888);
    const qfloat16 *src = reinterpret_cast<const qfloat16 *>(h.rgba16f.data());
    for (int y = 0; y < h.h; ++y) {
        uchar *line = img.scanLine(y);
        for (int x = 0; x < h.w; ++x) {
            const size_t o = (size_t(y) * h.w + x) * 4;
            float r = float(src[o]), g = float(src[o + 1]), b = float(src[o + 2]);
            if (h.hdr) {
                const float L = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                const float Lw = 6.0f;
                const float Lt = L * (1.0f + L / (Lw * Lw)) / (1.0f + L);
                const float s = L > 1e-4f ? Lt / L : 1.0f;
                r *= s; g *= s; b *= s;
            } else {
                r = rolloff(r); g = rolloff(g); b = rolloff(b);
            }
            line[x * 3 + 0] = uchar(std::clamp(srgb(r) * 255.0f + 0.5f, 0.0f, 255.0f));
            line[x * 3 + 1] = uchar(std::clamp(srgb(g) * 255.0f + 0.5f, 0.0f, 255.0f));
            line[x * 3 + 2] = uchar(std::clamp(srgb(b) * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }

    bool okDim = false;
    const int maxdim = argc >= 5 ? QString::fromLocal8Bit(argv[4]).toInt(&okDim) : 0;
    if (okDim && maxdim > 0 && (img.width() > maxdim || img.height() > maxdim))
        img = img.scaled(maxdim, maxdim, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    if (!img.save(QString::fromLocal8Bit(argv[3]))) {
        std::fprintf(stderr, "vantapaper: thumbnail save failed\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("ctl"))
        return runControlClient(argc, argv);

    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("probe"))
        return runProbe(argc, argv);

    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("thumbnail"))
        return runThumbnail(argc, argv);

    QGuiApplication app(argc, argv);

    QVulkanInstance inst;
    inst.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (!inst.create())
        qFatal("vantapaper: failed to create Vulkan instance");

    // Fallback wallpaper source; the config's "path" overrides it (see Daemon).
    const QString dir = QDir::homePath() + QStringLiteral("/Pictures/wallpapers");

    Daemon daemon(&inst, dir);
    daemon.start();

    return app.exec();
}
