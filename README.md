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

✅ **M1 complete** — stereo image playback. Loads a side-by-side JPG/PNG (stb_image),
detects the SBS layout from the filename (`*_2x1` / `*_half_2x1`, aspect fallback),
uploads it as a Vulkan texture, and samples the correct half into each display view
(left-eye views ← left half, right-eye views ← right half) across all N views of the
active rendering mode. Builds on the M0 skeleton (SDL3 window → OpenXR stereo session
→ runtime weave). Verified on macOS (Apple Silicon, MoltenVK) against a local
`displayxr-runtime` dev build; Windows scaffolded but unverified. Next: M2 (video).

```bash
# show a side-by-side stereo image (press V to cycle display modes, Esc to quit)
scripts/run_mediaplayer_handle_vk_macos.sh assets/test_LR_2x1.png
```

With no file argument it falls back to a RED|BLUE left/right test pattern. See `PRD.md`
§11 for the milestone map.

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
     Fill in on first release per docs/roadmap/demo-distribution.md. -->
| Media player | Verified against runtime |
|---|---|
| (unreleased) | — |

## License

TBD (match the DisplayXR demo convention).
