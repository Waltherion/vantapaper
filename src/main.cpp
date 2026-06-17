#include <QGuiApplication>
#include <QVulkanInstance>
#include <QScreen>
#include <rhi/qrhi.h>

#include <LayerShellQt/window.h>

#include "rhiwindow.h"

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    QVulkanInstance inst;
    inst.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (!inst.create())
        qFatal("vantapaper: failed to create Vulkan instance");

    // Optional first argument: output connector name (e.g. "DP-1") to place the
    // wallpaper on a specific monitor. Defaults to the primary screen.
    QScreen *targetScreen = nullptr;
    if (argc > 1) {
        const QString want = QString::fromLocal8Bit(argv[1]);
        for (QScreen *s : QGuiApplication::screens()) {
            if (s->name() == want) {
                targetScreen = s;
                break;
            }
        }
        if (!targetScreen)
            qWarning("vantapaper: no screen named '%s'; using primary", qPrintable(want));
    }

    RhiWindow window(&inst);
    if (targetScreen)
        window.setScreen(targetScreen);

    // Put the window on the wlr-layer-shell background layer, filling the whole
    // output and reserving no exclusive zone (-1 = ignore other surfaces' zones).
    if (LayerShellQt::Window *ls = LayerShellQt::Window::get(&window)) {
        ls->setLayer(LayerShellQt::Window::LayerBackground);
        ls->setAnchors(LayerShellQt::Window::Anchors(
            LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
            | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        ls->setExclusiveZone(-1);
        ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        ls->setScope(QStringLiteral("vantapaper"));
        if (targetScreen)
            ls->setScreen(targetScreen);
    } else {
        qWarning("vantapaper: LayerShellQt::Window::get returned null -- not on a layer surface!");
    }

    window.resize(1280, 720); // hint only; the compositor sizes us when anchored to all edges
    window.show();

    return app.exec();
}
