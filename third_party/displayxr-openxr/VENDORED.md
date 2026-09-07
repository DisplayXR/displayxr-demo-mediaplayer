# Vendored DisplayXR OpenXR headers

This directory holds a **pinned copy** of the OpenXR core + DisplayXR extension
headers. Per CLAUDE.md's golden rule, this app couples to the runtime *only* through
the OpenXR extension wire protocol — we never `#include`/link runtime-internal source.
These headers are the wire-protocol surface, vendored so the build never reaches back
into the runtime tree.

## Source

Copied from `DisplayXR/displayxr-runtime` at:

    src/external/openxr_includes/openxr/

Pinned commit: `f1347340ea0dcc5d24da2f228b59d63441ca3ec7`
(`f1347340e refactor(test-apps): macOS cube_handle capture → xrCaptureAtlasDXR + mip parity [#396 W6]`)

`XR_DXR_xlib_window_binding.h` alone is pinned newer, from
`19c4e014f0d8f79e554c5e18860152c3efc5137a`
(`19c4e014f linux(#660): Phase 3a — XR_DXR_xlib_window_binding`).

`XR_DXR_android_surface_binding.h` is pinned newer still, from
`8664e15130462086baf52ff155efaf0e93a8785b` — **spec v2**, which adds the
`XrEventDataAndroidWindowLayoutHintDXR` mini-window layout hint (runtime#1396).

> ⚠️ That commit is the tip of the **unmerged** runtime PR branch
> `feat/1396-android-window-layout-hint` (DisplayXR/displayxr-runtime#1398), not a
> commit on `main`. **Re-pin this one header to the merge commit once that PR
> lands**, and re-copy it in case the struct changed in review. Nothing else in
> this directory moved.

> When `DisplayXR/displayxr-extensions` is published as a standalone repo, re-pin
> from there (FetchContent or a vendored snapshot) instead of the runtime tree.

## Files

| File | Purpose |
|---|---|
| `openxr/openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h` | Core OpenXR API + Vulkan graphics binding (`XR_USE_GRAPHICS_API_VULKAN`) |
| `openxr/openxr_extension_helpers.h`, `openxr_reflection*.h`, `openxr_loader_negotiation.h` | Core support headers |
| `openxr/XR_DXR_cocoa_window_binding.h` | macOS NSView/CAMetalLayer window binding (`XrCocoaWindowBindingCreateInfoDXR`) |
| `openxr/XR_DXR_win32_window_binding.h` | Windows HWND window binding (`XrWin32WindowBindingCreateInfoDXR`) |
| `openxr/XR_DXR_xlib_window_binding.h` | Desktop-Linux X11 window binding (`XrXlibWindowBindingCreateInfoDXR`: Display* + Window XID) |
| `openxr/XR_DXR_android_surface_binding.h` | Android app-owned Surface binding (`XrAndroidSurfaceBindingCreateInfoDXR`, `xrSetAndroidSurfaceDXR`, `xrSetAndroidWindowGeometryDXR`) + the spec-v2 `XrEventDataAndroidWindowLayoutHintDXR` mini-window layout hint |
| `openxr/XR_DXR_display_info.h` | Display pixel dims / metadata used to size the swapchain |
| `openxr/XR_DXR_view_rig.h` | Declarative display rig — the app declares, the runtime returns render-ready views (no app-side Kooima) |
| `openxr/XR_DXR_workspace_file_dialog.h` | Tier-1 spatial file picker (`xrRequestFilePickerDXR`) for Open; native dialog fallback when unsupported |
| `openxr/XR_DXR_display_zones.h`, `XR_DXR_local_3d_zone.h` | Mixed 2D/3D region paradigm (ADR-027). Vendored for completeness; not used by this app yet |
| `openxr/XR_DXR_atlas_capture.h` | `xrCaptureAtlasDXR` composed-atlas capture (agent-side debugging) |
| `openxr/XR_DXR_weave.h` | Browser/inline-3D weave surface. Vendored for completeness; not used by this app |
| `openxr/XR_DXR_macos_gl_binding.h`, `XR_DXR_mcp_tools.h`, `XR_DXR_spatial_workspace.h` | Rest of the extension surface, vendored so the snapshot is a whole directory rather than a hand-picked subset. Not used by this app |

## Updating

Re-copy the files above from the runtime checkout, bump the pinned commit here, and
rebuild. Do not edit the headers in place.
