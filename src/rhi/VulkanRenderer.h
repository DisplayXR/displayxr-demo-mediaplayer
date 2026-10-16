// SPDX-License-Identifier: Apache-2.0
//
// VulkanRenderer — the thin RHI seam (PRD §4 "RHI"). For M0 it does exactly one
// thing: clear the left and right halves of the SBS swapchain image to two
// distinct colors and present. No geometry, no textures, no depth. M1+ grows this
// into the SBS-split sampler + UI pass.
//
// The Vulkan instance/device/queue are owned by XrSession (the runtime selects
// them); this class only creates the per-image views + framebuffers, a color-only
// render pass, and the frame ring.
//
// Frame submission (runtime INV-4.10, displayxr-common dxr::VkFrameRing): N frames in
// flight (default 3; DXR_VK_FRAMES_IN_FLIGHT overrides, 1 = the old submit->wait) and
// ONE vkQueueSubmit per frame. Uploads, the atlas pass and the HUD pass all RECORD into
// the open frame's command buffer (FrameCmd()); App calls SubmitFrame() once, before it
// releases the HUD / atlas swapchain images. Nothing on the frame path waits the frame
// it just submitted. Descriptor sets are per ring slot (written each frame), and
// anything a queued frame may still read is released through the ring's deferDestroy().
#pragma once

#include <vulkan/vulkan.h>

#include <vk_frame_ring.h>  // displayxr-common: dxr::VkFrameRing (INV-4.10)

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
    // UV offset from this view's sample to the same scene point in the OTHER eye. Only
    // read by the Pro QC views (difference / anaglyph); 0 everywhere else.
    float otherDX = 0.0f, otherDY = 0.0f;
};

// Media Player Pro grade, applied in the source shader to every DrawViews draw:
// exposure (stops, linear light) -> ASC CDL -> 3D LUT. The default is the identity,
// which is all the base player ever uses. See shaders/sbs.frag Grade().
struct GradeParams {
    float exposure = 0.0f;
    bool cdlOn = false;
    float slope[3] = {1.f, 1.f, 1.f};
    float offset[3] = {0.f, 0.f, 0.f};
    float power[3] = {1.f, 1.f, 1.f};
    float saturation = 1.0f;
    bool lutOn = false;          // ignored unless a LUT was uploaded (HasLut3D)
    int qcMode = 0;              // 0 normal, 1 eye difference, 2 anaglyph
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    // `images` are the VkImages backing the OpenXR SBS swapchain; `format`/`width`/
    // `height` describe it. Builds image views, framebuffers, render pass, pipeline and
    // the frame ring (command buffers + fences allocated once).
    bool Initialize(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue graphicsQueue,
                    uint32_t queueFamilyIndex, VkFormat format, uint32_t width, uint32_t height,
                    const std::vector<VkImage>& images);
    void Shutdown();

    // The open frame's command buffer, opening one (waiting only the fence of the frame
    // that used its ring slot N frames ago) if none is open. Every per-frame recording
    // below goes through it; ImGuiLayer::RenderToHud records into it too.
    VkCommandBuffer FrameCmd() { return ring_.beginFrame(); }
    // Submit everything recorded this frame: EXACTLY one vkQueueSubmit, no wait. Call
    // BEFORE releasing the HUD / atlas swapchain images (the runtime composes on the same
    // queue, so our work must be queued ahead of it). No-op when no frame is open.
    void SubmitFrame();
    uint32_t FramesInFlight() const { return ring_.framesInFlight(); }

    // Record a copy of an RGBA8 buffer into an external (e.g. HUD swapchain) VkImage into
    // the frame command buffer, leaving it in COLOR_ATTACHMENT_OPTIMAL (the layout the
    // runtime expects at release). Submitted by SubmitFrame().
    //
    // This is a vkCmdCopyBufferToImage — a raw byte store with no format conversion in
    // either direction — so it is the matched transfer for ENCODED bytes into an _SRGB
    // image (#78): the CPU raster hands over display-referred RGBA and the _SRGB format
    // is simply the honest declaration of what those bytes are. A blit would convert.
    bool UploadToSwapchainImage(VkImage image, const uint8_t* rgba, uint32_t width,
                                uint32_t height);

    // Debug: read the given swapchain image back and write it to `path` as PNG.
    // Must be called after ClearViews/DrawViews and before the image is released. Proves
    // exactly what the app submitted to the runtime. Returns false on failure. Debug-only
    // and deliberately blocking: it submits the open frame early and drains the ring.
    bool DumpImage(uint32_t imageIndex, const char* path);

    // Debug: read back an arbitrary RGBA8 image (e.g. the HUD swapchain image, left
    // in COLOR_ATTACHMENT_OPTIMAL) and write it to `path` as PNG. Proves what was
    // rendered into an off-screen target. Returns false on failure.
    bool DumpExternalImage(VkImage image, uint32_t width, uint32_t height, const char* path);

    // Record a clear of the acquired swapchain image into the frame command buffer: each
    // of the N views gets its own color written into its `rects[v]` tile (the same rect
    // submitted to the runtime). Submitted by SubmitFrame() before the release.
    bool ClearViews(uint32_t imageIndex, const ClearColor* colors,
                    const XrSession::ViewRect* rects, uint32_t viewCount);

    // Upload an RGBA8 image as the source for DrawViews (images / test patterns). Records
    // into the frame command buffer (opening one if needed); the staging buffer and any
    // replaced plane image are released once the GPU is past them.
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
    // `uvs[v]`. Clears the rest of the image to black. Records into the frame command
    // buffer; SubmitFrame() submits it.
    // When true, DrawViews clears the letterbox region to alpha 0 (instead of opaque
    // black) so the runtime composes it through to the desktop (MEDIAPLAYER_TRANSPARENT).
    void SetTransparentLetterbox(bool t) { transparentLetterbox_ = t; }

    // Letterbox/background color for DrawViews (the area outside the fitted content).
    // Defaults to black; the idle logo screen sets it to dark grey so the mark sits on
    // a calm backdrop with no black bars. Ignored in transparent-letterbox mode (alpha 0).
    void SetBackground(float r, float g, float b) { background_ = {r, g, b, 1.0f}; }

    // Pro grade + QC mode for subsequent DrawViews calls (identity by default).
    void SetGrade(const GradeParams& g) { grade_ = g; }
    const GradeParams& Grade() const { return grade_; }

    // Upload a 3D LUT (N^3 RGB floats, red fastest - the .cube order) as an RGBA16F 3D
    // texture. `domainMin/Max` are the LUT's input domain (scalar: the same for R, G, B).
    // A one-off blocking upload (user action, not per frame). Returns false on failure.
    bool UploadLut3D(const float* rgb, uint32_t size, float domainMin, float domainMax);
    void ClearLut3D();
    bool HasLut3D() const { return lut_.view != VK_NULL_HANDLE; }

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
    // format_ is an _SRGB format, i.e. the attachment encodes on store (#78). Drives the
    // shader's display-referred -> scene-linear step and ToTargetClear().
    bool srgbTarget_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<VkImage> images_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    dxr::VkFrameRing ring_;
    bool submitErrLogged_ = false;

    std::vector<VkImageView> imageViews_;
    std::vector<VkFramebuffer> framebuffers_;

    // Textured-blit pipeline (M1: SBS image).
    VkDescriptorSetLayout descSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    // One set PER RING SLOT, written by DrawViews for the slot it records into: updating
    // a set that a queued command buffer still references is undefined behaviour.
    static constexpr uint32_t kMaxSlots = dxr::detail::kMaxFramesInFlight;
    VkDescriptorSet descSets_[kMaxSlots] = {};
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
    // The open frame samples a shared import. The D3D11 producer has no GPU-side sync
    // with us (it CPU-waits its own copy, then publishes) and may refill a texture as
    // soon as we move on, so SubmitFrame() keeps that path frame-synchronous (drains
    // the ring) exactly as before — the frame ring buys nothing there that is safe.
    bool sharedSampledThisFrame_ = false;
    SharedImport* ImportSharedRGBA(void* handle, uint32_t w, uint32_t h);
#endif
    // Pro grade: the 3D LUT at binding 3, or a 1x1x1 dummy when none is loaded.
    GradeParams grade_{};
    struct Lut3D {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t size = 0;
        float domainMin = 0.f, domainMax = 1.f;
    } lut_, lutDummy_;
    bool CreateLutImage(Lut3D& dst, uint32_t n, const uint16_t* rgba16f);
    void DestroyLut(Lut3D& l, bool deferred);

    VkImage dummyImage_ = VK_NULL_HANDLE;       // 1x1, bound to unused plane slots
    VkDeviceMemory dummyMemory_ = VK_NULL_HANDLE;
    VkImageView dummyView_ = VK_NULL_HANDLE;
    bool transparentLetterbox_ = false; // clear letterbox to alpha 0 (transparent-bg mode)
    ClearColor background_{0.0f, 0.0f, 0.0f, 1.0f}; // DrawViews letterbox/background fill

    bool CreatePipeline();
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    // Display-referred ClearColor -> the value to hand the attachment (decoded to
    // scene-linear when the attachment encodes on store, verbatim otherwise).
    VkClearColorValue ToTargetClear(const ClearColor& c) const;
    // Record plane (re)creation + a staged copy into `cmd` (the open frame). The staging
    // buffer and a replaced plane's image/view/memory go to the ring's deferDestroy (a
    // queued frame may still read them). `recreated` = the image view changed.
    bool RecordPlaneUpload(VkCommandBuffer cmd, int idx, const uint8_t* data, uint32_t w,
                           uint32_t h, VkFormat fmt, uint32_t bytesPerTexel, bool& recreated);
    bool EnsureDummy();
    void BindDescriptors(VkDescriptorSet set);  // point the 3 sampler bindings at planes_/dummy
    void DestroyTexture();
};

} // namespace mp
