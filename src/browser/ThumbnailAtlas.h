// SPDX-License-Identifier: Apache-2.0
//
// ThumbnailAtlas: the media grid's thumbnails, as fixed-size slots packed into a few large
// textures with one ImGui descriptor each. A texture per thumbnail would not fit: the grid
// shows 50+ thumbnails and ImGuiLayer's descriptor pool holds 16 sets.
//
// The descriptors come from ImGuiLayer's pool through the ImGui Vulkan backend, so
// initialize the atlas after ImGuiLayer::Init and shut it down before
// ImGuiLayer::Shutdown. Render thread only. Which media sits in which slot is
// ThumbnailCache's business.
#pragma once

#include <vulkan/vulkan.h>

#include "imgui.h"

#include <cstdint>
#include <vector>

namespace mp::browser {

class ThumbnailAtlas {
public:
    struct Thumb {
        ImTextureID texture{};
        ImVec2 uv0, uv1;  // the slot's image, inset half a texel so filtering stays inside it
        uint32_t w = 0, h = 0;  // the image's size, px
    };

    ThumbnailAtlas() = default;
    ~ThumbnailAtlas();

    ThumbnailAtlas(const ThumbnailAtlas&) = delete;
    ThumbnailAtlas& operator=(const ThumbnailAtlas&) = delete;

    // `slots` slots of up to `tile` x `tile` RGBA8 pixels. `framesInFlight` is the frame
    // ring depth, which sets how long an upload buffer must wait before reuse.
    bool Initialize(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t tile, uint32_t slots,
                    uint32_t framesInFlight);
    void Shutdown();

    uint32_t Tile() const { return tile_; }
    uint32_t SlotCount() const { return (uint32_t)sizes_.size(); }

    // Copy a w x h image into `slot`. False if the slot or size is out of range, or if this
    // frame's upload budget is spent (then try again next frame).
    bool Upload(int slot, const uint8_t* rgba, uint32_t w, uint32_t h);
    // Record this frame's uploads into the frame command buffer, before
    // ImGuiLayer::RenderToHud draws with them.
    void RecordUploads(VkCommandBuffer cmd);
    // What to draw for `slot`. False before its first upload.
    bool Get(int slot, Thumb& out) const;

private:
    static constexpr uint32_t kPageMax = 2048;       // page edge, px
    static constexpr uint32_t kUploadsPerFrame = 4;

    struct Page {
        uint32_t w = 0, h = 0;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        bool initialized = false;  // has had an upload, so its layout is defined
    };
    struct Staging {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
    };
    struct Size { uint32_t w = 0, h = 0; };
    struct PendingUpload { int slot; Size size; };

    bool CreateObjects(uint32_t slots, uint32_t framesInFlight);
    bool AddPage(uint32_t w, uint32_t h);
    bool AddStaging(VkDeviceSize size);
    void SlotOrigin(int slot, uint32_t& x, uint32_t& y) const;  // px within its page
    VkDeviceSize TileBytes() const { return (VkDeviceSize)tile_ * tile_ * 4; }

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t tile_ = 0;
    uint32_t cols_ = 0;  // slots per page row (and rows per page)
    uint32_t perPage_ = 0;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::vector<Page> pages_;
    std::vector<Staging> staging_;        // a ring: one buffer per frame in flight, plus one
    size_t stagingNext_ = 0;
    std::vector<Size> sizes_;             // per slot: the last image's size, 0 x 0 before any
    std::vector<PendingUpload> pending_;  // this frame's, in staging buffer order
};

} // namespace mp::browser
