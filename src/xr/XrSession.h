// SPDX-License-Identifier: Apache-2.0
//
// XrSession — OpenXR lifecycle for a DisplayXR `_handle` Vulkan client.
//
// Owns the OpenXR instance/system/session, the Vulkan instance+device the runtime
// asked us to create, the LOCAL reference space, and a single side-by-side (SBS)
// stereo swapchain (both eyes packed into one image; eye 0 = left half, eye 1 =
// right half). The app never weaves — it submits a plain stereo projection layer
// and the runtime/display-processor does the weaving.
//
// Re-typed (not linked) from displayxr-runtime/test_apps (cube_handle_vk_*),
// trimmed to what an M0 clear-both-eyes skeleton needs.
#pragma once

#include "XrCommon.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mp {

class XrSession {
public:
    // PRIMARY_STEREO is 2 on a classic HMD, but a DisplayXR light-field display
    // reports N views (e.g. a 4-view panel packed as a 2x2 atlas). We size for the
    // runtime's actual count, up to this cap (matches the reference handle apps).
    static constexpr uint32_t kMaxViews = 8;

    // A view's tile within the swapchain image (clear target == submitted subImage).
    struct ViewRect {
        int32_t x = 0, y = 0;
        uint32_t w = 0, h = 0;
    };

    // Optional window-space HUD overlay submitted alongside the projection layer.
    // Placement is in window fractions [0..1]; disparity shifts it per-eye (negative
    // = toward the viewer). Render into the HUD swapchain image before EndFrame.
    struct HudSubmit {
        bool enabled = false;
        float x = 0.015f;
        float y = 0.02f;
        float width = 0.30f;
        float height = 0.05f;
        float disparity = 0.0f;  // 0 = zero-disparity plane (screen depth)
        int32_t srcW = 0;  // sub-rect of the HUD swapchain to show (0 => full)
        int32_t srcH = 0;
    };

    // Per-frame state handed between BeginFrame/EndFrame.
    struct Frame {
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        bool shouldRender = false;
        uint32_t imageIndex = 0;       // acquired swapchain image
        uint32_t viewCount = 0;        // == XrSession::ActiveViewCount()
        XrView views[kMaxViews];       // per-view pose + fov from xrLocateViews
    };

    XrSession() = default;
    ~XrSession();

    XrSession(const XrSession&) = delete;
    XrSession& operator=(const XrSession&) = delete;

    // Full bring-up: instance + system + stereo view config + Vulkan device (per
    // runtime requirements) + session (with window binding) + LOCAL space + SBS
    // swapchain. `nativeWindowHandle` is an NSView* (macOS) or HWND (Windows).
    //
    // `placeWindow` (optional) fires after the system-properties query and BEFORE
    // xrCreateSession, with the 3D panel's top-left in virtual-desktop pixels from
    // XrDisplayDesktopPositionDXR (display_info v16). The session binding captures
    // the native handle, so the window position must be settled by then for the
    // display processor's phase tracking to start right. (0, 0) = primary monitor
    // or unknown (old runtimes ignore the chained struct, leaving the zero-init).
    bool Initialize(void* nativeWindowHandle,
                    const std::function<void(int32_t left, int32_t top)>& placeWindow = {});
    void Shutdown();

    // Drain the OpenXR event queue, driving the session state machine
    // (READY→xrBeginSession, STOPPING→xrEndSession, EXITING→exit).
    void PollEvents();

    bool IsRunning() const { return sessionRunning_; }
    bool ExitRequested() const { return exitRequested_; }

    // Frame loop. BeginFrame does xrWaitFrame/xrBeginFrame and, when shouldRender,
    // locates the two eye views and acquires the swapchain image. The caller then
    // renders into `imageIndex` and calls EndFrame to submit the projection layer.
    bool BeginFrame(Frame& frame);
    bool EndFrame(Frame& frame, const ViewRect* rects, const HudSubmit* hud = nullptr);

    // True when the session was created with transparent-background compositing
    // (MEDIAPLAYER_TRANSPARENT=1) — the renderer should clear letterbox to alpha 0.
    bool TransparentBackground() const { return transparentBg_; }

    // Window-space HUD swapchain (created in Initialize if the runtime supports it).
    bool HasHud() const { return hasHud_; }
    uint32_t HudWidth() const { return hudSwapchain_.width; }
    uint32_t HudHeight() const { return hudSwapchain_.height; }
    int64_t HudFormat() const { return hudSwapchain_.format; }
    const std::vector<VkImage>& HudImages() const { return hudVkImages_; }
    bool AcquireHudImage(uint32_t& imageIndex);
    bool ReleaseHudImage();

    // --- Open-file (XR_DXR_workspace_file_dialog) ---
    // Outcome of a RequestFilePicker call. Pending → a completion event will arrive
    // (poll TakePickedFile); the others mean the caller should use a native dialog.
    enum class PickerStatus { Pending, FallbackTier0, Unsupported, Error };
    bool HasFilePicker() const { return pfnRequestFilePicker_ != nullptr; }
    // Ask the workspace for a spatial open-file picker (image/video filters). Async:
    // on Pending, the result arrives via TakePickedFile() once the user picks.
    PickerStatus RequestFilePicker();
    // True once when a file-picker completion landed since the last call; fills `path`
    // (empty on cancel). Drains the latch.
    bool TakePickedFile(std::string& path);

    // --- Atlas capture (XR_DXR_atlas_capture, the 'I' key) ---
    // True if the runtime exposes the capture entry point.
    bool HasAtlasCapture() const { return pfnCaptureAtlasEXT_ != nullptr; }
    // Ask the runtime to snapshot the composed multi-view atlas to "<pathPrefix>_atlas.png".
    // Non-blocking: the readback runs at the next EndFrame, so the PNG lands shortly after.
    bool CaptureAtlas(const std::string& pathPrefix);

    // --- Agent tools (XR_DXR_mcp_tools) ---
    // The app exposes its playback controls as MCP tools on the per-process server the
    // runtime hosts. Registration is never load-bearing: when the MCP capability gate is
    // off (or the runtime predates the extension) HasMcpTools() is false and every call
    // below is an inert no-op, so the player runs identically with no agent surface.
    //
    // A tool invocation arrives as XrEventDataMCPToolCallDXR through PollEvents() — i.e.
    // on the main loop — and is dispatched to the handler installed via SetMcpToolHandler.
    // The handler runs synchronously there (player state is consistent, no locking), and
    // its returned JSON is submitted back to the agent. Every call is answered; an
    // unanswered call fails to the agent after ~5 s.

    // Dispatch handler: given a registered tool's bare name and its JSON arguments,
    // perform the action and return the JSON result value. Set `success=false` (it
    // defaults true) to make the agent receive a tool error with that JSON.
    using McpToolHandler = std::function<std::string(const std::string& toolName,
                                                     const std::string& argsJson,
                                                     bool& success)>;
    // True once the extension is present AND its entry points resolved — registration
    // and dispatch are live. False ⇒ all MCP calls below are inert.
    bool HasMcpTools() const { return hasMcpToolsExt_ && pfnRegisterMcpTool_ != nullptr; }
    // Declare the app's stable id (must equal the manifest `id`; linter INV-10.1). Call
    // once before registering tools. No-op (returns false) when HasMcpTools() is false.
    bool SetMcpAppId(const std::string& appId);
    // Register one tool. `name` is the bare tool name; `description` is agent-facing API
    // documentation; `schemaJson` is a JSON Schema object for the arguments (empty ⇒ no
    // args). Returns false on failure or when MCP is unavailable.
    bool RegisterMcpTool(const std::string& name, const std::string& description,
                         const std::string& schemaJson);
    // Install the handler invoked when an agent calls a registered tool.
    void SetMcpToolHandler(McpToolHandler handler) { mcpToolHandler_ = std::move(handler); }

    // Vulkan handles the runtime selected for us — handed to the renderer.
    VkInstance VkInstanceHandle() const { return vkInstance_; }
    VkPhysicalDevice PhysicalDevice() const { return physicalDevice_; }
    VkDevice Device() const { return device_; }
    VkQueue GraphicsQueue() const { return graphicsQueue_; }
    uint32_t GraphicsQueueFamily() const { return queueFamilyIndex_; }

    // Zero-copy D3D11VA->Vulkan decode interop (issue #28). True when the device exposes
    // VK_KHR_external_memory_win32; deviceLUID (when valid) pins the D3D11 decode device to
    // the same adapter. Both are false/invalid off-Windows or on drivers lacking interop.
    bool ZeroCopyCapable() const { return zeroCopyCapable_; }
    bool DeviceLUIDValid() const { return deviceLUIDValid_; }
    const uint8_t* DeviceLUID() const { return deviceLUID_; }  // VK_LUID_SIZE bytes

    // Swapchain + tile layout for the renderer. The active rendering mode's N views
    // pack into a tileColumns x tileRows grid of equally-sized tiles within one
    // swapchain image. The *active* counts come from the current rendering mode and
    // change at runtime (e.g. mono passthrough <-> 4-view 3D); the located array is
    // always sized to the max-over-all-modes count (see BeginFrame).
    int64_t SwapchainFormat() const { return swapchain_.format; }
    uint32_t SwapchainWidth() const { return swapchain_.width; }
    uint32_t SwapchainHeight() const { return swapchain_.height; }
    uint32_t MaxViewCount() const { return viewCount_; }
    uint32_t ActiveViewCount() const;
    uint32_t ActiveTileColumns() const;
    uint32_t ActiveTileRows() const;
    float ActiveViewScaleX() const;
    float ActiveViewScaleY() const;
    const char* ActiveModeName() const;

    // Compute each active view's tile rect from the canvas (window) pixel size:
    // per-view extent = canvas_px * viewScale, clamped to the max tile
    // (swapchain / tileGrid), tiled row-major. The same rects are used to clear
    // and to submit, so the runtime samples exactly what we wrote. Returns the
    // active view count; fills rects[0..count).
    uint32_t ComputeViewRects(uint32_t canvasPxWidth, uint32_t canvasPxHeight,
                              ViewRect* rects) const;
    // Cycle to the next requestable rendering mode (the 'V' key). No-op when the
    // rendering-mode extension is unavailable or the mode is workspace-locked.
    void RequestNextMode();
    // Request a specific mode by index (used by MEDIAPLAYER_START_MODE for testing).
    void RequestMode(uint32_t modeIndex);
    // VkImage handles backing the swapchain (length == imageCount).
    const std::vector<VkImage>& SwapchainImages() const { return swapchainVkImages_; }

private:
    bool InitInstanceAndSystem();
    bool CreateVulkanDevice();
    bool CreateSessionWithWindowBinding(void* nativeWindowHandle);
    void EnumerateRenderingModes();
    bool CreateLocalSpace();
    bool CreateSwapchain();
    void CreateHudSwapchain(uint32_t width, uint32_t height);

    struct SwapchainInfo {
        XrSwapchain handle = XR_NULL_HANDLE;
        int64_t format = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t imageCount = 0;
    };

    // One vendor-defined rendering mode (e.g. mono passthrough, 4-view 3D).
    struct RenderingMode {
        uint32_t modeIndex = 0;
        uint32_t viewCount = 1;
        uint32_t tileColumns = 1;
        uint32_t tileRows = 1;
        float viewScaleX = 1.0f;   // per-view atlas size = canvas_px * viewScale
        float viewScaleY = 1.0f;
        bool hardware3D = false;
        bool requestable = false;
        char name[XR_MAX_SYSTEM_NAME_SIZE] = {};
    };
    const RenderingMode* CurrentMode() const;

    // OpenXR
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    ::XrSession session_ = XR_NULL_HANDLE;  // OpenXR handle (global scope: our class shadows the name)
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfigType_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews_;
    uint32_t viewCount_ = 0;       // max over all modes (xrLocateViews capacity)
    uint32_t tileColumns_ = 1;     // fallback tiling when rendering-mode ext absent
    uint32_t tileRows_ = 1;
    SwapchainInfo swapchain_;
    std::vector<VkImage> swapchainVkImages_;

    // Window-space HUD swapchain (small; optional).
    SwapchainInfo hudSwapchain_;
    std::vector<VkImage> hudVkImages_;
    bool hasHud_ = false;

    // Rendering modes (XR_DXR_display_info v8+). Empty when the runtime doesn't
    // expose them, in which case we fall back to the max view count + derived tiling.
    std::vector<RenderingMode> modes_;
    uint32_t currentModeIndex_ = 0;  // modeIndex value of the active mode
    PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumModes_ = nullptr;
    PFN_xrRequestDisplayRenderingModeDXR pfnRequestMode_ = nullptr;

    // Open-file picker (XR_DXR_workspace_file_dialog). PFN is null when the runtime
    // doesn't expose the extension (→ caller uses a native dialog).
    bool hasFilePickerExt_ = false;
    PFN_xrRequestFilePickerDXR pfnRequestFilePicker_ = nullptr;
    XrAsyncRequestIdDXR pendingPickerId_ = XR_NULL_ASYNC_REQUEST_ID_DXR;
    bool hasPickedFile_ = false;
    std::string pickedFile_;

    // Atlas capture (XR_DXR_atlas_capture). PFN null when the runtime lacks it.
    bool hasAtlasCaptureExt_ = false;
    PFN_xrCaptureAtlasDXR pfnCaptureAtlasEXT_ = nullptr;

    // Agent tools (XR_DXR_mcp_tools). All PFNs null when the runtime lacks the extension
    // or the MCP capability gate is off — the whole feature is then inert.
    void HandleMcpToolCall(const XrEventDataMCPToolCallDXR* call);
    bool hasMcpToolsExt_ = false;
    PFN_xrSetMCPAppInfoDXR pfnSetMcpAppInfo_ = nullptr;
    PFN_xrRegisterMCPToolDXR pfnRegisterMcpTool_ = nullptr;
    PFN_xrGetMCPToolCallArgsDXR pfnGetMcpToolCallArgs_ = nullptr;
    PFN_xrSubmitMCPToolResultDXR pfnSubmitMcpToolResult_ = nullptr;
    McpToolHandler mcpToolHandler_;

    // Capabilities discovered at instance creation.
    bool hasWindowBindingExt_ = false;
    bool hasDisplayInfoExt_ = false;
    bool transparentBg_ = false;   // MEDIAPLAYER_TRANSPARENT — letterbox composes through
    uint32_t displayPixelWidth_ = 0;
    uint32_t displayPixelHeight_ = 0;
    // 3D panel top-left in virtual-desktop pixels (XrDisplayDesktopPositionDXR,
    // display_info v16). (0, 0) = primary/unknown — the safe default.
    int32_t displayDesktopLeft_ = 0;
    int32_t displayDesktopTop_ = 0;

    // Session state machine.
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool exitRequested_ = false;

    // Vulkan (created against the runtime's requirements).
    VkInstance vkInstance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;

    // Zero-copy decode interop (issue #28); Windows-only, off elsewhere.
    bool zeroCopyCapable_ = false;
    bool deviceLUIDValid_ = false;
    uint8_t deviceLUID_[VK_LUID_SIZE] = {};
};

} // namespace mp
