// SPDX-License-Identifier: Apache-2.0
// Central include + ordering for OpenXR (Vulkan binding) and the DisplayXR
// window-binding extension. Vulkan must be included before openxr_platform.h so
// the XrGraphicsBindingVulkanKHR struct is defined.
#pragma once

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <openxr/XR_EXT_display_info.h>
#include <openxr/XR_EXT_workspace_file_dialog.h>  // Tier-1 spatial file picker (Open)
#include <openxr/XR_EXT_atlas_capture.h>           // 'I' key — snapshot the composed atlas
#include <openxr/XR_EXT_mcp_tools.h>               // app-defined agent tools (per-app MCP)
#if defined(__APPLE__)
#  include <openxr/XR_EXT_cocoa_window_binding.h>
#elif defined(_WIN32)
#  include <openxr/XR_EXT_win32_window_binding.h>
#else
// Linux: no window-binding extension exists yet (build-green port, #30) — the
// runtime never advertises one, so hasWindowBindingExt_/hasHud_ stay false and
// no window-space layer is ever submitted. But EndFrame still needs the type
// declared. The struct is platform-neutral and wire-shared by BOTH binding
// headers (each carries this exact #ifndef-guarded block); mirror it here
// verbatim until a Linux binding header lands in displayxr-extensions.
#ifndef XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT ((XrStructureType)1000999002)

typedef struct XrCompositionLayerWindowSpaceEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT
    const void* XR_MAY_ALIAS    next;       //!< Pointer to next structure in chain
    XrCompositionLayerFlags     layerFlags; //!< e.g. XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    XrSwapchainSubImage         subImage;   //!< Source swapchain + rect
    float                       x;          //!< Left edge, fraction of window width  [0..1]
    float                       y;          //!< Top edge, fraction of window height   [0..1]
    float                       width;      //!< Fraction of window width  [0..1]
    float                       height;     //!< Fraction of window height [0..1]
    float                       disparity;  //!< Horizontal shift, fraction of window width.
                                            //!< 0 = screen depth, negative = toward viewer
} XrCompositionLayerWindowSpaceEXT;
#endif
#endif

#include "Log.h"

// Run an OpenXR call; on failure, log and `return false` from the caller.
#define XR_CHECK(call)                                            \
    do {                                                          \
        XrResult _r = (call);                                     \
        if (XR_FAILED(_r)) {                                      \
            LOG_ERROR("%s failed: XrResult=%d", #call, (int)_r);  \
            return false;                                         \
        }                                                         \
    } while (0)
