// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// SbsRenderer — side-by-side blit for the Android media-player port of
// displayxr-demo-mediaplayer. Adapts David's src/rhi/VulkanRenderer (the SBS
// split + sbs.frag/fullscreen.vert pipeline) to this repo's OpenXR-Android
// harness, which renders into TWO per-view swapchains (one image per eye)
// rather than a single SBS image. drawEye() blits one UV half of the source,
// full-viewport, into the eye's swapchain image; the runtime's Leia DP weaves.
//
//   - uploadTexture(): RGBA8 source (mode 0) — SBS still images via stb.
//   - uploadYUV():     planar Y + chroma (mode 1 I420 / mode 2 NV12) — the
//                      AMediaCodec video path; the GPU does the BT.709 convert.
//
// Planes are reused frame-to-frame (reallocated only on a size/format change)
// with a persistent host-visible staging buffer each, so per-frame video upload
// is a memcpy + one transfer submit — no per-frame Vulkan object churn.
#pragma once

#define VK_USE_PLATFORM_ANDROID_KHR  // vulkan_android.h: AHardwareBuffer import structs + PFNs
#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>

struct SbsRenderer {
	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
	          uint32_t queueFamily, VkFormat format);

	// RGBA8 still image (mode 0). Replaces the current source.
	bool uploadTexture(const uint8_t *rgba, uint32_t width, uint32_t height);

	// Planar YUV video frame (the decoder's native output). nv12: plane1 =
	// interleaved UV (w/2 x h/2, 2 bytes/texel), plane2 unused. !nv12 (I420):
	// plane1 = U, plane2 = V (each w/2 x h/2). Planes are tightly packed.
	bool uploadYUV(const uint8_t *y, const uint8_t *uv_or_u, const uint8_t *v,
	               uint32_t width, uint32_t height, bool nv12, bool fullRange);

	// Zero-copy video (Android): import the decoder's AHardwareBuffer and select
	// it as the active source (sourceMode_ = 3). The first call lazily builds the
	// ycbcr conversion + immutable sampler + pipeline from the stream's external
	// format. Imports are cached by AHardwareBuffer pointer (the AImageReader
	// cycles a bounded pool), so steady-state cost is one descriptor rewrite per
	// frame. Returns false on import failure (caller can fall back).
	bool setVideoAhb(struct AHardwareBuffer *ahb, uint32_t width, uint32_t height);

	bool hasSource() const {
		return planes_[0].view != VK_NULL_HANDLE || ahbActiveView_ != VK_NULL_HANDLE;
	}

	// Transport overlay (scrub bar + play/pause + load button + time), drawn
	// into each eye at zero disparity (screen plane). progress in [0,1]. left/
	// right are short time strings (e.g. "0:42"). Layout: transport_ui.h.
	void setOverlay(bool visible, float progress, bool paused, const char *left,
	                const char *right);

	// Render the active mode's `viewCount` views into TILES of the single atlas
	// image (size atlasW x atlasH) in ONE render pass, then blocks until the GPU
	// finishes. Each view v occupies tile (v%cols, v/cols) sized renderW x
	// renderH; its content is min-to-min fit (MatchMinRect) within the tile
	// (shorter side matched, longer axis cropped/letterboxed), scissor-clipped
	// to the tile, with the transport overlay drawn per tile. Stereo SBS slices
	// the source by column (left half → view 0, right half → view 1); `mono`
	// sends the whole image to every view. The caller submits N projection views
	// over this atlas with per-tile imageRects.
	void drawAtlas(VkImage image, uint32_t atlasW, uint32_t atlasH, uint32_t renderW,
	               uint32_t renderH, uint32_t cols, uint32_t rows, uint32_t viewCount,
	               float contentAspect, bool mono, const float clearRgb[3]);

	void cleanup();

private:
	struct Plane {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		uint32_t w = 0, h = 0;
		VkFormat fmt = VK_FORMAT_UNDEFINED;
		bool initialized = false;  // false right after (re)create → barrier from UNDEFINED
		VkBuffer staging = VK_NULL_HANDLE;
		VkDeviceMemory stagingMem = VK_NULL_HANDLE;
		VkDeviceSize stagingSize = 0;
		void *mapped = nullptr;
	};
	struct Target {  // cached per swapchain image
		VkImageView view = VK_NULL_HANDLE;
		VkFramebuffer fb = VK_NULL_HANDLE;
		uint32_t w = 0, h = 0;
	};

	const Target &targetFor(VkImage image, uint32_t w, uint32_t h);
	bool initOverlayPipeline();
	void drawOverlay(VkCommandBuffer cmd, uint32_t w, uint32_t h);
	uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
	bool ensurePlane(int idx, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bytesPerTexel);
	void recordPlaneCopy(VkCommandBuffer cmd, int idx, const uint8_t *src, uint32_t w,
	                     uint32_t h, uint32_t bytesPerTexel);
	bool ensureDummy();
	void bindDescriptors();  // point the 3 sampler slots at planes_/dummy
	void destroyPlane(Plane &p);

	VkPhysicalDevice phys_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t queueFamily_ = 0;
	VkFormat format_ = VK_FORMAT_UNDEFINED;

	VkRenderPass renderPass_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;
	VkDescriptorPool descPool_ = VK_NULL_HANDLE;
	VkDescriptorSet descSet_ = VK_NULL_HANDLE;
	VkCommandPool cmdPool_ = VK_NULL_HANDLE;
	VkCommandBuffer cmd_ = VK_NULL_HANDLE;
	VkFence fence_ = VK_NULL_HANDLE;

	Plane planes_[3];
	int sourceMode_ = 0;          // 0 RGBA, 1 I420, 2 NV12, 3 AHB-ycbcr (pushed to the shader)
	float sourceFullRange_ = 1.0f;

	// ── Zero-copy AHB video path (Android) ──────────────────────────────────
	// One ycbcr conversion + immutable sampler + pipeline per stream (built lazily
	// from the first frame's external format). Per-AHB imports (image+memory+view)
	// are cached so the AImageReader's pooled buffers each import once.
	struct AhbImport {
		struct AHardwareBuffer *ahb = nullptr;  // we hold an AHardwareBuffer_acquire ref
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
	};
	bool ensureAhbPipeline(const VkAndroidHardwareBufferFormatPropertiesANDROID &fmt);
	const AhbImport *importAhb(struct AHardwareBuffer *ahb, uint32_t w, uint32_t h);
	void destroyAhbImport(AhbImport &imp);

	VkSamplerYcbcrConversion ahbYcbcr_ = VK_NULL_HANDLE;
	VkSampler ahbSampler_ = VK_NULL_HANDLE;  // immutable, carries ahbYcbcr_
	VkDescriptorSetLayout ahbSetLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout ahbPipeLayout_ = VK_NULL_HANDLE;
	VkPipeline ahbPipeline_ = VK_NULL_HANDLE;
	VkDescriptorPool ahbDescPool_ = VK_NULL_HANDLE;
	VkDescriptorSet ahbDescSet_ = VK_NULL_HANDLE;
	uint64_t ahbExternalFormat_ = 0;  // VkExternalFormatANDROID.externalFormat for this stream
	bool ahbInited_ = false;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID pfnGetAhbProps_ = nullptr;

	static constexpr int kAhbCacheCap = 8;
	AhbImport ahbCache_[kAhbCacheCap];
	int ahbCacheCount_ = 0;
	// Active (current-frame) import, bound by drawAtlas when sourceMode_ == 3.
	VkImage ahbActiveImage_ = VK_NULL_HANDLE;
	VkImageView ahbActiveView_ = VK_NULL_HANDLE;
	uint32_t ahbActiveW_ = 0, ahbActiveH_ = 0;
	VkImage dummyImage_ = VK_NULL_HANDLE;
	VkDeviceMemory dummyMemory_ = VK_NULL_HANDLE;
	VkImageView dummyView_ = VK_NULL_HANDLE;

	std::unordered_map<VkImage, Target> targets_;

	// 2D triangle overlay pipeline (transport bar geometry). Vertices are
	// (vec2 NDC, vec4 RGBA); alpha-blended over the video. The vertex buffer is
	// host-visible and rebuilt each drawOverlay (serialized by drawEye's
	// per-eye fence wait, so the prior submit is done before we overwrite).
	VkPipelineLayout ovPipeLayout_ = VK_NULL_HANDLE;
	VkPipeline ovPipeline_ = VK_NULL_HANDLE;
	VkBuffer ovVbo_ = VK_NULL_HANDLE;
	VkDeviceMemory ovVboMem_ = VK_NULL_HANDLE;
	void *ovVboMapped_ = nullptr;
	uint32_t ovVboCapVerts_ = 0;

	// Transport overlay state (set per frame from main).
	bool ovVisible_ = false;
	float ovProgress_ = 0.0f;
	bool ovPaused_ = false;
	char ovLeft_[12] = {0};
	char ovRight_[12] = {0};
};
