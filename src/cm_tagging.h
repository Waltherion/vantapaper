#pragma once

class QWindow;
struct wl_display;
struct wl_event_queue;
struct wp_color_manager_v1;
struct wp_color_management_surface_v1;
struct wp_image_description_v1;

namespace cm {

// Persistent colour-management handle for one window's wl_surface. Create it once
// the surface exists, then switch the surface's image description as the monitor
// flips between HDR and SDR. Qt's Vulkan path never tags the surface itself, so we
// drive wp-color-management-v1 directly here.
class SurfaceColor
{
public:
    explicit SurfaceColor(QWindow *window);
    ~SurfaceColor();

    bool valid() const { return m_manager != nullptr && m_cmSurface != nullptr; }

    void setWindowsScrgb(); // HDR: linear scRGB, true blacks + HDR headroom
    void setSrgb();         // SDR: plain sRGB (relative)

private:
    void applyDescription(wp_image_description_v1 *desc, const char *label);

    wl_display *m_display = nullptr;
    wl_event_queue *m_queue = nullptr;
    wp_color_manager_v1 *m_manager = nullptr;
    wp_color_management_surface_v1 *m_cmSurface = nullptr;
    bool m_supportsScrgb = false;
};

} // namespace cm
