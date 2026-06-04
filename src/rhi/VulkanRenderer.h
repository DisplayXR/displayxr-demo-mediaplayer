// SPDX-License-Identifier: BSL-1.0
//
// VulkanRenderer — the thin RHI seam (PRD §4 "RHI"). For M0 it does exactly one
// thing: clear the left and right halves of the SBS swapchain image to two
// distinct colors and present. No geometry, no textures, no depth. M1+ grows this
// into the SBS-split sampler + UI pass.
//
// The Vulkan instance/device/queue are owned by XrSession (the runtime selects
// them); this class only creates the per-image views + framebuffers, a color-only
// render pass, and a command buffer/fence.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "xr/XrSession.h"  // for XrSession::ViewRect

namespace mp {

struct ClearColor {
    float r, g, b, a;
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    // `images` are the VkImages backing the OpenXR SBS swapchain; `format`/`width`/
    // `height` describe it. Builds image views, framebuffers, render pass, command
    // pool/buffer and a fence.
    bool Initialize(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue graphicsQueue,
                    uint32_t queueFamilyIndex, VkFormat format, uint32_t width, uint32_t height,
                    const std::vector<VkImage>& images);
    void Shutdown();

    // Debug: read the given swapchain image back and write it to `path` as PNG.
    // Must be called after ClearViews and before the image is released. Proves
    // exactly what the app submitted to the runtime. Returns false on failure.
    bool DumpImage(uint32_t imageIndex, const char* path);

    // Record + submit a clear of the acquired swapchain image: each of the N views
    // gets its own color written into its `rects[v]` tile (the same rect submitted
    // to the runtime). Blocks until the GPU finishes (the runtime samples the image
    // right after we release it), mirroring the reference handle apps.
    bool ClearViews(uint32_t imageIndex, const ClearColor* colors,
                    const XrSession::ViewRect* rects, uint32_t viewCount);

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<VkImage> images_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    std::vector<VkImageView> imageViews_;
    std::vector<VkFramebuffer> framebuffers_;
};

} // namespace mp
