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

🚧 **Pre-M0** — design complete, implementation starting. See `PRD.md` for the
full design and `docs/M0-KICKOFF.md` for the first build milestone.

## Requirements (planned)

- A working DisplayXR runtime install (or dev build) — this app cannot run without it.
- Vulkan SDK, CMake, a C++17 compiler. FFmpeg (vcpkg on Windows; system/Homebrew elsewhere).

## Build (planned — see `docs/M0-KICKOFF.md`)

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

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
