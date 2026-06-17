#include <QGuiApplication>
#include <QVulkanInstance>
#include <QScreen>
#include <QDir>
#include <QTimer>
#include <rhi/qrhi.h>

#include <LayerShellQt/window.h>

#include "rhiwindow.h"
#include "playlist.h"
#include "hdr_image.h"

#include <memory>
#include <vector>

static void setupLayerShell(QWindow *w, QScreen *screen)
{
    if (LayerShellQt::Window *ls = LayerShellQt::Window::get(w)) {
        ls->setLayer(LayerShellQt::Window::LayerBackground);
        ls->setAnchors(LayerShellQt::Window::Anchors(
            LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
            | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        ls->setExclusiveZone(-1);
        ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        ls->setScope(QStringLiteral("vantapaper"));
        ls->setScreen(screen);
    } else {
        qWarning("vantapaper: not on a layer surface for %s", qPrintable(screen->name()));
    }
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    QVulkanInstance inst;
    inst.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (!inst.create())
        qFatal("vantapaper: failed to create Vulkan instance");

    // Wallpaper source: the active theme's folder (a static path; `current` is a
    // symlink Walther flips per theme). Config/override comes in Fase 1b.
    const QString dir = QDir::homePath() + QStringLiteral("/.config/themes/current/wallpapers");
    Playlist playlist;
    playlist.load(dir);
    if (playlist.isEmpty())
        qWarning("vantapaper: no images found in %s", qPrintable(dir));

    // One output per screen.
    std::vector<std::unique_ptr<WallpaperOutput>> outputs;
    for (QScreen *screen : QGuiApplication::screens()) {
        auto w = std::make_unique<WallpaperOutput>(&inst, screen);
        setupLayerShell(w.get(), screen);
        w->resize(screen->size());
        outputs.push_back(std::move(w));
        qInfo("vantapaper: output on %s (%dx%d)", qPrintable(screen->name()),
              screen->size().width(), screen->size().height());
    }

    // Decode one image and push it to every output (shared across them).
    auto showCurrent = [&]() {
        const QString path = playlist.current();
        if (path.isEmpty())
            return;
        HdrImage h = decodeImage(path);
        if (!h.valid()) {
            qWarning("vantapaper: failed to decode %s", qPrintable(path));
            return;
        }
        auto img = std::make_shared<const HdrImage>(std::move(h));
        for (auto &o : outputs)
            o->setImage(img);
        qInfo("vantapaper: showing %s", qPrintable(path));
    };

    showCurrent();
    for (auto &o : outputs)
        o->show();

    // Temporary test-only autorotation (Fase 1b adds real IPC + timer + pause).
    // VANTAPAPER_ROTATE_SECS=N rotates to the next image every N seconds.
    QTimer rotateTimer;
    bool okSecs = false;
    const int secs = qEnvironmentVariable("VANTAPAPER_ROTATE_SECS").toInt(&okSecs);
    if (okSecs && secs > 0) {
        QObject::connect(&rotateTimer, &QTimer::timeout, &app, [&]() {
            playlist.next();
            showCurrent();
        });
        rotateTimer.start(secs * 1000);
        qInfo("vantapaper: test rotation every %d s", secs);
    }

    return app.exec();
}
