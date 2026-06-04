// SPDX-License-Identifier: BSL-1.0
#include "VulkanRenderer.h"

#include "Log.h"
#include "rhi/Shaders.h"

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
    // Descriptor set layout: binding 0 = combined image sampler (the SBS texture).
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci = {};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &descSetLayout_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorSetLayout failed");
        return false;
    }

    // Push constants: uvOffset (vec2) + uvScale (vec2) = 16 bytes, fragment stage.
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(float) * 4;

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

    // Descriptor pool + a single set (rebound to the texture in UploadTexture).
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
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

bool VulkanRenderer::UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) return false;
    DestroyTexture();

    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;

    // GPU texture (sampled).
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &textureImage_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateImage (texture) failed");
        return false;
    }
    VkMemoryRequirements ireq;
    vkGetImageMemoryRequirements(device_, textureImage_, &ireq);
    VkMemoryAllocateInfo iai = {};
    iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    iai.allocationSize = ireq.size;
    iai.memoryTypeIndex = FindMemoryType(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (iai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device_, &iai, nullptr, &textureMemory_) != VK_SUCCESS) {
        LOG_ERROR("texture memory alloc failed");
        return false;
    }
    vkBindImageMemory(device_, textureImage_, textureMemory_, 0);

    // Host-visible staging buffer with the pixels.
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

    // Upload: UNDEFINED -> TRANSFER_DST -> copy -> SHADER_READ_ONLY_OPTIMAL.
    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &cbi);

    VkImageMemoryBarrier toDst = {};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = textureImage_;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(commandBuffer_, staging, textureImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
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

    // View + bind into the descriptor set.
    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = textureImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vci, nullptr, &textureView_) != VK_SUCCESS) {
        LOG_ERROR("texture image view failed");
        return false;
    }

    VkDescriptorImageInfo dii = {};
    dii.sampler = sampler_;
    dii.imageView = textureView_;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &dii;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    LOG_INFO("VulkanRenderer: uploaded %ux%u texture", width, height);
    return true;
}

bool VulkanRenderer::DrawViews(uint32_t imageIndex, const XrSession::ViewRect* rects,
                               const ViewUV* uvs, uint32_t viewCount) {
    if (imageIndex >= framebuffers_.size() || textureView_ == VK_NULL_HANDLE) return false;

    vkResetCommandBuffer(commandBuffer_, 0);
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &bi);

    VkClearValue clear = {};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};  // black background outside the tiles
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

        VkRect2D sc = {};
        sc.offset = {rects[v].x, rects[v].y};
        sc.extent = {rects[v].w, rects[v].h};
        vkCmdSetScissor(commandBuffer_, 0, 1, &sc);

        const float pc[4] = {uvs[v].offX, uvs[v].offY, uvs[v].scaleX, uvs[v].scaleY};
        vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), pc);
        vkCmdDraw(commandBuffer_, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer_);
    vkEndCommandBuffer(commandBuffer_);

    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
    if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) {
        LOG_ERROR("DrawViews vkQueueSubmit failed");
        return false;
    }
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    return true;
}

void VulkanRenderer::DestroyTexture() {
    if (textureView_) { vkDestroyImageView(device_, textureView_, nullptr); textureView_ = VK_NULL_HANDLE; }
    if (textureImage_) { vkDestroyImage(device_, textureImage_, nullptr); textureImage_ = VK_NULL_HANDLE; }
    if (textureMemory_) { vkFreeMemory(device_, textureMemory_, nullptr); textureMemory_ = VK_NULL_HANDLE; }
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

void VulkanRenderer::Shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_);
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
