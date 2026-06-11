// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "sbs_renderer.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <cmath>
#include <cstring>
#include <vector>

#include "fullscreen.vert.h"  // fullscreen_vert_data (SPIR-V)
#include "overlay.frag.h"     // overlay_frag_data (SPIR-V)
#include "overlay.vert.h"     // overlay_vert_data (SPIR-V)
#include "sbs.frag.h"         // sbs_frag_data (SPIR-V)
#include "sbs_ahb.frag.h"     // sbs_ahb_frag_data (SPIR-V) — zero-copy ycbcr blit
#include "transport_ui.h"     // shared bar/button layout fractions

namespace {
// One overlay vertex: clip-space position + straight RGBA. Matches overlay.vert.
struct OvVert {
	float x, y;        // NDC
	float r, g, b, a;  // colour
};

// Seven-segment masks for digits 0-9. Bit0=a(top) 1=b(top-right) 2=c(bottom-
// right) 3=d(bottom) 4=e(bottom-left) 5=f(top-left) 6=g(middle).
constexpr uint8_t kSeg7[10] = {
    0x3F,  // 0: a b c d e f
    0x06,  // 1: b c
    0x5B,  // 2: a b g e d
    0x4F,  // 3: a b g c d
    0x66,  // 4: f g b c
    0x6D,  // 5: a f g c d
    0x7D,  // 6: a f g e c d
    0x07,  // 7: a b c
    0x7F,  // 8: all
    0x6F,  // 9: a b c d f g
};
}  // namespace

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
	if (!initOverlayPipeline()) {
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

// 2D triangle pipeline for the transport overlay: (vec2 NDC, vec4 RGBA) verts,
// alpha-blended, no descriptors/push-constants, sharing the color render pass.
bool
SbsRenderer::initOverlayPipeline()
{
	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	if (vkCreatePipelineLayout(device_, &plci, nullptr, &ovPipeLayout_) != VK_SUCCESS) {
		LOGE("overlay pipeline layout failed");
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
	VkShaderModule vert = makeModule(overlay_vert_data, sizeof(overlay_vert_data));
	VkShaderModule frag = makeModule(overlay_frag_data, sizeof(overlay_frag_data));
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		LOGE("overlay shader module creation failed");
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

	VkVertexInputBindingDescription bind = {0, sizeof(OvVert), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription attrs[2] = {};
	attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OvVert, x)};
	attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OvVert, r)};
	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = attrs;

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
	cba.blendEnable = VK_TRUE;
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.alphaBlendOp = VK_BLEND_OP_ADD;
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
	gp.layout = ovPipeLayout_;
	gp.renderPass = renderPass_;
	VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &ovPipeline_);
	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	if (r != VK_SUCCESS) {
		LOGE("overlay vkCreateGraphicsPipelines failed: %d", (int)r);
		return false;
	}

	// Host-visible vertex buffer, rebuilt each frame (capacity is generous —
	// the whole transport bar is a few hundred verts).
	ovVboCapVerts_ = 6144;
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = (VkDeviceSize)ovVboCapVerts_ * sizeof(OvVert);
	bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device_, &bci, nullptr, &ovVbo_) != VK_SUCCESS) {
		LOGE("overlay vbo create failed");
		return false;
	}
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(device_, ovVbo_, &mr);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = findMemoryType(
	    mr.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (vkAllocateMemory(device_, &mai, nullptr, &ovVboMem_) != VK_SUCCESS) {
		LOGE("overlay vbo alloc failed");
		return false;
	}
	vkBindBufferMemory(device_, ovVbo_, ovVboMem_, 0);
	vkMapMemory(device_, ovVboMem_, 0, bci.size, 0, &ovVboMapped_);
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

void
SbsRenderer::setOverlay(bool visible, float progress, bool paused, const char *left,
                        const char *right)
{
	ovVisible_ = visible;
	ovProgress_ = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
	ovPaused_ = paused;
	std::strncpy(ovLeft_, left ? left : "", sizeof(ovLeft_) - 1);
	ovLeft_[sizeof(ovLeft_) - 1] = '\0';
	std::strncpy(ovRight_, right ? right : "", sizeof(ovRight_) - 1);
	ovRight_[sizeof(ovRight_) - 1] = '\0';
}

// Build + draw the transport overlay as alpha-blended triangles (overlay
// pipeline). Must be called inside the render pass, after the SBS draw. Drawn
// identically in both eyes → zero disparity → sits at the screen plane. All
// geometry is anchored to transport_ui.h fractions so it lines up exactly with
// the touch hit-test in main.cpp. The "px round" discs use the w/h aspect so
// the knob reads as a circle, not an ellipse.
void
SbsRenderer::drawOverlay(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
	if (!ovVisible_ || ovPipeline_ == VK_NULL_HANDLE) return;

	std::vector<OvVert> v;
	v.reserve(2048);
	const float aspect = (float)h / (float)w;  // x-fraction per y-fraction for round discs

	// transport_ui fractions are SCREEN fractions (0 = top, 1 = bottom),
	// matching the touch hit-test. Vulkan NDC has +Y pointing DOWN, so the map
	// is the plain affine one — no vertical flip. (An earlier flip compensated
	// for the Leia weave presenting the eye tile upside-down; the runtime's
	// tiled-atlas rework made tiles upright, which left the bar drawing at
	// screen-top while touch stayed at screen-bottom.) X is unchanged
	// (play=left, load=right).
	auto NX = [](float fx) { return fx * 2.0f - 1.0f; };
	auto NY = [](float fy) { return fy * 2.0f - 1.0f; };
	auto pt = [&](float fx, float fy, const float c[4]) {
		v.push_back({NX(fx), NY(fy), c[0], c[1], c[2], c[3]});
	};
	auto quad = [&](float x0, float y0, float x1, float y1, const float c[4]) {
		pt(x0, y0, c); pt(x1, y0, c); pt(x1, y1, c);
		pt(x0, y0, c); pt(x1, y1, c); pt(x0, y1, c);
	};
	auto tri = [&](float ax, float ay, float bx, float by, float cx, float cy, const float c[4]) {
		pt(ax, ay, c); pt(bx, by, c); pt(cx, cy, c);
	};
	auto disc = [&](float cx, float cy, float ry, const float c[4]) {
		const float rx = ry * aspect;  // round in pixels
		const int N = 18;
		float prevx = cx + rx, prevy = cy;
		for (int i = 1; i <= N; ++i) {
			const float a = (float)i / N * 6.2831853f;
			const float nx = cx + rx * cosf(a), ny = cy + ry * sinf(a);
			tri(cx, cy, prevx, prevy, nx, ny, c);
			prevx = nx; prevy = ny;
		}
	};
	// Vertical-gradient quad: cT along the top edge, cB along the bottom.
	auto quadV = [&](float x0, float y0, float x1, float y1, const float cT[4],
	                 const float cB[4]) {
		pt(x0, y0, cT); pt(x1, y0, cT); pt(x1, y1, cB);
		pt(x0, y0, cT); pt(x1, y1, cB); pt(x0, y1, cB);
	};
	// Annulus arc [a0,a1] whose inner edge carries cIn and whose outer edge
	// fades to alpha 0 — the per-vertex-colour trick that buys soft shadows
	// and halos without textures. Round in pixels like disc().
	auto softRing = [&](float cx, float cy, float ry0, float ry1, float a0, float a1,
	                    const float cIn[4]) {
		const float cOut[4] = {cIn[0], cIn[1], cIn[2], 0.0f};
		const int N = 18;
		float pc = cosf(a0), ps = sinf(a0);
		for (int i = 1; i <= N; ++i) {
			const float a = a0 + (a1 - a0) * (float)i / N;
			const float nc = cosf(a), ns = sinf(a);
			pt(cx + ry0 * aspect * pc, cy + ry0 * ps, cIn);
			pt(cx + ry1 * aspect * pc, cy + ry1 * ps, cOut);
			pt(cx + ry1 * aspect * nc, cy + ry1 * ns, cOut);
			pt(cx + ry0 * aspect * pc, cy + ry0 * ps, cIn);
			pt(cx + ry1 * aspect * nc, cy + ry1 * ns, cOut);
			pt(cx + ry0 * aspect * nc, cy + ry0 * ns, cIn);
			pc = nc; ps = ns;
		}
	};

	// Palette: one cyan accent family, neutral darks/whites everywhere else.
	// Icon/text/glyph colours keep alpha 1.0 and dim via RGB instead, so
	// overlapping quads (7-seg corners, folder tab/body seam) don't
	// double-blend into hot or dark spots.
	const float colPanel[4] = {0.04f, 0.05f, 0.07f, 0.62f};
	const float colShadow[4] = {0.0f, 0.0f, 0.0f, 0.25f};
	const float colShadow0[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	const float colEdge[4] = {1.0f, 1.0f, 1.0f, 0.07f};
	const float colEdge0[4] = {1.0f, 1.0f, 1.0f, 0.0f};
	const float colTrack[4] = {0.78f, 0.80f, 0.86f, 0.32f};
	const float colFill[4] = {0.18f, 0.80f, 0.88f, 1.0f};
	const float colHalo[4] = {0.18f, 0.80f, 0.88f, 0.35f};
	const float colKnob[4] = {0.97f, 0.98f, 1.0f, 1.0f};
	const float colIcon[4] = {0.93f, 0.95f, 0.98f, 1.0f};
	const float colText[4] = {0.78f, 0.80f, 0.85f, 1.0f};
	const float colGlyph[4] = {0.03f, 0.04f, 0.06f, 1.0f};  // near-black on the accent pill

	const float kPi2 = 6.2831853f;
	const float midY = (tui::kRowY0 + tui::kRowY1) * 0.5f;

	// Panel: slim translucent pill spanning exactly the interactive band
	// (kRowY0..kRowY1, so the drawn slab IS the hit band). Soft drop shadow
	// from gradient bands along the straight edges + outward-facing soft half
	// rings at the caps (they abut the bands seamlessly at the cap tangents),
	// then a faint top-edge highlight for a hint of depth.
	const float pY0 = tui::kRowY0, pY1 = tui::kRowY1;
	const float pr = (pY1 - pY0) * 0.5f;  // pill end radius = half height
	const float pX0 = 0.012f, pX1 = 0.988f;
	const float capL = pX0 + pr * aspect, capR = pX1 - pr * aspect;
	const float sh = 0.014f;  // shadow spread
	quadV(capL, pY0 - sh, capR, pY0, colShadow0, colShadow);
	quadV(capL, pY1, capR, pY1 + sh, colShadow, colShadow0);
	softRing(capL, midY, pr, pr + sh, kPi2 * 0.25f, kPi2 * 0.75f, colShadow);
	softRing(capR, midY, pr, pr + sh, -kPi2 * 0.25f, kPi2 * 0.25f, colShadow);
	quad(capL, pY0, capR, pY1, colPanel);
	disc(capL, midY, pr, colPanel);
	disc(capR, midY, pr, colPanel);
	quadV(capL, pY0 + 0.0015f, capR, pY0 + 0.0065f, colEdge, colEdge0);

	// Play / pause. The play triangle gets a small rightward optical offset —
	// a bounding-box-centred right-pointing triangle reads left-of-centre.
	const float bcx = (tui::kBtnX0 + tui::kBtnX1) * 0.5f;
	const float bh = 0.022f;  // glyph half-height
	if (ovPaused_) {  // PLAY: right-pointing triangle, ~equilateral in pixels
		const float tw = 2.0f * bh * 0.92f * aspect;
		const float ox = tw * 0.07f;  // optical centring
		tri(bcx - tw * 0.5f + ox, midY - bh, bcx - tw * 0.5f + ox, midY + bh,
		    bcx + tw * 0.5f + ox, midY, colIcon);
	} else {  // PAUSE: two rounded bars
		const float gw = 2.0f * bh * 0.80f * aspect;  // glyph width
		const float bw = gw * 0.32f;                  // bar width
		const float br = bw / aspect * 0.5f;          // cap radius (y units)
		const float xs[2] = {bcx - gw * 0.5f, bcx + gw * 0.5f - bw};
		for (float x0 : xs) {
			quad(x0, midY - bh + br, x0 + bw, midY + bh - br, colIcon);
			disc(x0 + bw * 0.5f, midY - bh + br, br, colIcon);
			disc(x0 + bw * 0.5f, midY + bh - br, br, colIcon);
		}
	}

	// Scrub track: thin neutral line; accent fill with a rounded leading end;
	// modest knob over a soft accent halo. Track thickness comes from
	// transport_ui so drawing and grab zone move together.
	const float trkH = (tui::kBarY1 - tui::kBarY0) * 0.5f;  // half-thickness
	const float bx0 = tui::kBarX0, bx1 = tui::kBarX1;
	quad(bx0, midY - trkH, bx1, midY + trkH, colTrack);
	disc(bx0, midY, trkH, colTrack);
	disc(bx1, midY, trkH, colTrack);
	const float fillX = bx0 + (bx1 - bx0) * ovProgress_;
	quad(bx0, midY - trkH, fillX, midY + trkH, colFill);
	disc(bx0, midY, trkH, colFill);
	disc(fillX, midY, trkH, colFill);
	const float kr = 0.014f;  // knob radius
	softRing(fillX, midY, kr, kr * 2.0f, 0.0f, kPi2, colHalo);
	disc(fillX, midY, kr, colKnob);

	// Load button: accent pill with a dark "open file" glyph — kept loud on
	// purpose (#15: a dim outline was invisible under the weave; it also
	// collided with a left-anchored total-time readout), but slimmed to match
	// the new proportions.
	const float lx0 = tui::kLoadX0, lx1 = tui::kLoadX1;
	{
		const float ph = 0.026f;  // pill half-height = end-cap radius
		const float cx0 = lx0 + ph * aspect, cx1 = lx1 - ph * aspect;
		quad(cx0, midY - ph, cx1, midY + ph, colFill);
		disc(cx0, midY, ph, colFill);
		disc(cx1, midY, ph, colFill);

		// Upload/open mark: a horizontal tray with rounded ends + an upward
		// arrow (triangle head, rounded stem) floating above it with a clear
		// notch of negative space. All strokes are matched in PIXELS (tray
		// thickness == stem width == 2*gr px) and sit above the ~0.004 weave
		// shimmer floor; the gap is likewise wide enough not to fuse. The
		// composite is vertically symmetric about midY (apex gh up, tray
		// bottom gh down), so it centres optically without a fudge.
		const float gcx = (lx0 + lx1) * 0.5f;
		const float gh = 0.015f;   // glyph half-extent (pill ph=0.026 → ~21% inset)
		const float gr = 0.003f;   // stroke half-thickness (0.006 ≈ 9.6 px @1600)
		// Tray (bottom): rounded-end bar, top edge at midY + gh - 2*gr.
		const float trayCy = midY + gh - gr;       // tray centreline
		const float tw2 = 0.013f * aspect;         // cap-centre half-span
		quad(gcx - tw2, trayCy - gr, gcx + tw2, trayCy + gr, colGlyph);
		disc(gcx - tw2, trayCy, gr, colGlyph);
		disc(gcx + tw2, trayCy, gr, colGlyph);
		// Arrow head: up-pointing triangle, apex at the glyph top.
		const float hh = 0.011f;            // head height
		const float hw = 0.009f * aspect;   // head half-width (~2.2x stem width)
		const float headB = midY - gh + hh; // head base
		tri(gcx, midY - gh, gcx - hw, headB, gcx + hw, headB, colGlyph);
		// Arrow stem: pixel-square width, tucked 0.0005 under the head base
		// (same-colour alpha-1 overlap, no seam), rounded bottom cap held a
		// 0.004 (≈6.4 px) notch above the tray.
		const float sw = gr * aspect;             // stem half-width (px == tray)
		const float stemCapCy = (trayCy - gr) - 0.004f - gr;
		quad(gcx - sw, headB - 0.0005f, gcx + sw, stemCapCy, colGlyph);
		disc(gcx, stemCapCy, gr, colGlyph);
	}

	// Seven-segment time readout. dh/dw are per-digit cell size; segments are
	// thin quads whose horizontal (ty) and vertical (tx) strokes are matched
	// in PIXELS, so digits read evenly. Dimmed via RGB (colText) so they don't
	// compete with the accent. Colon is two square dots.
	auto digit = [&](float x0, float topY, float dw, float dh, int d) {
		if (d < 0 || d > 9) return;
		const uint8_t m = kSeg7[d];
		const float ty = dh * 0.18f, tx = ty * aspect;  // stroke, px-matched
		const float x1 = x0 + dw, y0 = topY, y1 = topY + dh, my = topY + dh * 0.5f;
		if (m & 0x01) quad(x0, y0, x1, y0 + ty, colText);              // a top
		if (m & 0x40) quad(x0, my - ty * 0.5f, x1, my + ty * 0.5f, colText);  // g mid
		if (m & 0x08) quad(x0, y1 - ty, x1, y1, colText);             // d bottom
		if (m & 0x20) quad(x0, y0, x0 + tx, my, colText);             // f top-left
		if (m & 0x02) quad(x1 - tx, y0, x1, my, colText);             // b top-right
		if (m & 0x10) quad(x0, my, x0 + tx, y1, colText);             // e bottom-left
		if (m & 0x04) quad(x1 - tx, my, x1, y1, colText);             // c bottom-right
	};
	auto drawTime = [&](const char *s, float startX) {
		const float dh = 0.028f, dw = dh * aspect * 0.58f;
		const float advD = dw * 1.42f, advC = dw * 0.60f;
		const float topY = midY - dh * 0.5f;
		float x = startX;
		for (const char *p = s; *p; ++p) {
			if (*p >= '0' && *p <= '9') {
				digit(x, topY, dw, dh, *p - '0');
				x += advD;
			} else if (*p == ':') {
				const float doth = dh * 0.18f, dotw = doth * aspect;  // = seg stroke
				const float cx = x + advC * 0.5f - dotw * 0.5f;
				quad(cx, topY + dh * 0.26f, cx + dotw, topY + dh * 0.26f + doth, colText);
				quad(cx, topY + dh * 0.60f, cx + dotw, topY + dh * 0.60f + doth, colText);
				x += advC;
			}
		}
	};
	// Total time is RIGHT-aligned to end just left of the Load button: a
	// left-anchored total overran the button zone for hour-long content (the
	// bright digits buried the Load glyph, #15).
	auto timeWidth = [&](const char *s) {
		const float dh = 0.028f, dw = dh * aspect * 0.58f;
		const float advD = dw * 1.42f, advC = dw * 0.60f;
		float wsum = 0.0f;
		for (const char *p = s; *p; ++p) {
			if (*p >= '0' && *p <= '9') wsum += advD;
			else if (*p == ':') wsum += advC;
		}
		return wsum;
	};
	drawTime(ovLeft_, tui::kElapsedX);
	drawTime(ovRight_, tui::kLoadX0 - 0.015f - timeWidth(ovRight_));

	if (v.empty() || v.size() > ovVboCapVerts_) return;
	std::memcpy(ovVboMapped_, v.data(), v.size() * sizeof(OvVert));
	VkDeviceSize off = 0;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ovPipeline_);
	vkCmdBindVertexBuffers(cmd, 0, 1, &ovVbo_, &off);
	vkCmdDraw(cmd, (uint32_t)v.size(), 1, 0, 0);
}

// ── Zero-copy AHB video path ──────────────────────────────────────────────
// Build the per-stream ycbcr conversion + immutable sampler + descriptor layout
// + pipeline from the first frame's external format. All frames of a stream
// share one external format, so this runs once.
bool
SbsRenderer::ensureAhbPipeline(const VkAndroidHardwareBufferFormatPropertiesANDROID &fmt)
{
	if (ahbInited_) return true;
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

	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
	VkDescriptorPoolCreateInfo dpci = {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	if (vkCreateDescriptorPool(device_, &dpci, nullptr, &ahbDescPool_) != VK_SUCCESS) {
		LOGE("ahb desc pool failed");
		return false;
	}
	VkDescriptorSetAllocateInfo dsai = {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = ahbDescPool_;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &ahbSetLayout_;
	if (vkAllocateDescriptorSets(device_, &dsai, &ahbDescSet_) != VK_SUCCESS) {
		LOGE("ahb desc set alloc failed");
		return false;
	}
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

	if (ahbCacheCount_ == kAhbCacheCap) {  // evict oldest (shouldn't hit: cache > pool depth)
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
	sourceMode_ = 3;
	sourceFullRange_ = 1.0f;  // unused in mode 3 (the ycbcr conversion owns range)
	return true;
}

void
SbsRenderer::drawAtlas(VkImage image, uint32_t atlasW, uint32_t atlasH, uint32_t renderW,
                       uint32_t renderH, uint32_t cols, uint32_t rows, uint32_t viewCount,
                       float contentAspect, bool mono, const float clearRgb[3])
{
	const bool useAhb = (sourceMode_ == 3 && ahbActiveView_ != VK_NULL_HANDLE);
	if (!useAhb && planes_[0].view == VK_NULL_HANDLE) return;
	VkPipeline pipe = useAhb ? ahbPipeline_ : pipeline_;
	VkPipelineLayout pl = useAhb ? ahbPipeLayout_ : pipeLayout_;
	VkDescriptorSet ds = useAhb ? ahbDescSet_ : descSet_;
	const Target &t = targetFor(image, atlasW, atlasH);
	const uint32_t c = cols ? cols : 1;

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
		bar.image = ahbActiveImage_;
		bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &bar);
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
		const float boxW = mono ? 1.0f : 1.0f / (float)c;
		const float offBase = mono ? 0.0f : (float)tile_x / (float)c;

		// Content quad, scissor-clipped to the tile.
		VkViewport vp = {vx, vy, cqW, cqH, 0.0f, 1.0f};
		VkRect2D sc = {{(int32_t)tx, (int32_t)ty}, {renderW, renderH}};
		vkCmdSetViewport(cmd_, 0, 1, &vp);
		vkCmdSetScissor(cmd_, 0, 1, &sc);
		vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, nullptr);
		SbsPush push = {};
		push.uvOffset[0] = offBase;
		push.uvOffset[1] = 0.0f;
		push.uvScale[0] = boxW;
		push.uvScale[1] = 1.0f;
		push.mode = sourceMode_;
		push.fullRange = sourceFullRange_;
		vkCmdPushConstants(cmd_, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(cmd_, 3, 1, 0, 0);

		// Transport overlay fills this tile (screen-fixed within the eye tile).
		VkViewport ovp = {tx, ty, (float)renderW, (float)renderH, 0.0f, 1.0f};
		vkCmdSetViewport(cmd_, 0, 1, &ovp);
		drawOverlay(cmd_, renderW, renderH);  // re-binds its own pipeline
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
SbsRenderer::cleanup()
{
	if (device_ == VK_NULL_HANDLE) return;
	vkDeviceWaitIdle(device_);
	// Zero-copy AHB resources.
	for (int i = 0; i < ahbCacheCount_; ++i) destroyAhbImport(ahbCache_[i]);
	ahbCacheCount_ = 0;
	ahbActiveImage_ = VK_NULL_HANDLE;
	ahbActiveView_ = VK_NULL_HANDLE;
	if (ahbPipeline_) vkDestroyPipeline(device_, ahbPipeline_, nullptr);
	if (ahbPipeLayout_) vkDestroyPipelineLayout(device_, ahbPipeLayout_, nullptr);
	if (ahbDescPool_) vkDestroyDescriptorPool(device_, ahbDescPool_, nullptr);
	if (ahbSetLayout_) vkDestroyDescriptorSetLayout(device_, ahbSetLayout_, nullptr);
	if (ahbSampler_) vkDestroySampler(device_, ahbSampler_, nullptr);
	if (ahbYcbcr_) vkDestroySamplerYcbcrConversion(device_, ahbYcbcr_, nullptr);
	ahbPipeline_ = VK_NULL_HANDLE;
	ahbPipeLayout_ = VK_NULL_HANDLE;
	ahbDescPool_ = VK_NULL_HANDLE;
	ahbSetLayout_ = VK_NULL_HANDLE;
	ahbSampler_ = VK_NULL_HANDLE;
	ahbYcbcr_ = VK_NULL_HANDLE;
	ahbInited_ = false;
	for (auto &kv : targets_) {
		if (kv.second.fb) vkDestroyFramebuffer(device_, kv.second.fb, nullptr);
		if (kv.second.view) vkDestroyImageView(device_, kv.second.view, nullptr);
	}
	targets_.clear();
	for (auto &p : planes_) destroyPlane(p);
	if (ovVboMapped_) vkUnmapMemory(device_, ovVboMem_);
	if (ovVbo_) vkDestroyBuffer(device_, ovVbo_, nullptr);
	if (ovVboMem_) vkFreeMemory(device_, ovVboMem_, nullptr);
	if (ovPipeline_) vkDestroyPipeline(device_, ovPipeline_, nullptr);
	if (ovPipeLayout_) vkDestroyPipelineLayout(device_, ovPipeLayout_, nullptr);
	ovVboMapped_ = nullptr;
	ovVbo_ = VK_NULL_HANDLE;
	ovVboMem_ = VK_NULL_HANDLE;
	ovPipeline_ = VK_NULL_HANDLE;
	ovPipeLayout_ = VK_NULL_HANDLE;
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
