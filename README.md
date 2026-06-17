# vantapaper

An HDR-correct wallpaper viewer for Hyprland (wlr-layer-shell) that keeps **true
blacks on OLED** — the one thing every existing Wayland wallpaper tool gets wrong
in HDR mode.

Status: **Fase 0a** — proving the core is even possible. Right now this is a bare
QRhi (Vulkan) test that puts an scRGB extended-linear **HDR** swapchain on a
layer-shell background surface and draws a test pattern:

- left ~45%: pure black `(0,0,0)` — on an OLED in HDR mode this must be a fully
  **off** pixel, indistinguishable from the bezel;
- center strip: SDR-white reference `(1.0)`;
- right ~45%: a gradient from SDR white up to `8.0` — must visibly **glow**
  brighter than the reference.

## Why

In Hyprland's HDR mode, SDR content is mapped up and the black floor gets lifted
(see hyprwm/Hyprland#9716), so OLED blacks look grey. Color-managed clients that
tag their own surface as HDR (Chromium, Qt6) are passed through correctly and keep
true black. vantapaper is built around that: own the swapchain, tag it HDR, decode
real HDR stills (UltraHDR gain-map JPEG via libultrahdr) that mpv-based tools
cannot read.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build

# Run on a specific output (connector name from `hyprctl monitors`), e.g. the OLED:
QT_FORCE_STDERR_LOGGING=1 ./build/vantapaper DP-1
```

Requires: Qt 6 (Gui + ShaderTools), layer-shell-qt, a Vulkan driver. Put the
monitor in HDR mode before running to judge the HDR output.

Note: Arch's Qt routes `qInfo`/`qDebug` to journald when stderr is not a TTY, so
set `QT_FORCE_STDERR_LOGGING=1` to see vantapaper's swapchain/HDR diagnostics
(backend, whether `HDRExtendedSrgbLinear` was negotiated, reported luminance).

## Roadmap

- **0a** QRhi HDR swapchain on layer-shell + test pattern  ← here
- **0b** Decode and show a real UltraHDR gain-map JPEG (libultrahdr)
- **1**  Core viewer: stills + video (libmpv), config, per-output
- **2**  Cycling, intelligent random, rotation, IPC, transitions
- **3**  Per-theme wallpaper mapping (hooks the theme switcher)
- **4**  Interactive overlays (a real terminal mapped onto a screen in the image)
