// SPDX-License-Identifier: Apache-2.0
#if defined(_WIN32)
// Zero-copy interop (#28) needs the Win32 Vulkan structs (import + keyed mutex). Define
// the platform before <vulkan/vulkan.h> (pulled in by the header) so it exposes them;
// windows.h must precede vulkan_win32.h, and NOMINMAX keeps its min/max macros away from
// std::max/std::min used below.
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "VulkanRenderer.h"

#include "Log.h"
#include "rhi/Shaders.h"

#include <algorithm>  // std::max / std::min (scissor clamp)

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {
VkShaderModule MakeShaderModule(VkDevice device, const uint32_t* spv, size_t byteSize) {
    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = byteSize;
    ci.pCode = spv;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}
} // namespace

namespace mp {

VulkanRenderer::~VulkanRenderer() { Shutdown(); }

bool VulkanRenderer::Initialize(VkPhysicalDevice physicalDevice, VkDevice device,
                                VkQueue graphicsQueue, uint32_t queueFamilyIndex,
                                VkFormat format, uint32_t width, uint32_t height,
                                const std::vector<VkImage>& images) {
    physicalDevice_ = physicalDevice;
    device_ = device;
    queue_ = graphicsQueue;
    format_ = format;
    width_ = width;
    height_ = height;
    images_ = images;

    // Color-only render pass. LOAD_OP_CLEAR fills the whole SBS image with the left
    // color; we then clear the right half separately inside the pass. finalLayout
    // is COLOR_ATTACHMENT_OPTIMAL, the layout the OpenXR Vulkan binding requires the
    // image to be left in at release time.
    VkAttachmentDescription color = {};
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rp = {};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    if (vkCreateRenderPass(device_, &rp, nullptr, &renderPass_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateRenderPass failed");
        return false;
    }

    // One image view + framebuffer per swapchain image.
    imageViews_.resize(images.size(), VK_NULL_HANDLE);
    framebuffers_.resize(images.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < images.size(); ++i) {
        VkImageViewCreateInfo iv = {};
        iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image = images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = format_;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.levelCount = 1;
        iv.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &iv, nullptr, &imageViews_[i]) != VK_SUCCESS) {
            LOG_ERROR("vkCreateImageView failed (image %zu)", i);
            return false;
        }

        VkFramebufferCreateInfo fb = {};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = renderPass_;
        fb.attachmentCount = 1;
        fb.pAttachments = &imageViews_[i];
        fb.width = width_;
        fb.height = height_;
        fb.layers = 1;
        if (vkCreateFramebuffer(device_, &fb, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            LOG_ERROR("vkCreateFramebuffer failed (image %zu)", i);
            return false;
        }
    }

    VkCommandPoolCreateInfo cp = {};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = queueFamilyIndex;
    if (vkCreateCommandPool(device_, &cp, nullptr, &commandPool_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = commandPool_;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &cb, &commandBuffer_) != VK_SUCCESS) {
        LOG_ERROR("vkAllocateCommandBuffers failed");
        return false;
    }

    VkFenceCreateInfo fc = {};
    fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &fc, nullptr, &fence_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateFence failed");
        return false;
    }

    if (!CreatePipeline()) return false;

    LOG_INFO("VulkanRenderer ready (%u framebuffers, %ux%u)",
             (uint32_t)framebuffers_.size(), width_, height_);
    return true;
}

uint32_t VulkanRenderer::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanRenderer::CreatePipeline() {
    // Descriptor set layout: bindings 0..2 = combined image samplers (RGBA in plane 0,
    // or Y + chroma planes for YUV video).
    VkDescriptorSetLayoutBinding bindings[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci = {};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 3;
    dslci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &descSetLayout_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorSetLayout failed");
        return false;
    }

    // Push constants: uvOffset(vec2)+uvScale(vec2)+mode(int)+fullRange(float) = 24 bytes.
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(float) * 6;

    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &descSetLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("vkCreatePipelineLayout failed");
        return false;
    }

    VkShaderModule vert = MakeShaderModule(device_, g_fullscreenVertSpv, sizeof(g_fullscreenVertSpv));
    VkShaderModule frag = MakeShaderModule(device_, g_sbsFragSpv, sizeof(g_sbsFragSpv));
    if (!vert || !frag) {
        LOG_ERROR("shader module creation failed");
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

    // No vertex input — the fullscreen triangle is generated from gl_VertexIndex.
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;  // both dynamic

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
    cba.blendEnable = VK_FALSE;

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
    gp.layout = pipelineLayout_;
    gp.renderPass = renderPass_;
    gp.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    if (r != VK_SUCCESS) {
        LOG_ERROR("vkCreateGraphicsPipelines failed: %d", (int)r);
        return false;
    }

    // Sampler: linear, clamp-to-edge (avoids wrap bleed across the SBS seam at 0.5).
    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateSampler failed");
        return false;
    }

    // Descriptor pool + a single set (3 samplers, rebound in UploadTexture/UploadYUV).
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 3;
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorPool failed");
        return false;
    }
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descPool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &descSetLayout_;
    if (vkAllocateDescriptorSets(device_, &dsai, &descSet_) != VK_SUCCESS) {
        LOG_ERROR("vkAllocateDescriptorSets failed");
        return false;
    }
    return true;
}

bool VulkanRenderer::ClearViews(uint32_t imageIndex, const ClearColor* colors,
                                const XrSession::ViewRect* rects, uint32_t viewCount) {
    if (imageIndex >= framebuffers_.size()) {
        LOG_ERROR("ClearViews: imageIndex %u out of range", imageIndex);
        return false;
    }
    if (viewCount == 0) return false;

    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &bi);

    // LOAD_OP_CLEAR fills the whole image with view 0's color; any area not covered
    // by a tile keeps it.
    VkClearValue clear = {};
    clear.color = {{colors[0].r, colors[0].g, colors[0].b, colors[0].a}};

    VkRenderPassBeginInfo rb = {};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = renderPass_;
    rb.framebuffer = framebuffers_[imageIndex];
    rb.renderArea.offset = {0, 0};
    rb.renderArea.extent = {width_, height_};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer_, &rb, VK_SUBPASS_CONTENTS_INLINE);

    // Paint each view's tile (the exact rect submitted to the runtime) with its color.
    for (uint32_t v = 0; v < viewCount; ++v) {
        VkClearAttachment att = {};
        att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att.colorAttachment = 0;
        att.clearValue.color = {{colors[v].r, colors[v].g, colors[v].b, colors[v].a}};
        VkClearRect rect = {};
        rect.rect.offset = {rects[v].x, rects[v].y};
        rect.rect.extent = {rects[v].w, rects[v].h};
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        vkCmdClearAttachments(commandBuffer_, 1, &att, 1, &rect);
    }

    vkCmdEndRenderPass(commandBuffer_);
    vkEndCommandBuffer(commandBuffer_);

    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
    if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) {
        LOG_ERROR("vkQueueSubmit failed");
        return false;
    }
    // Block until done — the runtime reads the image immediately after release.
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    return true;
}

bool VulkanRenderer::RecordPlaneUpload(VkCommandBuffer cmd, int idx, const uint8_t* data,
        uint32_t w, uint32_t h, VkFormat fmt, uint32_t bytesPerTexel, bool& recreated,
        std::vector<std::pair<VkBuffer, VkDeviceMemory>>& staging) {
    Plane& p = planes_[idx];
    const VkDeviceSize bytes = (VkDeviceSize)w * h * bytesPerTexel;

    recreated = (p.image == VK_NULL_HANDLE || p.w != w || p.h != h || p.fmt != fmt);
    if (recreated) {
        if (p.view) { vkDestroyImageView(device_, p.view, nullptr); p.view = VK_NULL_HANDLE; }
        if (p.image) { vkDestroyImage(device_, p.image, nullptr); p.image = VK_NULL_HANDLE; }
        if (p.memory) { vkFreeMemory(device_, p.memory, nullptr); p.memory = VK_NULL_HANDLE; }

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
        if (vkCreateImage(device_, &ici, nullptr, &p.image) != VK_SUCCESS) {
            LOG_ERROR("vkCreateImage (plane %d) failed", idx);
            return false;
        }
        VkMemoryRequirements ireq;
        vkGetImageMemoryRequirements(device_, p.image, &ireq);
        VkMemoryAllocateInfo iai = {};
        iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        iai.allocationSize = ireq.size;
        iai.memoryTypeIndex = FindMemoryType(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (iai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(device_, &iai, nullptr, &p.memory) != VK_SUCCESS) {
            LOG_ERROR("plane memory alloc failed");
            return false;
        }
        vkBindImageMemory(device_, p.image, p.memory, 0);
        p.w = w;
        p.h = h;
        p.fmt = fmt;
        p.initialized = false;

        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = p.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &vci, nullptr, &p.view) != VK_SUCCESS) {
            LOG_ERROR("plane image view failed");
            return false;
        }
    }

    // Host-visible staging buffer with the plane's pixels (freed by the caller post-submit).
    VkBuffer sbuf = VK_NULL_HANDLE;
    VkDeviceMemory smem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device_, &bci, nullptr, &sbuf);
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(device_, sbuf, &breq);
    VkMemoryAllocateInfo bai = {};
    bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = breq.size;
    bai.memoryTypeIndex = FindMemoryType(breq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &bai, nullptr, &smem);
    vkBindBufferMemory(device_, sbuf, smem, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, smem, 0, bytes, 0, &mapped);
    memcpy(mapped, data, (size_t)bytes);
    vkUnmapMemory(device_, smem);
    staging.emplace_back(sbuf, smem);

    VkImageMemoryBarrier toDst = {};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = p.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = p.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = p.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags srcStage = p.initialized
        ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toDst);

    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, sbuf, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);
    p.initialized = true;
    return true;
}

// Record N plane uploads into one command buffer, submit once, wait, free staging. Blocks
// (the runtime samples the result right after) — one GPU round-trip per frame, not per plane.
namespace {
struct PlaneSpec { const uint8_t* data; uint32_t w, h; VkFormat fmt; uint32_t bpt; };
}

bool VulkanRenderer::UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) return false;
#if defined(_WIN32)
    sharedActive_ = false;  // CPU RGBA source -> leave the zero-copy path
#endif
    if (!EnsureDummy()) return false;

    std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging;
    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &cbi);
    bool rec = false;
    const bool ok = RecordPlaneUpload(commandBuffer_, 0, rgba, width, height,
                                      VK_FORMAT_R8G8B8A8_UNORM, 4, rec, staging);
    vkEndCommandBuffer(commandBuffer_);
    if (ok) {
        vkResetFences(device_, 1, &fence_);
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &commandBuffer_;
        vkQueueSubmit(queue_, 1, &si, fence_);
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    }
    for (auto& s : staging) {
        vkDestroyBuffer(device_, s.first, nullptr);
        vkFreeMemory(device_, s.second, nullptr);
    }
    if (!ok) return false;

    const bool modeChanged = (sourceMode_ != 0);
    sourceMode_ = 0;
    sourceFullRange_ = 0.0f;
    if (rec || modeChanged) {
        BindDescriptors();
        if (rec) LOG_INFO("VulkanRenderer: RGBA texture (re)created %ux%u", width, height);
    }
    return true;
}

bool VulkanRenderer::UploadYUV(const uint8_t* plane0, const uint8_t* plane1, const uint8_t* plane2,
                               uint32_t width, uint32_t height, int format, bool fullRange) {
    if (!plane0 || !plane1 || width == 0 || height == 0) return false;
#if defined(_WIN32)
    sharedActive_ = false;  // CPU-plane upload -> leave the zero-copy path
#endif
    if (format == 0 && !plane2) return false;
    if (!EnsureDummy()) return false;
    const uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;

    // 0 = I420 (Y, U, V); 1 = NV12 (Y, interleaved UV).
    PlaneSpec specs[3];
    int count;
    int mode;
    specs[0] = {plane0, width, height, VK_FORMAT_R8_UNORM, 1};
    if (format == 1) {
        specs[1] = {plane1, cw, ch, VK_FORMAT_R8G8_UNORM, 2};
        count = 2;
        mode = 2;
    } else {
        specs[1] = {plane1, cw, ch, VK_FORMAT_R8_UNORM, 1};
        specs[2] = {plane2, cw, ch, VK_FORMAT_R8_UNORM, 1};
        count = 3;
        mode = 1;
    }

    std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging;
    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &cbi);
    bool rec[3] = {false, false, false};
    bool ok = true;
    for (int i = 0; i < count && ok; ++i) {
        ok = RecordPlaneUpload(commandBuffer_, i, specs[i].data, specs[i].w, specs[i].h,
                               specs[i].fmt, specs[i].bpt, rec[i], staging);
    }
    vkEndCommandBuffer(commandBuffer_);
    if (ok) {
        vkResetFences(device_, 1, &fence_);
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &commandBuffer_;
        vkQueueSubmit(queue_, 1, &si, fence_);
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    }
    for (auto& s : staging) {
        vkDestroyBuffer(device_, s.first, nullptr);
        vkFreeMemory(device_, s.second, nullptr);
    }
    if (!ok) return false;

    const bool modeChanged = (sourceMode_ != mode);
    sourceMode_ = mode;
    sourceFullRange_ = fullRange ? 1.0f : 0.0f;
    if (rec[0] || rec[1] || rec[2] || modeChanged) {
        BindDescriptors();
        if (rec[0]) LOG_INFO("VulkanRenderer: YUV source %ux%u mode=%d", width, height, mode);
    }
    return true;
}

bool VulkanRenderer::EnsureDummy() {
    if (dummyView_ != VK_NULL_HANDLE) return true;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8_UNORM;
    ici.extent = {1, 1, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &dummyImage_) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, dummyImage_, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &dummyMemory_) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, dummyImage_, dummyMemory_, 0);

    // Transition UNDEFINED -> SHADER_READ_ONLY (content unused: the shader never samples
    // a dummy-bound plane, but the descriptor still needs a valid view in a read layout).
    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &cbi);
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.image = dummyImage_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(commandBuffer_);
    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
    vkQueueSubmit(queue_, 1, &si, fence_);
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);

    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = dummyImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device_, &vci, nullptr, &dummyView_) == VK_SUCCESS;
}

void VulkanRenderer::BindDescriptors() {
    // Bind the planes the current mode samples; everything else gets the dummy.
    VkImageView views[3] = {planes_[0].view, dummyView_, dummyView_};
    VkImageLayout layouts[3] = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
#if defined(_WIN32)
    if (sharedActive_) {  // zero-copy: imported NV12 plane views, sampled in GENERAL layout
        views[0] = sharedY_;
        views[1] = sharedUV_;
        views[2] = dummyView_;
        layouts[0] = VK_IMAGE_LAYOUT_GENERAL;
        layouts[1] = VK_IMAGE_LAYOUT_GENERAL;
    } else
#endif
    if (sourceMode_ == 1) {            // I420: Y, U, V
        views[1] = planes_[1].view;
        views[2] = planes_[2].view;
    } else if (sourceMode_ == 2) {     // NV12: Y, interleaved UV
        views[1] = planes_[1].view;
    }
    VkDescriptorImageInfo dii[3] = {};
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        dii[i].sampler = sampler_;
        dii[i].imageView = views[i] ? views[i] : dummyView_;
        dii[i].imageLayout = layouts[i];
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &dii[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
}

bool VulkanRenderer::DrawViews(uint32_t imageIndex, const XrSession::ViewRect* rects,
                               const XrSession::ViewRect* clipRects,
                               const ViewUV* uvs, uint32_t viewCount) {
    bool haveSource = planes_[0].view != VK_NULL_HANDLE;
#if defined(_WIN32)
    haveSource = haveSource || sharedActive_;
#endif
    if (imageIndex >= framebuffers_.size() || !haveSource) return false;

    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &bi);

#if defined(_WIN32)
    // Zero-copy (#28): the imported NV12 image starts in UNDEFINED; move it to GENERAL once
    // (its contents live in shared memory the D3D11 producer writes, so we never transition
    // it again — the keyed mutex below orders that write against this sample).
    if (sharedActive_ && sharedLayoutReady_ && !*sharedLayoutReady_ && sharedImage_) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = sharedImage_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &b);
        *sharedLayoutReady_ = true;
    }
#endif

    VkClearValue clear = {};
    // Letterbox/background: the configured fill (opaque black by default, dark grey for
    // the idle logo); alpha 0 in transparent-bg mode so the runtime composes those
    // pixels through to the desktop.
    clear.color = {{background_.r, background_.g, background_.b,
                    transparentLetterbox_ ? 0.0f : background_.a}};
    VkRenderPassBeginInfo rb = {};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = renderPass_;
    rb.framebuffer = framebuffers_[imageIndex];
    rb.renderArea.offset = {0, 0};
    rb.renderArea.extent = {width_, height_};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer_, &rb, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 1, &descSet_, 0, nullptr);

    for (uint32_t v = 0; v < viewCount; ++v) {
        VkViewport vp = {};
        vp.x = (float)rects[v].x;
        vp.y = (float)rects[v].y;
        vp.width = (float)rects[v].w;
        vp.height = (float)rects[v].h;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer_, 0, 1, &vp);

        // Scissor to the eye's tile (intersected with the content rect). The content
        // rect may be convergence-shifted past the tile edge; clipping here truncates
        // the overhang so it can't bleed into the adjacent eye's tile.
        const XrSession::ViewRect& clip = clipRects[v];
        const int32_t x0 = std::max(rects[v].x, clip.x);
        const int32_t y0 = std::max(rects[v].y, clip.y);
        const int32_t x1 = std::min(rects[v].x + (int32_t)rects[v].w, clip.x + (int32_t)clip.w);
        const int32_t y1 = std::min(rects[v].y + (int32_t)rects[v].h, clip.y + (int32_t)clip.h);
        VkRect2D sc = {};
        sc.offset = {x0, y0};
        sc.extent = {(uint32_t)std::max(0, x1 - x0), (uint32_t)std::max(0, y1 - y0)};
        vkCmdSetScissor(commandBuffer_, 0, 1, &sc);

        // uvOffset(vec2) + uvScale(vec2) + mode(int) + fullRange(float) = 24 bytes.
        struct { float off[2]; float scale[2]; int32_t mode; float fullRange; } pc = {
            {uvs[v].offX, uvs[v].offY}, {uvs[v].scaleX, uvs[v].scaleY},
            sourceMode_, sourceFullRange_};
        vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(commandBuffer_, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer_);
    vkEndCommandBuffer(commandBuffer_);

    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
#if defined(_WIN32)
    // Zero-copy (#28): take the shared texture's keyed mutex (key 0) for this submit so the
    // GPU waits for the D3D11 producer's CopySubresourceRegion to finish before sampling,
    // and hands it back (key 0) when done. Same key both sides -> drop-safe (a published-but-
    // never-sampled frame leaves the mutex released, so the producer can re-acquire it).
    const uint64_t kmAcqKey = 1;      // producer released with key 1
    const uint64_t kmRelKey = 0;      // hand back to the producer with key 0
    const uint32_t kmTimeout = 2000;  // ms; a stuck producer shouldn't hang the render thread
    VkWin32KeyedMutexAcquireReleaseInfoKHR km = {
        VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
    // Keyed mutex disabled: the producer CPU-waits for the copy before publishing (coarse
    // sync), so no cross-API mutex is needed here. Kept behind a flag to re-enable for
    // experiments (MEDIAPLAYER_ZC_KM=1).
    static const bool kmOn = [] {
        const char* e = std::getenv("MEDIAPLAYER_ZC_KM");
        return e && *e && *e != '0';
    }();
    if (sharedActive_ && sharedMemory_ && kmOn) {
        km.acquireCount = 1;
        km.pAcquireSyncs = &sharedMemory_;
        km.pAcquireKeys = &kmAcqKey;
        km.pAcquireTimeouts = &kmTimeout;
        km.releaseCount = 1;
        km.pReleaseSyncs = &sharedMemory_;
        km.pReleaseKeys = &kmRelKey;
        si.pNext = &km;
    }
#endif
    if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) {
        LOG_ERROR("DrawViews vkQueueSubmit failed");
        return false;
    }
    // 2s cap (not infinite) so a wedged zero-copy import can't hang the render thread.
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, 2000000000ULL);
    return true;
}

#if defined(_WIN32)
VulkanRenderer::SharedImport* VulkanRenderer::ImportSharedNV12(void* handle, uint32_t w,
                                                               uint32_t h) {
    for (auto& kv : imports_) if (kv.first == handle) return &kv.second;  // already imported

    SharedImport imp;
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext;
    ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // needed for the per-plane R8/R8G8 views
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;  // == DXGI_FORMAT_NV12
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &imp.image) != VK_SUCCESS) {
        LOG_WARN("BindSharedNV12: vkCreateImage failed");
        return nullptr;
    }

    // Memory types compatible with this imported D3D11 handle, intersected with the image's.
    auto pfnHandleProps = (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(
        device_, "vkGetMemoryWin32HandlePropertiesKHR");
    uint32_t handleTypeBits = 0xFFFFFFFFu;
    if (pfnHandleProps) {
        VkMemoryWin32HandlePropertiesKHR hp = {
            VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (pfnHandleProps(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
                           (HANDLE)handle, &hp) == VK_SUCCESS)
            handleTypeBits = hp.memoryTypeBits;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, imp.image, &req);
    const uint32_t chosenType = FindMemoryType(req.memoryTypeBits & handleTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    LOG_INFO("zc-import: reqBits=0x%x handleBits=0x%x -> type=%u size=%llu", req.memoryTypeBits,
             handleTypeBits, chosenType, (unsigned long long)req.size);

    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = imp.image;  // imported D3D texture memory must be a dedicated allocation
    VkImportMemoryWin32HandleInfoKHR imp32 = {
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imp32.pNext = &ded;
    imp32.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    imp32.handle = (HANDLE)handle;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp32;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = chosenType;
    if (vkAllocateMemory(device_, &mai, nullptr, &imp.memory) != VK_SUCCESS) {
        LOG_WARN("BindSharedNV12: import vkAllocateMemory failed");
        vkDestroyImage(device_, imp.image, nullptr);
        return nullptr;
    }
    if (vkBindImageMemory(device_, imp.image, imp.memory, 0) != VK_SUCCESS) {
        LOG_WARN("BindSharedNV12: vkBindImageMemory failed");
        vkFreeMemory(device_, imp.memory, nullptr);
        vkDestroyImage(device_, imp.image, nullptr);
        return nullptr;
    }

    // Per-plane views feed the existing NV12 shader path (Y = R8, UV = R8G8).
    auto makeView = [&](VkFormat fmt, VkImageAspectFlagBits aspect, VkImageView* out) {
        VkImageViewCreateInfo v = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = imp.image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = fmt;
        v.subresourceRange = {(VkImageAspectFlags)aspect, 0, 1, 0, 1};
        return vkCreateImageView(device_, &v, nullptr, out) == VK_SUCCESS;
    };
    if (!makeView(VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_PLANE_0_BIT, &imp.planeY) ||
        !makeView(VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_PLANE_1_BIT, &imp.planeUV)) {
        LOG_WARN("BindSharedNV12: plane view create failed");
        if (imp.planeY) vkDestroyImageView(device_, imp.planeY, nullptr);
        vkFreeMemory(device_, imp.memory, nullptr);
        vkDestroyImage(device_, imp.image, nullptr);
        return nullptr;
    }
    imports_.emplace_back(handle, imp);
    LOG_INFO("VulkanRenderer: imported shared NV12 %ux%u (zero-copy #28)", w, h);
    return &imports_.back().second;
}

bool VulkanRenderer::BindSharedNV12(void* sharedHandle, uint32_t width, uint32_t height,
                                    bool fullRange) {
    if (!sharedHandle) return false;
    SharedImport* imp = ImportSharedNV12(sharedHandle, width, height);
    if (!imp) return false;
    if (planes_[0].view != VK_NULL_HANDLE) DestroyTexture();  // drop CPU planes; sample the import
    sharedActive_ = true;
    sharedImage_ = imp->image;
    sharedMemory_ = imp->memory;
    sharedLayoutReady_ = &imp->layoutReady;
    sharedY_ = imp->planeY;
    sharedUV_ = imp->planeUV;
    sourceMode_ = 2;  // NV12
    sourceFullRange_ = fullRange ? 1.0f : 0.0f;
    EnsureDummy();
    BindDescriptors();
    return true;
}

void VulkanRenderer::ClearSharedImports() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    for (auto& kv : imports_) {
        SharedImport& s = kv.second;
        if (s.planeUV) vkDestroyImageView(device_, s.planeUV, nullptr);
        if (s.planeY) vkDestroyImageView(device_, s.planeY, nullptr);
        if (s.image) vkDestroyImage(device_, s.image, nullptr);
        if (s.memory) vkFreeMemory(device_, s.memory, nullptr);
    }
    imports_.clear();
    sharedActive_ = false;
    sharedImage_ = VK_NULL_HANDLE;
    sharedMemory_ = VK_NULL_HANDLE;
    sharedLayoutReady_ = nullptr;
    sharedY_ = VK_NULL_HANDLE;
    sharedUV_ = VK_NULL_HANDLE;
}
#endif  // _WIN32

void VulkanRenderer::DestroyTexture() {
    for (Plane& p : planes_) {
        if (p.view) vkDestroyImageView(device_, p.view, nullptr);
        if (p.image) vkDestroyImage(device_, p.image, nullptr);
        if (p.memory) vkFreeMemory(device_, p.memory, nullptr);
        p = Plane{};
    }
    if (dummyView_) { vkDestroyImageView(device_, dummyView_, nullptr); dummyView_ = VK_NULL_HANDLE; }
    if (dummyImage_) { vkDestroyImage(device_, dummyImage_, nullptr); dummyImage_ = VK_NULL_HANDLE; }
    if (dummyMemory_) { vkFreeMemory(device_, dummyMemory_, nullptr); dummyMemory_ = VK_NULL_HANDLE; }
}

bool VulkanRenderer::UploadToSwapchainImage(VkImage image, const uint8_t* rgba,
                                            uint32_t width, uint32_t height) {
    if (image == VK_NULL_HANDLE || !rgba) return false;
    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device_, &bci, nullptr, &staging);
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(device_, staging, &breq);
    VkMemoryAllocateInfo bai = {};
    bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = breq.size;
    bai.memoryTypeIndex = FindMemoryType(breq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &bai, nullptr, &stagingMem);
    vkBindBufferMemory(device_, staging, stagingMem, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, stagingMem, 0, bytes, 0, &mapped);
    memcpy(mapped, rgba, (size_t)bytes);
    vkUnmapMemory(device_, stagingMem);

    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &cbi);

    // We overwrite the whole image, so UNDEFINED old layout is fine each frame.
    VkImageMemoryBarrier toDst = {};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(commandBuffer_, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // Leave it in COLOR_ATTACHMENT_OPTIMAL for the runtime at release time.
    VkImageMemoryBarrier toColor = toDst;
    toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toColor);
    vkEndCommandBuffer(commandBuffer_);

    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
    vkQueueSubmit(queue_, 1, &si, fence_);
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
    return true;
}

bool VulkanRenderer::DumpImage(uint32_t imageIndex, const char* path) {
    if (imageIndex >= images_.size()) return false;

    const VkDeviceSize bytes = (VkDeviceSize)width_ * height_ * 4;

    // Host-visible staging buffer to copy the image into.
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bci, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, buf, &memReq);
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    uint32_t memType = UINT32_MAX;
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want) {
            memType = i;
            break;
        }
    }
    if (memType == UINT32_MAX) { vkDestroyBuffer(device_, buf, nullptr); return false; }

    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = memType;
    if (vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buf, nullptr);
        return false;
    }
    vkBindBufferMemory(device_, buf, mem, 0);

    // Copy the image (left in COLOR_ATTACHMENT_OPTIMAL by ClearViews) into the buffer.
    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &cbi);

    VkImageMemoryBarrier toSrc = {};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.image = images_[imageIndex];
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(commandBuffer_, images_[imageIndex],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

    // Restore the layout the runtime expects at release time.
    VkImageMemoryBarrier toColor = toSrc;
    toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toColor);
    vkEndCommandBuffer(commandBuffer_);

    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
    vkQueueSubmit(queue_, 1, &si, fence_);
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);

    // Map + write PNG. Swizzle BGRA->RGBA when the swapchain format is BGRA.
    void* mapped = nullptr;
    vkMapMemory(device_, mem, 0, bytes, 0, &mapped);
    auto* px = static_cast<unsigned char*>(mapped);
    const bool isBGRA = (format_ == VK_FORMAT_B8G8R8A8_UNORM ||
                         format_ == VK_FORMAT_B8G8R8A8_SRGB);
    if (isBGRA) {
        for (VkDeviceSize i = 0; i < bytes; i += 4) {
            unsigned char t = px[i];
            px[i] = px[i + 2];
            px[i + 2] = t;
        }
    }
    const int ok = stbi_write_png(path, (int)width_, (int)height_, 4, px, (int)width_ * 4);
    vkUnmapMemory(device_, mem);

    vkFreeMemory(device_, mem, nullptr);
    vkDestroyBuffer(device_, buf, nullptr);

    if (ok) LOG_INFO("Wrote atlas dump -> %s (%ux%u)", path, width_, height_);
    else LOG_ERROR("stbi_write_png failed for %s", path);
    return ok != 0;
}

bool VulkanRenderer::DumpExternalImage(VkImage image, uint32_t width, uint32_t height,
                                       const char* path) {
    if (image == VK_NULL_HANDLE || width == 0 || height == 0) return false;
    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;

    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device_, &bci, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, buf, &memReq);
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    uint32_t memType = UINT32_MAX;
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want) { memType = i; break; }
    }
    if (memType == UINT32_MAX) { vkDestroyBuffer(device_, buf, nullptr); return false; }

    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = memType;
    if (vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buf, nullptr);
        return false;
    }
    vkBindBufferMemory(device_, buf, mem, 0);

    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &cbi);

    VkImageMemoryBarrier toSrc = {};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.image = image;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(commandBuffer_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);

    VkImageMemoryBarrier toColor = toSrc;
    toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toColor);
    vkEndCommandBuffer(commandBuffer_);

    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
    vkQueueSubmit(queue_, 1, &si, fence_);
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);

    void* mapped = nullptr;
    vkMapMemory(device_, mem, 0, bytes, 0, &mapped);  // HUD is R8G8B8A8 — no swizzle
    const int ok = stbi_write_png(path, (int)width, (int)height, 4, mapped, (int)width * 4);
    vkUnmapMemory(device_, mem);
    vkFreeMemory(device_, mem, nullptr);
    vkDestroyBuffer(device_, buf, nullptr);

    if (ok) LOG_INFO("Wrote HUD dump -> %s (%ux%u)", path, width, height);
    else LOG_ERROR("stbi_write_png failed for %s", path);
    return ok != 0;
}

void VulkanRenderer::Shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_);
#if defined(_WIN32)
    ClearSharedImports();  // zero-copy (#28) imported textures
#endif
    DestroyTexture();
    if (sampler_) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (descPool_) { vkDestroyDescriptorPool(device_, descPool_, nullptr); descPool_ = VK_NULL_HANDLE; }
    if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (descSetLayout_) { vkDestroyDescriptorSetLayout(device_, descSetLayout_, nullptr); descSetLayout_ = VK_NULL_HANDLE; }
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto iv : imageViews_) if (iv) vkDestroyImageView(device_, iv, nullptr);
    framebuffers_.clear();
    imageViews_.clear();
    if (fence_) { vkDestroyFence(device_, fence_, nullptr); fence_ = VK_NULL_HANDLE; }
    if (commandPool_) { vkDestroyCommandPool(device_, commandPool_, nullptr); commandPool_ = VK_NULL_HANDLE; }
    if (renderPass_) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    device_ = VK_NULL_HANDLE;
}

} // namespace mp
