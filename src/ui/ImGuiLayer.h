// SPDX-License-Identifier: Apache-2.0
//
// ImGuiLayer (M4) — renders a Dear ImGui transport bar into the window-space HUD
// swapchain. The runtime composites that HUD image into both eyes with a small
// disparity, so the 2D UI gains "subtle 3D" parallax for free (PRD §9) — no per-eye
// double-draw here. Owns the ImGui-specific Vulkan objects (render pass, per-image
// framebuffers, descriptor pool, a command buffer + fence); the device/queue/images
// are borrowed from XrSession.
//
// Built without ImGui (MEDIAPLAYER_WITH_IMGUI undefined), Init() returns false and
// the caller falls back to the CPU-rasterized text HUD.
//
// PLATFORM SPLIT. The render side is platform-neutral — it targets whatever VkImages
// the caller hands it. Only the *input/timing* side differs:
//   MEDIAPLAYER_IMGUI_SDL defined  -> desktop: imgui_impl_sdl3 pumps input and dt
//                                     (plus a Win32 WM_MOUSEMOVE subclass, see the .cpp)
//   not defined                    -> Android: no platform backend at all. The caller
//                                     feeds pointer events via PushPointer() and dt is
//                                     measured internally.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace mp {

class ImGuiLayer {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Bring up ImGui + the Vulkan backend targeting the HUD swapchain images.
    // `nativeWindow` is an `SDL_Window*` on desktop and nullptr on platforms without
    // a platform backend (Android) — it is only used to reach the Win32 HWND.
    bool Init(void* nativeWindow, VkInstance instance, VkPhysicalDevice physicalDevice,
              VkDevice device, VkQueue queue, uint32_t queueFamily, VkFormat hudFormat,
              uint32_t hudWidth, uint32_t hudHeight, const std::vector<VkImage>& hudImages);
    void Shutdown();
    bool Ready() const { return ready_; }

    // Forward one SDL event (argument is an `SDL_Event*`) to the ImGui backend.
    // No-op where there is no platform backend.
    void ProcessEvent(const void* sdlEvent);

    // Platforms with no ImGui platform backend (Android) queue pointer events here.
    // Coordinates are in HUD pixels. THREAD-SAFE: this is called from the Android UI
    // thread while ImGui itself is driven from the render thread, so it only enqueues —
    // it must never touch ImGui state. BeginFrame() drains the queue.
    //   action: 0 = down, 1 = up, 2 = move
    void PushPointer(int action, float hudX, float hudY);

    // Re-target the layer at a new HUD image set (e.g. an orientation change resized
    // the tile). Keeps the ImGui context, style and font atlas; rebuilds only the
    // render pass, image views and framebuffers. The caller must have idled the device.
    bool RecreateTarget(VkFormat hudFormat, uint32_t hudWidth, uint32_t hudHeight,
                        const std::vector<VkImage>& hudImages);

    // Start an ImGui frame. The mouse is remapped from window points into HUD pixels
    // using the HUD placement rect (window fractions) so clicks land on the widgets.
    // After this returns, the caller issues ImGui widget calls, then RenderToHud().
    void BeginFrame(float winPointW, float winPointH,
                    float rectX, float rectY, float rectW, float rectH);

    // Render the built UI into hudImages[imageIndex]; blocks until the GPU finishes
    // (the runtime samples the HUD image right after the caller releases it).
    void RenderToHud(uint32_t imageIndex);

    // True while the pointer is over an ImGui widget (so the app can ignore the click).
    bool WantCaptureMouse() const;

private:
    // Build the render pass / views / framebuffers over `hudImages`. Shared by Init()
    // and RecreateTarget(); tears down any previous target first.
    bool CreateTargetObjects(VkFormat hudFormat, uint32_t hudWidth, uint32_t hudHeight,
                             const std::vector<VkImage>& hudImages);
    void DestroyTargetObjects();

    bool ready_ = false;
    void* nativeWindow_ = nullptr;  // borrowed SDL_Window*; used to fetch the live HWND (Win32)
    // Cursor in window points from SDL motion events — a fallback source. On Windows the
    // primary source is the raw WM_MOUSEMOVE position captured by the window-proc subclass
    // (see ImGuiLayer.cpp): for the hidden workspace HWND, both the SDL poll and SDL's own
    // motion events clamp/collapse to a window edge, while the forwarded WM_MOUSEMOVE lParam
    // is the correct content-space position. -1 until the first event arrives.
    float lastMouseX_ = -1.0f;
    float lastMouseY_ = -1.0f;
    // Remap context cached each BeginFrame so ProcessEvent can place the corrected cursor in
    // io.MousePos *before* a button event reaches ImGui — otherwise a click latches at the
    // stale SDL position (the press lands off the widget; it takes a second click once hover
    // has caught up). rectW_ <= 0 means "not yet valid".
    float remapRectX_ = 0.0f, remapRectY_ = 0.0f, remapRectW_ = 0.0f, remapRectH_ = 0.0f;
    float remapDivW_ = 0.0f, remapDivH_ = 0.0f;

    // Pointer queue for platforms with no ImGui platform backend. Drained in BeginFrame.
    struct PointerEvent { int action; float x, y; };
    std::mutex pointerMutex_;
    std::vector<PointerEvent> pointerQueue_;
    // Touch has no hover: after a release the last-pressed widget would stay hovered and
    // WantCaptureMouse() would stay true forever, swallowing every later tap on the
    // content. So the frame after a release we park the cursor off-screen.
    bool parkPointerNextFrame_ = false;
    double lastFrameSeconds_ = -1.0;  // internal dt source when there is no platform backend

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t hudWidth_ = 0;
    uint32_t hudHeight_ = 0;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<VkImageView> imageViews_;
    std::vector<VkFramebuffer> framebuffers_;
};

} // namespace mp
