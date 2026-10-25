// SPDX-License-Identifier: Apache-2.0
// Central include + ordering for OpenXR (Vulkan binding) and the DisplayXR
// window-binding extension. Vulkan must be included before openxr_platform.h so
// the XrGraphicsBindingVulkanKHR struct is defined.
#pragma once

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <openxr/XR_DXR_display_info.h>
#include <dxr_view_config.h>                       // displayxr-common: DxrSelectViewConfigType (#1486) + DxrAliasInactiveViews (ADR-041)
#include <openxr/XR_DXR_workspace_file_dialog.h>  // Tier-1 spatial file picker (Open)
#include <openxr/XR_DXR_atlas_capture.h>           // 'I' key — snapshot the composed atlas
#include <openxr/XR_DXR_mcp_tools.h>               // app-defined agent tools (per-app MCP)
#include <openxr/XR_DXR_cursor_depth.h>            // depth-aware cursor (spec v7: measured disparity)
#include <openxr/XR_DXR_input_providers.h>         // which input provider holds this app's roles
#include <openxr/XR_DXR_lift.h>                    // "Convert to 3D": 2D->3D conversion (spec v4)
#include <openxr/XR_DXR_local_3d_zone.h>           // Pro: panels as flat 2D (Local2D layers)
#if defined(__APPLE__)
#  include <openxr/XR_DXR_cocoa_window_binding.h>
#elif defined(_WIN32)
#  include <openxr/XR_DXR_win32_window_binding.h>
#else
#  include <openxr/XR_DXR_xlib_window_binding.h>
#  include <openxr/XR_DXR_wayland_surface_binding.h>
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
