# M5 Notes — macOS (Vulkan / MoltenVK)

Milestone 5 formalizes the macOS path. Per `PRD.md` §11, the exit criterion is the
**same Vulkan codebase** running on a `cube_handle_vk_macos`-style CAMetalLayer/NSView
surface, with the loader image pinned. There is **no separate macOS backend** — macOS
rides the one Vulkan codebase via MoltenVK.

## Exit criteria (Definition of Done for M5)

1. `cmake -S . -B build -G Ninja && cmake --build build` produces
   `mediaplayer_handle_vk_macos`.
2. The app brings up the Vulkan/OpenXR stereo session against the DisplayXR runtime on
   macOS and routes L/R into the correct views — verified visually.
3. The two-`libvulkan` dev-tree loader conflict is avoided by pinning `XR_RUNTIME_JSON`.

All three are met. M5 needed **no new code** — the path was built incrementally during
M0–M4; this milestone is the formal verification + documentation.

## How the macOS path works (already in the codebase)

- **Window / surface:** `SDL_Metal_CreateView()` yields an `NSView` whose backing layer is
  a `CAMetalLayer` (`src/platform/Window.cpp`). That handle is passed to the runtime via
  `XR_DXR_cocoa_window_binding` (`src/xr/XrSession.cpp` — `XrCocoaWindowBindingCreateInfoDXR`).
  The OpenXR graphics binding is still the **Vulkan** one (`XrGraphicsBindingVulkanKHR`).
- **MoltenVK as a portability driver:** the loader needs the enumeration extension + flag
  to surface MoltenVK. We enable `VK_KHR_portability_enumeration` on the `VkInstance`
  (with `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`) and the mandatory
  `VK_KHR_portability_subset` on the `VkDevice` (`src/xr/XrSession.cpp:194,289`).
- **Video decode:** FFmpeg **VideoToolbox** hwaccel (M3), with the universal
  download→upload fallback. `CVMetalTextureCache` zero-copy is a deferred v2 optimization.

## The loader gotcha (and the fix)

The one real macOS-vk gotcha is the **dev-tree Vulkan loader-image conflict** — the dev
build's `libvulkan` vs the installed runtime's. It is an environment/setup issue, not a
capability limit (the old "MoltenVK fails with `VK_ERROR_EXTENSION_NOT_PRESENT`" note is
stale). The fix is to **share one loader image / pin `XR_RUNTIME_JSON`** so the OpenXR
loader picks the intended runtime.

`scripts/run_mediaplayer_handle_vk_macos.sh` does this automatically: it defaults
`XR_RUNTIME_JSON` to the sibling checkout's dev manifest
(`../displayxr-runtime/build/openxr_displayxr-dev.json`); override by exporting your own.
The shipped `.app` bundle resolves the loader/ICD from its own `Contents/Resources/lib`
(set up by `installer/macos/create_app_bundle.sh`), so installed runs don't hit the
dev-tree conflict at all.

## Verification (pixel proof, 2026-06-04)

Built on Apple **M1 Pro** and run against a local `displayxr-runtime` dev build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/tmp/openxr-install && cmake --build build

# 2-view (Squeezed SBS) — dump atlas + HUD after warmup
MEDIAPLAYER_HUD=1 MEDIAPLAYER_START_MODE=3 \
  MEDIAPLAYER_DUMP_ATLAS=/tmp/atlas_m3.png MEDIAPLAYER_DUMP_HUD=/tmp/hud_m3.png \
  scripts/run_mediaplayer_handle_vk_macos.sh assets/test_LR_2x1.png

# 4-view (Quad)
MEDIAPLAYER_HUD=1 MEDIAPLAYER_START_MODE=4 \
  MEDIAPLAYER_DUMP_ATLAS=/tmp/atlas_m4.png \
  scripts/run_mediaplayer_handle_vk_macos.sh assets/test_LR_2x1.png
```

Observed from the run log + dumps:

- `XR_RUNTIME_JSON` resolved to the dev manifest; `XR_DXR_cocoa_window_binding`
  negotiated over the NSView; `Vulkan device: Apple M1 Pro` with
  `VK_KHR_portability_enumeration` (instance) and `VK_KHR_portability_subset` (device).
- **Squeezed SBS atlas:** left view tile = green **L**, right view tile = magenta **R** —
  correct per-eye routing into the 2×1 atlas.
- **Quad atlas:** 2×2 tiling, both rows green **L** | magenta **R** — correct 4-view layout.
- **HUD:** transport bar + convergence slider render; stats line reads
  `114 FPS · Squeezed SBS · 3840×1080 SBS-full`.

### Useful env knobs

| Var | Effect |
|---|---|
| `MEDIAPLAYER_START_MODE=<n>` | 0=2D, 1=Anaglyph, 3=Squeezed SBS, 4=Quad |
| `MEDIAPLAYER_HUD=1` | show the window-space stats HUD |
| `MEDIAPLAYER_DUMP_ATLAS=<png>` | dump the swapchain atlas after ~100 frames |
| `MEDIAPLAYER_DUMP_HUD=<png>` | dump the HUD layer after ~100 frames |
| `MEDIAPLAYER_LOG_DEBUG=1` | verbose loader / extension logs |

## Known non-blocker

`displayxr-runtime#413` — the window-space HUD only composites in 2-view modes (it's
missing in 2D and in the Quad bottom row). This is runtime-side and does not affect the
media weave; tracked there, not in this demo.
