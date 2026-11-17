// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// StereoGridRenderer — the 3D grid's projection pass (#180, docs/BROWSER.md "3D grid").
// Under the browser's "3D only" filter the panel is in 3D and this pass REPLACES
// SbsRenderer::drawAtlas for the frame: it clears the atlas to the browser's background
// and draws every tile of mp::browser::StereoFrame into every view's tile, the left eye's
// crop into the left-eye views and the right eye's into the others, at the rect the HUD
// laid out THIS frame. The ImGui HUD (a window-space layer at disparity 0) stays the flat
// chrome on top, transparent over the tiles.
//
// It samples the ThumbnailAtlas pages through their own ImGui descriptor sets (the set
// layout here is defined identically to the ImGui Vulkan backend's, so the sets are
// compatible): one bind per page and view, one instanced draw each, never one per tile.
//
// Records into the frame command buffer (SbsRenderer::frameCmd) after the HUD pass, which
// recorded the thumbnail uploads; endFrame() submits both. Instances go through a ring of
// host-visible buffers, one more than the frames in flight.
#pragma once

#include "browser/StereoGrid.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>

struct StereoGridRenderer {
	bool init(VkPhysicalDevice phys, VkDevice device, VkFormat format, uint32_t framesInFlight);
	void cleanup();

	// Record the grid into `image` (the atlas, atlasW x atlasH): views 0..viewCount-1 at
	// tiles of renderW x renderH in a `cols`-column grid. Returns false (nothing recorded)
	// when the frame is not active or a resource is missing; the caller then draws the
	// atlas as usual.
	bool draw(VkCommandBuffer cmd, VkImage image, uint32_t atlasW, uint32_t atlasH, uint32_t renderW,
	          uint32_t renderH, uint32_t cols, uint32_t viewCount, const mp::browser::StereoFrame &frame);

private:
	struct Instance {
		float rect[4];
		float uv[4];
		float fade[2];
	};
	struct Target {
		VkImageView view = VK_NULL_HANDLE;
		VkFramebuffer fb = VK_NULL_HANDLE;
		uint32_t w = 0, h = 0;
	};
	struct Buffer {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		Instance *mapped = nullptr;
	};
	static constexpr uint32_t kMaxTiles = 256;  // a screen of tiles many times over
	static constexpr uint32_t kMaxBuffers = 8;

	const Target *targetFor(VkImage image, uint32_t w, uint32_t h);
	uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

	VkPhysicalDevice phys_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkFormat format_ = VK_FORMAT_UNDEFINED;
	bool srgbTarget_ = false;
	VkRenderPass renderPass_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	Buffer buffers_[kMaxBuffers];
	uint32_t bufferCount_ = 0;
	uint32_t nextBuffer_ = 0;
	std::unordered_map<VkImage, Target> targets_;
	bool loggedFirst_ = false;
};
