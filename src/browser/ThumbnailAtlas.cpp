// SPDX-License-Identifier: Apache-2.0
#include "browser/ThumbnailAtlas.h"

#include "Log.h"

#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cstring>

namespace mp::browser {

namespace {

uint32_t FindMemoryType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
             VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

ThumbnailAtlas::~ThumbnailAtlas() { Shutdown(); }

bool ThumbnailAtlas::Initialize(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t tile, uint32_t slots,
                                uint32_t framesInFlight) {
    if (device_ != VK_NULL_HANDLE || tile == 0 || tile > kPageMax || slots == 0) return false;
    physicalDevice_ = physicalDevice;
    device_ = device;
    tile_ = tile;
    cols_ = kPageMax / tile;
    perPage_ = cols_ * cols_;
    sizes_.assign(slots, Size{});
    if (!CreateObjects(slots, framesInFlight)) {
        LOG_WARN("ThumbnailAtlas: could not create %u slots of %u px", slots, tile);
        Shutdown();
        return false;
    }
    LOG_INFO("ThumbnailAtlas: %u slots of %u px on %zu pages", slots, tile, pages_.size());
    return true;
}

void ThumbnailAtlas::Shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_);  // frames in flight may still sample the pages
    for (Page& page : pages_) {
        if (page.descriptor) ImGui_ImplVulkan_RemoveTexture(page.descriptor);
        vkDestroyImageView(device_, page.view, nullptr);
        vkDestroyImage(device_, page.image, nullptr);
        vkFreeMemory(device_, page.memory, nullptr);
    }
    for (Staging& s : staging_) {
        vkDestroyBuffer(device_, s.buffer, nullptr);
        vkFreeMemory(device_, s.memory, nullptr);
    }
    vkDestroySampler(device_, sampler_, nullptr);
    pages_.clear();
    staging_.clear();
    sizes_.clear();
    pending_.clear();
    stagingNext_ = 0;
    sampler_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

bool ThumbnailAtlas::CreateObjects(uint32_t slots, uint32_t framesInFlight) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) return false;

    for (uint32_t first = 0; first < slots; first += perPage_) {
        const uint32_t n = std::min(slots - first, perPage_);
        if (!AddPage(std::min(n, cols_) * tile_, (n + cols_ - 1) / cols_ * tile_)) return false;
    }
    for (uint32_t i = 0; i < framesInFlight + 1; ++i) {
        if (!AddStaging(TileBytes() * kUploadsPerFrame)) return false;
    }
    return true;
}

bool ThumbnailAtlas::AddPage(uint32_t w, uint32_t h) {
    pages_.push_back(Page{});
    Page& page = pages_.back();
    page.w = w;
    page.h = h;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;  // encoded bytes, sampled as-is like the rest of the UI
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ii, nullptr, &page.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, page.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(physicalDevice_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &page.memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, page.image, page.memory, 0) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = page.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &page.view) != VK_SUCCESS) return false;

    page.descriptor = ImGui_ImplVulkan_AddTexture(sampler_, page.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return page.descriptor != VK_NULL_HANDLE;
}

bool ThumbnailAtlas::AddStaging(VkDeviceSize size) {
    staging_.push_back(Staging{});
    Staging& s = staging_.back();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device_, &bi, nullptr, &s.buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, s.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(physicalDevice_, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &s.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, s.buffer, s.memory, 0) != VK_SUCCESS ||
        vkMapMemory(device_, s.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        return false;
    }
    s.mapped = (uint8_t*)mapped;
    return true;
}

void ThumbnailAtlas::SlotOrigin(int slot, uint32_t& x, uint32_t& y) const {
    const uint32_t i = (uint32_t)slot % perPage_;
    x = i % cols_ * tile_;
    y = i / cols_ * tile_;
}

bool ThumbnailAtlas::Upload(int slot, const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (slot < 0 || slot >= (int)sizes_.size() || !rgba || w == 0 || h == 0 || w > tile_ || h > tile_ ||
        pending_.size() == kUploadsPerFrame) {
        return false;
    }
    std::memcpy(staging_[stagingNext_].mapped + pending_.size() * TileBytes(), rgba, (size_t)w * h * 4);
    pending_.push_back({slot, {w, h}});
    sizes_[slot] = {w, h};
    return true;
}

void ThumbnailAtlas::RecordUploads(VkCommandBuffer cmd) {
    if (pending_.empty()) return;
    std::vector<VkBufferImageCopy> copies;
    for (size_t p = 0; p < pages_.size(); ++p) {
        copies.clear();
        for (size_t i = 0; i < pending_.size(); ++i) {
            const PendingUpload& u = pending_[i];
            if ((uint32_t)u.slot / perPage_ != p) continue;
            uint32_t x = 0, y = 0;
            SlotOrigin(u.slot, x, y);
            VkBufferImageCopy c{};
            c.bufferOffset = i * TileBytes();
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageOffset = {(int32_t)x, (int32_t)y, 0};
            c.imageExtent = {u.size.w, u.size.h, 1};
            copies.push_back(c);
        }
        if (copies.empty()) continue;

        // The first barrier also waits for earlier frames that still sample other slots.
        Page& page = pages_[p];
        Barrier(cmd, page.image,
                page.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, page.initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(cmd, staging_[stagingNext_].buffer, page.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               (uint32_t)copies.size(), copies.data());
        Barrier(cmd, page.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        page.initialized = true;
    }
    pending_.clear();
    stagingNext_ = (stagingNext_ + 1) % staging_.size();
}

bool ThumbnailAtlas::Get(int slot, Thumb& out) const {
    if (slot < 0 || slot >= (int)sizes_.size() || sizes_[slot].w == 0) return false;
    const Size size = sizes_[slot];
    const Page& page = pages_[(uint32_t)slot / perPage_];
    uint32_t x = 0, y = 0;
    SlotOrigin(slot, x, y);
    const float pw = (float)page.w, ph = (float)page.h;
    // Half a texel in from each edge, so linear filtering never reads the neighbouring slot.
    out.texture = (ImTextureID)page.descriptor;
    out.uv0 = ImVec2((x + 0.5f) / pw, (y + 0.5f) / ph);
    out.uv1 = ImVec2((x + size.w - 0.5f) / pw, (y + size.h - 0.5f) / ph);
    return true;
}

} // namespace mp::browser
