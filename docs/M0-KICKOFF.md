# M0 Kickoff — Skeleton

This is the build prompt for **Milestone 0** of the DisplayXR Stereo Media Player.
Run a fresh Claude Code session **in this repo** and paste the prompt in §"Prompt"
below. Read `../CLAUDE.md` and `../PRD.md` first.

---

## Goal

A buildable skeleton that opens a window and brings up an OpenXR **stereo** session
against the DisplayXR runtime, clearing the two eye views to distinct colors. No
decode, no UI yet — just prove the window → OpenXR → runtime → display path.

## Exit criteria (Definition of Done for M0)

1. `cmake -S . -B build && cmake --build build` succeeds on the dev machine.
2. Binary `mediaplayer_handle_vk_<platform>` launches, creates an SDL3 window, and
   hands its native window handle to the runtime via the window-binding extension
   (`XR_EXT_win32_window_binding` on Windows; the macOS/Android equivalents stubbed
   or wired per platform).
3. An OpenXR instance + system + **stereo** session + swapchain are created against
   the DisplayXR runtime (verified via the runtime's `loaded from:` log line).
4. The render loop clears **view[0] (left)** and **view[1] (right)** to two
   *different* solid colors and submits a projection layer each frame.
5. On a 3D display the two colors weave; on a 2D fallback you at least see one
   color. Clean shutdown (no validation errors, no leaks on exit).

## Reference (read, adapt, re-type — do NOT link the runtime tree)

- **Golden template:** `../displayxr-runtime/test_apps/cube_handle_vk_win/`
  - `xr_session.cpp` — OpenXR instance/system/session/swapchain lifecycle + frame loop + view submit
  - `vk_renderer.cpp` — Vulkan device/swapchain-image plumbing (take only what M0 needs: clear + present)
  - `main.cpp` — window creation + window-binding extension wiring
- **macOS surface:** `../displayxr-runtime/test_apps/cube_handle_vk_macos/main.mm`
  (CAMetalLayer-backed NSView)
- **Extension headers:** vendor a pinned copy of
  `DisplayXR/displayxr-extensions` into `third_party/displayxr-openxr/`.
- **Swapchain/canvas semantics:** `../displayxr-runtime/docs/specs/runtime/swapchain-model.md`.

## Constraints (from CLAUDE.md — do not violate)

- **Extension-wire-protocol only.** No `#include`/link of runtime internals
  (`xrt_*`, `aux_*`, compositor/driver source). Vendor extension headers instead.
- App class is **`_handle`** — the app owns the window and renders **both eyes**.
- **The app never weaves.** Submit plain stereo views; the runtime/DP weaves.
- Keep the dependency set to OpenXR loader + SDL3 + Vulkan for M0. (FFmpeg / ImGui /
  stb come in M1–M4.)
- Portable Vulkan: one code path for Win/Linux/macOS; isolate only
  window/surface creation per platform.

## Deliverables for M0

- `CMakeLists.txt` (FetchContent-pin SDL3; find Vulkan + OpenXR loader; vendor
  extension headers).
- `src/` skeleton mirroring the PRD module map but stubbed:
  `App`, `XrSession` (real for M0), `RHI`/Vulkan (clear+present only),
  empty `MediaSource` / `VideoDecoder` / `ImageDecoder` / `FrameRing` / `Ui`.
- A `run_mediaplayer_handle_vk_<platform>` helper that sets `XR_RUNTIME_JSON` to the
  local dev runtime.
- Update `README.md` status to "M0 complete" with the exact build/run commands that
  worked.

## Dev loop notes

- Requires a running/installed DisplayXR runtime. Point `XR_RUNTIME_JSON` at a dev
  build; on Windows use a **non-elevated** terminal (elevated procs ignore
  `XR_RUNTIME_JSON`).
- Confirm the right runtime loaded via `%LOCALAPPDATA%\DisplayXR\` log (`loaded from:`).
- macOS: if `xrGetVulkanGraphicsDeviceKHR` fails, it's the two-`libvulkan`
  loader-image conflict — share one loader image / pin `XR_RUNTIME_JSON`, not a code bug.

---

## Prompt (paste into a fresh Claude Code session in this repo)

> You are implementing **Milestone 0** of the DisplayXR Stereo Media Player.
> Read `CLAUDE.md` and `PRD.md` in this repo, and `docs/M0-KICKOFF.md` (this file)
> in full before writing code.
>
> Build a minimal skeleton: an SDL3 window that hands its native handle to the
> DisplayXR runtime via the window-binding extension, creates an OpenXR **stereo**
> session + swapchain, and runs a render loop that clears the left view to one color
> and the right view to another, submitting a projection layer each frame.
>
> Use `../displayxr-runtime/test_apps/cube_handle_vk_win/` (`xr_session.cpp`,
> `vk_renderer.cpp`, `main.cpp`) as the copy-from reference, taking only what a
> clear-and-present skeleton needs. **Hard rule:** do not link or `#include` any
> runtime-internal source (`xrt_*`, `aux_*`, compositor/driver) — vendor the
> DisplayXR OpenXR **extension headers** from `DisplayXR/displayxr-extensions` into
> `third_party/displayxr-openxr/` instead. The app is a `_handle` client that
> renders both eyes and **never weaves**.
>
> Set up CMake (FetchContent-pin SDL3; find Vulkan + the OpenXR loader), a stubbed
> `src/` module layout per the PRD, and a run helper that points `XR_RUNTIME_JSON`
> at a local dev runtime. Target binary name `mediaplayer_handle_vk_<platform>`.
>
> Work on a branch in `.claude/worktrees/`, not on `main`. When it builds and the
> two eye-views clear to distinct colors against the runtime, update `README.md`
> status to "M0 complete" with the exact commands that worked, and summarize what
> you did + any deviations.
