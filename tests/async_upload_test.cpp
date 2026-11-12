// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the upload copy off the render thread (rhi/AsyncUpload.h) and the ring's pin
// (media/FrameRing.h, queue mode):
//   - the switch, the slot count, the staging layout of I420 / NV12 / P016 frames;
//   - the slot state machine: free -> filling -> ready -> in flight -> free, a slot never
//     reserved while the GPU frame that read it has not finished (Retire), stale tickets,
//     the newest-wins drop of fills nobody will take, cancel, a failed copy;
//   - the pin: the producer never gets a pinned buffer back (a drop, a trim or the consumer
//     moving on parks it), and a pinned ring delivers exactly the frames, drops, trims and
//     repeats an unpinned one does;
//   - the Uploader against a producer thread, with plain memory standing in for the GPU
//     staging: frames copied ahead and on demand arrive whole and as the right frame, no slot
//     is written while a "GPU frame" still reads it, no free slot falls back, and it shuts
//     down while the producer keeps publishing.
// Pure (no Vulkan).

#include "media/FrameRing.h"
#include "rhi/AsyncUpload.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using mp::FrameRing;
using mp::PixFormat;
namespace au = mp::asyncup;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// ---------------------------------------------------------------------------
// Switch, slot count, layout
// ---------------------------------------------------------------------------

void SwitchAndSlots() {
    CHECK(au::Enabled(nullptr), "unset: on");
    CHECK(au::Enabled("1") && au::Enabled("on") && au::Enabled("yes") && au::Enabled(""), "anything else: on");
    CHECK(!au::Enabled("0") && !au::Enabled("off") && !au::Enabled("FALSE") && !au::Enabled("No"),
          "0 / off / false / no: off");
    CHECK(au::SlotCount(3) == 5, "3 frames in flight: 5 slots (in flight + filling + ready)");
    CHECK(au::SlotCount(1) == 3, "1 in flight: 3");
    CHECK(au::SlotCount(0) == 3, "never fewer than 3");
    CHECK(au::SlotCount(8) == 10, "8 in flight: 10");
    CHECK(au::SlotCount(100) == au::kMaxSlots, "capped");
}

void Layouts() {
    au::Layout l;
    CHECK(au::LayoutFor(0, 7, 5, l), "I420 7x5");
    CHECK(l.planes == 3 && l.w[0] == 7 && l.h[0] == 5 && l.w[1] == 4 && l.h[1] == 3 && l.w[2] == 4 && l.h[2] == 3,
          "I420 chroma is (w+1)/2 x (h+1)/2");
    CHECK(l.rowBytes[0] == 7 && l.rowBytes[1] == 4 && l.offset[0] == 0 && l.offset[1] == 256 && l.offset[2] == 512 &&
              l.total == 768,
          "I420 planes packed at 256-byte aligned offsets");

    CHECK(au::LayoutFor(1, 3840, 2160, l), "NV12 UHD");
    CHECK(l.planes == 2 && l.bpt[0] == 1 && l.bpt[1] == 2 && l.rowBytes[1] == 3840 && l.offset[1] == 8294400 &&
              l.total == 8294400 + 4147200,
          "NV12: Y then interleaved UV");

    CHECK(au::LayoutFor(2, 7680, 2160, l), "P016 two UHD eyes");
    CHECK(l.planes == 2 && l.bpt[0] == 2 && l.bpt[1] == 4 && l.rowBytes[0] == 15360 && l.rowBytes[1] == 15360 &&
              l.total == 33177600u + 16588800u,
          "P016 7680x2160: 47.5 MB");
    for (int p = 0; p < l.planes; ++p) CHECK(l.offset[p] % au::kPlaneAlign == 0, "aligned");

    CHECK(!au::LayoutFor(3, 16, 16, l) && !au::LayoutFor(-1, 16, 16, l), "unknown format");
    CHECK(!au::LayoutFor(1, 0, 16, l) && !au::LayoutFor(1, 16, 0, l), "empty frame");

    FrameRing::Frame f;
    f.width = 8;
    f.height = 4;
    f.format = PixFormat::NV12;
    f.plane[0].assign(32, 0);
    f.plane[1].assign(16, 0);
    CHECK(au::LayoutFor(f, l) && l.format == 1, "an NV12 ring frame");
    f.plane[1].assign(15, 0);
    CHECK(!au::LayoutFor(f, l), "a plane shorter than its layout: the render thread uploads it");
    f.plane[1].assign(16, 0);
    f.gpu = true;
    CHECK(!au::LayoutFor(f, l), "a zero-copy frame is not staged");
    f.gpu = false;
    f.format = PixFormat::RGBA;
    CHECK(!au::LayoutFor(f, l), "an RGBA frame is not staged");
    CHECK(au::UploadFormat(PixFormat::I420) == 0 && au::UploadFormat(PixFormat::NV12) == 1 &&
              au::UploadFormat(PixFormat::P016) == 2 && au::UploadFormat(PixFormat::RGBA) == -1,
          "UploadYUV's format numbers");
}

// ---------------------------------------------------------------------------
// Slot state machine
// ---------------------------------------------------------------------------

void SlotLife() {
    int srcA = 0, srcB = 0;  // two sources (only their addresses matter)
    au::SlotTable t(3);
    CHECK(t.Count() == 3 && t.FreeCount() == 3, "all free");

    const au::Ticket a = t.Reserve(&srcA, 1, true);
    CHECK(a.Valid() && t.State(a.slot) == au::SlotState::Filling && t.Filling(a) && !t.Ready(a), "free -> filling");
    CHECK(!t.Record(a, 1), "a slot still being written is never recorded");
    CHECK(t.Finish(a, true) && t.State(a.slot) == au::SlotState::Ready && t.Ready(a) && t.Ahead(a), "-> ready");
    const au::Ticket found = t.Find(&srcA, 1);
    CHECK(found.slot == a.slot && found.gen == a.gen, "found by source and serial");
    CHECK(!t.Find(&srcB, 1).Valid() && !t.Find(&srcA, 2).Valid(), "only that frame of that source");
    t.Claim(a);
    CHECK(t.Record(a, 10) && t.State(a.slot) == au::SlotState::InFlight && t.Frame(a.slot) == 10, "-> in flight");
    CHECK(!t.Find(&srcA, 1).Valid(), "an in-flight fill is not found again");

    // Fill the other two, then nothing is free while slot a's frame runs.
    const au::Ticket b = t.Reserve(&srcA, 2, false);
    const au::Ticket c = t.Reserve(&srcA, 3, false);
    CHECK(b.Valid() && c.Valid() && b.slot != a.slot && c.slot != a.slot && b.slot != c.slot, "distinct slots");
    CHECK(!t.Reserve(&srcA, 4, false).Valid() && t.FreeCount() == 0, "none free: no reserve");

    CHECK(t.Retire(9) == 0 && t.State(a.slot) == au::SlotState::InFlight, "frame 10 not finished: still in flight");
    CHECK(!t.Reserve(&srcA, 4, false).Valid(), "never a slot the GPU may still read");
    CHECK(t.Retire(10) == 1 && t.State(a.slot) == au::SlotState::Free, "frame 10 finished: free");
    const au::Ticket d = t.Reserve(&srcA, 4, false);
    CHECK(d.slot == a.slot && d.gen != a.gen, "reused with a new generation");
    CHECK(!t.Ready(a) && !t.Filling(a) && !t.Record(a, 11), "the old ticket names nothing any more");

    // A failed copy (no staging memory) frees the slot; it is not a skip.
    CHECK(!t.Finish(d, false) && t.State(d.slot) == au::SlotState::Free && t.Skipped() == 0, "failed copy: free");
    CHECK(t.Fills() == 4 && t.LastFill(d.slot) == 4, "fills counted");
}

void SlotDrops() {
    int srcA = 0, srcB = 0;
    au::SlotTable t(5);
    const au::Ticket r1 = t.Reserve(&srcA, 1, true);
    const au::Ticket r2 = t.Reserve(&srcA, 2, true);
    const au::Ticket f3 = t.Reserve(&srcA, 3, true);  // still being copied
    const au::Ticket r4 = t.Reserve(&srcA, 4, true);
    const au::Ticket rb = t.Reserve(&srcB, 9, true);
    t.Finish(r1, true);
    t.Finish(r2, true);
    t.Finish(r4, true);
    t.Finish(rb, true);
    t.Claim(r2);  // claimed: no drop touches it

    // The render thread takes frame 4: older unclaimed fills and other sources go.
    const int n = t.DropStale(&srcA, 4);
    CHECK(n == 3, "frame 1 (ready), frame 3 (copying) and the other source's are dropped");
    CHECK(t.State(r1.slot) == au::SlotState::Free && t.State(rb.slot) == au::SlotState::Free,
          "ready -> free: the GPU never read them");
    CHECK(t.Ready(r2), "the claimed fill stays");
    CHECK(t.Ready(r4), "the taken frame itself stays");
    CHECK(t.State(f3.slot) == au::SlotState::Filling && !t.Find(&srcA, 3).Valid(),
          "a running copy is not freed under the upload thread, and not found any more");
    CHECK(t.Skipped() == 2, "the two ready ones count now");
    CHECK(!t.Finish(f3, true) && t.State(f3.slot) == au::SlotState::Free && t.Skipped() == 3,
          "the dropped copy is freed when it ends, and counts then");

    // Cancel: a claimed fill the render thread will not record.
    t.Cancel(r2);
    CHECK(t.State(r2.slot) == au::SlotState::Free && t.Skipped() == 4, "cancel a ready fill: free, skipped");
    const au::Ticket f5 = t.Reserve(&srcA, 5, false);
    t.Claim(f5);
    t.Cancel(f5);
    CHECK(t.Filling(f5), "cancel a running copy: it runs on");
    CHECK(!t.Finish(f5, true) && !t.Ready(f5) && t.Skipped() == 5, "and is freed when it ends");

    // A source change drops everything unclaimed of the others.
    const au::Ticket g = t.Reserve(&srcB, 10, true);
    t.Finish(g, true);
    CHECK(t.DropOtherSources(&srcB) == 1 && t.Ready(r4) == false, "srcA's frame 4 goes, srcB's stays");
    CHECK(t.Ready(g), "srcB's fill stays");
    CHECK(t.DropOtherSources(nullptr) == 1 && t.FreeCount() == 5, "nullptr: all of them");
}

// Random operations on a table; the invariant the GPU depends on: a slot recorded for frame F
// is never reserved again before Retire(>= F), and a slot being written is never recorded.
void SlotInvariantSoak() {
    std::mt19937 rng(42);
    au::SlotTable t(5);
    int src = 0;
    std::vector<au::Ticket> filling, ready;
    uint64_t frame = 1, completed = 0;
    int64_t serial = 0;
    uint64_t inFlightFrame[au::kMaxSlots] = {};
    bool inFlight[au::kMaxSlots] = {};
    for (int step = 0; step < 20000; ++step) {
        const int op = (int)(rng() % 5);
        if (op == 0) {
            const au::Ticket r = t.Reserve(&src, ++serial, rng() % 2 == 0);
            if (r.Valid()) {
                CHECK(!inFlight[r.slot], "reserved a slot a GPU frame still reads");
                filling.push_back(r);
            }
        } else if (op == 1 && !filling.empty()) {
            const au::Ticket r = filling.front();
            filling.erase(filling.begin());
            if (t.Finish(r, rng() % 10 != 0)) ready.push_back(r);
        } else if (op == 2 && !ready.empty()) {
            const au::Ticket r = ready.back();
            ready.pop_back();
            if (t.Record(r, frame)) {
                inFlight[r.slot] = true;
                inFlightFrame[r.slot] = frame;
            }
        } else if (op == 3) {
            ++frame;  // the next GPU frame; the one 3 back has finished
            if (frame > 3) completed = frame - 3;
            t.Retire(completed);
            for (int s = 0; s < t.Count(); ++s)
                if (inFlight[s] && inFlightFrame[s] <= completed) inFlight[s] = false;
        } else if (op == 4 && !ready.empty()) {
            t.DropStale(&src, serial);  // drop everything older than the newest
        }
        for (const au::Ticket& r : filling) CHECK(!t.Record(r, frame), "recorded a slot still being written");
        for (int s = 0; s < t.Count(); ++s)
            if (inFlight[s]) CHECK(t.State(s) == au::SlotState::InFlight, "an in-flight slot left early");
        if (g_failures) return;
    }
}

// ---------------------------------------------------------------------------
// FrameRing pin (queue mode)
// ---------------------------------------------------------------------------

void Mark(FrameRing::Frame& f, uint8_t v) {
    f.plane[0].assign(4, v);
    f.format = PixFormat::NV12;
    f.width = 2;
    f.height = 2;
}

void RingPinBasics() {
    {
        FrameRing lw;  // latest-wins
        Mark(lw.WriteBuffer(), 1);
        lw.Publish();
        CHECK(lw.PinNextQueued(0) == nullptr, "latest-wins: no pin");
    }
    FrameRing ring;
    ring.SetQueueDepth(3);
    for (int k = 1; k <= 3; ++k) {
        Mark(ring.WriteBuffer(), (uint8_t)k);
        ring.Publish();
    }
    const FrameRing::Frame* p = ring.PinNextQueued(0);
    CHECK(p && p->serial == 1 && ring.IsPinned(p), "the oldest waiting frame");
    CHECK(ring.PinNextQueued(0) == nullptr, "one pin at a time");
    ring.Unpin(p);
    CHECK(!ring.IsPinned(p), "unpinned");
    p = ring.PinNextQueued(1);
    CHECK(p && p->serial == 2, "the next one after serial 1");
    ring.Unpin(p);
    CHECK(ring.PinNextQueued(3) == nullptr, "nothing newer than 3");

    // A gpu frame is skipped.
    FrameRing g;
    g.SetQueueDepth(3);
    g.WriteBuffer().gpu = true;
    g.Publish();
    Mark(g.WriteBuffer(), 2);
    g.WriteBuffer().gpu = false;
    g.Publish();
    p = g.PinNextQueued(0);
    CHECK(p && p->serial == 2, "the zero-copy frame is not pinned, the next CPU frame is");
    g.Unpin(p);

    // The depth clamp keeps room for the pin: 9 asks for 4, as before.
    FrameRing d;
    d.SetQueueDepth(9);
    int drops = 0;
    for (int k = 1; k <= 6; ++k) {
        Mark(d.WriteBuffer(), (uint8_t)k);
        drops += d.Publish() ? 1 : 0;
    }
    CHECK(drops == 2, "depth clamped to 4");

    // The publish hook: every Publish, on the producer's thread; removed, never again.
    static int calls = 0;
    FrameRing h;
    h.SetQueueDepth(2);
    h.SetPublishHook([](void* ctx) { ++*static_cast<int*>(ctx); }, &calls);
    h.Publish();
    h.Publish();
    h.SetPublishHook(nullptr, nullptr);
    h.Publish();
    CHECK(calls == 2, "hook per publish, none after removal");
}

// A pinned frame keeps its pixels while the producer drops it, the consumer moves past it, or
// a trim retires it: its buffer never comes back as WriteBuffer() until Unpin.
void RingPinNeverToProducer() {
    FrameRing ring;
    ring.SetQueueDepth(2);
    Mark(ring.WriteBuffer(), 1);
    ring.Publish();
    Mark(ring.WriteBuffer(), 2);
    ring.Publish();
    const FrameRing::Frame* p = ring.PinNextQueued(0);  // frame 1, at the head of a full queue
    CHECK(p && p->serial == 1, "pinned frame 1");
    for (int k = 3; k < 40; ++k) {  // the producer races on; frame 1 is dropped at once
        FrameRing::Frame& w = ring.WriteBuffer();
        CHECK(&w != p, "the producer never gets the pinned buffer");
        Mark(w, (uint8_t)k);
        ring.Publish();
        if (k % 3 == 0) ring.AcquireLatest();  // the consumer moves on too
    }
    CHECK(p->serial == 1 && p->plane[0][0] == 1, "the pinned frame's pixels never changed");
    ring.AcquireLatest();  // empty the queue, so the next publish takes a free buffer
    ring.AcquireLatest();
    ring.Unpin(p);  // released while pinned: it goes to the free list now
    Mark(ring.WriteBuffer(), 0);
    ring.Publish();
    CHECK(&ring.WriteBuffer() == p, "after Unpin the buffer goes back to the producer");

    // Taken by the consumer while pinned, then the consumer moves past it.
    FrameRing r2;
    r2.SetQueueDepth(3);
    Mark(r2.WriteBuffer(), 1);
    r2.Publish();
    const FrameRing::Frame* q = r2.PinNextQueued(0);
    CHECK(r2.AcquireLatest() == q, "the consumer takes the pinned frame (both read it)");
    for (int k = 2; k < 30; ++k) {
        CHECK(&r2.WriteBuffer() != q, "...and the producer still never gets it");
        Mark(r2.WriteBuffer(), (uint8_t)k);
        r2.Publish();
        r2.AcquireLatest();
    }
    CHECK(q->plane[0][0] == 1, "unchanged");
    r2.Unpin(q);
}

// Pins change nothing the consumer or the counters see: a pinned ring and an unpinned twin
// fed the same publishes and takes deliver the same frames, drops, trims and repeats, while
// the pinned frame's pixels never change and its buffer is never the producer's.
void RingPinTwinSoak() {
    std::mt19937 rng(7);
    FrameRing a, b;
    a.SetQueueDepth(3);
    b.SetQueueDepth(3);
    const FrameRing::Frame* pin = nullptr;
    uint8_t pinMark = 0;
    int64_t lastPinned = 0;
    int k = 0;
    long dropsA = 0, dropsB = 0;
    for (int step = 0; step < 50000; ++step) {
        const unsigned r = rng() % 100;
        if (r < 48) {  // publish to both
            ++k;
            FrameRing::Frame& wa = a.WriteBuffer();
            if (pin) CHECK(&wa != pin, "the producer got the pinned buffer");
            Mark(wa, (uint8_t)k);
            Mark(b.WriteBuffer(), (uint8_t)k);
            dropsA += a.Publish() ? 1 : 0;
            dropsB += b.Publish() ? 1 : 0;
        } else if (r < 96) {  // the consumer's take, one per "refresh"
            const FrameRing::Frame* fa = a.AcquireLatest();
            const FrameRing::Frame* fb = b.AcquireLatest();
            CHECK((fa == nullptr) == (fb == nullptr), "both rings give a frame, or neither");
            if (fa && fb) CHECK(fa->serial == fb->serial && fa->plane[0][0] == fb->plane[0][0], "the same frame");
        } else if (!pin) {  // the upload thread pins the next waiting frame
            pin = a.PinNextQueued(lastPinned);
            if (pin) {
                lastPinned = pin->serial;
                pinMark = pin->plane[0][0];
            }
        } else {  // its copy is done
            CHECK(pin->plane[0][0] == pinMark && pin->serial == lastPinned, "a pinned frame changed");
            a.Unpin(pin);
            pin = nullptr;
        }
        if (g_failures) return;
    }
    CHECK(dropsA == dropsB, "the same drops");
    CHECK(a.TrimmedFrames() == b.TrimmedFrames(), "the same trims");
    CHECK(a.RepeatedFrames() == b.RepeatedFrames(), "the same repeats");
    CHECK(dropsA > 0 && a.RepeatedFrames() > 0, "the soak saw drops and repeats");
}

// ---------------------------------------------------------------------------
// Uploader, with plain memory for the staging
// ---------------------------------------------------------------------------

// A frame's pixels: every row one byte value, from the frame number and the row, so a row of
// another frame (a torn copy, a slot rewritten under the GPU) shows at any sample.
uint8_t RowY(int64_t k, size_t r) { return (uint8_t)(k * 13 + (int64_t)r); }
uint8_t RowUV(int64_t k, size_t r) { return (uint8_t)(k * 7 + (int64_t)r * 3 + 1); }

void Fill(FrameRing::Frame& f, int64_t k, int w, int h) {
    const size_t yRow = (size_t)w, uvRow = (size_t)((w + 1) / 2) * 2, uvRows = (size_t)((h + 1) / 2);
    f.plane[0].resize(yRow * h);
    f.plane[1].resize(uvRow * uvRows);
    for (size_t r = 0; r < (size_t)h; ++r) std::memset(f.plane[0].data() + r * yRow, RowY(k, r), yRow);
    for (size_t r = 0; r < uvRows; ++r) std::memset(f.plane[1].data() + r * uvRow, RowUV(k, r), uvRow);
    f.width = w;
    f.height = h;
    f.format = PixFormat::NV12;
    f.gpu = false;
}

struct FakeStaging {
    std::vector<uint8_t> mem[au::kMaxSlots];
    std::atomic<int> allocs{0};
    std::atomic<bool> fail{false};
    au::Uploader::EnsureFn Fn() {
        return [this](int slot, size_t bytes) -> void* {
            if (fail) return nullptr;
            if (mem[slot].size() < bytes) {
                mem[slot].resize(bytes);
                ++allocs;
            }
            return mem[slot].data();
        };
    }
    // Is the slot frame k, every row? (sampled at the row's start, middle and end)
    bool Holds(int slot, const au::Layout& l, int64_t k) const {
        const std::vector<uint8_t>& m = mem[slot];
        if (m.size() < l.total) return false;
        for (int p = 0; p < l.planes; ++p)
            for (size_t r = 0; r < l.h[p]; ++r) {
                const uint8_t want = p == 0 ? RowY(k, r) : RowUV(k, r);
                const uint8_t* row = m.data() + l.offset[p] + r * l.rowBytes[p];
                if (row[0] != want || row[l.rowBytes[p] / 2] != want || row[l.rowBytes[p] - 1] != want) return false;
            }
        return true;
    }
};

void UploaderOnDemand() {
    FakeStaging st;
    FrameRing ring;  // latest-wins: copies on demand only. Outlives the uploader, as in App.
    au::Uploader up(3, 2, st.Fn());
    CHECK(up.Running() && up.Slots() == 3, "the upload thread runs");
    up.SetSource(&ring);

    uint64_t frame = 0;
    std::vector<au::Ticket> held;
    for (int64_t k = 1; k <= 3; ++k) {
        Fill(ring.WriteBuffer(), k, 64, 16);
        ring.Publish();
        const FrameRing::Frame* f = ring.AcquireLatest();
        const au::Ticket t = up.Claim(&ring, *f);
        CHECK(t.Valid(), "handed over");
        const au::Uploader::Done d = up.Wait(t);
        CHECK(d.ok && !d.ahead && d.layout.format == 1, "copied on demand");
        CHECK(st.Holds(t.slot, d.layout, k), "the slot holds the frame, every row");
        up.Record(t, ++frame);  // this GPU frame reads it; not retired yet
        held.push_back(t);
    }
    // All three slots are read by GPU frames 1..3 now: the next frame cannot be staged.
    Fill(ring.WriteBuffer(), 4, 64, 16);
    ring.Publish();
    const FrameRing::Frame* f4 = ring.AcquireLatest();
    CHECK(!up.Claim(&ring, *f4).Valid(), "no free slot: the render thread copies it itself");
    mp::AsyncUploadTally tally = up.TakeTally();
    CHECK(tally.noSlot == 1 && tally.copies == 3 && tally.ahead == 0, "counted: 3 copies, 1 without a slot");
    for (const au::Ticket& t : held) CHECK(up.StateOf(t.slot) == au::SlotState::InFlight, "in flight");

    up.Retire(1);  // GPU frame 1 finished: its slot is free again
    const au::Ticket t4 = up.Claim(&ring, *f4);
    CHECK(t4.Valid() && t4.slot == held[0].slot, "frame 1's slot, now free");
    const au::Uploader::Done d4 = up.Wait(t4);
    CHECK(d4.ok && st.Holds(t4.slot, d4.layout, 4), "frame 4 copied");
    CHECK(st.Holds(held[1].slot, d4.layout, 2) && st.Holds(held[2].slot, d4.layout, 3),
          "the slots still in flight were not touched");

    // A synchronous upload replaced it: cancelled, never recorded.
    up.Cancel(t4);
    CHECK(up.StateOf(t4.slot) == au::SlotState::Free, "cancelled: free");

    // No staging memory: the copy fails and the render thread copies the frame.
    st.fail = true;
    Fill(ring.WriteBuffer(), 5, 64, 16);
    ring.Publish();
    const FrameRing::Frame* f5 = ring.AcquireLatest();
    const au::Ticket t5 = up.Claim(&ring, *f5);
    const au::Uploader::Done d5 = up.Wait(t5);
    CHECK(t5.Valid() && !d5.ok && up.StateOf(t5.slot) == au::SlotState::Free, "failed copy: copy it yourself");
    st.fail = false;
    tally = up.TakeTally();
    CHECK(tally.failed == 1 && tally.skipped == 1 && tally.copies == 1, "1 failed, 1 skipped (the cancel)");

    // Not a staged format: the render thread uploads it.
    FrameRing::Frame rgba;
    rgba.format = PixFormat::RGBA;
    rgba.width = rgba.height = 4;
    rgba.plane[0].assign(64, 0);
    rgba.serial = 99;
    CHECK(!up.Claim(&ring, rgba).Valid(), "RGBA: not staged");
}

void UploaderAhead() {
    FakeStaging st;
    FrameRing ring;  // declared first: the uploader unhooks it in its destructor
    ring.SetQueueDepth(3);
    au::Uploader up(5, 1, st.Fn());
    up.SetSource(&ring);
    Fill(ring.WriteBuffer(), 1, 128, 32);
    ring.Publish();  // wakes the upload thread through the ring's hook
    // Wait (bounded) for the copy ahead to land in a slot.
    bool staged = false;
    for (int i = 0; i < 2000 && !staged; ++i) {
        for (int s = 0; s < up.Slots(); ++s) staged = staged || up.StateOf(s) == au::SlotState::Ready;
        if (!staged) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(staged, "the waiting frame was copied ahead, before any take");
    const FrameRing::Frame* f = ring.AcquireLatest();
    CHECK(f && f->serial == 1, "taken");
    const au::Ticket t = up.Claim(&ring, *f);
    const au::Uploader::Done d = up.Wait(t);
    CHECK(t.Valid() && d.ok && d.ahead && d.waitMs < 50.0, "the take finds it staged: nothing to wait for");
    CHECK(st.Holds(t.slot, d.layout, 1), "and it is frame 1");
    up.Record(t, 1);
    const mp::AsyncUploadTally tally = up.TakeTally();
    CHECK(tally.copies == 1 && tally.ahead == 1, "one copy, ahead");
    up.SetSource(nullptr);  // unhooked: nothing is copied ahead any more
    Fill(ring.WriteBuffer(), 2, 128, 32);
    ring.Publish();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // a 16 KB copy takes microseconds
    CHECK(up.TakeTally().copies == 0, "no source: frame 2 is not copied ahead");
}

// The upload thread against a producer thread: a queue-mode ring at about the render loop's
// rate, frames of ~3 MB (banded on 3 threads). Every taken frame is whole and the right one,
// the slots the "GPU" reads are never rewritten before their frame retires, most frames are
// copied ahead, and the uploader shuts down while the producer keeps publishing.
void UploaderSoak() {
    FakeStaging st;
    const int kInFlight = 3;
    auto up = std::make_unique<au::Uploader>(au::SlotCount(kInFlight), 3, st.Fn());
    FrameRing ring;
    ring.SetQueueDepth(3);
    up->SetSource(&ring);

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        for (int64_t k = 1; !stop; ++k) {
            Fill(ring.WriteBuffer(), k, 2048, 1024);  // NV12: 2 MB Y + 1 MB UV
            ring.Publish();                          // serial == k
            std::this_thread::sleep_for(std::chrono::microseconds(1500));
        }
    });

    struct Reading {
        int slot;
        uint64_t frame;
        int64_t serial;
        au::Layout layout;
    };
    std::deque<Reading> gpu;  // what the "GPU" frames read, until they retire
    uint64_t frame = 0;
    int64_t lastSerial = 0;
    int taken = 0, ahead = 0, fallback = 0, wrong = 0, rewritten = 0;
    for (int it = 0; it < 300; ++it) {
        ++frame;  // OpenFrame: the frame kInFlight back has finished
        if (frame > (uint64_t)kInFlight) {
            const uint64_t done = frame - kInFlight;
            while (!gpu.empty() && gpu.front().frame <= done) {
                const Reading& g = gpu.front();
                if (!st.Holds(g.slot, g.layout, g.serial)) ++rewritten;  // written while the GPU read it
                gpu.pop_front();
            }
            up->Retire(done);
        }
        if (const FrameRing::Frame* f = ring.AcquireLatest()) {
            CHECK(f->serial > lastSerial, "frames in order");
            lastSerial = f->serial;
            const au::Ticket t = up->Claim(&ring, *f);
            if (!t.Valid()) {
                ++fallback;
            } else {
                const au::Uploader::Done d = up->Wait(t);  // the frame is ours until the next take
                if (d.ok) {
                    if (!st.Holds(t.slot, d.layout, f->serial)) ++wrong;
                    up->Record(t, frame);
                    gpu.push_back({t.slot, frame, f->serial, d.layout});
                    ++taken;
                    ahead += d.ahead ? 1 : 0;
                } else {
                    up->Cancel(t);
                    ++fallback;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(1500));
    }
    CHECK(wrong == 0, "every staged frame is whole and the frame taken");
    CHECK(rewritten == 0, "no slot was written while a GPU frame read it");
    CHECK(taken > 50, "frames were staged and taken");
    CHECK(ahead > 0, "some were copied ahead of the take");
    CHECK(fallback == 0, "never short of a slot");
    const mp::AsyncUploadTally tally = up->TakeTally();
    CHECK(tally.noSlot == 0 && tally.failed == 0, "no fallbacks counted");
    CHECK(tally.threads >= 2, "the copy ran in bands");
    std::printf("  soak: %d taken (%d copied ahead), %u copies, %u skipped, copy avg %.3f ms on %d thread(s)\n", taken,
                ahead, tally.copies, tally.skipped, tally.copies ? tally.copyMsSum / tally.copies : 0.0,
                tally.threads);

    up.reset();  // unhooks the ring and joins while the producer still publishes
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    producer.join();
    // No pin outlives the uploader: the next waiting frame can be pinned again.
    Fill(ring.WriteBuffer(), 1000000, 16, 16);
    ring.Publish();
    const FrameRing::Frame* p = ring.PinNextQueued(0);
    CHECK(p != nullptr, "the uploader left no pin behind");
    if (p) ring.Unpin(p);
}

}  // namespace

int main() {
    SwitchAndSlots();
    Layouts();
    SlotLife();
    SlotDrops();
    SlotInvariantSoak();
    RingPinBasics();
    RingPinNeverToProducer();
    RingPinTwinSoak();
    UploaderOnDemand();
    UploaderAhead();
    UploaderSoak();
    if (g_failures) {
        std::fprintf(stderr, "async_upload_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("async_upload_test: all passed\n");
    return 0;
}
