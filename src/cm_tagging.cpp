#include "cm_tagging.h"

#include <QGuiApplication>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformnativeinterface.h>

#include <wayland-client.h>
#include "color-management-v1-client-protocol.h"

#include <cstdio>
#include <cstring>

namespace {

struct ManagerState {
    wp_color_manager_v1 *manager = nullptr;
    uint32_t name = 0;
    uint32_t version = 0;
    bool supportsWindowsScrgb = false;
    bool done = false;
};

void registryGlobal(void *data, wl_registry *reg, uint32_t name,
                    const char *iface, uint32_t version)
{
    auto *st = static_cast<ManagerState *>(data);
    if (std::strcmp(iface, wp_color_manager_v1_interface.name) == 0) {
        st->name = name;
        st->version = version;
        st->manager = static_cast<wp_color_manager_v1 *>(
            wl_registry_bind(reg, name, &wp_color_manager_v1_interface, 1));
    }
}
void registryGlobalRemove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kRegistryListener = { registryGlobal, registryGlobalRemove };

void mgrSupportedIntent(void *, wp_color_manager_v1 *, uint32_t) {}
void mgrSupportedFeature(void *data, wp_color_manager_v1 *, uint32_t feature)
{
    if (feature == WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB)
        static_cast<ManagerState *>(data)->supportsWindowsScrgb = true;
}
void mgrSupportedTfNamed(void *, wp_color_manager_v1 *, uint32_t) {}
void mgrSupportedPrimariesNamed(void *, wp_color_manager_v1 *, uint32_t) {}
void mgrDone(void *data, wp_color_manager_v1 *) { static_cast<ManagerState *>(data)->done = true; }
const wp_color_manager_v1_listener kManagerListener = {
    mgrSupportedIntent, mgrSupportedFeature, mgrSupportedTfNamed,
    mgrSupportedPrimariesNamed, mgrDone
};

struct ImageState {
    bool ready = false;
    bool failed = false;
};
void imgFailed(void *data, wp_image_description_v1 *, uint32_t, const char *msg)
{
    static_cast<ImageState *>(data)->failed = true;
    std::fprintf(stderr, "vantapaper: image description failed: %s\n", msg ? msg : "(no message)");
}
void imgReady(void *data, wp_image_description_v1 *, uint32_t) { static_cast<ImageState *>(data)->ready = true; }
void imgReady2(void *data, wp_image_description_v1 *, uint32_t, uint32_t) { static_cast<ImageState *>(data)->ready = true; }
const wp_image_description_v1_listener kImageListener = { imgFailed, imgReady, imgReady2 };

wl_display *waylandDisplay()
{
    if (auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        return app->display();
    return nullptr;
}

wl_surface *waylandSurface(QWindow *window)
{
    QPlatformNativeInterface *ni = QGuiApplication::platformNativeInterface();
    if (!ni)
        return nullptr;
    return static_cast<wl_surface *>(ni->nativeResourceForWindow("surface", window));
}

} // namespace

namespace cm {

bool tagWindowWindowsScrgb(QWindow *window)
{
    wl_display *display = waylandDisplay();
    wl_surface *surface = waylandSurface(window);
    if (!display || !surface) {
        std::fprintf(stderr, "vantapaper: no native wl_display/wl_surface (not on Wayland?)\n");
        return false;
    }

    ManagerState mgr;
    wl_registry *registry = wl_display_get_registry(display);

    // Dedicated event queue: our synchronous roundtrips must dispatch ONLY our own
    // color-management objects, never Qt's surfaces. Otherwise a roundtrip inside
    // exposeEvent re-enters Qt's event loop and a second output's tagging runs
    // nested inside the first -- leaving the first stuck (only one surface tagged).
    // Objects bound from this registry inherit its queue.
    wl_event_queue *queue = wl_display_create_queue(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), queue);

    wl_registry_add_listener(registry, &kRegistryListener, &mgr);
    wl_display_roundtrip_queue(display, queue); // resolve globals -> bind manager
    if (!mgr.manager) {
        std::fprintf(stderr, "vantapaper: compositor has no wp_color_manager_v1\n");
        return false;
    }

    wp_color_manager_v1_add_listener(mgr.manager, &kManagerListener, &mgr);
    while (!mgr.done) { // collect all supported_* events until 'done'
        if (wl_display_roundtrip_queue(display, queue) < 0)
            break;
    }
    if (!mgr.supportsWindowsScrgb) {
        std::fprintf(stderr, "vantapaper: compositor lacks the windows_scrgb feature\n");
        return false;
    }

    wp_image_description_v1 *desc = wp_color_manager_v1_create_windows_scrgb(mgr.manager);
    ImageState img;
    wp_image_description_v1_add_listener(desc, &kImageListener, &img);
    while (!img.ready && !img.failed) {
        if (wl_display_roundtrip_queue(display, queue) < 0)
            break;
    }
    if (!img.ready) {
        std::fprintf(stderr, "vantapaper: Windows-scRGB image description never became ready\n");
        return false;
    }

    wp_color_management_surface_v1 *cmSurface =
        wp_color_manager_v1_get_surface(mgr.manager, surface);
    wp_color_management_surface_v1_set_image_description(
        cmSurface, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);

    // set_image_description has copy semantics; the description may be destroyed now.
    wp_image_description_v1_destroy(desc);

    // Flush the requests; the (double-buffered) image description is applied on
    // the surface's next wl_surface.commit, which the next rendered frame does.
    wl_display_flush(display);

    std::fprintf(stderr, "vantapaper: surface tagged Windows-scRGB -> true-black HDR\n");

    // cmSurface, manager and registry are intentionally leaked: they must remain
    // alive for the surface's (process) lifetime.
    return true;
}

} // namespace cm
