# DisplayXR Stereo Media Player

A lightweight, cross-platform **stereo media player** for the
[DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime). Plays
already-stereo **images** (side-by-side JPG/PNG) and **video** (side-by-side
H.264/H.265/AV1) on a 3D display, with a subtle 3D UI.

It's an OpenXR **client app**: the DisplayXR runtime + display processor do the
weaving. The player just decodes a stereo pair and submits left/right views — no
vendor SR SDK, no weaving, no CUDA dependency.

> The minimal, un-bloated counterpart to Leia's `LeiaPlayerWin` — built on
> OpenXR + FFmpeg + SDL3 + Dear ImGui + stb. One Vulkan codebase for Windows,
> Linux, and macOS (Android planned).

## Status

✅ **M0–M6 complete — v1.0.0.** The full milestone arc is in: SDL3 window → OpenXR stereo
session → runtime weave (M0); SBS L/R routing (M1); FFmpeg video decode through a
triple-buffered `FrameRing` with aspect-correct letterboxing (M2); per-OS hardware decode
(VideoToolbox / D3D11VA+NVDEC / VAAPI) with software fallback (M3); a subtle-3D Dear ImGui
transport bar with per-eye parallax, convergence/L-R-swap controls, fullscreen, open-file,
and low-latency frame-exact scrubbing (M4); the macOS Vulkan path (M5); and Windows + macOS
installers with CI release-on-tag (M6).

Stereo images (JPG/PNG via stb_image) and SBS video (`*_2x1` / `*_half_2x1`) share the same
decode → atlas → submit path; the runtime + display processor do all weaving.

**Verified on macOS (Apple Silicon, MoltenVK)** against a local `displayxr-runtime` dev
build — correct per-eye L/R routing in 2-view (Squeezed SBS) and 4-view (Quad) modes, HUD
compositing, 114 FPS on M1 Pro. See [`docs/M5-NOTES.md`](docs/M5-NOTES.md). Windows builds
in CI; the macOS Vulkan path uses `VK_KHR_portability_enumeration` + `VK_KHR_portability_subset`
over an `XR_DXR_cocoa_window_binding` NSView.

Also in: live (continuous) window resize, an FPS counter in the title bar, and a
window-space stats **HUD** (toggle with **SHIFT+TAB**) showing fps / mode / source /
canvas / per-view tile.

```bash
# play a side-by-side stereo video or image
scripts/run_mediaplayer_handle_vk_macos.sh /path/to/clip_2x1.mp4
scripts/run_mediaplayer_handle_vk_macos.sh assets/test_LR_2x1.png
```

Keys: **V** cycles display modes, **SHIFT+TAB** toggles the HUD, **Esc** quits. With no
file argument it falls back to a RED|BLUE left/right test pattern. See `PRD.md` §11 for
the milestone map.

## Requirements

- A working DisplayXR runtime install (or dev build) — this app cannot run without it.
- Vulkan (SDK, or Homebrew `vulkan-loader` + `molten-vk` on macOS), an OpenXR loader,
  CMake ≥ 3.24, a C++17 compiler. SDL3 is fetched and built automatically.
- FFmpeg / Dear ImGui / stb arrive in M1–M4 (not needed for M0).

## Build & run (macOS — verified)

```bash
# Configure + build. The OpenXR loader is auto-found via find_package; if you built
# the Khronos OpenXR-SDK to a custom prefix, hint it with -DCMAKE_PREFIX_PATH.
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/tmp/openxr-install
cmake --build build

# Run against a local DisplayXR dev runtime (helper sets XR_RUNTIME_JSON to the
# sibling checkout's dev manifest; override XR_RUNTIME_JSON to point elsewhere).
scripts/run_mediaplayer_handle_vk_macos.sh
# equivalently:
XR_RUNTIME_JSON=/path/to/displayxr-runtime/build/openxr_displayxr-dev.json \
    ./build/mediaplayer_handle_vk_macos
```

Press **Esc** or close the window to exit. The two-plus eye views clear to distinct
colors and weave on a 3D display (a single color shows on a 2D-fallback mode).
Set `MEDIAPLAYER_LOG_DEBUG=1` for verbose logs.

> If `find_package(OpenXR)` finds nothing, the build fetches and compiles the
> pinned Khronos OpenXR-SDK loader automatically.

## Supported formats (v1 target)

| Type | Formats | Notes |
|---|---|---|
| Images | SBS JPG/PNG (`*_2x1`, `*_half_2x1`) | HEIF/LIF behind `MEDIAPLAYER_WITH_HEIF` (off by default) |
| Video | SBS H.264 / H.265 / AV1 (MP4/MKV) | hardware decode via FFmpeg, CPU fallback |

## Runtime compatibility

<!-- Covenant: which displayxr-runtime versions this demo is verified against.
     Updated on each release per docs/roadmap/demo-distribution.md. -->
| Media player | Verified against runtime |
|---|---|
| v1.0.0 | v1.10.2 |

v1.0.0 is verified against the DisplayXR runtime **v1.10.2** release line (the
current `versions.json[runtime]` pin). Coupling is **extension-wire-protocol only** —
the demo needs the runtime's window-binding + stereo-session extensions
(`XR_DXR_win32_window_binding` / `XR_DXR_cocoa_window_binding`, `XR_DXR_display_info`),
so any runtime exposing that protocol should work, but v1.10.2 is the combination this
release was validated against.

## License

Apache-2.0 — see `LICENSE`. Bundled media samples carry their own licenses.
