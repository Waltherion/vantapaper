#include <QGuiApplication>
#include <QCoreApplication>
#include <QVulkanInstance>
#include <QLocalSocket>
#include <QStringList>
#include <QDir>
#include <rhi/qrhi.h>

#include "daemon.h"
#include "hdr_image.h"

#include <cstdio>

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

int main(int argc, char **argv)
{
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("ctl"))
        return runControlClient(argc, argv);

    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("probe"))
        return runProbe(argc, argv);

    QGuiApplication app(argc, argv);

    QVulkanInstance inst;
    inst.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (!inst.create())
        qFatal("vantapaper: failed to create Vulkan instance");

    // Wallpaper source: the active theme's folder (static path; `current` is a
    // symlink Walther flips per theme). Rotation/transition come from the config.
    const QString dir = QDir::homePath() + QStringLiteral("/.config/themes/current/wallpapers");

    Daemon daemon(&inst, dir);
    daemon.start();

    return app.exec();
}
