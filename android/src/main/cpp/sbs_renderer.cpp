// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0

#include "sbs_renderer.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fullscreen.vert.h"  // fullscreen_vert_data (SPIR-V)
#include "sbs.frag.h"         // sbs_frag_data (SPIR-V)
#include "sbs_ahb.frag.h"     // sbs_ahb_frag_data (SPIR-V) — zero-copy ycbcr blit

#define LOG_TAG "mediaplayer_vk_android"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Must match shaders/sbs.frag's push_constant block (24 bytes).
struct SbsPush {
	float uvOffset[2];
	float uvScale[2];
	int32_t mode;     // 0 = RGBA, 1 = I420, 2 = NV12
	float fullRange;  // 1 = full/JPEG range, 0 = limited/MPEG range
};

uint32_t
SbsRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((typeBits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & props) == props) {
			return i;
		}
	}
	return UINT32_MAX;
}

bool
SbsRenderer::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
                  uint32_t queueFamily, VkFormat format)
{
	phys_ = phys;
	device_ = device;
	queue_ = queue;
	queueFamily_ = queueFamily;
	format_ = format;

	// ── Color-only render pass. Swapchain image arrives undefined (we CLEAR),
	// and the runtime expects COLOR_ATTACHMENT_OPTIMAL at release. ──
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
		LOGE("vkCreateRenderPass failed");
		return false;
	}

	// ── Descriptor set layout: 3 combined image samplers (plane 0..2). ──
	VkDescriptorSetLayoutBinding b[3] = {};
	for (uint32_t i = 0; i < 3; ++i) {
		b[i].binding = i;
		b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dslci = {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 3;
	dslci.pBindings = b;
	if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &setLayout_) != VK_SUCCESS) {
		LOGE("vkCreateDescriptorSetLayout failed");
		return false;
	}

	VkPushConstantRange pc = {};
	pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pc.offset = 0;
	pc.size = sizeof(SbsPush);
	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pc;
	if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeLayout_) != VK_SUCCESS) {
		LOGE("vkCreatePipelineLayout failed");
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
	VkShaderModule vert = makeModule(fullscreen_vert_data, sizeof(fullscreen_vert_data));
	VkShaderModule frag = makeModule(sbs_frag_data, sizeof(sbs_frag_data));
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		LOGE("shader module creation failed");
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

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
	VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	if (r != VK_SUCCESS) {
		LOGE("vkCreateGraphicsPipelines failed: %d", (int)r);
		return false;
	}

	VkSamplerCreateInfo sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) {
		LOGE("vkCreateSampler failed");
		return false;
	}

	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
	VkDescriptorPoolCreateInfo dpci = {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	if (vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_) != VK_SUCCESS) {
		LOGE("vkCreateDescriptorPool failed");
		return false;
	}
	VkDescriptorSetAllocateInfo dsai = {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = descPool_;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &setLayout_;
	if (vkAllocateDescriptorSets(device_, &dsai, &descSet_) != VK_SUCCESS) {
		LOGE("vkAllocateDescriptorSets failed");
		return false;
	}

	VkCommandPoolCreateInfo cpci = {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = queueFamily_;
	if (vkCreateCommandPool(device_, &cpci, nullptr, &cmdPool_) != VK_SUCCESS) {
		LOGE("vkCreateCommandPool failed");
		return false;
	}
	VkCommandBufferAllocateInfo cbai = {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = cmdPool_;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(device_, &cbai, &cmd_) != VK_SUCCESS) {
		LOGE("vkAllocateCommandBuffers failed");
		return false;
	}
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) {
		LOGE("vkCreateFence failed");
		return false;
	}
	if (!ensureDummy()) {
		return false;
	}
	// Zero-copy AHB import entrypoint (device extension fn — must be loaded, not
	// linked). Built lazily into a ycbcr pipeline on the first video frame.
	pfnGetAhbProps_ = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
	    device_, "vkGetAndroidHardwareBufferPropertiesANDROID");
	if (pfnGetAhbProps_ == nullptr) {
		LOGE("vkGetAndroidHardwareBufferPropertiesANDROID unavailable (AHB zero-copy off)");
	}
	LOGI("SbsRenderer initialized (format=0x%x)", (uint32_t)format_);
	return true;
}

void
SbsRenderer::destroyPlane(Plane &p)
{
	if (p.view) vkDestroyImageView(device_, p.view, nullptr);
	if (p.image) vkDestroyImage(device_, p.image, nullptr);
	if (p.memory) vkFreeMemory(device_, p.memory, nullptr);
	if (p.staging) vkDestroyBuffer(device_, p.staging, nullptr);
	if (p.stagingMem) {
		vkUnmapMemory(device_, p.stagingMem);
		vkFreeMemory(device_, p.stagingMem, nullptr);
	}
	p = Plane{};
}

// (Re)create plane idx if the size/format changed. Allocates a device-local
// sampled image + a persistently-mapped host-visible staging buffer.
bool
SbsRenderer::ensurePlane(int idx, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bytesPerTexel)
{
	Plane &p = planes_[idx];
	if (p.image != VK_NULL_HANDLE && p.w == w && p.h == h && p.fmt == fmt) {
		return false;  // reused, no recreate
	}
	destroyPlane(p);
	p.w = w;
	p.h = h;
	p.fmt = fmt;

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	vkCreateImage(device_, &ici, nullptr, &p.image);
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(device_, p.image, &mr);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	vkAllocateMemory(device_, &mai, nullptr, &p.memory);
	vkBindImageMemory(device_, p.image, p.memory, 0);

	VkImageViewCreateInfo vci = {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = p.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = fmt;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCreateImageView(device_, &vci, nullptr, &p.view);

	p.stagingSize = (VkDeviceSize)w * h * bytesPerTexel;
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = p.stagingSize;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vkCreateBuffer(device_, &bci, nullptr, &p.staging);
	VkMemoryRequirements bmr;
	vkGetBufferMemoryRequirements(device_, p.staging, &bmr);
	VkMemoryAllocateInfo bmai = {};
	bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	bmai.allocationSize = bmr.size;
	bmai.memoryTypeIndex = findMemoryType(
	    bmr.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	vkAllocateMemory(device_, &bmai, nullptr, &p.stagingMem);
	vkBindBufferMemory(device_, p.staging, p.stagingMem, 0);
	vkMapMemory(device_, p.stagingMem, 0, p.stagingSize, 0, &p.mapped);
	p.initialized = false;
	return true;  // recreated
}

// Append a plane copy (memcpy into staging + transition + buffer→image copy +
// transition to SHADER_READ) to an already-recording command buffer.
void
SbsRenderer::recordPlaneCopy(VkCommandBuffer cmd, int idx, const uint8_t *src, uint32_t w,
                             uint32_t h, uint32_t bytesPerTexel)
{
	Plane &p = planes_[idx];
	std::memcpy(p.mapped, src, (size_t)w * h * bytesPerTexel);

	VkImageMemoryBarrier toDst = {};
	toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toDst.oldLayout = p.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	                                : VK_IMAGE_LAYOUT_UNDEFINED;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.image = p.image;
	toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	toDst.srcAccessMask = p.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
	toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {w, h, 1};
	vkCmdCopyBufferToImage(cmd, p.staging, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	                       &region);

	VkImageMemoryBarrier toRead = toDst;
	toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &toRead);
	p.initialized = true;
}

bool
SbsRenderer::ensureDummy()
{
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8_UNORM;
	ici.extent = {1, 1, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device_, &ici, nullptr, &dummyImage_) != VK_SUCCESS) return false;
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(device_, dummyImage_, &mr);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	vkAllocateMemory(device_, &mai, nullptr, &dummyMemory_);
	vkBindImageMemory(device_, dummyImage_, dummyMemory_, 0);
	VkImageViewCreateInfo vci = {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = dummyImage_;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8_UNORM;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(device_, &vci, nullptr, &dummyView_) != VK_SUCCESS) return false;

	// Transition UNDEFINED → SHADER_READ_ONLY so it's valid to sample.
	vkResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd_, &cbbi);
	VkImageMemoryBarrier bar = {};
	bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar.image = dummyImage_;
	bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
	vkEndCommandBuffer(cmd_);
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd_;
	vkResetFences(device_, 1, &fence_);
	vkQueueSubmit(queue_, 1, &si, fence_);
	vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
	return true;
}

void
SbsRenderer::bindDescriptors()
{
	VkDescriptorImageInfo info[3] = {};
	for (uint32_t i = 0; i < 3; ++i) {
		info[i].sampler = sampler_;
		info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	info[0].imageView = planes_[0].view ? planes_[0].view : dummyView_;
	// slot1: used by YUV (modes 1/2); slot2: used by I420 (mode 1) only.
	info[1].imageView = (sourceMode_ != 0 && planes_[1].view) ? planes_[1].view : dummyView_;
	info[2].imageView = (sourceMode_ == 1 && planes_[2].view) ? planes_[2].view : dummyView_;
	VkWriteDescriptorSet w[3] = {};
	for (uint32_t i = 0; i < 3; ++i) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = descSet_;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[i].pImageInfo = &info[i];
	}
	vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
}

bool
SbsRenderer::uploadTexture(const uint8_t *rgba, uint32_t width, uint32_t height)
{
	vkDeviceWaitIdle(device_);
	ensurePlane(0, width, height, VK_FORMAT_R8G8B8A8_UNORM, 4);
	sourceMode_ = 0;
	sourceFullRange_ = 1.0f;

	vkResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd_, &cbbi);
	recordPlaneCopy(cmd_, 0, rgba, width, height, 4);
	vkEndCommandBuffer(cmd_);
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd_;
	vkResetFences(device_, 1, &fence_);
	vkQueueSubmit(queue_, 1, &si, fence_);
	vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
	bindDescriptors();
	LOGI("uploadTexture %ux%u (RGBA)", width, height);
	return true;
}

bool
SbsRenderer::uploadYUV(const uint8_t *y, const uint8_t *uv_or_u, const uint8_t *v,
                       uint32_t width, uint32_t height, bool nv12, bool fullRange)
{
	const uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;
	bool recreated = false;
	recreated |= ensurePlane(0, width, height, VK_FORMAT_R8_UNORM, 1);   // Y
	if (nv12) {
		recreated |= ensurePlane(1, cw, ch, VK_FORMAT_R8G8_UNORM, 2);   // interleaved UV
	} else {
		recreated |= ensurePlane(1, cw, ch, VK_FORMAT_R8_UNORM, 1);     // U
		recreated |= ensurePlane(2, cw, ch, VK_FORMAT_R8_UNORM, 1);     // V
	}
	const int prevMode = sourceMode_;
	sourceMode_ = nv12 ? 2 : 1;
	sourceFullRange_ = fullRange ? 1.0f : 0.0f;

	vkResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd_, &cbbi);
	recordPlaneCopy(cmd_, 0, y, width, height, 1);
	if (nv12) {
		recordPlaneCopy(cmd_, 1, uv_or_u, cw, ch, 2);
	} else {
		recordPlaneCopy(cmd_, 1, uv_or_u, cw, ch, 1);
		recordPlaneCopy(cmd_, 2, v, cw, ch, 1);
	}
	vkEndCommandBuffer(cmd_);
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd_;
	vkResetFences(device_, 1, &fence_);
	vkQueueSubmit(queue_, 1, &si, fence_);
	vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
	if (recreated || prevMode != sourceMode_) {
		bindDescriptors();
	}
	return true;
}

const SbsRenderer::Target &
SbsRenderer::targetFor(VkImage image, uint32_t w, uint32_t h)
{
	auto it = targets_.find(image);
	if (it != targets_.end() && it->second.w == w && it->second.h == h) {
		return it->second;
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
	vkCreateImageView(device_, &vci, nullptr, &t.view);
	VkFramebufferCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fci.renderPass = renderPass_;
	fci.attachmentCount = 1;
	fci.pAttachments = &t.view;
	fci.width = w;
	fci.height = h;
	fci.layers = 1;
	vkCreateFramebuffer(device_, &fci, nullptr, &t.fb);
	targets_[image] = t;
	return targets_[image];
}

// ── Zero-copy AHB video path ──────────────────────────────────────────────
// Build the per-stream ycbcr conversion + immutable sampler + descriptor layout
// + pipeline from the first frame's external format. All frames of a stream
// share one external format, so this runs once.
bool
SbsRenderer::ensureAhbPipeline(const VkAndroidHardwareBufferFormatPropertiesANDROID &fmt)
{
	if (ahbInited_) {
		if (fmt.externalFormat == ahbExternalFormat_) return true;
		// A different decoder (AVC vs VP9, or a FORMAT_CHANGED mid-stream) can
		// hand us a different vendor format. The ycbcr conversion is baked into
		// an IMMUTABLE sampler, so it cannot be pointed at the new format --
		// the whole chain has to be rebuilt. Sampling the new images through
		// the old conversion is undefined, and in practice wrong colours.
		LOGI("AHB external format changed %llu -> %llu; rebuilding the ycbcr pipeline",
		     (unsigned long long)ahbExternalFormat_, (unsigned long long)fmt.externalFormat);
		vkDeviceWaitIdle(device_);
		for (int i = 0; i < ahbCacheCount_; ++i) destroyAhbImport(ahbCache_[i]);
		ahbCacheCount_ = 0;
		ahbActiveImage_ = VK_NULL_HANDLE;
		ahbActiveView_ = VK_NULL_HANDLE;
		ahbActiveImageR_ = VK_NULL_HANDLE;
		ahbActiveViewR_ = VK_NULL_HANDLE;
		ahbStereo_ = false;
		destroyAhbPipeline();
	}
	ahbExternalFormat_ = fmt.externalFormat;

	VkExternalFormatANDROID extFmt = {};
	extFmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
	extFmt.externalFormat = fmt.externalFormat;

	VkSamplerYcbcrConversionCreateInfo cci = {};
	cci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
	cci.pNext = &extFmt;
	cci.format = VK_FORMAT_UNDEFINED;  // sampling an external format
	cci.ycbcrModel = fmt.suggestedYcbcrModel;
	cci.ycbcrRange = fmt.suggestedYcbcrRange;
	cci.components = fmt.samplerYcbcrConversionComponents;
	cci.xChromaOffset = fmt.suggestedXChromaOffset;
	cci.yChromaOffset = fmt.suggestedYChromaOffset;
	cci.chromaFilter = VK_FILTER_LINEAR;
	cci.forceExplicitReconstruction = VK_FALSE;
	if (vkCreateSamplerYcbcrConversion(device_, &cci, nullptr, &ahbYcbcr_) != VK_SUCCESS) {
		LOGE("vkCreateSamplerYcbcrConversion failed");
		return false;
	}

	VkSamplerYcbcrConversionInfo convInfo = {};
	convInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
	convInfo.conversion = ahbYcbcr_;
	VkSamplerCreateInfo sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.pNext = &convInfo;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.unnormalizedCoordinates = VK_FALSE;
	if (vkCreateSampler(device_, &sci, nullptr, &ahbSampler_) != VK_SUCCESS) {
		LOGE("ycbcr sampler failed");
		return false;
	}

	// Binding 0 = combined image sampler with the IMMUTABLE ycbcr sampler baked in.
	VkDescriptorSetLayoutBinding b = {};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	b.pImmutableSamplers = &ahbSampler_;
	VkDescriptorSetLayoutCreateInfo dslci = {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 1;
	dslci.pBindings = &b;
	if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &ahbSetLayout_) != VK_SUCCESS) {
		LOGE("ahb set layout failed");
		return false;
	}

	VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SbsPush)};
	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &ahbSetLayout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pc;
	if (vkCreatePipelineLayout(device_, &plci, nullptr, &ahbPipeLayout_) != VK_SUCCESS) {
		LOGE("ahb pipe layout failed");
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
	VkShaderModule vert = makeModule(fullscreen_vert_data, sizeof(fullscreen_vert_data));
	VkShaderModule frag = makeModule(sbs_ahb_frag_data, sizeof(sbs_ahb_frag_data));
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		LOGE("ahb shader module creation failed");
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
	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
	gp.layout = ahbPipeLayout_;
	gp.renderPass = renderPass_;
	VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &ahbPipeline_);
	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	if (r != VK_SUCCESS) {
		LOGE("ahb pipeline failed: %d", (int)r);
		return false;
	}

	// TWO sets, always. Set 0 is the single-source / left-eye binding (unchanged for
	// every SBS and mono stream); set 1 is the right eye of a dual-track stream. They
	// have to be separate SETS rather than one set rewritten between draws, because
	// both eyes are drawn into the atlas inside ONE command buffer -- a mid-recording
	// vkUpdateDescriptorSets would apply to both draws, not to the second one.
	// Allocating the pair unconditionally keeps the single-source path byte-identical
	// (it just never touches set 1) and costs one descriptor.
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
	VkDescriptorPoolCreateInfo dpci = {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.maxSets = 2;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	if (vkCreateDescriptorPool(device_, &dpci, nullptr, &ahbDescPool_) != VK_SUCCESS) {
		LOGE("ahb desc pool failed");
		return false;
	}
	const VkDescriptorSetLayout layouts[2] = {ahbSetLayout_, ahbSetLayout_};
	VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkDescriptorSetAllocateInfo dsai = {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = ahbDescPool_;
	dsai.descriptorSetCount = 2;
	dsai.pSetLayouts = layouts;
	if (vkAllocateDescriptorSets(device_, &dsai, sets) != VK_SUCCESS) {
		LOGE("ahb desc set alloc failed");
		return false;
	}
	ahbDescSet_ = sets[0];
	ahbDescSetR_ = sets[1];
	ahbInited_ = true;
	LOGI("AHB ycbcr pipeline ready (externalFormat=%llu model=%d range=%d)",
	     (unsigned long long)fmt.externalFormat, (int)fmt.suggestedYcbcrModel,
	     (int)fmt.suggestedYcbcrRange);
	return true;
}

// Import (or cache-hit) an AHardwareBuffer as a Vulkan image aliasing its memory.
const SbsRenderer::AhbImport *
SbsRenderer::importAhb(struct AHardwareBuffer *ahb, uint32_t w, uint32_t h)
{
	for (int i = 0; i < ahbCacheCount_; ++i) {
		if (ahbCache_[i].ahb == ahb) return &ahbCache_[i];
	}
	if (pfnGetAhbProps_ == nullptr) return nullptr;

	VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps = {};
	fmtProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
	VkAndroidHardwareBufferPropertiesANDROID props = {};
	props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
	props.pNext = &fmtProps;
	if (pfnGetAhbProps_(device_, ahb, &props) != VK_SUCCESS) {
		LOGE("vkGetAndroidHardwareBufferProperties failed");
		return nullptr;
	}
	if (!ensureAhbPipeline(fmtProps)) return nullptr;

	// VkImage aliasing the AHB. External format → image/view format UNDEFINED,
	// usage SAMPLED only, and the ycbcr conversion must be set on the view.
	VkExternalFormatANDROID extFmt = {};
	extFmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
	extFmt.externalFormat = fmtProps.externalFormat;
	VkExternalMemoryImageCreateInfo extMem = {};
	extMem.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	extMem.pNext = &extFmt;
	extMem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &extMem;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmtProps.format;  // VK_FORMAT_UNDEFINED for an external format
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage image = VK_NULL_HANDLE;
	if (vkCreateImage(device_, &ici, nullptr, &image) != VK_SUCCESS) {
		LOGE("ahb vkCreateImage failed");
		return nullptr;
	}

	VkImportAndroidHardwareBufferInfoANDROID importInfo = {};
	importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
	importInfo.buffer = ahb;
	VkMemoryDedicatedAllocateInfo dedicated = {};
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.pNext = &importInfo;
	dedicated.image = image;
	uint32_t memType = 0;
	for (uint32_t i = 0; i < 32; ++i) {
		if (props.memoryTypeBits & (1u << i)) {
			memType = i;
			break;
		}
	}
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.pNext = &dedicated;
	mai.allocationSize = props.allocationSize;
	mai.memoryTypeIndex = memType;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (vkAllocateMemory(device_, &mai, nullptr, &memory) != VK_SUCCESS) {
		LOGE("ahb vkAllocateMemory failed");
		vkDestroyImage(device_, image, nullptr);
		return nullptr;
	}
	if (vkBindImageMemory(device_, image, memory, 0) != VK_SUCCESS) {
		LOGE("ahb vkBindImageMemory failed");
		vkFreeMemory(device_, memory, nullptr);
		vkDestroyImage(device_, image, nullptr);
		return nullptr;
	}

	VkSamplerYcbcrConversionInfo convInfo = {};
	convInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
	convInfo.conversion = ahbYcbcr_;
	VkImageViewCreateInfo ivci = {};
	ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivci.pNext = &convInfo;
	ivci.image = image;
	ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivci.format = fmtProps.format;  // UNDEFINED for external; the conversion drives it
	ivci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView view = VK_NULL_HANDLE;
	if (vkCreateImageView(device_, &ivci, nullptr, &view) != VK_SUCCESS) {
		LOGE("ahb vkCreateImageView failed");
		vkFreeMemory(device_, memory, nullptr);
		vkDestroyImage(device_, image, nullptr);
		return nullptr;
	}

	AHardwareBuffer_acquire(ahb);  // own a ref; released in destroyAhbImport

	if (ahbCacheCount_ == kAhbCacheCap) {
		// Unreachable while kAhbCacheCap > the reader pool depth AND every
		// stream's imports are dropped (resetVideoAhb) before the next stream
		// opens. If it fires, one of those broke and we are destroying a live
		// import every frame -- the picture tears between eyes. Say so.
		static bool warned = false;
		if (!warned) {
			warned = true;
			LOGE("AHB import cache overflow (cap=%d) — evicting a live import; "
			     "kAhbCacheCap must exceed the AImageReader pool depth",
			     kAhbCacheCap);
		}
		destroyAhbImport(ahbCache_[0]);
		for (int i = 1; i < ahbCacheCount_; ++i) ahbCache_[i - 1] = ahbCache_[i];
		ahbCacheCount_--;
	}
	AhbImport &slot = ahbCache_[ahbCacheCount_++];
	slot.ahb = ahb;
	slot.image = image;
	slot.memory = memory;
	slot.view = view;
	return &slot;
}

void
SbsRenderer::destroyAhbImport(AhbImport &imp)
{
	if (imp.view) vkDestroyImageView(device_, imp.view, nullptr);
	if (imp.memory) vkFreeMemory(device_, imp.memory, nullptr);
	if (imp.image) vkDestroyImage(device_, imp.image, nullptr);
	if (imp.ahb) AHardwareBuffer_release(imp.ahb);
	imp = AhbImport{};
}

bool
SbsRenderer::setVideoAhb(struct AHardwareBuffer *ahb, uint32_t width, uint32_t height)
{
	const AhbImport *imp = importAhb(ahb, width, height);
	if (imp == nullptr) return false;
	// Point the (immutable-sampler) descriptor at this frame's view. Safe to
	// rewrite each frame: drawAtlas waits idle, so the prior submit is done.
	VkDescriptorImageInfo dii = {};
	dii.imageView = imp->view;
	dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkWriteDescriptorSet wr = {};
	wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	wr.dstSet = ahbDescSet_;
	wr.dstBinding = 0;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wr.pImageInfo = &dii;
	vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
	ahbActiveImage_ = imp->image;
	ahbActiveView_ = imp->view;
	ahbActiveW_ = width;
	ahbActiveH_ = height;
	ahbStereo_ = false;  // single source: leave (or never enter) the dual path
	sourceMode_ = 3;
	sourceFullRange_ = 1.0f;  // unused in mode 3 (the ycbcr conversion owns range)
	return true;
}

bool
SbsRenderer::setVideoAhbStereo(struct AHardwareBuffer *ahbL, uint32_t wL, uint32_t hL,
                               struct AHardwareBuffer *ahbR, uint32_t wR, uint32_t hR)
{
	// Every exit below leaves the renderer in a consistent SINGLE-source state with
	// the left eye bound (flat, but correct and never black), and says why exactly
	// once per stream -- `stereoBindLogged_` is reset with the stream in
	// resetVideoAhb(), so a first-play-through failure cannot hide behind a
	// process-lifetime `static bool warned` that a previous clip already tripped.
	auto fail = [&](const char *why) {
		if (!stereoBindLogged_) {
			stereoBindLogged_ = true;
			LOGE("[LVF] stereo bind FAILED (%s): L=%ux%u R=%ux%u extFmt=%llu inited=%d "
			     "setL=%p setR=%p — showing the LEFT eye in both views",
			     why, wL, hL, wR, hR, (unsigned long long)ahbExternalFormat_, (int)ahbInited_,
			     (void *)ahbDescSet_, (void *)ahbDescSetR_);
		}
		return false;
	};

	// Left first, through the ordinary single-source path: it builds/validates the
	// per-stream ycbcr pipeline and leaves a usable flat-left picture bound if the
	// right eye then fails. (setVideoAhb clears ahbStereo_.)
	if (!setVideoAhb(ahbL, wL, hL)) return fail("left import");
	if (ahbR == nullptr) return fail("no right buffer");
	if (wR != wL || hR != hL) {
		// The two eye tracks of a dual-track file are the same codec at the same
		// resolution by construction. If they are not, the ONE ycbcr conversion built
		// from the left stream's external format is not valid for the right one, and
		// sampling through it is undefined -- so refuse and stay flat-left.
		return fail("eye sizes differ");
	}
	const AhbImport *impR = importAhb(ahbR, wR, hR);
	if (impR == nullptr) return fail("right import");

	// importAhb() REBUILDS the whole ycbcr chain if the right buffer carries a
	// different vendor external format from the left -- and that rebuild destroys the
	// left import this call just bound, clearing ahbActiveView_.
	//
	// Bailing out here was a PERMANENT flat-left: next frame the left import would
	// rebuild back to its own format, the right would rebuild again, and the two would
	// alternate forever with neither pair ever binding. Since the rebuild has already
	// happened and the pipeline now matches the RIGHT eye's format, re-importing the
	// left against it converges in one step instead. If it still does not take, the
	// two streams genuinely disagree and flat-left is the honest answer.
	if (ahbActiveView_ == VK_NULL_HANDLE) {
		LOGI("[LVF] eye streams reported different external formats; rebinding the left "
		     "against the rebuilt pipeline");
		if (!setVideoAhb(ahbL, wL, hL)) return fail("left re-import after format rebuild");
		impR = importAhb(ahbR, wR, hR);
		if (impR == nullptr || ahbActiveView_ == VK_NULL_HANDLE) {
			return fail("external formats will not converge");
		}
	}

	VkDescriptorImageInfo dii = {};
	dii.imageView = impR->view;
	dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkWriteDescriptorSet wr = {};
	wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	wr.dstSet = ahbDescSetR_;
	wr.dstBinding = 0;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wr.pImageInfo = &dii;
	vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
	ahbActiveImageR_ = impR->image;
	ahbActiveViewR_ = impR->view;
	ahbStereo_ = true;
	if (!stereoBindLogged_) {
		stereoBindLogged_ = true;
		LOGI("[LVF] first stereo pair BOUND: %ux%u per eye, extFmt=%llu, setL=%p setR=%p",
		     wL, hL, (unsigned long long)ahbExternalFormat_, (void *)ahbDescSet_,
		     (void *)ahbDescSetR_);
	}
	return true;
}

void
SbsRenderer::drawAtlas(VkImage image, uint32_t atlasW, uint32_t atlasH, uint32_t renderW,
                       uint32_t renderH, uint32_t cols, uint32_t rows, uint32_t viewCount,
                       float contentAspect, bool mono, const float clearRgb[3])
{
	// EVERY handle the AHB path is about to bind is checked, not just the view. The
	// failure this prevents is not a wrong picture, it is a HANG: binding a destroyed
	// pipeline/descriptor set faults the GPU, and this function's own
	// vkWaitForFences(UINT64_MAX) then never returns, which presents as the app
	// freezing with no error anywhere. resetVideoAhb() destroys all of these together
	// when a stream ends, so any one of them being null means "the stream this state
	// belonged to is gone" and the right answer is to draw nothing.
	const bool useAhb = (sourceMode_ == 3 && ahbActiveView_ != VK_NULL_HANDLE &&
	                     ahbActiveImage_ != VK_NULL_HANDLE && ahbPipeline_ != VK_NULL_HANDLE &&
	                     ahbDescSet_ != VK_NULL_HANDLE);
	if (!useAhb && planes_[0].view == VK_NULL_HANDLE) return;
	// Dual-source (LVF v2): each eye has its OWN imported image, so `mono` and the
	// SBS column split do not apply -- every view samples its own image whole. Falls
	// back to the single-source draw (left eye to both views: flat, but correct and
	// never a fault) unless BOTH eyes' images and BOTH descriptor sets are live.
	const bool dual = useAhb && ahbStereo_ && ahbActiveViewR_ != VK_NULL_HANDLE &&
	                  ahbActiveImageR_ != VK_NULL_HANDLE && ahbDescSetR_ != VK_NULL_HANDLE;
	VkPipeline pipe = useAhb ? ahbPipeline_ : pipeline_;
	VkPipelineLayout pl = useAhb ? ahbPipeLayout_ : pipeLayout_;
	VkDescriptorSet ds = useAhb ? ahbDescSet_ : descSet_;
	const Target &t = targetFor(image, atlasW, atlasH);
	const uint32_t c = cols ? cols : 1;

	// ── Convergence: sign, and where it comes from ───────────────────────────
	//
	// THE CONVENTION: POSITIVE convergence = NEARER (content comes toward the viewer),
	// NEGATIVE = FURTHER (content recedes behind the glass). This is the vendor convention, and it is
	// the default here. An earlier draft of this reader assumed the opposite; the
	// mapping below is the corrected one.
	//
	// DERIVATION (first principles), so the code can be checked rather than believed.
	// Put the screen plane at distance D in front of the viewer and the eyes at
	// x = -e/2 (left) and x = +e/2 (right). A scene point on the centre line at depth
	// D + b (b > 0 = BEHIND the glass) projects onto the screen at
	//     x_eye = e_x * b / (D + b)
	// so x_L = -(e/2)*b/(D+b) and x_R = +(e/2)*b/(D+b). Hence x_R > x_L for b > 0:
	// content BEHIND the screen has the right eye's image to the RIGHT of the left
	// eye's (uncrossed disparity), and the deeper it sits the larger x_R - x_L is.
	// So pushing content BACK means INCREASING x_R - x_L, and pulling it FORWARD means
	// decreasing it.
	//
	// Displaying content shifted RIGHT by s means each screen pixel shows what used to
	// be s to its LEFT, i.e. it SUBTRACTS s from the sampling u. With "positive =
	// nearer", split half per eye:
	//     sampled:    uL += -c/2              uR += +c/2
	//     displayed:  left moves RIGHT by c/2, right moves LEFT by c/2  (x_R-x_L -= c)
	// -> positive c reduces uncrossed disparity: content comes forward. Correct.
	//
	// WHY THIS SIGN, on evidence rather than taste:
	//   - the vendor camera SDK reconvergence shader adds `viewPosition * c` to the
	//     sampling u with viewPosition = -0.5 for the left view and +0.5 for the
	//     right -- exactly the mapping above.
	//   - Its diopter->convergence conversion is NEGATIVE-signed: focus at infinity
	//     gives c = 0 and focusing nearer drives c increasingly negative.
	//   - The physics agrees. These files come from a PARALLEL two-camera rig, where
	//     a point at distance Z has x_R - x_L = -f*b/Z < 0 -- every object is crossed,
	//     i.e. floating in front of the glass, with only infinity at the screen. The
	//     correction such a rig needs is therefore always "push back", and a real
	//     vendor-camera v1 capture carries c ~ -0.05 throughout. Under this convention that
	//     negative value pushes back, which is the whole point of the metadata.
	//
	// `setprop debug.dxr.mp.conv_sign -1` inverts the mapping without a rebuild (the
	// A/B that settled it); `debug.dxr.mp.conv_scale 0` disables convergence entirely,
	// which is the same thing a `_noreconv` filename asks for.
	//
	// Not modelled: the vendor shader also crops/zooms by (1 - |c|) about the centre to
	// hide the edge strip the shift exposes. Here that strip is clamp-to-edge instead
	// (both AHB samplers are CLAMP_TO_EDGE), so a large |c| smears the outer column
	// rather than reframing. At |c| ~ 0.05 that is a 2.5%-of-width edge artifact.
	if (!convPropsRead_) {
		convPropsRead_ = true;
		char sp[PROP_VALUE_MAX] = {};
		if (__system_property_get("debug.dxr.mp.conv_sign", sp) > 0 && sp[0]) {
			const float v = (float)std::atof(sp);
			if (v < 0.0f) convSign_ = -1.0f;
			else if (v > 0.0f) convSign_ = 1.0f;
		}
		sp[0] = '\0';
		if (__system_property_get("debug.dxr.mp.conv_scale", sp) > 0 && sp[0]) {
			const float v = (float)std::atof(sp);
			if (v >= 0.0f && v <= 10.0f) convScale_ = v;
		}
		LOGI("[LVF] convergence knobs: sign=%.0f scale=%.3f", convSign_, convScale_);
	}
	// Half the shift per eye; the LEFT eye samples at -c/2 and the right at +c/2, per
	// the derivation above.
	const float convHalf = dual ? convergence_ * convSign_ * convScale_ * 0.5f : 0.0f;

	vkResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd_, &cbbi);

	// Zero-copy AHB: acquire the imported image from the FOREIGN queue (the video
	// decoder/display engine wrote it) to our graphics family before sampling.
	// oldLayout UNDEFINED is correct for an external image — the pixels live in
	// the AHardwareBuffer, not Vulkan's layout tracking, so they're preserved.
	if (useAhb) {
		VkImageMemoryBarrier bar = {};
		bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		bar.srcAccessMask = 0;
		bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		bar.dstQueueFamilyIndex = queueFamily_;
		bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		// Both eyes' images are written by the decoder on the foreign queue, so both
		// need the acquire -- barriering only the left one leaves the right eye's
		// pixels formally undefined (in practice: intermittently stale or torn).
		VkImageMemoryBarrier bars[2] = {bar, bar};
		bars[0].image = ahbActiveImage_;
		bars[1].image = ahbActiveImageR_;
		const uint32_t nbars = dual ? 2u : 1u;
		vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
		                     nbars, bars);
	}

	// One render pass over the WHOLE atlas: clear it black once (the letterbox
	// bars inside each tile are this cleared black), then render every view's
	// tile into a sub-rect via per-tile viewport + scissor.
	VkClearValue clear = {};
	clear.color = {{clearRgb[0], clearRgb[1], clearRgb[2], 1.0f}};
	VkRenderPassBeginInfo rpbi = {};
	rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpbi.renderPass = renderPass_;
	rpbi.framebuffer = t.fb;
	rpbi.renderArea.extent = {atlasW, atlasH};
	rpbi.clearValueCount = 1;
	rpbi.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd_, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	for (uint32_t v = 0; v < viewCount; ++v) {
		const uint32_t tile_x = v % c;
		const uint32_t tile_y = v / c;
		const float tx = (float)(tile_x * renderW);
		const float ty = (float)(tile_y * renderH);

		// Min-to-min (MatchMinRect) fit WITHIN the tile: content's shorter side
		// == the tile's shorter side, centered; the longer axis crops (scissor)
		// or letterboxes (cleared black). No stretch.
		const float tileMin = renderW < renderH ? (float)renderW : (float)renderH;
		float cqW, cqH;
		if (contentAspect >= 1.0f) {
			cqH = tileMin;
			cqW = tileMin * contentAspect;
		} else {
			cqW = tileMin;
			cqH = tileMin / contentAspect;
		}
		const float vx = tx + ((float)renderW - cqW) * 0.5f;
		const float vy = ty + ((float)renderH - cqH) * 0.5f;

		// UV: mono → whole image to every view; stereo SBS → this view's column
		// slice (left half to view 0, right half to view 1 for a 2×1 layout).
		// Which eye this tile is. Same column rule the SBS split uses (tile_x 0 =
		// left), so a 2x2 quad mode gets left/right/left/right rather than every view
		// past the first showing the right eye. With one column there is only one eye
		// a tile can be, and the view index is all that is left to go on.
		const bool rightEye = (c > 1) ? (tile_x != 0) : (v != 0);
		// Dual adds: this eye's WHOLE image, shifted by half the convergence.
		const float boxW = (dual || mono) ? 1.0f : 1.0f / (float)c;
		float offBase = (dual || mono) ? 0.0f : (float)tile_x / (float)c;
		if (dual) offBase += rightEye ? convHalf : -convHalf;
		VkDescriptorSet dsv = (dual && rightEye) ? ahbDescSetR_ : ds;

		// Content quad, scissor-clipped to the tile.
		VkViewport vp = {vx, vy, cqW, cqH, 0.0f, 1.0f};
		VkRect2D sc = {{(int32_t)tx, (int32_t)ty}, {renderW, renderH}};
		vkCmdSetViewport(cmd_, 0, 1, &vp);
		vkCmdSetScissor(cmd_, 0, 1, &sc);
		vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dsv, 0, nullptr);
		SbsPush push = {};
		push.uvOffset[0] = offBase;
		push.uvOffset[1] = 0.0f;
		push.uvScale[0] = boxW;
		push.uvScale[1] = 1.0f;
		push.mode = sourceMode_;
		push.fullRange = sourceFullRange_;
		vkCmdPushConstants(cmd_, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(cmd_, 3, 1, 0, 0);
	}

	vkCmdEndRenderPass(cmd_);
	vkEndCommandBuffer(cmd_);

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd_;
	vkResetFences(device_, 1, &fence_);
	vkQueueSubmit(queue_, 1, &si, fence_);
	vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
}

void
SbsRenderer::destroyAhbPipeline()
{
	if (ahbPipeline_) vkDestroyPipeline(device_, ahbPipeline_, nullptr);
	if (ahbPipeLayout_) vkDestroyPipelineLayout(device_, ahbPipeLayout_, nullptr);
	if (ahbDescPool_) vkDestroyDescriptorPool(device_, ahbDescPool_, nullptr);  // frees ahbDescSet_
	if (ahbSetLayout_) vkDestroyDescriptorSetLayout(device_, ahbSetLayout_, nullptr);
	if (ahbSampler_) vkDestroySampler(device_, ahbSampler_, nullptr);
	if (ahbYcbcr_) vkDestroySamplerYcbcrConversion(device_, ahbYcbcr_, nullptr);
	ahbPipeline_ = VK_NULL_HANDLE;
	ahbPipeLayout_ = VK_NULL_HANDLE;
	ahbDescPool_ = VK_NULL_HANDLE;
	ahbDescSet_ = VK_NULL_HANDLE;
	ahbDescSetR_ = VK_NULL_HANDLE;
	ahbSetLayout_ = VK_NULL_HANDLE;
	ahbSampler_ = VK_NULL_HANDLE;
	ahbYcbcr_ = VK_NULL_HANDLE;
	ahbExternalFormat_ = 0;
	ahbInited_ = false;
}

void
SbsRenderer::resetVideoAhb()
{
	if (device_ == VK_NULL_HANDLE) return;
	// drawAtlas() is synchronous (fence wait before return), so nothing is in
	// flight on the render thread -- but this is a once-per-open path and an
	// idle wait costs nothing here, whereas a stale-image guess costs a tear.
	vkDeviceWaitIdle(device_);
	for (int i = 0; i < ahbCacheCount_; ++i) destroyAhbImport(ahbCache_[i]);
	ahbCacheCount_ = 0;
	ahbActiveImage_ = VK_NULL_HANDLE;
	ahbActiveView_ = VK_NULL_HANDLE;
	ahbActiveImageR_ = VK_NULL_HANDLE;
	ahbActiveViewR_ = VK_NULL_HANDLE;
	ahbStereo_ = false;
	convergence_ = 0.0f;
	stereoBindLogged_ = false;  // per-STREAM, so the next clip reports its own outcome
	ahbActiveW_ = ahbActiveH_ = 0;
	if (sourceMode_ == 3) sourceMode_ = 0;  // nothing bound until the next source
	destroyAhbPipeline();
}

void
SbsRenderer::cleanup()
{
	if (device_ == VK_NULL_HANDLE) return;
	vkDeviceWaitIdle(device_);
	// Zero-copy AHB resources.
	for (int i = 0; i < ahbCacheCount_; ++i) destroyAhbImport(ahbCache_[i]);
	ahbCacheCount_ = 0;
	ahbActiveImage_ = VK_NULL_HANDLE;
	ahbActiveView_ = VK_NULL_HANDLE;
	ahbActiveImageR_ = VK_NULL_HANDLE;
	ahbActiveViewR_ = VK_NULL_HANDLE;
	ahbStereo_ = false;
	destroyAhbPipeline();
	for (auto &kv : targets_) {
		if (kv.second.fb) vkDestroyFramebuffer(device_, kv.second.fb, nullptr);
		if (kv.second.view) vkDestroyImageView(device_, kv.second.view, nullptr);
	}
	targets_.clear();
	for (auto &p : planes_) destroyPlane(p);
	if (dummyView_) vkDestroyImageView(device_, dummyView_, nullptr);
	if (dummyImage_) vkDestroyImage(device_, dummyImage_, nullptr);
	if (dummyMemory_) vkFreeMemory(device_, dummyMemory_, nullptr);
	if (fence_) vkDestroyFence(device_, fence_, nullptr);
	if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
	if (descPool_) vkDestroyDescriptorPool(device_, descPool_, nullptr);
	if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
	if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
	if (pipeLayout_) vkDestroyPipelineLayout(device_, pipeLayout_, nullptr);
	if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
	if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
	dummyView_ = VK_NULL_HANDLE;
	dummyImage_ = VK_NULL_HANDLE;
	dummyMemory_ = VK_NULL_HANDLE;
	fence_ = VK_NULL_HANDLE;
	cmdPool_ = VK_NULL_HANDLE;
	descPool_ = VK_NULL_HANDLE;
	sampler_ = VK_NULL_HANDLE;
	pipeline_ = VK_NULL_HANDLE;
	pipeLayout_ = VK_NULL_HANDLE;
	setLayout_ = VK_NULL_HANDLE;
	renderPass_ = VK_NULL_HANDLE;
	device_ = VK_NULL_HANDLE;
}
