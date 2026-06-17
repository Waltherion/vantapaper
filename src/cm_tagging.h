#pragma once

class QWindow;

namespace cm {

// Tag the window's underlying wl_surface as "Windows-scRGB" via the
// wp-color-management-v1 protocol: sRGB primaries, extended-linear transfer,
// where pixel value 0.0 maps to 0 cd/m² (true black) and 1.0 to 80 cd/m².
//
// This is the whole point of vantapaper: Qt's Vulkan path never attaches a
// color-management image description to the surface, so Hyprland treats it as
// SDR and lifts the black floor (sdrbrightness). Tagging it ourselves makes the
// compositor pass it through as real HDR with true blacks -- exactly what
// Chromium does and mpv-based wallpapers cannot.
//
// Must be called after the window has a native wl_surface (i.e. once exposed).
// Returns true on success. The created protocol objects are deliberately kept
// alive for the process lifetime.
bool tagWindowWindowsScrgb(QWindow *window);

} // namespace cm
