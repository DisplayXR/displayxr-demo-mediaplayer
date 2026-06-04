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

// Per-view UV sub-region of the source texture to sample into that view's tile.
// Left eye of an SBS frame = {0,0, 0.5,1}; right eye = {0.5,0, 0.5,1}; mono = {0,0,1,1}.
struct ViewUV {
    float offX, offY, scaleX, scaleY;
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

    // Copy an RGBA8 buffer into an external (e.g. HUD swapchain) VkImage, leaving it
    // in COLOR_ATTACHMENT_OPTIMAL (the layout the runtime expects at release).
    bool UploadToSwapchainImage(VkImage image, const uint8_t* rgba, uint32_t width,
                                uint32_t height);

    // Debug: read the given swapchain image back and write it to `path` as PNG.
    // Must be called after ClearViews and before the image is released. Proves
    // exactly what the app submitted to the runtime. Returns false on failure.
    bool DumpImage(uint32_t imageIndex, const char* path);

    // Debug: read back an arbitrary RGBA8 image (e.g. the HUD swapchain image, left
    // in COLOR_ATTACHMENT_OPTIMAL) and write it to `path` as PNG. Proves what was
    // rendered into an off-screen target. Returns false on failure.
    bool DumpExternalImage(VkImage image, uint32_t width, uint32_t height, const char* path);

    // Record + submit a clear of the acquired swapchain image: each of the N views
    // gets its own color written into its `rects[v]` tile (the same rect submitted
    // to the runtime). Blocks until the GPU finishes (the runtime samples the image
    // right after we release it), mirroring the reference handle apps.
    bool ClearViews(uint32_t imageIndex, const ClearColor* colors,
                    const XrSession::ViewRect* rects, uint32_t viewCount);

    // Upload an RGBA8 image as the source texture for DrawViews. Replaces any prior
    // texture. Safe to call once at load time.
    bool UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height);
    bool HasTexture() const { return textureView_ != VK_NULL_HANDLE; }

    // Draw the uploaded texture into each view's tile, sampling the UV sub-region in
    // `uvs[v]`. Clears the rest of the image to black. Blocks until the GPU finishes.
    bool DrawViews(uint32_t imageIndex, const XrSession::ViewRect* rects,
                   const ViewUV* uvs, uint32_t viewCount);

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

    // Textured-blit pipeline (M1: SBS image).
    VkDescriptorSetLayout descSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSet_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    // Source texture (the whole SBS frame; views sample sub-regions). Reused across
    // video frames — reallocated only when the dimensions change.
    VkImage textureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory_ = VK_NULL_HANDLE;
    VkImageView textureView_ = VK_NULL_HANDLE;
    uint32_t texW_ = 0;
    uint32_t texH_ = 0;
    bool textureInitialized_ = false;  // false right after (re)create -> first barrier from UNDEFINED

    bool CreatePipeline();
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    void DestroyTexture();
};

} // namespace mp
