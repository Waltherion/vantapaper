# vantapaper

An HDR-correct wallpaper daemon for Hyprland (wlr-layer-shell) that keeps **true
blacks on OLED** — the one thing every existing Wayland wallpaper tool gets wrong
in HDR mode.

It owns its own Vulkan swapchain and tags it through `wp-color-management-v1`, so
HDR wallpapers display with real highlights *and* genuinely off black pixels on an
OLED, while SDR monitors (and HDR-off) get correct, untagged sRGB output. It decodes
real HDR stills — including UltraHDR gain-map JPEGs that mpv-based tools cannot read.

## Why

In Hyprland's HDR mode the compositor maps SDR content up and lifts the black floor
(see [hyprwm/Hyprland#9716](https://github.com/hyprwm/Hyprland/issues/9716)), so OLED
blacks look grey. Color-managed clients that tag their own surface keep true black.
vantapaper is built around exactly that: own the swapchain, tag it correctly per
output, and render in linear scRGB.

## Features

- **True black on OLED** in HDR mode via `wp-color-management-v1` (windows-scRGB).
- **Per-output, live HDR/SDR.** Each monitor renders in its current mode; toggling a
  monitor's HDR on/off is picked up live (polls Hyprland) and the surface re-tags
  without a restart. HDR images on an SDR monitor are tone-mapped, not blown out.
- **Wide HDR still support:** AVIF, JPEG-XL, HEIC/HEIF (PQ/HLG, 10/12-bit), 16-bit
  HDR PNG (cICP), UltraHDR gain-map JPEG, plus all the usual SDR formats. ICC
  profiles are read via lcms2.
- **Autorotation** with pause/play/next/previous, ascending **or** shuffle-bag
  random order (every image once before any repeat).
- **Transitions:** fade, wipe, slide (push), grow, shrink — random mix per change,
  or none.
- **Async decode** (background thread, parallel rows + transfer LUTs) so switches
  never stutter; idle cost is near-zero (renders once, only animates a transition).
- **IPC** control via a small `vantapaper ctl` client.
- Writes a per-output symlink to the current image for lock-screen integration.

## Build

Requires: Qt 6 (Gui + GuiPrivate + ShaderTools + Network + Concurrent), layer-shell-qt,
wayland-client, wayland-protocols, libavif, libjxl, libheif, libultrahdr, lcms2, and a
working **Vulkan** driver (HDR on Wayland needs the Vulkan RHI backend).

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Arch users can build the package from the included `PKGBUILD` (`makepkg -si`).

## Run

vantapaper is a daemon: launch one instance, it covers every output.

```sh
setsid -f vantapaper        # detach into the background
```

It reads wallpapers from the folder set by `path` in the config (default
`~/Pictures/wallpapers`). Rotation, order and transitions come from the config too.

Note: Arch's Qt routes `qInfo` to journald when stderr is not a TTY; set
`QT_FORCE_STDERR_LOGGING=1` to see swapchain/HDR diagnostics.

### Control

```sh
vantapaper ctl next            # advance
vantapaper ctl previous        # go back
vantapaper ctl toggle-pause    # pause/resume autorotation
vantapaper ctl pause | play
vantapaper ctl status          # prints "paused" / "playing"
vantapaper ctl reload          # re-scan the folder
vantapaper ctl show <path>     # jump to a specific image
```

Example Hyprland binds:

```ini
bind = SUPER, L, exec, vantapaper ctl next
bind = SUPER, J, exec, vantapaper ctl previous
bind = SUPER, K, exec, vantapaper ctl toggle-pause
```

### Utilities

```sh
vantapaper probe <image>              # report whether vantapaper can decode a file
vantapaper thumbnail <in> <out> [px]  # HDR-aware tone-mapped sRGB thumbnail (for pickers)
```

## Configuration

`~/.config/vantapaper/config.jsonc` (written with defaults on first run; JSONC, so
`//` and `/* */` comments and trailing commas are allowed):

```jsonc
{
  "path": "~/Pictures/wallpapers",  // source folder ("~" and $ENV are expanded)

  "durationSecs": 300,        // seconds between automatic changes
  "startPaused": false,       // start with autorotation paused

  // "ascending" -> alphabetical. "random" -> shuffle-bag (every image once
  // before any repeat, never repeating across a reshuffle).
  "sort": "ascending",

  "transition": {
    // any of: "fade", "wipe", "slide", "grow", "shrink"  (one picked at random per change)
    // use [] or ["none"] for instant switches
    "enabled": ["fade", "wipe", "slide", "grow", "shrink"],
    "durationMs": 600
  }
}
```

The play/paused state is also persisted to `$XDG_STATE_HOME/vantapaper/play-state`,
so it survives restarts and theme switches.

## How it works

One `WallpaperOutput` per `QScreen` owns a QRhi/Vulkan swapchain. On an HDR monitor it
uses an extended-linear scRGB swapchain and tags the surface windows-scRGB; otherwise
it uses an sRGB swapchain with no CM tag and the fragment shader applies the sRGB OETF.
Images are decoded to fp16 linear (1.0 = 203 nits) on a background thread; PQ/HLG/sRGB
transfer and BT.2020→BT.709 gamut conversion happen at load time. Switching cross-fades
two textures through a transition shader. Hyprland's per-monitor `colorManagementPreset`
is polled so HDR↔SDR toggles re-negotiate the swapchain live.

## Roadmap

- Video wallpapers (libmpv underlay)
- Interactive overlays — a real terminal mapped onto a "screen" inside the image
- Wallpaper Engine scene/web compatibility (maybe)

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
