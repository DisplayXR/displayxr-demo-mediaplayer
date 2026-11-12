// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
#include "stereo_grid.h"

#include "ColorPolicy.h"

#include "stereo_grid.frag.h"
#include "stereo_grid.vert.h"

#include <android/log.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#define SG_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "MediaPlayerVK", __VA_ARGS__)
#define SG_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MediaPlayerVK", __VA_ARGS__)

namespace {

struct GridPush {
	float background[4];
	float srgbTarget;
};

} // namespace

uint32_t
StereoGridRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
	}
	return UINT32_MAX;
}

bool
StereoGridRenderer::init(VkPhysicalDevice phys, VkDevice device, VkFormat format, uint32_t framesInFlight)
{
	phys_ = phys;
	device_ = device;
	format_ = format;
	srgbTarget_ = mp::color::IsSrgb8((int64_t)format);

	// The atlas arrives undefined and is cleared; the runtime expects
	// COLOR_ATTACHMENT_OPTIMAL at release (as SbsRenderer's pass).
	VkAttachmentDescription att = {};
	att.format = format_;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &ref;
	VkRenderPassCreateInfo rpci = {};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = 1;
	rpci.pAttachments = &att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	if (vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_) != VK_SUCCESS) {
		SG_LOGE("stereo grid: vkCreateRenderPass failed");
		return false;
	}

	// Defined exactly as the ImGui Vulkan backend's texture set layout (one combined image
	// sampler, binding 0, fragment stage), so the ThumbnailAtlas pages' sets bind here.
	VkDescriptorSetLayoutBinding b = {};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo dslci = {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 1;
	dslci.pBindings = &b;
	if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &setLayout_) != VK_SUCCESS) {
		SG_LOGE("stereo grid: vkCreateDescriptorSetLayout failed");
		return false;
	}
	VkPushConstantRange pc = {};
	pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pc.size = sizeof(GridPush);
	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pc;
	if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeLayout_) != VK_SUCCESS) {
		SG_LOGE("stereo grid: vkCreatePipelineLayout failed");
		return false;
	}

	auto makeModule = [&](const uint32_t *code, size_t bytes) {
		VkShaderModuleCreateInfo smci = {};
		smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		smci.codeSize = bytes;
		smci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(device_, &smci, nullptr, &m);
		return m;
	};
	VkShaderModule vert = makeModule(stereo_grid_vert_data, sizeof(stereo_grid_vert_data));
	VkShaderModule frag = makeModule(stereo_grid_frag_data, sizeof(stereo_grid_frag_data));
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		SG_LOGE("stereo grid: shader module creation failed");
		if (vert) vkDestroyShaderModule(device_, vert, nullptr);
		if (frag) vkDestroyShaderModule(device_, frag, nullptr);
		return false;
	}
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkVertexInputBindingDescription vb = {};
	vb.binding = 0;
	vb.stride = sizeof(Instance);
	vb.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
	VkVertexInputAttributeDescription va[3] = {};
	va[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(Instance, rect)};
	va[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(Instance, uv)};
	va[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(Instance, fade)};
	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vb;
	vi.vertexAttributeDescriptionCount = 3;
	vi.pVertexAttributeDescriptions = va;
	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba = {};
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
	                     VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dynStates;
	VkGraphicsPipelineCreateInfo gp = {};
	gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &dyn;
	gp.layout = pipeLayout_;
	gp.renderPass = renderPass_;
	const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	if (r != VK_SUCCESS) {
		SG_LOGE("stereo grid: vkCreateGraphicsPipelines failed: %d", (int)r);
		return false;
	}

	// One instance buffer per frame that can be in flight, plus one: a buffer is rewritten
	// only after as many later grid frames have begun, and each begin waited for the frame
	// that many back.
	bufferCount_ = std::min(kMaxBuffers, std::max(1u, framesInFlight) + 1u);
	const VkDeviceSize bytes = (VkDeviceSize)sizeof(Instance) * kMaxTiles * 2;
	for (uint32_t i = 0; i < bufferCount_; ++i) {
		Buffer &buf = buffers_[i];
		VkBufferCreateInfo bci = {};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = bytes;
		bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device_, &bci, nullptr, &buf.buffer) != VK_SUCCESS) return false;
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device_, buf.buffer, &req);
		const uint32_t type = findMemoryType(
		    req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (type == UINT32_MAX) return false;
		VkMemoryAllocateInfo mai = {};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = type;
		if (vkAllocateMemory(device_, &mai, nullptr, &buf.memory) != VK_SUCCESS) return false;
		vkBindBufferMemory(device_, buf.buffer, buf.memory, 0);
		void *p = nullptr;
		if (vkMapMemory(device_, buf.memory, 0, bytes, 0, &p) != VK_SUCCESS) return false;
		buf.mapped = (Instance *)p;
	}
	SG_LOGI("stereo grid ready (%u instance buffers, %s target)", bufferCount_, srgbTarget_ ? "sRGB" : "UNORM");
	return true;
}

const StereoGridRenderer::Target *
StereoGridRenderer::targetFor(VkImage image, uint32_t w, uint32_t h)
{
	auto it = targets_.find(image);
	if (it != targets_.end() && it->second.w == w && it->second.h == h) return &it->second;
	if (it != targets_.end()) {  // never in practice: the atlas swapchain is fixed-size
		vkDestroyFramebuffer(device_, it->second.fb, nullptr);
		vkDestroyImageView(device_, it->second.view, nullptr);
		targets_.erase(it);
	}
	Target t;
	t.w = w;
	t.h = h;
	VkImageViewCreateInfo vci = {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format_;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(device_, &vci, nullptr, &t.view) != VK_SUCCESS) return nullptr;
	VkFramebufferCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fci.renderPass = renderPass_;
	fci.attachmentCount = 1;
	fci.pAttachments = &t.view;
	fci.width = w;
	fci.height = h;
	fci.layers = 1;
	if (vkCreateFramebuffer(device_, &fci, nullptr, &t.fb) != VK_SUCCESS) {
		vkDestroyImageView(device_, t.view, nullptr);
		return nullptr;
	}
	targets_[image] = t;
	return &targets_[image];
}

bool
StereoGridRenderer::draw(VkCommandBuffer cmd, VkImage image, uint32_t atlasW, uint32_t atlasH, uint32_t renderW,
                         uint32_t renderH, uint32_t cols, uint32_t viewCount,
                         const mp::browser::StereoFrame &frame)
{
	using namespace mp::browser;
	if (!frame.active || cmd == VK_NULL_HANDLE || pipeline_ == VK_NULL_HANDLE || bufferCount_ == 0 ||
	    viewCount == 0 || renderW == 0 || renderH == 0)
		return false;
	const Target *target = targetFor(image, atlasW, atlasH);
	if (target == nullptr) return false;
	const uint32_t c = cols ? cols : 1;

	// The tiles grouped by page (stable, so a page's tiles keep grid order): one bind and
	// one instanced draw per page and view. Instances: every tile's left crop, then every
	// tile's right crop, each half in that order.
	const uint32_t n = (uint32_t)std::min<size_t>(frame.tiles.size(), kMaxTiles);
	std::vector<uint32_t> order(n);
	for (uint32_t i = 0; i < n; ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		return frame.tiles[a].texture < frame.tiles[b].texture;
	});
	Buffer &buf = buffers_[nextBuffer_];
	nextBuffer_ = (nextBuffer_ + 1) % bufferCount_;
	for (uint32_t eye = 0; eye < 2; ++eye) {
		for (uint32_t k = 0; k < n; ++k) {
			const StereoTile &t = frame.tiles[order[k]];
			Instance &inst = buf.mapped[eye * n + k];
			UvRect q;
			if (!ViewQuad(t.x0, t.y0, t.x1, t.y1, frame.hudW, frame.hudH, q)) q = UvRect{0.0f, 0.0f, 0.0f, 0.0f};
			const UvRect &uv = eye ? t.right : t.left;
			const float rect[4] = {q.u0, q.v0, q.u1, q.v1}, crop[4] = {uv.u0, uv.v0, uv.u1, uv.v1};
			std::memcpy(inst.rect, rect, sizeof(rect));
			std::memcpy(inst.uv, crop, sizeof(crop));
			const float tw = t.x1 - t.x0, th = t.y1 - t.y0;
			inst.fade[0] = tw > 0.0f ? frame.fade / tw : 0.0f;
			inst.fade[1] = th > 0.0f ? frame.fade / th : 0.0f;
		}
	}

	auto toTarget = [&](float v) { return srgbTarget_ ? mp::color::DisplayReferredToSceneLinear(v) : v; };
	VkClearValue clear = {};
	clear.color = {{toTarget(frame.background[0]), toTarget(frame.background[1]), toTarget(frame.background[2]),
	                1.0f}};
	VkRenderPassBeginInfo rpbi = {};
	rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpbi.renderPass = renderPass_;
	rpbi.framebuffer = target->fb;
	rpbi.renderArea.extent = {atlasW, atlasH};
	rpbi.clearValueCount = 1;
	rpbi.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	if (n > 0) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &buf.buffer, &zero);
		GridPush push = {};
		push.background[0] = frame.background[0];
		push.background[1] = frame.background[1];
		push.background[2] = frame.background[2];
		push.background[3] = 1.0f;
		push.srgbTarget = srgbTarget_ ? 1.0f : 0.0f;
		vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		for (uint32_t v = 0; v < viewCount; ++v) {
			const uint32_t tx = v % c, ty = v / c;
			int32_t sx = 0, sy = 0;
			uint32_t sw = 0, sh = 0;
			if (!ViewScissor(frame.clip, frame.hudW, frame.hudH, tx, ty, renderW, renderH, sx, sy, sw, sh)) continue;
			const VkViewport vp = {(float)(tx * renderW), (float)(ty * renderH), (float)renderW, (float)renderH,
			                       0.0f, 1.0f};
			const VkRect2D sc = {{sx, sy}, {sw, sh}};
			vkCmdSetViewport(cmd, 0, 1, &vp);
			vkCmdSetScissor(cmd, 0, 1, &sc);
			const uint32_t base = RightEye(v, c) ? n : 0;
			for (uint32_t k = 0; k < n;) {
				const uint64_t tex = frame.tiles[order[k]].texture;
				uint32_t end = k + 1;
				while (end < n && frame.tiles[order[end]].texture == tex) ++end;
				if (tex != 0) {
					const VkDescriptorSet set = (VkDescriptorSet)tex;
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &set, 0,
					                        nullptr);
					vkCmdDraw(cmd, 4, end - k, 0, base + k);
				}
				k = end;
			}
		}
	}
	vkCmdEndRenderPass(cmd);
	if (!loggedFirst_) {
		loggedFirst_ = true;
		SG_LOGI("stereo grid: first frame, %u tiles, %u views of %ux%u", n, viewCount, renderW, renderH);
	}
	return true;
}

void
StereoGridRenderer::cleanup()
{
	if (device_ == VK_NULL_HANDLE) return;
	for (auto &kv : targets_) {
		vkDestroyFramebuffer(device_, kv.second.fb, nullptr);
		vkDestroyImageView(device_, kv.second.view, nullptr);
	}
	targets_.clear();
	for (uint32_t i = 0; i < kMaxBuffers; ++i) {
		Buffer &buf = buffers_[i];
		if (buf.mapped) vkUnmapMemory(device_, buf.memory);
		if (buf.buffer) vkDestroyBuffer(device_, buf.buffer, nullptr);
		if (buf.memory) vkFreeMemory(device_, buf.memory, nullptr);
		buf = Buffer{};
	}
	bufferCount_ = 0;
	nextBuffer_ = 0;
	if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
	if (pipeLayout_) vkDestroyPipelineLayout(device_, pipeLayout_, nullptr);
	if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
	if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
	pipeline_ = VK_NULL_HANDLE;
	pipeLayout_ = VK_NULL_HANDLE;
	setLayout_ = VK_NULL_HANDLE;
	renderPass_ = VK_NULL_HANDLE;
	device_ = VK_NULL_HANDLE;
	loggedFirst_ = false;
}
