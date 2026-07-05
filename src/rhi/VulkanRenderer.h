// SPDX-License-Identifier: Apache-2.0
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
#include <utility>
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

    // Upload an RGBA8 image as the source for DrawViews (images / test patterns).
    bool UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height);

    // Upload a planar YUV frame (the video decoder's native output): the shader does the
    // BT.709 convert AND the downscale via sampling, so no swscale runs on the decode
    // thread. format: 0 = I420 (plane1=U, plane2=V), 1 = NV12 (plane1=interleaved UV,
    // plane2 ignored). Chroma planes are (w+1)/2 x (h+1)/2.
    bool UploadYUV(const uint8_t* plane0, const uint8_t* plane1, const uint8_t* plane2,
                   uint32_t width, uint32_t height, int format, bool fullRange);

#if defined(_WIN32)
    // Zero-copy (#28): bind a shared D3D11 NV12 texture (by its shared NT handle) as the
    // video source instead of uploading CPU planes. The texture is imported into Vulkan
    // (cached per handle) and its two planes bound as Y (R8) + UV (R8G8); DrawViews syncs
    // the D3D11 producer copy against the sample with a keyed mutex. Returns false (caller
    // falls back to UploadYUV) on any import failure.
    bool BindSharedRGBA(void* sharedHandle, uint32_t width, uint32_t height);
    // Release all imported shared textures (call on media change / shutdown).
    void ClearSharedImports();
#endif

    bool HasTexture() const {
#if defined(_WIN32)
        if (sharedActive_) return true;
#endif
        return planes_[0].view != VK_NULL_HANDLE;
    }

    // Draw the uploaded texture into each view's tile, sampling the UV sub-region in
    // `uvs[v]`. Clears the rest of the image to black. Blocks until the GPU finishes.
    // When true, DrawViews clears the letterbox region to alpha 0 (instead of opaque
    // black) so the runtime composes it through to the desktop (MEDIAPLAYER_TRANSPARENT).
    void SetTransparentLetterbox(bool t) { transparentLetterbox_ = t; }

    // Letterbox/background color for DrawViews (the area outside the fitted content).
    // Defaults to black; the idle logo screen sets it to dark grey so the mark sits on
    // a calm backdrop with no black bars. Ignored in transparent-letterbox mode (alpha 0).
    void SetBackground(float r, float g, float b) { background_ = {r, g, b, 1.0f}; }

    // `rects` are the per-view content rects (the viewport — may be convergence-
    // shifted); `clipRects` are the per-view tile bounds the drawing is scissored to,
    // so a shifted content rect can never spill into the neighbouring eye's tile.
    bool DrawViews(uint32_t imageIndex, const XrSession::ViewRect* rects,
                   const XrSession::ViewRect* clipRects,
                   const ViewUV* uvs, uint32_t viewCount);

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;  // graphics family; needed for the #28 external->graphics acquire
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

    // Up to three source planes. RGBA images use plane 0 only (mode 0); video uses Y +
    // chroma (mode 1 = I420, mode 2 = NV12). Each is reused across frames, reallocated
    // only when its size/format changes. Planes a mode doesn't sample bind a 1x1 dummy.
    struct Plane {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        bool initialized = false;  // false right after (re)create -> barrier from UNDEFINED
    } planes_[3];
    int sourceMode_ = 0;             // 0 RGBA, 1 I420, 2 NV12 (pushed to the shader)
    float sourceFullRange_ = 0.0f;

#if defined(_WIN32)
    // Zero-copy interop state (#28). imports_ caches one entry per shared handle. The producer
    // converts the decoded NV12 to a packed RGBA texture on the D3D side, so we import a plain
    // single-plane RGBA image (offset 0, no multiplanar plane-alignment mismatch) and sample it
    // through the main pipeline as mode 0. The bound entry's memory drives the per-frame
    // EXTERNAL->graphics ownership acquire in DrawViews.
    struct SharedImport {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;       // single R8G8B8A8 color view
        bool layoutReady = false;                // acquired UNDEFINED->GENERAL once
    };
    std::vector<std::pair<void*, SharedImport>> imports_;  // keyed by shared handle
    bool sharedActive_ = false;                // current frame is a shared RGBA texture
    VkDeviceMemory sharedMemory_ = VK_NULL_HANDLE;  // bound import's memory
    VkImage sharedImage_ = VK_NULL_HANDLE;
    VkImageView sharedView_ = VK_NULL_HANDLE;  // bound import's RGBA view
    bool* sharedLayoutReady_ = nullptr;        // -> bound import's layoutReady
    SharedImport* ImportSharedRGBA(void* handle, uint32_t w, uint32_t h);
#endif
    VkImage dummyImage_ = VK_NULL_HANDLE;       // 1x1, bound to unused plane slots
    VkDeviceMemory dummyMemory_ = VK_NULL_HANDLE;
    VkImageView dummyView_ = VK_NULL_HANDLE;
    bool transparentLetterbox_ = false; // clear letterbox to alpha 0 (transparent-bg mode)
    ClearColor background_{0.0f, 0.0f, 0.0f, 1.0f}; // DrawViews letterbox/background fill

    bool CreatePipeline();
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    // Record plane (re)creation + a staged copy into `cmd` (already recording). The
    // staging buffer/memory is appended to `staging` for the caller to free post-submit.
    // `recreated` is set when the image view changed (so the descriptor needs rebinding).
    bool RecordPlaneUpload(VkCommandBuffer cmd, int idx, const uint8_t* data, uint32_t w,
                           uint32_t h, VkFormat fmt, uint32_t bytesPerTexel, bool& recreated,
                           std::vector<std::pair<VkBuffer, VkDeviceMemory>>& staging);
    bool EnsureDummy();
    void BindDescriptors();          // point the 3 sampler bindings at planes_/dummy
    void DestroyTexture();
};

} // namespace mp
