// Copyright 2026, The DisplayXR Project and its contributors
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
#include "video_decoder.h"  // kVideoReaderMaxImages — the AHB cache is derived from it

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

	// ── Dual-source zero-copy video ("LVF v2": one FULL view per container track) ──
	// Imports BOTH eyes' AHardwareBuffers and binds the left one for view 0 and the
	// right one for view 1 (views >= 2 follow the same left/right column rule the SBS
	// path uses, so a 2x2 quad mode still gets a sensible eye per tile). Each view
	// samples its own image WHOLE -- there is no half to slice -- plus the per-eye
	// convergence shift from setConvergence().
	//
	// Both tracks are the same codec at the same resolution, so both buffers carry the
	// same vendor external format and share the ONE ycbcr conversion / pipeline built
	// for this stream; a mismatch is asserted (logged + refused) rather than sampled
	// through the wrong conversion.
	//
	// Returns false if either import failed. On a RIGHT-eye failure the left import
	// stays bound in single-source mode, so the picture degrades to flat-left rather
	// than to black. Calling setVideoAhb() again leaves dual mode.
	bool setVideoAhbStereo(struct AHardwareBuffer *ahbL, uint32_t wL, uint32_t hL,
	                       struct AHardwareBuffer *ahbR, uint32_t wR, uint32_t hR);

	// Convergence for the dual path, in FRACTION OF VIEW WIDTH, half applied to each
	// eye. POSITIVE = NEARER (content toward the viewer), negative = further behind
	// the glass. Ignored by every other source mode. Derivation in drawAtlas().
	void setConvergence(float c) { convergence_ = c; }
	float convergence() const { return convergence_; }
	bool stereoDual() const { return ahbStereo_; }

	// Drop every zero-copy resource belonging to the CURRENT stream: the cached
	// per-AHB imports (and the AHardwareBuffer refs that keep the old reader's
	// pool alive) and the ycbcr pipeline built from that stream's external
	// format. Call whenever the VideoDecoder is stopped so a new one can be
	// opened. Without this the old reader's imports stay cached alongside the
	// new reader's, the cache overflows, and importAhb() evicts -- destroys --
	// a live import under the descriptor set every frame: the picture tears
	// between eyes. Blocks until the GPU is idle; render-thread only.
	void resetVideoAhb();

	bool hasSource() const {
		return planes_[0].view != VK_NULL_HANDLE || ahbActiveView_ != VK_NULL_HANDLE;
	}

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
	void destroyAhbPipeline();  // ycbcr conversion + sampler + layouts + pipeline + pool
	const AhbImport *importAhb(struct AHardwareBuffer *ahb, uint32_t w, uint32_t h);
	void destroyAhbImport(AhbImport &imp);

	VkSamplerYcbcrConversion ahbYcbcr_ = VK_NULL_HANDLE;
	VkSampler ahbSampler_ = VK_NULL_HANDLE;  // immutable, carries ahbYcbcr_
	VkDescriptorSetLayout ahbSetLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout ahbPipeLayout_ = VK_NULL_HANDLE;
	VkPipeline ahbPipeline_ = VK_NULL_HANDLE;
	VkDescriptorPool ahbDescPool_ = VK_NULL_HANDLE;
	VkDescriptorSet ahbDescSet_ = VK_NULL_HANDLE;   // single source, or the LEFT eye
	VkDescriptorSet ahbDescSetR_ = VK_NULL_HANDLE;  // RIGHT eye (dual-source only)
	uint64_t ahbExternalFormat_ = 0;  // VkExternalFormatANDROID.externalFormat for this stream
	bool ahbInited_ = false;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID pfnGetAhbProps_ = nullptr;

	// MUST exceed the number of DISTINCT buffers one stream can cycle through.
	// That is NOT the AImageReader maxImages: the BufferQueue behind it also
	// allocates the codec's own dequeued buffers, and a single 10-image reader
	// was measured handing out more than 12 distinct AHardwareBuffers. Size for
	// the BufferQueue's hard ceiling (NUM_BUFFER_SLOTS = 64) -- an entry is a
	// few handles, and resetVideoAhb() drops them all when the stream ends, so
	// the cost is nil and eviction becomes genuinely unreachable.
	//
	// A DUAL-TRACK stream doubles the demand: TWO decoders, each with its own
	// AImageReader, both feeding this one cache. The measured worst case for a single
	// 10-image reader was "more than 12" distinct AHardwareBuffers, so two of them is
	// ~26 -- still comfortably inside 64, which is why the cap did not have to move
	// for dual. The static_assert is now written against the two-reader case; if
	// kVideoReaderMaxImages ever grows, re-check it against the overflow LOGE in
	// importAhb(), which is the symptom that fires first (and shows up as tearing
	// between the eyes, not as an allocation failure).
	static constexpr int kAhbCacheCap = 64;
	static_assert(kAhbCacheCap > 2 * kVideoReaderMaxImages,
	              "AHB import cache must be larger than BOTH eyes' reader pools");
	AhbImport ahbCache_[kAhbCacheCap];
	int ahbCacheCount_ = 0;
	// Active (current-frame) import, bound by drawAtlas when sourceMode_ == 3.
	VkImage ahbActiveImage_ = VK_NULL_HANDLE;
	VkImageView ahbActiveView_ = VK_NULL_HANDLE;
	uint32_t ahbActiveW_ = 0, ahbActiveH_ = 0;
	// Right eye of a dual-source stream. ahbStereo_ is the ONLY thing that switches
	// drawAtlas onto the two-source path, so a failed right import (which leaves this
	// clear) automatically falls back to the single-source draw.
	VkImage ahbActiveImageR_ = VK_NULL_HANDLE;
	VkImageView ahbActiveViewR_ = VK_NULL_HANDLE;
	bool ahbStereo_ = false;
	float convergence_ = 0.0f;   // fraction of view width; see drawAtlas()
	float convSign_ = 1.0f;      // debug.dxr.mp.conv_sign: 1 = vendor convention (+ = nearer), -1 inverts
	float convScale_ = 1.0f;     // debug.dxr.mp.conv_scale (0 = off, 1 = as authored)
	bool convPropsRead_ = false;
	// One decisive line per STREAM about whether the pair bound, and if not, why.
	// Deliberately not a function-static: a `static bool warned` tripped by an earlier
	// clip would silence exactly the case that matters -- the FIRST play-through of
	// the next one.
	bool stereoBindLogged_ = false;
	VkImage dummyImage_ = VK_NULL_HANDLE;
	VkDeviceMemory dummyMemory_ = VK_NULL_HANDLE;
	VkImageView dummyView_ = VK_NULL_HANDLE;

	std::unordered_map<VkImage, Target> targets_;

};
