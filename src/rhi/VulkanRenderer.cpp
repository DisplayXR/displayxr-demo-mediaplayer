// SPDX-License-Identifier: BSL-1.0
#include "VulkanRenderer.h"

#include "Log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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

    LOG_INFO("VulkanRenderer ready (%u framebuffers, %ux%u)",
             (uint32_t)framebuffers_.size(), width_, height_);
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
