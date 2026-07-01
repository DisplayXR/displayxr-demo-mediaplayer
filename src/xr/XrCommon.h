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
