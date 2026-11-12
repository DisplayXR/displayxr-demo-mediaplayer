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
//
// Staging: the per-frame CPU uploads (the source planes, the CPU text HUD) copy into a
// PERSISTENT, persistently mapped host-visible buffer, one per plane per ring slot, reused
// every N frames and reallocated only when the size changes (AcquireStaging). The slot's
// fence, which beginFrame() waits before a frame records into that slot, is what makes the
// reuse safe: the frame that last copied from the buffer has finished on the GPU. The copy
// itself runs in row bands on a small worker pool (rhi/BandCopy.h) when the plane is big
// enough to pay for it. (A LUT upload is a one-off and keeps its own blocking staging.)
//
// Off the render thread (rhi/AsyncUpload.h, MEDIAPLAYER_ASYNC_UPLOAD, on by default): a video /
// live frame from a FrameRing is copied into staging by the upload thread instead (ahead of
// the take for a queue-mode ring, else on demand from the take), into STAGING SLOTS of its
// own; the render thread records only the buffer-to-image copy (StageFrame / CommitUpload).
// A slot is reused only after the frame that read it has passed its fence (Retire, from
// OpenFrame), the same guarantee as the per-ring-slot staging above.
#pragma once

#include <vulkan/vulkan.h>

#include <vk_frame_ring.h>  // displayxr-common: dxr::VkFrameRing (INV-4.10)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "media/FrameRing.h"
#include "rhi/AsyncUpload.h"
#include "rhi/BandCopy.h"
#include "rhi/PushPack.h"
#include "rhi/RenderPerf.h"
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
    // VR180 / VR360 viewer (#155): when vrMode != 0 the offset/scale above are the eye's
    // TANGENT window, not a source rect, and the shader samples through the sphere. Built
    // by vr::PackModeFlags / vr::PackHeading (src/media/VrView.h). All 0 = a flat draw.
    uint32_t vrMode = 0;     // OR-ed into the push block's `mode` (bits 8+)
    float vrPitch = 0.0f;    // radians
    uint32_t vrHeading = 0;  // OR-ed into viewFlags (bits 16-31)
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
    int qcMode = 0;              // 0 normal, 1 eye difference, 2 anaglyph, 3 false colour
    float zebraLevel = 0.0f;     // > 0: zebras at or above this graded luma (0..1)
    int channel = 0;             // 0 RGB, 1 R, 2 G, 3 B, 4 alpha, 5 luma (shown as grey)
    int wipeMode = 0;            // 0 off, 1 before | after the grade, 2 left eye | right eye
    float wipePos = 0.5f;        // the wipe line across the picture, 0..1
    float gamma = 1.0f;          // display gamma trim, applied after the grade and QC view
    // Per-panel display calibration (#133), the last transform: after the grade, QC view,
    // gamma trim and channel view. Ignored unless a calibration LUT was uploaded (HasCalibrationLut). Not part of
    // the show grade: bypassing the grade keeps it.
    bool calOn = false;
    // HDR sources (#134): 0 auto (roll off HLG / PQ highlights, clip scene-linear EXR at
    // white), 1 always roll off, 2 never (clip), 3 raw (the signal goes into the grade as
    // if it were SDR, for a show LUT that expects PQ / HLG). peakNits 0 = the stream's own.
    // For HLG, peakNits is the display peak the OOTF renders for (default 1000).
    int toneMap = 0;
    float peakNits = 0.0f;
    bool lutPqShaper = false;    // the LUT is an OCIO bake expecting PQ-coded linear (EXR)
    // An untagged YUV source (#169): 0 as tagged (SDR), 1 take it as BT.2100 HLG, 2 as PQ
    // (BT.2020 matrix and primaries). A tagged HDR source and an RGBA one are unaffected.
    int forceTransfer = 0;
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
    VkCommandBuffer FrameCmd() { return ring_.frameOpen() ? ring_.current() : OpenFrame(); }
    // Submit everything recorded this frame: EXACTLY one vkQueueSubmit, no wait. Call
    // BEFORE releasing the HUD / atlas swapchain images (the runtime composes on the same
    // queue, so our work must be queued ahead of it). No-op when no frame is open.
    void SubmitFrame();
    uint32_t FramesInFlight() const { return ring_.framesInFlight(); }

    // The render thread's timings, always on (rhi/RenderPerf.h): the last closed window
    // (~10 s; serial 0 until one has closed). App reports the time it spent blocked in
    // xrWaitFrame each frame, which separates the display's pacing from our own work.
    const RenderPerfWindow& LastPerfWindow() const { return perf_.Last(); }
    void NoteXrWait(double ms) { perf_.XrWait(ms); }
    // The render loop's stages (App): a stage's time in this loop iteration (several calls
    // add up), then the iteration's end with its wall time, loop top to loop top.
    void NoteStage(RenderStage s, double ms) { perf_.Stage(s, ms); }
    void EndStageLoop(double loopMs) { perf_.EndLoop(loopMs); }

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
    // into the frame command buffer (opening one if needed); the pixels go through the
    // plane's persistent staging (see the file comment) and a replaced plane image is
    // released once the GPU is past it.
    bool UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height);

    // Upload a planar YUV frame (the video decoder's native output): the shader does the
    // Y'CbCr convert AND the downscale via sampling, so no swscale runs on the decode
    // thread. matrix = FrameRing ColorMatrix (0 = BT.709, the default; 1 = BT.601;
    // 2 = BT.2020 NCL), honoured by the MP_GRADE shader both desktop players compile. format: 0 = I420 (plane1=U, plane2=V), 1 = NV12 (plane1=interleaved UV,
    // plane2 ignored), 2 = P016 (NV12 layout, 16-bit MSB-aligned samples - Pro's
    // 10-bit capture). Chroma planes are (w+1)/2 x (h+1)/2.
    bool UploadYUV(const uint8_t* plane0, const uint8_t* plane1, const uint8_t* plane2,
                   uint32_t width, uint32_t height, int format, bool fullRange, int matrix = 0);
    int SourceMatrix() const { return sourceMatrix_; }  // last YUV upload's matrix (UI)

    // Off-thread upload (rhi/AsyncUpload.h; MEDIAPLAYER_ASYNC_UPLOAD, on by default). Every
    // render-loop iteration: SetUploadSource(the ring the source frames come from, or nullptr),
    // then for a frame just taken from it StageFrame(ring, frame) in place of UploadYUV
    // (false: not staged, call UploadYUV as before), then CommitUpload() before the draw.
    // The copy into staging runs on the upload thread meanwhile; CommitUpload records the
    // buffer-to-image copy into the frame command buffer, waiting only for a copy that is
    // still running. Until CommitUpload the frame may still be read: do not acquire from that
    // ring again before it. A synchronous source upload (UploadTexture / UploadYUV /
    // BindSharedRGBA) in between replaces the staged frame, as it would have replaced the
    // uploaded one. All no-ops when the switch is off.
    bool AsyncUploadOn() const { return uploader_ != nullptr; }
    void SetUploadSource(FrameRing* ring);
    bool StageFrame(const FrameRing* ring, const FrameRing::Frame& f);
    void CommitUpload();
    // The HDR tags of the source just uploaded (#134): transfer 0 SDR, 1 HLG, 2 PQ, 3 scene-
    // linear stored as PQ; BT.2020 primaries; the stream's peak in nits (0 = unknown).
    void SetSourceHdr(int transfer, bool bt2020, float peakNits);
    // What the shader does with them under the current grade (UI / status / notes).
    struct HdrState {
        int transfer = 0;      // the source's (0 SDR, 1 HLG, 2 PQ, 3 scene-linear as PQ)
        bool raw = false;      // tone map "raw": the shader treats it as SDR
        bool rollOff = false;
        bool bt2020 = false;
        float peakNits = 0.0f;
        bool pqShaper = false;  // scene-linear into an OCIO look, unclipped (see GradeParams)
        // Scene-linear EXR under tone map auto (#169): values above white go into the CDL
        // and LUT unclipped (clipped after the grade), so highlights survive a grade.
        bool overWhite = false;
        bool forced = false;    // the transfer came from GradeParams::forceTransfer
    };
    HdrState SourceHdr() const;
    // The same decision for any tags under the current grade (the compare clip B, the
    // "Convert to 3D" input frame): transfer 0 SDR, 1 HLG, 2 PQ, 3 scene-linear as PQ.
    // An untagged (SDR) transfer becomes GradeParams::forceTransfer when one is set.
    HdrState HdrFor(int transfer, bool bt2020, float peakNits) const;
    // The Y'CbCr matrix to decode with: BT.2020 (2) when HdrFor forces an untagged source.
    int EffectiveMatrix(int transfer, int matrix) const { return ForcesHdr(transfer) ? 2 : matrix; }
    bool ForcesHdr(int transfer) const {
        return transfer == 0 && (grade_.forceTransfer == 1 || grade_.forceTransfer == 2);
    }

    // Media Player Pro's clip-vs-clip wipe: a second source (B) at bindings 4..6, sampled by
    // the MP_GRADE shader right of the wipe line (GradeParams::wipeMode 3). Same formats and
    // upload path as UploadTexture / UploadYUV; ClearCompare drops it. The base player
    // never calls these.
    bool UploadCompareTexture(const uint8_t* rgba, uint32_t width, uint32_t height);
    bool UploadCompareYUV(const uint8_t* plane0, const uint8_t* plane1, const uint8_t* plane2, uint32_t width,
                          uint32_t height, int format, bool fullRange, int matrix = 0);
    void ClearCompare();
    // B's HDR tags (#169), as SetSourceHdr for A; an RGBA B upload resets them to SDR.
    void SetCompareHdr(int transfer, bool bt2020, float peakNits);
    bool HasCompare() const { return compareMode_ >= 0; }

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
    // The same with the texels already packed (color::PackRgba16f), e.g. on a loader thread.
    bool UploadLut3DPacked(const uint16_t* rgba16f, uint32_t size, float domainMin, float domainMax);
    void ClearLut3D();
    bool HasLut3D() const { return lut_.view != VK_NULL_HANDLE; }
    // The per-panel display calibration LUT (#133) at binding 7, domain 0..1.
    bool UploadCalibrationLut(const uint16_t* rgba16f, uint32_t size);
    void ClearCalibrationLut();
    bool HasCalibrationLut() const { return cal_.view != VK_NULL_HANDLE; }

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
    // Bumped by every submitted frame: a staging buffer marked with the current value was
    // already copied from in the open frame (see AcquireStaging).
    uint64_t frameSerial_ = 1;
    VkCommandBuffer OpenFrame();  // FrameCmd's slow path: beginFrame(), timed

    // Persistent staging (see the file comment). One buffer per user per ring slot; users
    // 0..5 are planes_[0..5], 6 is the CPU text-HUD fallback (UploadToSwapchainImage).
    static constexpr int kStagingUsers = 7;
    static constexpr int kHudStaging = 6;
    struct Staging {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;      // mapped once at creation, unmapped at release
        VkDeviceSize size = 0;       // capacity
        uint64_t usedSerial = 0;     // frameSerial_ of the last frame that copied from it
    };
    // The open frame's staging for `user`, at least `bytes`: mapped memory to write and the
    // buffer to copy from. Null when there is no open frame, the buffer was already used in
    // this frame, or it cannot be made: the caller then uses a one-off buffer.
    void* AcquireStaging(int user, VkDeviceSize bytes, VkBuffer& buffer);
    // A one-off staging buffer, released by deferDestroy once this frame has executed (the
    // only path before persistent staging). Null on failure.
    void* OneOffStaging(VkDeviceSize bytes, VkBuffer& buffer);
    bool CreateStaging(Staging& s, VkDeviceSize bytes);  // any thread (the upload thread too)
    std::atomic<bool> stagingTypeLogged_{false};  // the staging memory type, logged at the first creation
    void ReleaseStaging(Staging& s, bool deferred);
    void TrimIdleStaging();  // releases buffers no frame copied from in the last perf window
    // Copy a plane (rows x rowBytes, packed) into mapped staging, in row bands on copyPool_
    // when it is big enough. Returns the threads it used.
    int CopyToStaging(void* dst, const uint8_t* src, size_t rowBytes, size_t rows);
    Staging staging_[kStagingUsers][dxr::detail::kMaxFramesInFlight];
    uint64_t trimSerial_ = 0;        // frameSerial_ when the last perf window closed
    int copyThreads_ = 1;            // bandcopy::UploadThreads(), read at Initialize
    std::unique_ptr<bandcopy::Pool> copyPool_;  // made at the first copy that splits

    // Off-thread upload (see the file comment). asyncSlots_[i] is slot i's staging: written by
    // the upload thread while it fills the slot (EnsureAsyncSlot reallocates it then), read by
    // the GPU after CommitUpload recorded it, released by TrimIdle / Shutdown while it is free.
    std::unique_ptr<asyncup::Uploader> uploader_;  // null: MEDIAPLAYER_ASYNC_UPLOAD=0
    Staging asyncSlots_[asyncup::kMaxSlots];
    std::atomic<uint64_t> asyncAllocs_{0};         // slot (re)allocations, for perf_
    FrameRing* uploadSource_ = nullptr;
    struct PendingUpload {  // staged at the take, recorded at CommitUpload
        bool active = false;
        bool cancelled = false;  // a synchronous source upload replaced it meanwhile
        asyncup::Ticket ticket;
        const FrameRing::Frame* frame = nullptr;  // the caller still holds it (the fallback copies it)
        bool fullRange = false;
        int matrix = 0;
    } pending_;
    void* EnsureAsyncSlot(int slot, size_t bytes);  // the upload thread
    void CancelPendingUpload() {
        if (pending_.active) pending_.cancelled = true;
    }
    RenderPerfMeter perf_;
    struct UploadTally {             // one Upload* call: its copies, for perf_
        double copyMs = 0.0;
        double bytes = 0.0;
        int threads = 1;
    };
    std::chrono::steady_clock::time_point perfEpoch_ = std::chrono::steady_clock::now();
    double PerfNow() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - perfEpoch_).count();
    }

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
    } planes_[6];                    // 0..2 the source; 3..5 the Pro compare source (B)
    int sourceMode_ = 0;             // 0 RGBA, 1 I420, 2 NV12 (pushed to the shader)
    int compareMode_ = -1;           // B: -1 none, else as sourceMode_
    bool compareFullRange_ = false, compare16_ = false;
    int compareMatrix_ = 0;
    int compareTransfer_ = 0;        // B's HDR tags (#169), see SetCompareHdr
    bool compareBt2020_ = false;
    float comparePeak_ = 0.0f;
    float sourceFullRange_ = 0.0f;
    bool source16_ = false;          // P016: the MP_GRADE shader uses 10-bit limited levels
    int sourceMatrix_ = 0;           // YUV matrix pushed to the MP_GRADE shader (0 = BT.709)
    int sourceTransfer_ = 0;         // HDR tags (#134), see SetSourceHdr
    bool sourceBt2020_ = false;
    float sourcePeak_ = 0.0f;

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
    } lut_, cal_, lutDummy_;  // cal_: the display calibration LUT at binding 7
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
    // Record plane (re)creation + a staged copy into `cmd` (the open frame). The pixels go
    // through the plane's persistent staging for this ring slot (a one-off buffer only when
    // the plane was already uploaded this frame); a replaced plane's image/view/memory goes
    // to the ring's deferDestroy (a queued frame may still read it). `recreated` = the
    // image view changed. The copy's time and size are added to `tally`.
    bool RecordPlaneUpload(VkCommandBuffer cmd, int idx, const uint8_t* data, uint32_t w,
                           uint32_t h, VkFormat fmt, uint32_t bytesPerTexel, bool& recreated,
                           UploadTally& tally);
    // Its two halves: (re)create plane `idx` for w x h of fmt (the old image to deferDestroy),
    // and record the copy into it from `buffer` at `offset` with its layout barriers.
    bool EnsurePlane(int idx, uint32_t w, uint32_t h, VkFormat fmt, bool& recreated);
    void RecordPlaneCopy(VkCommandBuffer cmd, int idx, VkBuffer buffer, VkDeviceSize offset);
    void NoteUpload(std::chrono::steady_clock::time_point t0, const UploadTally& tally);
    bool EnsureDummy();
    void BindDescriptors(VkDescriptorSet set);  // point the 3 sampler bindings at planes_/dummy
    void DestroyTexture();
};

} // namespace mp
