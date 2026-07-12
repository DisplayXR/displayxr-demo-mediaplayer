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
| `openxr/XR_DXR_display_info.h` | Display pixel dims / metadata used to size the swapchain |
| `openxr/XR_DXR_workspace_file_dialog.h` | Tier-1 spatial file picker (`xrRequestFilePickerDXR`) for Open; native dialog fallback when unsupported |

## Updating

Re-copy the files above from the runtime checkout, bump the pinned commit here, and
rebuild. Do not edit the headers in place.
