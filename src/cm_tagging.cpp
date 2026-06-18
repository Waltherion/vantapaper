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

SurfaceColor::SurfaceColor(QWindow *window)
{
    m_display = waylandDisplay();
    wl_surface *surface = waylandSurface(window);
    if (!m_display || !surface) {
        std::fprintf(stderr, "vantapaper: no native wl_display/wl_surface (not on Wayland?)\n");
        return;
    }

    ManagerState mgr;
    wl_registry *registry = wl_display_get_registry(m_display);

    // Dedicated event queue: our synchronous roundtrips dispatch ONLY our own
    // color-management objects, never Qt's surfaces -- otherwise a roundtrip
    // re-enters Qt's event loop. Objects bound from this registry inherit the queue.
    m_queue = wl_display_create_queue(m_display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), m_queue);

    wl_registry_add_listener(registry, &kRegistryListener, &mgr);
    wl_display_roundtrip_queue(m_display, m_queue); // resolve globals -> bind manager
    if (!mgr.manager) {
        std::fprintf(stderr, "vantapaper: compositor has no wp_color_manager_v1\n");
        return;
    }
    m_manager = mgr.manager;

    wp_color_manager_v1_add_listener(m_manager, &kManagerListener, &mgr);
    while (!mgr.done) {
        if (wl_display_roundtrip_queue(m_display, m_queue) < 0)
            break;
    }
    m_supportsScrgb = mgr.supportsWindowsScrgb;

    m_cmSurface = wp_color_manager_v1_get_surface(m_manager, surface);
}

SurfaceColor::~SurfaceColor() = default; // objects live for the surface's lifetime

void SurfaceColor::applyDescription(wp_image_description_v1 *desc, const char *label)
{
    if (!desc)
        return;
    ImageState img;
    wp_image_description_v1_add_listener(desc, &kImageListener, &img);
    while (!img.ready && !img.failed) {
        if (wl_display_roundtrip_queue(m_display, m_queue) < 0)
            break;
    }
    if (!img.ready) {
        std::fprintf(stderr, "vantapaper: image description (%s) never became ready\n", label);
        wp_image_description_v1_destroy(desc);
        return;
    }
    wp_color_management_surface_v1_set_image_description(
        m_cmSurface, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_image_description_v1_destroy(desc); // set has copy semantics
    wl_display_flush(m_display);
    std::fprintf(stderr, "vantapaper: surface tagged %s\n", label);
}

void SurfaceColor::setWindowsScrgb()
{
    if (!valid() || !m_supportsScrgb)
        return;
    applyDescription(wp_color_manager_v1_create_windows_scrgb(m_manager), "Windows-scRGB (HDR)");
}

void SurfaceColor::setSrgb()
{
    if (!valid())
        return;
    wp_image_description_creator_params_v1 *creator =
        wp_color_manager_v1_create_parametric_creator(m_manager);
    wp_image_description_creator_params_v1_set_tf_named(
        creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
    wp_image_description_creator_params_v1_set_primaries_named(
        creator, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    // create() consumes the creator and yields the image description.
    applyDescription(wp_image_description_creator_params_v1_create(creator), "sRGB (SDR)");
}

} // namespace cm
