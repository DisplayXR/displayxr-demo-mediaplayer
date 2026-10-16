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

#include "ColorPolicy.h"  // swapchain encoding policy (#78)
#include "Log.h"
#include "rhi/Shaders.h"

#include <algorithm>  // std::max / std::min (scissor clamp)
#include <cstring>    // std::memcpy (half-float LUT pack, staging copies)
#include <vector>

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

namespace {
// Fragment push constants - must match `PushConstants` in shaders/sbs.frag (std430
// push-constant offsets: vec2 at 8, vec4 at 16).
struct DrawPush {
    float off[2];        //  0
    float scale[2];      //  8
    int32_t mode;        // 16
    float fullRange;     // 20
    float srgbTarget;    // 24
    int32_t qcMode;      // 28
    float otherDelta[2]; // 32
    float exposure;      // 40
    float lutOn;         // 44
    float cdlSlope[4];   // 48  .a = saturation
    float cdlOffset[4];  // 64  .a = CDL on
    float cdlPower[4];   // 80
    float lutDomain[4];  // 96  x = min, y = 1/(max-min), z = N
};
static_assert(sizeof(DrawPush) == 112, "DrawPush must match the shader's PushConstants");

// IEEE 754 binary32 -> binary16 (round half up on the dropped bits), for the RGBA16F LUT.
uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t e32 = (x >> 23) & 0xffu;
    int32_t exp = (int32_t)e32 - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (e32 == 0xffu) return (uint16_t)(sign | 0x7c00u | (mant ? 0x200u : 0));
    if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) h += 1;
        return (uint16_t)(sign | h);
    }
    uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
    if (mant & 0x1000u) h += 1;  // a carry into the exponent is the correct rounding
    return (uint16_t)h;
}
}  // namespace

VulkanRenderer::~VulkanRenderer() { Shutdown(); }

// A clear value written through an _SRGB attachment is taken as scene-linear and encoded
// by the hardware on store, so a display-referred colour (which is what every colour in
// this app is — background_, the letterbox fill, the RED|BLUE test pattern) has to be
// decoded first or it stores lighter than asked. Alpha is never encoded (#78).
VkClearColorValue VulkanRenderer::ToTargetClear(const ClearColor& c) const {
    if (!srgbTarget_) return VkClearColorValue{{c.r, c.g, c.b, c.a}};
    return VkClearColorValue{{color::DisplayReferredToSceneLinear(c.r),
                              color::DisplayReferredToSceneLinear(c.g),
                              color::DisplayReferredToSceneLinear(c.b), c.a}};
}

bool VulkanRenderer::Initialize(VkPhysicalDevice physicalDevice, VkDevice device,
                                VkQueue graphicsQueue, uint32_t queueFamilyIndex,
                                VkFormat format, uint32_t width, uint32_t height,
                                const std::vector<VkImage>& images) {
    physicalDevice_ = physicalDevice;
    device_ = device;
    queue_ = graphicsQueue;
    queueFamilyIndex_ = queueFamilyIndex;
    format_ = format;
    // An _SRGB swapchain view encodes on store (#78): the shader must hand it scene-
    // linear values and clear colours must be decoded first. On the UNORM path nothing
    // converts and every write is the byte-for-byte passthrough it always was.
    srgbTarget_ = color::IsSrgb8((int64_t)format);
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

    // The frame ring: N command buffers + N fences + a one-off upload buffer, all
    // allocated ONCE (INV-4.10). Frames in flight: default 3, DXR_VK_FRAMES_IN_FLIGHT
    // overrides (1 = the old submit->wait, for an A/B). Before CreatePipeline, which
    // sizes the per-slot descriptor sets from it.
    if (!ring_.init(device_, physicalDevice_, queue_, queueFamilyIndex, dxr::VkFrameRing::Config{})) {
        LOG_ERROR("VkFrameRing init failed");
        return false;
    }

    if (!CreatePipeline()) return false;

    LOG_INFO("VulkanRenderer ready (%u framebuffers, %ux%u, format=%d %s, %u frames in flight)",
             (uint32_t)framebuffers_.size(), width_, height_, (int)format_,
             srgbTarget_ ? "sRGB — shader + clears converted to scene-linear"
                         : "UNORM — passthrough",
             ring_.framesInFlight());
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
    // or Y + chroma planes for YUV video); binding 3 = the Pro grade's 3D LUT.
    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci = {};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 4;
    dslci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &descSetLayout_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorSetLayout failed");
        return false;
    }

    // Push constants: see DrawPush (112 bytes; the spec guarantees 128).
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(DrawPush);

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

    // Descriptor pool + one set per ring slot (3 samplers each), written by DrawViews for
    // the slot it records into — never a set a queued frame still references.
    const uint32_t nSlots = ring_.framesInFlight();
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 4 * nSlots;
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = nSlots;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorPool failed");
        return false;
    }
    VkDescriptorSetLayout layouts[kMaxSlots];
    for (uint32_t i = 0; i < nSlots; ++i) layouts[i] = descSetLayout_;
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descPool_;
    dsai.descriptorSetCount = nSlots;
    dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device_, &dsai, descSets_) != VK_SUCCESS) {
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

    VkCommandBuffer cmd = FrameCmd();  // the frame's one command buffer
    if (cmd == VK_NULL_HANDLE) return false;

    // LOAD_OP_CLEAR fills the whole image with view 0's color; any area not covered
    // by a tile keeps it.
    VkClearValue clear = {};
    clear.color = ToTargetClear(colors[0]);

    VkRenderPassBeginInfo rb = {};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = renderPass_;
    rb.framebuffer = framebuffers_[imageIndex];
    rb.renderArea.offset = {0, 0};
    rb.renderArea.extent = {width_, height_};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);

    // Paint each view's tile (the exact rect submitted to the runtime) with its color.
    for (uint32_t v = 0; v < viewCount; ++v) {
        VkClearAttachment att = {};
        att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att.colorAttachment = 0;
        att.clearValue.color = ToTargetClear(colors[v]);
        VkClearRect rect = {};
        rect.rect.offset = {rects[v].x, rects[v].y};
        rect.rect.extent = {rects[v].w, rects[v].h};
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        vkCmdClearAttachments(cmd, 1, &att, 1, &rect);
    }

    vkCmdEndRenderPass(cmd);
    // No submit, no wait: SubmitFrame() submits the frame once, before the release.
    return true;
}

bool VulkanRenderer::RecordPlaneUpload(VkCommandBuffer cmd, int idx, const uint8_t* data,
        uint32_t w, uint32_t h, VkFormat fmt, uint32_t bytesPerTexel, bool& recreated) {
    Plane& p = planes_[idx];
    const VkDeviceSize bytes = (VkDeviceSize)w * h * bytesPerTexel;

    recreated = (p.image == VK_NULL_HANDLE || p.w != w || p.h != h || p.fmt != fmt);
    if (recreated) {
        // A queued frame may still sample the old plane: release it only once the GPU
        // is past every frame submitted so far (and the open one).
        if (p.view || p.image || p.memory) {
            VkDevice dev = device_;
            VkImageView oldView = p.view;
            VkImage oldImage = p.image;
            VkDeviceMemory oldMem = p.memory;
            ring_.deferDestroy([dev, oldView, oldImage, oldMem]() {
                if (oldView) vkDestroyImageView(dev, oldView, nullptr);
                if (oldImage) vkDestroyImage(dev, oldImage, nullptr);
                if (oldMem) vkFreeMemory(dev, oldMem, nullptr);
            });
            p.view = VK_NULL_HANDLE;
            p.image = VK_NULL_HANDLE;
            p.memory = VK_NULL_HANDLE;
        }

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

    // Host-visible staging buffer with the plane's pixels (released via deferDestroy once
    // the frame that copies from it has executed).
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
    {
        VkDevice dev = device_;
        ring_.deferDestroy([dev, sbuf, smem]() {
            vkDestroyBuffer(dev, sbuf, nullptr);
            vkFreeMemory(dev, smem, nullptr);
        });
    }

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

// Record N plane uploads into the FRAME command buffer, ahead of the DrawViews pass that
// samples them (queue order + the barriers in RecordPlaneUpload). No submit of its own, no
// wait: the frame's single submit carries upload + draw + HUD (INV-4.10).
namespace {
struct PlaneSpec { const uint8_t* data; uint32_t w, h; VkFormat fmt; uint32_t bpt; };
}

bool VulkanRenderer::UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) return false;
#if defined(_WIN32)
    sharedActive_ = false;  // CPU RGBA source -> leave the zero-copy path
#endif
    if (!EnsureDummy()) return false;

    VkCommandBuffer cmd = FrameCmd();
    if (cmd == VK_NULL_HANDLE) return false;
    bool rec = false;
    const bool ok = RecordPlaneUpload(cmd, 0, rgba, width, height, VK_FORMAT_R8G8B8A8_UNORM, 4,
                                      rec);
    if (!ok) return false;

    sourceMode_ = 0;
    sourceFullRange_ = 0.0f;
    // Descriptor sets are written per frame (DrawViews), so nothing to rebind here.
    if (rec) LOG_INFO("VulkanRenderer: RGBA texture (re)created %ux%u", width, height);
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
    if (format == 2) {  // P016 (Pro 10-bit capture): 16-bit planes, sampled as NV12
        specs[0] = {plane0, width, height, VK_FORMAT_R16_UNORM, 2};
        specs[1] = {plane1, cw, ch, VK_FORMAT_R16G16_UNORM, 4};
        count = 2;
        mode = 2;
    } else if (format == 1) {
        specs[1] = {plane1, cw, ch, VK_FORMAT_R8G8_UNORM, 2};
        count = 2;
        mode = 2;
    } else {
        specs[1] = {plane1, cw, ch, VK_FORMAT_R8_UNORM, 1};
        specs[2] = {plane2, cw, ch, VK_FORMAT_R8_UNORM, 1};
        count = 3;
        mode = 1;
    }

    VkCommandBuffer cmd = FrameCmd();
    if (cmd == VK_NULL_HANDLE) return false;
    bool rec[3] = {false, false, false};
    bool ok = true;
    for (int i = 0; i < count && ok; ++i) {
        ok = RecordPlaneUpload(cmd, i, specs[i].data, specs[i].w, specs[i].h, specs[i].fmt,
                               specs[i].bpt, rec[i]);
    }
    if (!ok) return false;

    sourceMode_ = mode;
    sourceFullRange_ = fullRange ? 1.0f : 0.0f;
    source16_ = (format == 2);
    // Descriptor sets are written per frame (DrawViews), so nothing to rebind here.
    if (rec[0]) LOG_INFO("VulkanRenderer: YUV source %ux%u mode=%d", width, height, mode);
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
    // A genuine one-off: the ring's sanctioned blocking submit, ahead of any frame that
    // binds the dummy.
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.image = dummyImage_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ring_.submitImmediate([&](VkCommandBuffer cmd) {
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &b);
    });

    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = dummyImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device_, &vci, nullptr, &dummyView_) == VK_SUCCESS;
}

void VulkanRenderer::BindDescriptors(VkDescriptorSet set) {
    // Bind the planes the current mode samples; everything else gets the dummy.
    VkImageView views[3] = {planes_[0].view, dummyView_, dummyView_};
    VkImageLayout layouts[3] = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
#if defined(_WIN32)
    // Zero-copy (#28): sample the imported RGBA texture (producer already did YUV->RGB) as
    // mode 0, in GENERAL layout (the imported image is left in GENERAL by the acquire barrier).
    if (sharedActive_) {
        views[0] = sharedView_;
        layouts[0] = VK_IMAGE_LAYOUT_GENERAL;
    } else
#endif
    if (sourceMode_ == 1) {            // I420: Y, U, V
        views[1] = planes_[1].view;
        views[2] = planes_[2].view;
    } else if (sourceMode_ == 2) {     // NV12: Y, interleaved UV
        views[1] = planes_[1].view;
    }
    VkDescriptorImageInfo dii[4] = {};
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        dii[i].sampler = sampler_;
        if (i == 3) {   // the Pro grade LUT, or its 1x1x1 dummy
            dii[i].imageView = lut_.view ? lut_.view : lutDummy_.view;
            dii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else {
            dii[i].imageView = views[i] ? views[i] : dummyView_;
            dii[i].imageLayout = layouts[i];
        }
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &dii[i];
    }
    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
}

bool VulkanRenderer::DrawViews(uint32_t imageIndex, const XrSession::ViewRect* rects,
                               const XrSession::ViewRect* clipRects,
                               const ViewUV* uvs, uint32_t viewCount) {
    bool haveSource = planes_[0].view != VK_NULL_HANDLE;
#if defined(_WIN32)
    haveSource = haveSource || sharedActive_;
#endif
    if (imageIndex >= framebuffers_.size() || !haveSource) return false;

    // Unused plane bindings point at the 1x1 dummy; BindSharedRGBA's DestroyTexture() drops
    // it, so make sure it exists before this frame writes its descriptor set (a one-off
    // blocking init the first time, a no-op after).
    if (!EnsureDummy()) return false;
    if (!lutDummy_.view) {
        const uint16_t one[4] = {0, 0, 0, 0x3c00u};
        if (!CreateLutImage(lutDummy_, 1, one)) return false;
    }

    // The frame's one command buffer (uploads may already have opened it). Its ring
    // slot's fence — the frame that used this slot N frames ago — has passed, so that
    // slot's descriptor set is free to rewrite now.
    VkCommandBuffer cmd = FrameCmd();
    if (cmd == VK_NULL_HANDLE) return false;
    VkDescriptorSet descSet = descSets_[ring_.slotIndex()];
    BindDescriptors(descSet);

#if defined(_WIN32)
    // Zero-copy (#28): the imported NV12 image is written by the D3D11 producer on the
    // external (foreign) queue, so before we sample it we must ACQUIRE queue-family
    // ownership from VK_QUEUE_FAMILY_EXTERNAL to our graphics family — every frame, not
    // just once. (The prior one-shot IGNORED->IGNORED barrier left the cross-API write
    // unsynchronized against the sample, faulting the GPU / TDR-timing out the submit.)
    // First frame uses UNDEFINED (nothing to preserve on our side yet); thereafter GENERAL,
    // since the producer wrote the image while it sat in GENERAL.
    if (sharedActive_ && sharedLayoutReady_ && sharedImage_) {
        const bool firstAcquire = !*sharedLayoutReady_;
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = firstAcquire ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = 0;  // ignored for an acquire; the external release makes writes available
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        b.dstQueueFamilyIndex = queueFamilyIndex_;
        b.image = sharedImage_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &b);
        *sharedLayoutReady_ = true;
    }
    if (sharedActive_) sharedSampledThisFrame_ = true;  // SubmitFrame keeps this path synchronous
#endif

    VkClearValue clear = {};
    // Letterbox/background: the configured fill (opaque black by default, dark grey for
    // the idle logo); alpha 0 in transparent-bg mode so the runtime composes those
    // pixels through to the desktop.
    clear.color = ToTargetClear(ClearColor{background_.r, background_.g, background_.b,
                                           transparentLetterbox_ ? 0.0f : background_.a});
    VkRenderPassBeginInfo rb = {};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = renderPass_;
    rb.framebuffer = framebuffers_[imageIndex];
    rb.renderArea.offset = {0, 0};
    rb.renderArea.extent = {width_, height_};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);

    // Both the CPU planar path and the zero-copy (#28) RGBA path use the main pipeline; the
    // shared RGBA import just binds its view into binding 0 as mode 0 (see BindDescriptors).
    VkPipelineLayout activeLayout = pipelineLayout_;
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                0, 1, &descSet, 0, nullptr);
    }

    for (uint32_t v = 0; v < viewCount; ++v) {
        VkViewport vp = {};
        vp.x = (float)rects[v].x;
        vp.y = (float)rects[v].y;
        vp.width = (float)rects[v].w;
        vp.height = (float)rects[v].h;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

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
        vkCmdSetScissor(cmd, 0, 1, &sc);

        DrawPush pc = {};
        pc.off[0] = uvs[v].offX;
        pc.off[1] = uvs[v].offY;
        pc.scale[0] = uvs[v].scaleX;
        pc.scale[1] = uvs[v].scaleY;
        pc.mode = sourceMode_;
        pc.fullRange = sourceFullRange_;
        pc.srgbTarget = srgbTarget_ ? 1.0f : 0.0f;
        pc.qcMode = grade_.qcMode;
        pc.otherDelta[0] = uvs[v].otherDX;
        pc.otherDelta[1] = uvs[v].otherDY;
        pc.exposure = grade_.exposure;
        pc.lutOn = (grade_.lutOn && lut_.view) ? 1.0f : 0.0f;
        for (int k = 0; k < 3; ++k) {
            pc.cdlSlope[k] = grade_.slope[k];
            pc.cdlOffset[k] = grade_.offset[k];
            pc.cdlPower[k] = grade_.power[k];
        }
        pc.cdlSlope[3] = grade_.saturation;
        pc.cdlOffset[3] = grade_.cdlOn ? 1.0f : 0.0f;
        pc.lutDomain[0] = lut_.domainMin;
        pc.lutDomain[1] = 1.0f / std::max(1e-6f, lut_.domainMax - lut_.domainMin);
        pc.lutDomain[2] = (float)std::max(1u, lut_.size);
        pc.lutDomain[3] = source16_ ? 1.0f : 0.0f;  // MP_GRADE: 10-bit levels for P016
        vkCmdPushConstants(cmd, activeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    // No submit, no wait: SubmitFrame() submits the frame once (with the HUD pass), before
    // the swapchain images are released.
    return true;
}

void VulkanRenderer::SubmitFrame() {
    if (!ring_.frameOpen()) return;
    const VkResult r = ring_.endFrame();  // EXACTLY one vkQueueSubmit, slot fence, no wait
    if (r != VK_SUCCESS && !submitErrLogged_) {
        submitErrLogged_ = true;
        LOG_ERROR("frame vkQueueSubmit failed: %d (logged once)", (int)r);
    }
#if defined(_WIN32)
    // Zero-copy (#28): the D3D11 producer refills its shared textures with no GPU-side
    // sync against our sample (it CPU-waits its own copy, then publishes), so a frame that
    // sampled one stays synchronous — the pre-ring behaviour, now via the ring. Windows
    // desktop GPUs have no Adreno-style busy-% governor, and this path cannot be made
    // asynchronous safely without a cross-API semaphore. The keyed-mutex experiment
    // (MEDIAPLAYER_ZC_KM, off by default) was dropped: it needed its own submit pNext.
    if (sharedSampledThisFrame_) {
        ring_.waitAll();
        sharedSampledThisFrame_ = false;
    }
#endif
}

#if defined(_WIN32)

VulkanRenderer::SharedImport* VulkanRenderer::ImportSharedRGBA(void* handle, uint32_t w,
                                                               uint32_t h) {
    for (auto& kv : imports_) if (kv.first == handle) return &kv.second;  // already imported

    SharedImport imp;
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext;
    ici.flags = 0;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;  // == DXGI_FORMAT_R8G8B8A8_UNORM (producer converted)
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    // OPTIMAL (#28): this shared surface is a D3D11 *render target* (the NV12->RGBA convert
    // draws into it), which the driver stores tiled — so Vulkan must import it tiled too, or it
    // reads the tiled bytes as if linear and the image comes out blocky. (Contrast the decoder's
    // NV12 texture, which is row-major/LINEAR. Single-plane RGBA also avoids the multiplanar
    // UV-alignment trap that blocked importing the NV12 directly.) MEDIAPLAYER_ZC_LINEAR=1 forces
    // LINEAR for A/B debugging.
    static const bool zcLinear = [] {
        const char* e = std::getenv("MEDIAPLAYER_ZC_LINEAR");
        return e && *e && *e != '0';
    }();
    ici.tiling = zcLinear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &imp.image) != VK_SUCCESS) {
        LOG_WARN("ImportSharedRGBA: vkCreateImage failed");
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
    LOG_INFO("zc-import: RGBA reqBits=0x%x handleBits=0x%x -> type=%u size=%llu", req.memoryTypeBits,
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
        LOG_WARN("ImportSharedRGBA: import vkAllocateMemory failed");
        vkDestroyImage(device_, imp.image, nullptr);
        return nullptr;
    }
    if (vkBindImageMemory(device_, imp.image, imp.memory, 0) != VK_SUCCESS) {
        LOG_WARN("ImportSharedRGBA: vkBindImageMemory failed");
        vkFreeMemory(device_, imp.memory, nullptr);
        vkDestroyImage(device_, imp.image, nullptr);
        return nullptr;
    }

    VkImageViewCreateInfo v = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = imp.image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = VK_FORMAT_R8G8B8A8_UNORM;
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &v, nullptr, &imp.view) != VK_SUCCESS) {
        LOG_WARN("ImportSharedRGBA: view create failed");
        vkFreeMemory(device_, imp.memory, nullptr);
        vkDestroyImage(device_, imp.image, nullptr);
        return nullptr;
    }
    imports_.emplace_back(handle, imp);
    LOG_INFO("VulkanRenderer: imported shared RGBA %ux%u (zero-copy #28)", w, h);
    return &imports_.back().second;
}

bool VulkanRenderer::BindSharedRGBA(void* sharedHandle, uint32_t width, uint32_t height) {
    if (!sharedHandle) return false;
    SharedImport* imp = ImportSharedRGBA(sharedHandle, width, height);
    if (!imp) return false;
    if (planes_[0].view != VK_NULL_HANDLE) DestroyTexture();  // drop CPU planes; sample the import
    sharedActive_ = true;
    sharedImage_ = imp->image;
    sharedMemory_ = imp->memory;
    sharedLayoutReady_ = &imp->layoutReady;
    sharedView_ = imp->view;
    sourceMode_ = 0;  // packed RGBA (producer already did YUV->RGB); sampled by the main pipeline
    sourceFullRange_ = 0.0f;
    // DrawViews writes sharedView_ into the frame's descriptor set (GENERAL layout, see the
    // sharedActive_ branch of BindDescriptors).
    return true;
}

void VulkanRenderer::ClearSharedImports() {
    // Media change / shutdown: every frame in flight may reference these imports.
    ring_.waitAll();
    for (auto& kv : imports_) {
        SharedImport& s = kv.second;
        if (s.view) vkDestroyImageView(device_, s.view, nullptr);
        if (s.image) vkDestroyImage(device_, s.image, nullptr);
        if (s.memory) vkFreeMemory(device_, s.memory, nullptr);
    }
    imports_.clear();
    sharedActive_ = false;
    sharedImage_ = VK_NULL_HANDLE;
    sharedMemory_ = VK_NULL_HANDLE;
    sharedLayoutReady_ = nullptr;
    sharedView_ = VK_NULL_HANDLE;
}
#endif  // _WIN32

void VulkanRenderer::DestroyTexture() {
    // Queued frames may still sample these: release them once the GPU is past every frame
    // submitted so far (immediately when the ring is not up).
    VkDevice dev = device_;
    for (Plane& p : planes_) {
        if (p.view || p.image || p.memory) {
            const VkImageView v = p.view;
            const VkImage i = p.image;
            const VkDeviceMemory m = p.memory;
            ring_.deferDestroy([dev, v, i, m]() {
                if (v) vkDestroyImageView(dev, v, nullptr);
                if (i) vkDestroyImage(dev, i, nullptr);
                if (m) vkFreeMemory(dev, m, nullptr);
            });
        }
        p = Plane{};
    }
    if (dummyView_ || dummyImage_ || dummyMemory_) {
        const VkImageView v = dummyView_;
        const VkImage i = dummyImage_;
        const VkDeviceMemory m = dummyMemory_;
        ring_.deferDestroy([dev, v, i, m]() {
            if (v) vkDestroyImageView(dev, v, nullptr);
            if (i) vkDestroyImage(dev, i, nullptr);
            if (m) vkFreeMemory(dev, m, nullptr);
        });
        dummyView_ = VK_NULL_HANDLE;
        dummyImage_ = VK_NULL_HANDLE;
        dummyMemory_ = VK_NULL_HANDLE;
    }
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
    {
        VkDevice dev = device_;
        ring_.deferDestroy([dev, staging, stagingMem]() {
            vkDestroyBuffer(dev, staging, nullptr);
            vkFreeMemory(dev, stagingMem, nullptr);
        });
    }

    // Recorded into the frame command buffer (after the atlas pass); SubmitFrame()
    // submits it before the HUD image is released.
    VkCommandBuffer cmd = FrameCmd();
    if (cmd == VK_NULL_HANDLE) return false;

    // We overwrite the whole image, so UNDEFINED old layout is fine each frame.
    VkImageMemoryBarrier toDst = {};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // Leave it in COLOR_ATTACHMENT_OPTIMAL for the runtime at release time.
    VkImageMemoryBarrier toColor = toDst;
    toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toColor);
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
    // Debug-only readback, deliberately blocking: submit the open frame now (so the copy
    // below sees this frame's draw), drain the ring, then copy with the ring's one-off
    // blocking submit. A HUD recorded after this lands in a second frame submit — only on
    // the one frame a dump is taken.
    SubmitFrame();
    ring_.waitAll();

    VkImageMemoryBarrier toSrc = {};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.image = images_[imageIndex];
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ring_.submitImmediate([&](VkCommandBuffer cmd) {
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width_, height_, 1};
        vkCmdCopyImageToBuffer(cmd, images_[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

        // Restore the layout the runtime expects at release time.
        VkImageMemoryBarrier toColor = toSrc;
        toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toColor);
    });

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

    // Debug-only readback, deliberately blocking: submit the open frame now (so the copy
    // below sees this frame's draw), drain the ring, then copy with the ring's one-off
    // blocking submit. A HUD recorded after this lands in a second frame submit — only on
    // the one frame a dump is taken.
    SubmitFrame();
    ring_.waitAll();

    VkImageMemoryBarrier toSrc = {};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.image = image;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ring_.submitImmediate([&](VkCommandBuffer cmd) {
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buf, 1, &region);

        VkImageMemoryBarrier toColor = toSrc;
        toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toColor);
    });

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
    DestroyLut(lut_, false);
    DestroyLut(lutDummy_, false);
    ring_.destroy();  // waits its fences, runs the deferred releases, frees cmds/fences
    if (sampler_) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (descPool_) { vkDestroyDescriptorPool(device_, descPool_, nullptr); descPool_ = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < kMaxSlots; ++i) descSets_[i] = VK_NULL_HANDLE;
    if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (descSetLayout_) { vkDestroyDescriptorSetLayout(device_, descSetLayout_, nullptr); descSetLayout_ = VK_NULL_HANDLE; }
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto iv : imageViews_) if (iv) vkDestroyImageView(device_, iv, nullptr);
    framebuffers_.clear();
    imageViews_.clear();
    if (renderPass_) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    device_ = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Pro grade: 3D LUT texture (binding 3)
// ---------------------------------------------------------------------------

bool VulkanRenderer::CreateLutImage(Lut3D& dst, uint32_t n, const uint16_t* rgba16f) {
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;  // linear filtering is mandatory for it
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_3D;
    ici.format = fmt;
    ici.extent = {n, n, n};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Lut3D l;
    if (vkCreateImage(device_, &ici, nullptr, &l.image) != VK_SUCCESS) {
        LOG_ERROR("LUT: vkCreateImage(%u^3) failed", n);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, l.image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device_, &ai, nullptr, &l.memory) != VK_SUCCESS) {
        vkDestroyImage(device_, l.image, nullptr);
        LOG_ERROR("LUT: memory alloc failed");
        return false;
    }
    vkBindImageMemory(device_, l.image, l.memory, 0);

    // Staging buffer, copied with a one-off blocking submit (LUT loads are user actions,
    // never per frame).
    const VkDeviceSize bytes = (VkDeviceSize)n * n * n * 4 * sizeof(uint16_t);
    VkBuffer sbuf = VK_NULL_HANDLE;
    VkDeviceMemory smem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device_, &bci, nullptr, &sbuf);
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(device_, sbuf, &breq);
    VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bai.allocationSize = breq.size;
    bai.memoryTypeIndex = FindMemoryType(
        breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &bai, nullptr, &smem);
    vkBindBufferMemory(device_, sbuf, smem, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, smem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, rgba16f, (size_t)bytes);
    vkUnmapMemory(device_, smem);

    ring_.submitImmediate([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = l.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy r = {};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {n, n, n};
        vkCmdCopyBufferToImage(cmd, sbuf, l.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    });
    vkDestroyBuffer(device_, sbuf, nullptr);
    vkFreeMemory(device_, smem, nullptr);

    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = l.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vci, nullptr, &l.view) != VK_SUCCESS) {
        vkDestroyImage(device_, l.image, nullptr);
        vkFreeMemory(device_, l.memory, nullptr);
        LOG_ERROR("LUT: image view failed");
        return false;
    }
    l.size = n;
    DestroyLut(dst, /*deferred=*/true);  // a queued frame may still sample the old one
    dst = l;
    return true;
}

void VulkanRenderer::DestroyLut(Lut3D& l, bool deferred) {
    if (l.view || l.image || l.memory) {
        VkDevice dev = device_;
        const VkImageView v = l.view;
        const VkImage i = l.image;
        const VkDeviceMemory m = l.memory;
        auto destroy = [dev, v, i, m]() {
            if (v) vkDestroyImageView(dev, v, nullptr);
            if (i) vkDestroyImage(dev, i, nullptr);
            if (m) vkFreeMemory(dev, m, nullptr);
        };
        if (deferred) ring_.deferDestroy(destroy);
        else destroy();
    }
    l = Lut3D{};
}

bool VulkanRenderer::UploadLut3D(const float* rgb, uint32_t size, float domainMin, float domainMax) {
    if (!rgb || size < 2 || size > 256 || device_ == VK_NULL_HANDLE) return false;
    const size_t count = (size_t)size * size * size;
    std::vector<uint16_t> px(count * 4);
    for (size_t i = 0; i < count; ++i) {
        px[i * 4 + 0] = FloatToHalf(rgb[i * 3 + 0]);
        px[i * 4 + 1] = FloatToHalf(rgb[i * 3 + 1]);
        px[i * 4 + 2] = FloatToHalf(rgb[i * 3 + 2]);
        px[i * 4 + 3] = 0x3c00u;  // 1.0
    }
    if (!CreateLutImage(lut_, size, px.data())) return false;
    lut_.domainMin = domainMin;
    lut_.domainMax = domainMax;
    LOG_INFO("LUT: uploaded %u^3 (domain %.3f..%.3f)", size, domainMin, domainMax);
    return true;
}

void VulkanRenderer::ClearLut3D() { DestroyLut(lut_, /*deferred=*/true); }

} // namespace mp
