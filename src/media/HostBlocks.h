// SPDX-License-Identifier: Apache-2.0
//
// HostBlocks - pooled, page-aligned host memory that a producer fills and the GPU reads in
// place (VK_EXT_external_memory_host; #238 phase 3, MEDIAPLAYER_ZERO_COPY_UPLOAD).
//
// Two lifetimes, kept apart on purpose:
//   - the MEMORY (Block): allocated once, freed when the last shared_ptr to it goes. The
//     renderer's import of a block holds one, because imported host memory must outlive the
//     VkDeviceMemory made from it (Vulkan spec, VkImportMemoryHostPointerInfoEXT).
//   - the CONTENTS (a Lease): the producer takes a free block, fills it, publishes it; the
//     block goes back to the pool's free list when the last Lease copy goes. A ring frame holds
//     one; the renderer copies it for each GPU frame that reads the block and drops it when
//     that frame has passed its fence. So a block is never refilled while the GPU reads it.
//
// Header-only, trunk (no SDK, no Vulkan): tests/zero_copy_test.cpp.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mp::hostmem {

// One page-aligned allocation. `id` is unique for the process's lifetime (never reused, unlike
// an address): the renderer's import cache is keyed by it.
struct Block {
    uint8_t* p = nullptr;
    size_t size = 0;
    uint64_t id = 0;
};

// Sizes are rounded up to 64 KB (a multiple of every GPU's import alignment seen: 4 KB on
// Intel / NVIDIA / AMD).
constexpr size_t kGranule = 64 * 1024;
inline size_t RoundUp(size_t n) { return (n + kGranule - 1) & ~(kGranule - 1); }

inline std::atomic<uint64_t>& NextId() {
    static std::atomic<uint64_t> id{1};
    return id;
}

// A block of at least `bytes`, optionally on NUMA node `node` (Windows; -1 = the OS's choice).
// Null on failure. Freed when the last reference goes.
inline std::shared_ptr<const Block> NewBlock(size_t bytes, int node = -1) {
    const size_t size = RoundUp(bytes);
    void* p = nullptr;
#if defined(_WIN32)
    p = node >= 0 ? VirtualAllocExNuma(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE,
                                       (DWORD)node)
                  : VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    (void)node;
    if (posix_memalign(&p, kGranule, size) != 0) p = nullptr;
#endif
    if (!p) return nullptr;
    Block* b = new Block{static_cast<uint8_t*>(p), size, NextId().fetch_add(1)};
    return std::shared_ptr<const Block>(b, [](const Block* x) {
#if defined(_WIN32)
        VirtualFree(x->p, 0, MEM_RELEASE);
#else
        std::free(x->p);
#endif
        delete x;
    });
}

// A block taken from a Pool: `block` keeps the memory, `hold` keeps it out of the free list
// (copy it to keep the contents; the last copy returns the block).
struct Lease {
    std::shared_ptr<const Block> block;
    std::shared_ptr<void> hold;
    explicit operator bool() const { return block != nullptr; }
};

// Blocks of one size. Grows on demand up to maxBlocks, then Take fails (the caller copies
// instead, or skips a frame). Thread-safe; outlives every Lease it handed out.
class Pool : public std::enable_shared_from_this<Pool> {
public:
    static std::shared_ptr<Pool> Create(size_t bytes, int maxBlocks, int node = -1) {
        return std::shared_ptr<Pool>(new Pool(bytes, maxBlocks, node));
    }
    size_t BlockBytes() const { return bytes_; }

    Lease Take() {
        std::shared_ptr<const Block> b;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!free_.empty()) {
                b = std::move(free_.back());
                free_.pop_back();
            } else if (made_ < max_) {
                b = NewBlock(bytes_, node_);
                if (b) ++made_;
            }
            if (b) ++out_;
        }
        if (!b) return {};
        std::shared_ptr<Pool> self = shared_from_this();
        const Block* raw = b.get();
        // The hold's deleter puts the block back; it owns a reference to the block itself, so the
        // memory stays alive until it is back in the list (or the pool is gone).
        std::shared_ptr<void> hold(const_cast<Block*>(raw), [self, b](void*) { self->Give(b); });
        return {b, hold};
    }
    int Made() const {
        std::lock_guard<std::mutex> lk(m_);
        return made_;
    }
    int Out() const {  // leased now
        std::lock_guard<std::mutex> lk(m_);
        return out_;
    }

private:
    Pool(size_t bytes, int maxBlocks, int node) : bytes_(RoundUp(bytes)), max_(maxBlocks), node_(node) {}
    void Give(const std::shared_ptr<const Block>& b) {
        std::lock_guard<std::mutex> lk(m_);
        free_.push_back(b);
        --out_;
    }
    mutable std::mutex m_;
    std::vector<std::shared_ptr<const Block>> free_;
    size_t bytes_;
    int max_;
    int node_;
    int made_ = 0, out_ = 0;
};

}  // namespace mp::hostmem
