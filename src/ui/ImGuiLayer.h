// SPDX-License-Identifier: BSL-1.0
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
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

struct SDL_Window;

namespace mp {

class ImGuiLayer {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Bring up ImGui + the SDL3/Vulkan backends targeting the HUD swapchain images.
    bool Init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
              VkDevice device, VkQueue queue, uint32_t queueFamily, VkFormat hudFormat,
              uint32_t hudWidth, uint32_t hudHeight, const std::vector<VkImage>& hudImages);
    void Shutdown();
    bool Ready() const { return ready_; }

    // Forward one SDL event (argument is an `SDL_Event*`) to the ImGui backend.
    void ProcessEvent(const void* sdlEvent);

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
    bool ready_ = false;
    SDL_Window* sdlWindow_ = nullptr;  // borrowed; used to fetch the live HWND (Win32)
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
