// SPDX-License-Identifier: Apache-2.0
#include "ui/ImGuiLayer.h"

#include "Log.h"

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include <cfloat>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#  include <climits>
#endif
#endif

namespace mp {

#if defined(MEDIAPLAYER_WITH_IMGUI)

namespace {
void CheckVk(VkResult r) {
    if (r != VK_SUCCESS) LOG_ERROR("ImGuiLayer: VkResult=%d", (int)r);
}

#if defined(_WIN32)
// Read the cursor straight from the window message stream (like the Gauss/model Win32
// demos): in workspace mode the runtime forwards a synthetic, content-space cursor via
// WM_MOUSEMOVE, which is correct by construction — unlike SDL's clamped copy or the real
// global cursor (GetCursorPos), which both miss because the app HWND is hidden. We subclass
// SDL's window proc, capture the lParam, then chain to the original.
WNDPROC g_originalWndProc = nullptr;
HWND    g_subclassedHwnd = nullptr;
LONG    g_wmMouseX = LONG_MIN;
LONG    g_wmMouseY = LONG_MIN;
LRESULT CALLBACK MouseCaptureWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
        g_wmMouseX = GET_X_LPARAM(lp);
        g_wmMouseY = GET_Y_LPARAM(lp);
    }
    return CallWindowProc(g_originalWndProc, hwnd, msg, wp, lp);
}
#endif

// Polished "dark glass" look: generous rounding, padded controls, faint translucent
// surfaces, one cyan accent. Centralizes all styling so the per-widget code stays clean.
void ApplyMediaPlayerStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 16.0f;
    s.FrameRounding = 10.0f;
    s.GrabRounding = 10.0f;
    s.PopupRounding = 10.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(18, 14);
    s.FramePadding = ImVec2(14, 9);
    s.ItemSpacing = ImVec2(12, 10);
    s.GrabMinSize = 18.0f;

    const ImVec4 accent(0.20f, 0.65f, 1.00f, 1.00f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.62f);
    c[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.16f);
    c[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    c[ImGuiCol_ButtonHovered] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.78f, 1.00f, 1.00f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
}
} // namespace

ImGuiLayer::~ImGuiLayer() { Shutdown(); }

bool ImGuiLayer::Init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
                      VkDevice device, VkQueue queue, uint32_t queueFamily, VkFormat hudFormat,
                      uint32_t hudWidth, uint32_t hudHeight,
                      const std::vector<VkImage>& hudImages) {
    if (hudImages.empty() || hudWidth == 0 || hudHeight == 0) return false;
    sdlWindow_ = window;
    device_ = device;
    queue_ = queue;
    hudWidth_ = hudWidth;
    hudHeight_ = hudHeight;

    // --- Render pass over the HUD image: clear to transparent, leave it in
    //     COLOR_ATTACHMENT_OPTIMAL (what the runtime expects at swapchain release).
    VkAttachmentDescription color{};
    color.format = hudFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    if (vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_) != VK_SUCCESS) {
        LOG_ERROR("ImGuiLayer: vkCreateRenderPass failed");
        return false;
    }

    // --- Per-image views + framebuffers.
    imageViews_.resize(hudImages.size());
    framebuffers_.resize(hudImages.size());
    for (size_t i = 0; i < hudImages.size(); ++i) {
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = hudImages[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = hudFormat;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &iv, nullptr, &imageViews_[i]) != VK_SUCCESS) {
            LOG_ERROR("ImGuiLayer: vkCreateImageView failed");
            return false;
        }
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = renderPass_;
        fb.attachmentCount = 1;
        fb.pAttachments = &imageViews_[i];
        fb.width = hudWidth_;
        fb.height = hudHeight_;
        fb.layers = 1;
        if (vkCreateFramebuffer(device_, &fb, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            LOG_ERROR("ImGuiLayer: vkCreateFramebuffer failed");
            return false;
        }
    }

    // --- Descriptor pool (font atlas + any user textures) and a command buffer/fence.
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 16;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &dp, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("ImGuiLayer: vkCreateDescriptorPool failed");
        return false;
    }

    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(device_, &cp, nullptr, &cmdPool_) != VK_SUCCESS) {
        LOG_ERROR("ImGuiLayer: vkCreateCommandPool failed");
        return false;
    }
    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool = cmdPool_;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &cba, &cmd_) != VK_SUCCESS) {
        LOG_ERROR("ImGuiLayer: vkAllocateCommandBuffers failed");
        return false;
    }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fci, nullptr, &fence_);

    // --- ImGui context + backends.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't litter an imgui.ini next to the binary
    io.FontGlobalScale = 1.9f;  // legible against the 1920x1080 HUD canvas (downscaled to window)
    ApplyMediaPlayerStyle();

    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        LOG_ERROR("ImGuiLayer: ImGui_ImplSDL3_InitForVulkan failed");
        ImGui::DestroyContext();
        return false;
    }
    ImGui_ImplVulkan_InitInfo vi{};
    vi.Instance = instance;
    vi.PhysicalDevice = physicalDevice;
    vi.Device = device_;
    vi.QueueFamily = queueFamily;
    vi.Queue = queue_;
    vi.DescriptorPool = descPool_;
    vi.RenderPass = renderPass_;
    vi.MinImageCount = 2;
    vi.ImageCount = (uint32_t)hudImages.size() < 2 ? 2 : (uint32_t)hudImages.size();
    vi.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    vi.CheckVkResultFn = CheckVk;
    if (!ImGui_ImplVulkan_Init(&vi)) {
        LOG_ERROR("ImGuiLayer: ImGui_ImplVulkan_Init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    ImGui_ImplVulkan_CreateFontsTexture();

#if defined(_WIN32)
    if (sdlWindow_ && !g_originalWndProc) {
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow_),
                                                 SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (hwnd) {
            g_subclassedHwnd = hwnd;
            g_originalWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC,
                                                          (LONG_PTR)MouseCaptureWndProc);
            LOG_INFO("ImGuiLayer: subclassed window proc for WM_MOUSEMOVE capture");
        }
    }
#endif

    ready_ = true;
    LOG_INFO("ImGuiLayer: ready (HUD %ux%u, %zu images)", hudWidth_, hudHeight_,
             hudImages.size());
    return true;
}

void ImGuiLayer::ProcessEvent(const void* sdlEvent) {
    if (!ready_) return;
    const SDL_Event* e = static_cast<const SDL_Event*>(sdlEvent);
    // Track the cursor from MOTION events only. Button-event coords are unreliable for the
    // hidden workspace HWND (they collapse to a window edge), so we keep the last motion pos.
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        lastMouseX_ = e->motion.x;
        lastMouseY_ = e->motion.y;
    }
    // Before a button event reaches ImGui, queue the corrected cursor position so the click
    // latches on the widget under the visible cursor (not the stale SDL position queued by the
    // backend during pumping). The subclass updated g_wmMouse from the same WM_LBUTTONDOWN.
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        float mx = lastMouseX_, my = lastMouseY_;
#if defined(_WIN32)
        if (g_wmMouseX != LONG_MIN) { mx = (float)g_wmMouseX; my = (float)g_wmMouseY; }
#endif
        if (remapRectW_ > 0.0f && remapRectH_ > 0.0f && remapDivW_ > 0.0f && remapDivH_ > 0.0f &&
            mx >= 0.0f) {
            const float u = ((mx / remapDivW_) - remapRectX_) / remapRectW_;
            const float v = ((my / remapDivH_) - remapRectY_) / remapRectH_;
            ImGui::GetIO().AddMousePosEvent(u * (float)hudWidth_, v * (float)hudHeight_);
        }
    }
    ImGui_ImplSDL3_ProcessEvent(e);
}

void ImGuiLayer::BeginFrame(float winPointW, float winPointH,
                            float rectX, float rectY, float rectW, float rectH) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    // We render into the HUD image, not the window — drive ImGui at HUD resolution.
    io.DisplaySize = ImVec2((float)hudWidth_, (float)hudHeight_);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // Remap the cursor (window points) into HUD pixels via the layer's placement rect, and
    // queue it as the LAST mouse-position event before NewFrame so it overrides the SDL
    // backend's window-space position. The divisor is the live Win32 client size: in
    // workspace mode the shell resizes the HWND externally and SDL's cached size can lag, so
    // GetClientRect gives the exact size the runtime scaled the cursor against. Off-Windows
    // (or if the HWND is unavailable) we fall back to the SDL point size.
    float remapW = winPointW, remapH = winPointH;
#if defined(_WIN32)
    if (sdlWindow_) {
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow_),
                                                 SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        RECT rc{};
        if (hwnd && GetClientRect(hwnd, &rc)) {
            const float cw = (float)(rc.right - rc.left);
            const float ch = (float)(rc.bottom - rc.top);
            if (cw > 0.0f && ch > 0.0f) { remapW = cw; remapH = ch; }
        }
    }
#endif
    if (rectW > 0.0f && rectH > 0.0f && remapW > 0.0f && remapH > 0.0f) {
        // Cursor source priority: the raw WM_MOUSEMOVE position the runtime forwards (correct
        // content-space coords, like the Gauss Win32 demo) > SDL motion events > the poll.
        float mx = lastMouseX_, my = lastMouseY_;
        if (mx < 0.0f && my < 0.0f) SDL_GetMouseState(&mx, &my);
#if defined(_WIN32)
        if (g_wmMouseX != LONG_MIN) { mx = (float)g_wmMouseX; my = (float)g_wmMouseY; }
#endif
        const float u = ((mx / remapW) - rectX) / rectW;
        const float v = ((my / remapH) - rectY) / rectH;
        io.AddMousePosEvent(u * (float)hudWidth_, v * (float)hudHeight_);

        // Cache for ProcessEvent so a click latches at this same corrected position.
        remapRectX_ = rectX; remapRectY_ = rectY; remapRectW_ = rectW; remapRectH_ = rectH;
        remapDivW_ = remapW; remapDivH_ = remapH;
    }
    ImGui::NewFrame();
}

void ImGuiLayer::RenderToHud(uint32_t imageIndex) {
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    if (imageIndex >= framebuffers_.size()) return;

    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fence_);
    vkResetCommandBuffer(cmd_, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // transparent: only widgets show
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = renderPass_;
    rp.framebuffer = framebuffers_[imageIndex];
    rp.renderArea.extent = {hudWidth_, hudHeight_};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(drawData, cmd_);
    vkCmdEndRenderPass(cmd_);
    vkEndCommandBuffer(cmd_);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    CheckVk(vkQueueSubmit(queue_, 1, &si, fence_));
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
}

bool ImGuiLayer::WantCaptureMouse() const {
    return ready_ && ImGui::GetIO().WantCaptureMouse;
}

void ImGuiLayer::Shutdown() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
#if defined(_WIN32)
    if (g_originalWndProc && g_subclassedHwnd) {
        SetWindowLongPtr(g_subclassedHwnd, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
        g_originalWndProc = nullptr;
        g_subclassedHwnd = nullptr;
    }
#endif
    if (ready_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        ready_ = false;
    }
    for (VkFramebuffer fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    for (VkImageView iv : imageViews_) if (iv) vkDestroyImageView(device_, iv, nullptr);
    framebuffers_.clear();
    imageViews_.clear();
    if (fence_) { vkDestroyFence(device_, fence_, nullptr); fence_ = VK_NULL_HANDLE; }
    if (cmdPool_) { vkDestroyCommandPool(device_, cmdPool_, nullptr); cmdPool_ = VK_NULL_HANDLE; }
    if (descPool_) { vkDestroyDescriptorPool(device_, descPool_, nullptr); descPool_ = VK_NULL_HANDLE; }
    if (renderPass_) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    device_ = VK_NULL_HANDLE;
}

#else  // !MEDIAPLAYER_WITH_IMGUI

ImGuiLayer::~ImGuiLayer() {}
bool ImGuiLayer::Init(SDL_Window*, VkInstance, VkPhysicalDevice, VkDevice, VkQueue, uint32_t,
                      VkFormat, uint32_t, uint32_t, const std::vector<VkImage>&) {
    return false;
}
void ImGuiLayer::ProcessEvent(const void*) {}
void ImGuiLayer::BeginFrame(float, float, float, float, float, float) {}
void ImGuiLayer::RenderToHud(uint32_t) {}
bool ImGuiLayer::WantCaptureMouse() const { return false; }
void ImGuiLayer::Shutdown() {}

#endif

} // namespace mp
