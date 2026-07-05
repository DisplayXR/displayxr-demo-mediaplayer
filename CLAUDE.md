# CLAUDE.md — DisplayXR Stereo Media Player (demo)

Guidance for Claude Code working in **this** repo (`displayxr-demo-mediaplayer`).
Read `PRD.md` first for the full design; this file is the operating manual.

## What this is

A lightweight, cross-platform **stereo media player** demo for the DisplayXR
runtime. It plays already-stereo images (SBS JPG/PNG) and video (SBS H.264/265/AV1)
on a 3D display, with a subtle 3D UI. It is an OpenXR **client app** — the
DisplayXR runtime + display processor do all weaving. This app never weaves and
never links a vendor SR SDK.

The minimal, un-bloated counterpart to Leia's `LeiaPlayerWin` (Qt6 + VLC + OpenCV +
TensorRT/CUDA). We shed all of that.

## THE GOLDEN RULE — extension-wire-protocol only

This app is coupled to the runtime **only by the OpenXR extension wire protocol**,
exactly like any third-party OpenXR app (and like the DisplayXR Unity plugin).

- **NEVER** `add_subdirectory`, link, or `#include` the runtime's internal source
  (`xrt_*.h`, `aux_*`, compositor/driver code). That breaks the ADR-019 boundary.
- **DO** consume the DisplayXR **extension headers** from the published
  `DisplayXR/displayxr-extensions` repo (vendor a pinned copy under
  `third_party/displayxr-openxr/`, or FetchContent-pin it).
- **DO** read the runtime's **test apps** as a copy-from reference (see map below) —
  read, adapt, re-type. Don't reach back into the tree at build time.

## Reference map (runtime repo — sibling checkout, e.g. `../displayxr-runtime`)

| Need | Reference (in `displayxr-runtime`) |
|---|---|
| `_handle` + Vulkan + OpenXR session/swapchain/submit + window binding (golden template) | `test_apps/cube_handle_vk_win/` → `xr_session.cpp`, `vk_renderer.cpp`, `main.cpp` |
| macOS Vulkan surface (CAMetalLayer/NSView via MoltenVK) | `test_apps/cube_handle_vk_macos/main.mm` |
| Android Vulkan surface | `test_apps/cube_handle_vk_android/` |
| Shared helpers used by the test apps | `test_apps/common/` (vendor the minimal subset you need — these apps aren't standalone) |
| Extension API surface | `src/external/openxr_includes/openxr/` (also published to `displayxr-extensions`) |
| Window-binding contract | `docs/specs/extensions/XR_EXT_win32_window_binding.md`, `XR_EXT_android_surface_binding` |
| Swapchain / canvas model (views use **canvas** size, not display size) | `docs/specs/runtime/swapchain-model.md` |
| Display dims + eye-tracking modes | `docs/specs/extensions/XR_EXT_display_info.md` |

## Stack (keep it this small)

OpenXR loader + **FFmpeg** (libav\*) + **SDL3** + **Dear ImGui** + **stb_image**
(+ optional **libheif** behind `MEDIAPLAYER_WITH_HEIF`, OFF by default).
**Not used:** Qt/QML, VLC, OpenCV, TensorRT, CUDA-as-a-dependency, vendor SR SDK.
CUDA/NVDEC may be used *transparently* when FFmpeg picks it — never a build dep.

- SDL3 / Dear ImGui / stb → FetchContent (pinned).
- FFmpeg → vcpkg on Windows, system/Homebrew elsewhere. hwaccel auto-selected per
  OS (D3D11VA/NVDEC, VideoToolbox, VAAPI, MediaCodec) with CPU software fallback.

## Platforms & graphics binding

**One Vulkan codebase covers Windows, Linux, and macOS** (macOS via MoltenVK over a
CAMetalLayer-backed `NSView`). Android is a v1.x stretch (also Vulkan).

> **macOS note:** Vulkan works on macOS — do not believe any older "MoltenVK is
> broken / `VK_ERROR_EXTENSION_NOT_PRESENT`" claim. The only real macOS-vk gotcha is
> a two-`libvulkan` dev-tree loader-image conflict (dev build vs installed runtime);
> share one loader image / pin `XR_RUNTIME_JSON`.

No Metal binding backend is required. A thin RHI seam is kept only as optional v2
perf headroom.

## Build & naming

- CMake, C++17. Binaries named `mediaplayer_handle_vk_{win,linux,macos}`
  (Android: `mediaplayer_handle_vk_android`).
- Cross-platform portability: prefer `os_*`-style portable calls; guard any
  platform API behind `#ifdef`. (This is a thin client app, so the surface is
  small — window/surface creation and the video-decode hwaccel selection.)

### Linux dev build (build-green, issue #30)

`./scripts/build_linux.sh` — system Vulkan + from-source OpenXR loader (pinned
`release-1.1.43`, kept equal to the CMake FetchContent `GIT_TAG`), then a Ninja
Release build of `mediaplayer_handle_vk_linux`. Deps: see the apt list in
`.github/workflows/build-linux.yml` (base toolchain + FFmpeg dev packages +
the X11/Wayland/ALSA headers SDL3 needs when built from source). That workflow
is **non-required** and fires only on `workflow_dispatch` + `linux*` branches.
`scripts/run_mediaplayer_handle_vk_linux.sh` sets the dev-runtime env
(`XR_RUNTIME_JSON`, `XRT_PLUGIN_SEARCH_PATH`, `OXR_ENABLE_VK_NATIVE_COMPOSITOR=1`,
`SIM_DISPLAY_OUTPUT=anaglyph`).

**Status: BUILD-GREEN, window binding wired.** `XR_EXT_xlib_window_binding`
(runtime Phase 3a) is fully wired: `Window.cpp` extracts the (Display*, XID)
pair from SDL's X11 properties (bundled as `Window::X11Handles` behind the
one-void* handle plumbing) and prefers SDL's x11 driver (XWayland on Wayland
desktops); `XrSession.cpp` chains `XrXlibWindowBindingCreateInfoEXT` when the
runtime advertises the extension. On-screen validation is gated on the
runtime's Linux Phase 1b/3b hardware bring-up. Recipe: the runtime repo's
`docs/guides/linux-demo-port.md`.

## Run / test

This app **requires a running/installed DisplayXR runtime**. Point the loader at a
dev build:

```bash
# Linux / macOS
XR_RUNTIME_JSON=/path/to/displayxr-runtime/build/openxr_displayxr-dev.json ./mediaplayer_handle_vk_...
```
```cmd
:: Windows — use a NON-elevated terminal (elevated procs ignore XR_RUNTIME_JSON and
:: fall back to HKLM ActiveRuntime). Or copy binaries into C:\Program Files\DisplayXR\Runtime.
```
**Which runtime DLL loaded?** the per-`xrCreateInstance` log under
`%LOCALAPPDATA%\DisplayXR\` (search `loaded from:`).

## Conventions (inherited from the DisplayXR org)

- **Feature work on a branch in a worktree** (`.claude/worktrees/`), not on `main`.
- **Include the GitHub issue number** in commit messages once this repo has issues.
- **Prefer local builds** for iteration; CI is for validation + releases.
- Distribution follows the demo pattern: own CI/tags, Windows NSIS `.exe` + macOS
  `.pkg` installers, `versions.json` autobump, `/dxr-release mediaplayer <ver>` from
  the runtime hub once wired. Carry a runtime-compat covenant in `README.md`.
- **Dev-build dependency rule — this repo is the reference.** OpenXR + the loader
  are **self-provisioned by CMake `FetchContent`** (`OpenXR-SDK`, pinned
  `release-*`), not a hardcoded SDK path and no separate `build-with-deps.bat` —
  a fresh clone builds with only the toolchain + Vulkan SDK. Keep it that way:
  never hardcode `C:/dev/openxr_sdk` / `C:/VulkanSDK/<ver>`; if you bump the
  vendored `openxr_includes/` headers, bump the `FetchContent` `GIT_TAG` to match.
  (The other demos had to retrofit this after shipping with the hardcoded paths.)

## Where to start

`docs/M0-KICKOFF.md` is the build prompt for the first milestone (skeleton: SDL3
window + OpenXR stereo session clearing both eyes). Milestones M0–M6 are in
`PRD.md` §11.

## MCP atlas capture (agent-side debugging)

`.mcp.json` registers the `displayxr` MCP server — the DisplayXR MCP adapter
installed by `DisplayXRMCPSetup` (`HKLM\Software\DisplayXR\Capabilities\MCP`).
When that capability is installed, **every OpenXR app process hosts an
in-process MCP server**, so a running `mediaplayer_handle_vk_win` exposes:

- `capture_frame` — writes the composed atlas as
  `%TEMP%\displayxr-mcp-capture-<pid>-<frame>.png` and returns the path
  (modes: `post-compose` default, `projection-only`). Read the PNG to see
  exactly what the display processor receives, per tile.
- `diff_projection`, `get_kooima_params`, `get_submitted_projection`,
  `get_display_info`, `get_runtime_metrics`, `tail_log`.

Workflow:

1. **Launch the app first**, then start the Claude session — or run `/mcp` →
   reconnect `displayxr` after launching (the adapter binds at spawn time).
2. `--target auto` attaches shell → service → unique app PID. If more than
   one OpenXR app is running, pin it: change args to `--target pid:<pid>`.
3. Call `capture_frame`, then Read the returned PNG path.

Non-Windows: set `DISPLAYXR_MCP_ADAPTER` to the adapter's install path before
launching Claude (the `.mcp.json` default is the Windows path).
