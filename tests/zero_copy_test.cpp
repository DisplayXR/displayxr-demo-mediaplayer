// SPDX-License-Identifier: Apache-2.0
//
// Zero-copy upload (#238 phase 3, MEDIAPLAYER_ZERO_COPY_UPLOAD), the parts that need no GPU:
//  1. media/HostBlocks.h: blocks are page-aligned and 64 KB-rounded (every GPU's import
//     alignment divides that), ids are unique and never reused, a block goes back to the pool
//     only when the LAST copy of its lease goes (the ring frame's and the renderer's), the pool
//     stops at its cap, a block's memory outlives the pool while someone holds it (the
//     renderer's import), and leases taken and dropped on several threads stay consistent.
//  2. A v210 frame with external planes: v210::Shape takes it (by the blocks' sizes, plane[]
//     empty) and refuses short blocks, v210::EyeRows points into the blocks, and the upload
//     thread's LayoutFor refuses it (the render thread records the GPU's read in place; the
//     upload thread must never copy from the empty planes).
//  3. The renderer's hold rule, simulated: a block leased into a ring frame, then kept for N
//     frames in flight after the producer reused the ring buffer, is never handed out again
//     until the hold for its frame is dropped.
//  4. LiveSourceBase's publishes clear external planes left in a reused ring buffer.
// Pure: no GPU, no SDK.

#include "media/FrameRing.h"
#include "media/HostBlocks.h"
#include "media/V210.h"
#include "pro/LiveSourceBase.h"
#include "rhi/AsyncUpload.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace mp;

void Blocks() {
    auto pool = hostmem::Pool::Create(22118400, 3);  // a 2160p v210 eye
    CHECK(pool->BlockBytes() % hostmem::kGranule == 0 && pool->BlockBytes() >= 22118400, "rounded to 64 KB");
    hostmem::Lease a = pool->Take(), b = pool->Take(), c = pool->Take();
    CHECK(a && b && c, "three blocks");
    CHECK(!pool->Take(), "the cap holds");
    CHECK(((uintptr_t)a.block->p % 4096) == 0 && ((uintptr_t)b.block->p % 4096) == 0, "page-aligned");
    std::set<uint64_t> ids = {a.block->id, b.block->id, c.block->id};
    CHECK(ids.size() == 3, "unique ids");
    a.block->p[0] = 1;
    a.block->p[a.block->size - 1] = 2;  // the whole block is writable
    // A second copy of a's hold (the renderer's): a is not back until both are gone.
    std::shared_ptr<void> rendererCopy = a.hold;
    const uint64_t aid = a.block->id;
    const uint8_t* ap = a.block->p;
    a = hostmem::Lease{};
    CHECK(pool->Out() == 3 && !pool->Take(), "still held by the renderer's copy");
    rendererCopy.reset();
    CHECK(pool->Out() == 2, "back once the last hold goes");
    hostmem::Lease d = pool->Take();
    CHECK(d && d.block->id == aid && d.block->p == ap, "the same block comes back (same id: the import is reused)");
    CHECK(pool->Made() == 3, "no new allocation");

    // The memory outlives the pool while someone holds the block (an import).
    std::shared_ptr<const hostmem::Block> keep = b.block;
    b = c = d = hostmem::Lease{};
    pool.reset();
    keep->p[0] = 7;  // still valid memory
    CHECK(keep->p[0] == 7, "block memory alive after the pool");
    keep.reset();

    // A new pool's blocks never reuse an id.
    auto p2 = hostmem::Pool::Create(4096, 2);
    hostmem::Lease e = p2->Take();
    CHECK(e && ids.count(e.block->id) == 0, "ids are never reused");

    // Threads: two producers take and drop leases, a third keeps copies a while.
    auto p3 = hostmem::Pool::Create(65536, 16);
    std::atomic<int> got{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 3; ++t)
        th.emplace_back([&, t] {
            std::deque<std::shared_ptr<void>> held;
            for (int i = 0; i < 20000; ++i) {
                hostmem::Lease l = p3->Take();
                if (l) {
                    ++got;
                    l.block->p[0] = (uint8_t)t;
                    if (t == 2) held.push_back(l.hold);
                    if (held.size() > 3) held.pop_front();
                }
            }
        });
    for (auto& x : th) x.join();
    CHECK(got > 0 && p3->Out() == 0 && p3->Made() <= 16, "threads: every lease back, cap kept");
}

FrameRing::Frame ExtFrame(const hostmem::Lease l[2], int eyeW, int h, size_t bytes) {
    FrameRing::Frame f;
    f.format = PixFormat::V210;
    f.eyes = 2;
    f.width = 2 * eyeW;
    f.height = h;
    for (int e = 0; e < 2; ++e)
        f.ext[e] = FrameRing::Frame::ExtPlane{l[e].block->p, bytes, l[e].block->id, l[e].block, l[e].hold};
    return f;
}

void ExtFrames() {
    const int eyeW = 3840, h = 2160;
    const size_t bytes = v210::RowBytes(eyeW) * (size_t)h;
    auto pool = hostmem::Pool::Create(bytes, 4);
    hostmem::Lease l[2] = {pool->Take(), pool->Take()};
    FrameRing::Frame f = ExtFrame(l, eyeW, h, bytes);
    int eyes = 0, w = 0;
    CHECK(f.HasExt() && f.plane[0].empty(), "external planes, plane[] empty");
    CHECK(v210::Shape(f, eyes, w) && eyes == 2 && w == eyeW, "Shape takes an external-plane frame");
    CHECK(v210::EyeRows(f, 0) == l[0].block->p && v210::EyeRows(f, 1) == l[1].block->p, "EyeRows points into the blocks");
    asyncup::Layout L;
    CHECK(!asyncup::LayoutFor(f, L, true), "the upload thread never stages an external-plane frame");
    f.ext[1].bytes = bytes - 1;
    CHECK(!v210::Shape(f, eyes, w), "a short block is refused");
    f.ext[1].bytes = bytes;
    f.ext[1].data = nullptr;
    CHECK(!v210::Shape(f, eyes, w), "a missing eye block is refused");
    // A plain v210 frame still stages (the old path is unchanged).
    FrameRing::Frame g;
    g.format = PixFormat::V210;
    g.eyes = 2;
    g.width = 2 * eyeW;
    g.height = h;
    g.plane[0].resize(bytes);
    g.plane[1].resize(bytes);
    CHECK(asyncup::LayoutFor(g, L, true) && L.format == 3, "a plane[] v210 frame still stages");
}

// The renderer's rule: hold a frame's leases until frame serial S has passed its fence (S <= the
// completed serial). The producer's ring may reuse its buffer meanwhile (dropping its own copy):
// the block must not come back to the pool before the renderer drops its hold.
void HoldRule() {
    auto pool = hostmem::Pool::Create(65536, 64);
    const uint64_t inFlight = 3;
    std::vector<std::pair<uint64_t, std::shared_ptr<void>>> holds;
    std::vector<std::pair<uint64_t, uint64_t>> readBy;  // (frame serial, block id) the GPU "reads"
    FrameRing::Frame ring[3];
    bool bad = false;
    for (uint64_t serial = 1; serial <= 500; ++serial) {
        // OpenFrame: frames <= serial - inFlight are done
        if (serial > inFlight) {
            const uint64_t done = serial - inFlight;
            size_t k = 0;
            for (auto& h : holds)
                if (h.first > done) holds[k++] = std::move(h);
            holds.resize(k);
        }
        // the producer fills its next ring buffer (dropping that buffer's old lease)
        FrameRing::Frame& f = ring[serial % 3];
        hostmem::Lease l[2] = {pool->Take(), pool->Take()};
        if (!l[0] || !l[1]) {
            bad = true;
            break;
        }
        // a block still read by an unfinished frame must never be handed out
        for (auto& r : readBy)
            if (r.first + inFlight > serial && (r.second == l[0].block->id || r.second == l[1].block->id)) bad = true;
        f = ExtFrame(l, 64, 2, v210::RowBytes(64) * 2);
        // the renderer records it this frame
        for (int e = 0; e < 2; ++e) {
            holds.emplace_back(serial, f.ext[e].hold);
            readBy.emplace_back(serial, f.ext[e].id);
        }
    }
    CHECK(!bad, "a block the GPU may still read is never handed out again");
    CHECK(pool->Made() <= 2 * (3 + 3 + 1), "the pool stays at ring + frames in flight");
}

class Src : public LiveSourceBase {
public:
    Src() { Ring().SetQueueDepth(0); }  // latest-wins: one producer buffer, reused every publish
    void Stop() override {}
    const char* Kind() const override { return "test"; }
    using LiveSourceBase::Publish;
};

void PublishClears() {
    Src src;
    auto pool = hostmem::Pool::Create(65536, 8);
    // Leave external planes in every buffer of the ring, then publish ordinary frames through each.
    for (int i = 0; i < FrameRing::kBufferCount; ++i) {
        hostmem::Lease l[2] = {pool->Take(), pool->Take()};
        FrameRing::Frame& f = src.Ring().BufferSlot(i);
        f.ext[0] = FrameRing::Frame::ExtPlane{l[0].block->p, 1, l[0].block->id, l[0].block, l[0].hold};
        f.ext[1] = FrameRing::Frame::ExtPlane{l[1].block->p, 1, l[1].block->id, l[1].block, l[1].hold};
    }
    CHECK(pool->Out() == 2 * FrameRing::kBufferCount, "leased into the ring");
    livepix::Nv12 n;
    n.Resize(64, 32);
    for (int i = 0; i < 2 * FrameRing::kBufferCount; ++i) {
        src.Publish(n, false, 0.0, nullptr, -1);
        const FrameRing::Frame* f = src.Ring().AcquireLatest();
        CHECK(f && !f->HasExt() && f->format == PixFormat::NV12, "a publish clears external planes");
    }
    CHECK(pool->Out() < 2 * FrameRing::kBufferCount, "and gives their blocks back");
}

}  // namespace

int main() {
    Blocks();
    ExtFrames();
    HoldRule();
    PublishClears();
    if (g_failures) {
        std::fprintf(stderr, "zero_copy_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("zero_copy_test: all passed\n");
    return 0;
}
