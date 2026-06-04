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
(`f1347340e refactor(test-apps): macOS cube_handle capture → xrCaptureAtlasEXT + mip parity [#396 W6]`)

> When `DisplayXR/displayxr-extensions` is published as a standalone repo, re-pin
> from there (FetchContent or a vendored snapshot) instead of the runtime tree.

## Files

| File | Purpose |
|---|---|
| `openxr/openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h` | Core OpenXR API + Vulkan graphics binding (`XR_USE_GRAPHICS_API_VULKAN`) |
| `openxr/openxr_extension_helpers.h`, `openxr_reflection*.h`, `openxr_loader_negotiation.h` | Core support headers |
| `openxr/XR_EXT_cocoa_window_binding.h` | macOS NSView/CAMetalLayer window binding (`XrCocoaWindowBindingCreateInfoEXT`) |
| `openxr/XR_EXT_win32_window_binding.h` | Windows HWND window binding (`XrWin32WindowBindingCreateInfoEXT`) |
| `openxr/XR_EXT_display_info.h` | Display pixel dims / metadata used to size the swapchain |

## Updating

Re-copy the files above from the runtime checkout, bump the pinned commit here, and
rebuild. Do not edit the headers in place.
