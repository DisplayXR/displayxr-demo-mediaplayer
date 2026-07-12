# PRD — DisplayXR Stereo Media Player (demo)

**Repo (target):** `DisplayXR/displayxr-demo-mediaplayer`
**Status:** Draft for review
**Author:** David (with Claude Code)
**Date:** 2026-06-03

---

## 1. Summary

A lightweight, cross-platform **stereo media player** demo for the DisplayXR
runtime. It plays already-stereo **images** (side-by-side JPG/PNG, optional
HEIF/LIF) and **video** (side-by-side H.264/H.265/AV1) on a 3D display, with a
deliberately **subtle 3D UI**. It is an OpenXR *client* app: the DisplayXR
runtime + display processor do all weaving — the player never touches a vendor
SR SDK and never weaves.

It is the minimal, un-bloated counterpart to Leia's `LeiaPlayerWin` (Qt6 + VLC +
OpenCV + TensorRT/CUDA + direct SR-SDK weaving). We delete that entire stack by
leaning on the runtime for weaving and on FFmpeg for portable hardware decode.

### Non-goals (v1)
- **No 2D→3D conversion.** No depth model, no TensorRT, no CUDA *requirement*.
  (Deferred to a possible v2 as an optional ONNX module.)
- **No vendor SR SDK.** Weaving is the runtime's job (ADR-007).
- **macOS is in v1** on the same Vulkan codebase (MoltenVK/CAMetalLayer; no Metal
  backend needed — §3). Android is a v1.x stretch.
- **No editing/transcoding.** Playback only.

---

## 2. Why this is small (the core insight)

On DisplayXR, **the runtime + display processor already weave** (ADR-007:
compositor/app never weaves). A direct SR app like LeiaPlayerWin must link the
SR SDK and run weave shaders (`ViewShift.hlsl`, `SRContainer`). Ours does not.
The whole player collapses to:

> decode a stereo pair → blit L into the **left** OpenXR view, R into the
> **right** OpenXR view → submit. The runtime weaves.

That removes an entire subsystem (SR SDK, weaver, weave shaders) versus the
reference, and is what keeps the dependency set tiny.

---

## 3. Target platforms & graphics binding

| Platform | OpenXR graphics binding | Native compositor | v1? |
|---|---|---|---|
| Windows | Vulkan | `vk_native` | ✅ |
| Linux | Vulkan | `vk_native` | ✅ |
| macOS | Vulkan (via MoltenVK / CAMetalLayer) | `vk_native` (macOS path) | ✅ |
| Android | Vulkan | `vk_native` | stretch (v1.x) |

**macOS works via Vulkan — one codebase covers all four platforms.** The
runtime's `vk_native` macOS path (CAMetalLayer-backed surface through MoltenVK) is
actively maintained — `#392` transparent-present / alpha-weave fixes landed
2026-06-02, and `cube_handle_vk_macos` is a working Vulkan handle app. (An earlier
"MoltenVK fails with `VK_ERROR_EXTENSION_NOT_PRESENT`" note in the runtime docs is
**stale** — last touched 2026-02-15, fixed since.) **No Metal binding backend is
required.**

The one real macOS gotcha is a **dev-tree Vulkan loader-image conflict** (the dev
build's `libvulkan` vs the installed runtime's) — an environment/setup issue solved
by sharing one loader image / pinning `XR_RUNTIME_JSON`, not a capability limit. A
thin RHI seam is still worth keeping as cheap insurance (and a future *optional*
Metal optimization), but it's no longer on the critical path.

---

## 4. Architecture

```
            ┌─────────────────────────────────────────────┐
            │  Stereo Media Player (this app, _handle)     │
            │                                              │
  file ──▶  │  MediaSource ──▶ DecodeThread ──▶ FrameRing  │
            │  (image|video)   (FFmpeg hwaccel) (triple-buf)│
            │                                     │        │
            │                                     ▼        │
            │  RenderLoop: sample latest frame ──▶ RHI ────┼──▶ OpenXR stereo
            │   + ImGui UI (per-eye parallax)     (Vulkan)  │   swapchain (L,R)
            └───────────────────────────────────────────────┘
                                    │
                          OpenXR loader → DisplayXR runtime
                                    │
                          Display Processor (weaves)  ──▶  3D display
```

- **App class:** `_handle` — the app owns its window (SDL3) and hands the real
  window handle to the runtime via the window-binding extension
  (`XR_DXR_win32_window_binding` / `XR_DXR_android_surface_binding`). The app
  renders **both eyes**, which is what enables the subtle-parallax UI.
- **OpenXR session:** one stereo swapchain (two views). Per frame: blit decoded
  **L→view[0]**, **R→view[1]**; draw UI quads into both with a per-eye
  horizontal shift for depth. No Kooima 3D scene required — flat stereo + shifted
  overlays.
- **Weaving:** none in-app. Runtime/DP owns it.

### Module map
| Module | Responsibility |
|---|---|
| `MediaSource` | Identify format from filename/container; route to image or video path. SBS layout detection (`*_2x1`, `*_half_2x1`, LVF/LIF). |
| `ImageDecoder` | stb_image for JPG/PNG; optional libheif (CMake-gated) for HEIF/LIF. Produces one RGBA texture (SBS) + layout metadata. |
| `VideoDecoder` | FFmpeg `libav*`; hwaccel auto-select per OS + software fallback; decode thread; emits frames into `FrameRing`. |
| `FrameRing` | Triple-buffered, lock-light handoff of decoded frames to the render thread. Decouples decode rate from display rate. |
| `RHI` | Thin render abstraction (Vulkan now; Metal seam reserved). Swapchain image upload, SBS split sampler, UI pass. |
| `XrSession` | OpenXR lifecycle: instance/system/session/swapchain, frame loop, view submit, window binding. |
| `Ui` | Dear ImGui: transport bar, scrubber, file open, fullscreen, parallax/convergence slider, format HUD. |
| `App` | Wires window + input (SDL3) + session + decode + UI. |

---

## 5. Dependencies (the "not bloated" budget)

| Dep | Purpose | Notes |
|---|---|---|
| OpenXR loader | Talk to the runtime | Already the DisplayXR contract |
| **FFmpeg** (libavcodec/format/util/swscale) | Portable HW video decode + demux | hwaccel = D3D11VA/NVDEC (Win), VAAPI/NVDEC (Linux), MediaCodec (Android). **CUDA/NVDEC is just one backend — used if present, never required.** |
| **SDL3** | Window + input + (file dialog) | Cross-platform incl. Android. (GLFW is the fallback if SDL3 is inconvenient.) |
| **Dear ImGui** | In-scene UI | A few source files; renders into our own pass; per-eye for depth |
| **stb_image** | JPG/PNG decode | Two headers, zero deps |
| Vulkan SDK | Render backend | — |
| *(optional)* **libheif** | HEIF/LIF images | LGPL, heavier → behind `MEDIAPLAYER_WITH_HEIF` CMake option, OFF by default |

**Explicitly NOT used:** Qt/QML, VLC, OpenCV, TensorRT, CUDA-as-a-dependency,
vendor SR SDK. (CUDA may still be *used* transparently when FFmpeg picks NVDEC —
that's the "use when available, don't depend" line.)

---

## 6. Performance design

- **Decode thread + triple-buffered `FrameRing`.** The OpenXR render loop samples
  the latest-ready frame at present time → no judder, decode decoupled from
  display.
- **One decode per frame for SBS.** Both eyes come from the same decoded frame;
  split with a UV offset in the sampler. Never decode twice.
- **Zero-copy interop where the platform allows** (D3D11VA→Vulkan external
  memory, MediaCodec→`AHardwareBuffer`→Vulkan), with a universal download→upload
  fallback. **Ship the fallback first; add zero-copy per platform as
  optimization.**
- **Half-SBS support** (`*_half_2x1`): horizontal upscale in the sampler, no CPU
  cost.
- Target: smooth 1080p/4K SBS at display refresh on a mid GPU with the fallback
  path; zero-copy for headroom on 4K60.

---

## 7. macOS (Vulkan path) + optional Metal later

macOS rides the **same Vulkan codebase** — `vk_native`'s CAMetalLayer/MoltenVK path
(§3). No separate binding backend is needed; `cube_handle_vk_macos` is the
reference. Specifics:
- **Window/surface:** an `NSView` whose backing layer is a `CAMetalLayer` (see
  `cube_handle_vk_macos/main.mm`); MoltenVK presents Vulkan into it. The OpenXR
  binding is still the Vulkan one.
- **Video decode:** FFmpeg **VideoToolbox** hwaccel; simplest path downloads to a
  Vulkan-uploaded texture (works today). `CVMetalTextureCache` zero-copy is a later
  optimization if profiling demands it.
- **Loader gotcha:** avoid the two-`libvulkan` dev-tree conflict by sharing one
  loader image / pinning `XR_RUNTIME_JSON` (already documented in the runtime).
- **Optional Metal RHI later:** the thin RHI seam leaves room for a native-Metal
  backend as a *perf optimization* (zero-copy VideoToolbox interop), never a
  requirement.

---

## 8. Supported formats (v1)

**Images:** SBS JPG/PNG (`Name_2x1.ext`, `Name_half_2x1.ext`) via stb_image. HEIF
behind `MEDIAPLAYER_WITH_HEIF` CMake option, **OFF by default** (libheif is LGPL +
heavier — opt-in for LIF content). 2D images shown centered as flat (no conversion).
**Stereo LIF** (`.lif`): the container is parsed directly (no vendor SDK) — the two
embedded views are decoded and composed to a full-SBS frame (`src/media/LifLoader.*`,
nlohmann/json for the metadata). Mono+depth LIFs fall back to flat 2D for now; their
depth-based rendering (porting the raycast LIF shader) is a later step.
**Video:** SBS H.264 / H.265 / AV1 in MP4/MKV via FFmpeg; `*_2x1` / `*_half_2x1`
naming. 2D video shown flat.
**Detection:** filename suffix first (matches Leia convention), container hints
second.

---

## 9. UI — "subtle 3D" (Dear ImGui)

Because the app renders both eyes, UI depth is free via per-eye horizontal shift.
- Transport bar floats just **in front of** the convergence plane (small positive
  parallax) — hovers over the image.
- Media sits at zero-disparity inside a shallow depth-graded bezel → reads as a
  window, not a sticker.
- Hover/cursor highlight one depth-step above the UI plane.
- **Small parallax budget**, everything near zero-disparity = "subtle."
- Controls: open file, play/pause, scrub, fullscreen, convergence/parallax
  slider, format HUD, L/R swap toggle.

---

## 10. Build, naming, distribution

- **Naming convention** (matches runtime test/demo apps):
  `mediaplayer_handle_vk_win`, `mediaplayer_handle_vk_linux`
  (`mediaplayer_handle_vk_android` stretch).
- **CMake**, C++17. FetchContent for SDL3 / Dear ImGui / stb; FFmpeg via vcpkg
  (Win) / system (Linux).
- **Standalone demo repo** following the established demo pattern:
  - Own CI (`build-*.yml`), own tags, installers (Windows NSIS `.exe` + macOS
    `.pkg` — both v1, Vulkan path, no Metal prerequisite), test-app/demo binary
    release zip.
  - Joins the **versions.json auto-bump** matrix and the meta-installer bundle on
    release (`/dxr-release mediaplayer <version>` from the runtime hub once wired).
  - Runtime-compat README covenant per `docs/roadmap/demo-distribution.md`.

---

## 11. Milestones

| # | Milestone | Exit criteria |
|---|---|---|
| M0 | Skeleton | CMake builds; SDL3 window opens; OpenXR instance/session created against runtime; clears both eyes to a color. |
| M1 | Stereo image | Load SBS JPG/PNG; L/R routed to correct views; weaves correctly on a 3D display. |
| M2 | Stereo video | FFmpeg software decode → FrameRing → smooth SBS playback; transport bar. |
| M3 | HW decode | Platform hwaccel auto-select + fallback; 4K SBS smooth. |
| M4 | Subtle 3D UI | ImGui per-eye parallax; convergence slider; format HUD; fullscreen. |
| M5 | macOS (Vulkan) | Same codebase on a `cube_handle_vk_macos`-style CAMetalLayer/NSView surface; loader-image pinned. **In v1.** |
| M6 | Polish + ship | Windows + macOS installers, CI, versions.json wiring, README compat covenant; v1.0.0 tag. |
| (v1.x) | Android | Vulkan on-device; `AHardwareBuffer` zero-copy. |
| (v2) | optional 2D→3D + optional Metal RHI | ONNX depth module; native-Metal zero-copy VideoToolbox interop. |

---

## 12. Resolved defaults (2026-06-03)

These were open questions; settled with sensible minimal-scope defaults. Revisit
any of them if product priorities shift.

- **HEIF images → behind `MEDIAPLAYER_WITH_HEIF`, OFF by default.** v1 ships
  stb_image (JPG/PNG SBS) only. libheif (LGPL, heavier) is an opt-in build flag for
  Leia HEIF content — not on the default path.
- **Stereo LIF container parsing → implemented, on by default.** A two-view `.lif`
  is parsed in-tree (`LifLoader`, nlohmann/json header-only) and composed to SBS — no
  vendor SDK, per the GOLDEN RULE. Mono+depth/LDI synthesis (raycast LIF shader port)
  is the next step. Filename `*_2x1` / `*_half_2x1` detection still covers plain SBS
  JPG/PNG.
- **Single-file open in v1**, via a tiny native dialog (SDL3 / tinyfiledialogs).
  *Low-cost stretch:* once a file is open, arrow-key prev/next across the
  containing folder (cheap, no playlist UI). Full playlist/library UI is post-v1.
  (Under the shell, `XR_DXR_workspace_file_dialog` is a nicer spatial picker later
  — Windows + workspace-mode only, so not the v1 default path.)
