// SPDX-License-Identifier: Apache-2.0
//
// AsyncUpload - the source frame's CPU copy into GPU staging, off the render thread.
//
// Why: Media Player Pro showing two 2160p59.94 DeckLink eyes side by side uploads a
// 7680x2160 P016 frame (47.5 MB) every frame. #235 made the staging persistent and banded the
// copy, but the copy (~8 ms on 8 threads) still ran on the render thread. With the runtime's
// xrEndFrame (~7.7 ms) that left the render thread at ~18 ms a frame: 53-55 fps (#218).
//
// How: an upload thread copies frames into STAGING SLOTS (one host-visible buffer per slot,
// holding all of a frame's planes) in row bands on its own pool (rhi/BandCopy). The render
// thread only records vkCmdCopyBufferToImage from a slot. A frame gets copied one of two ways:
//   - ahead (queue mode: the live sources). Woken by the ring's Publish, the upload thread
//     pins the next waiting frame (FrameRing::PinNextQueued) and copies it while it waits in
//     the queue. By the time the render thread takes it, it is normally staged already.
//   - on demand. A frame the render thread takes that is not staged (a latest-wins ring:
//     file playback, the camera, streams; or a frame that arrived just before the take) is
//     handed over at the take (Claim). It is copied while the render thread does the rest of
//     its frame (the Pro hooks, xrWaitFrame), which waits at the commit only for what is left.
// The render thread stays the ring's only consumer: the ring's cadence (queue mode's one
// frame per refresh), its drops, trims and repeats are unchanged.
//
// A slot's life (SlotTable, under the Uploader's lock):
//   Free -> Filling (the upload thread owns it and writes it) -> Ready (written; nothing
//   writes it again) -> InFlight(F) (frame F records a GPU copy from it) -> Free once frame F
//   has finished on the GPU (Retire: the render thread knows that when its frame ring has
//   waited F's fence). A Ready slot the render thread will never record (a newer frame came,
//   the source changed) goes straight back to Free: the GPU never read that fill.
// So the upload thread only writes a Free slot, which no submitted GPU frame still reads, and
// the render thread only records a Ready slot, which nothing writes any more.
//
// The ring frame being copied cannot change under the copy either: an ahead copy holds the
// ring's pin (the producer never gets a pinned buffer back), and an on-demand copy reads the
// frame the render thread holds as the ring's consumer, which stays its own until its next
// AcquireLatest. The render thread waits for that copy (Wait) in the same render-loop
// iteration, before anything can acquire again.
//
// Pure apart from the slot memory, which the owner supplies (VulkanRenderer: persistently
// mapped host-visible buffers; the test: plain memory). tests/async_upload_test.cpp.
#pragma once

#include "media/FrameRing.h"
#include "rhi/BandCopy.h"
#include "rhi/RenderPerf.h"  // AsyncUploadTally

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace mp::asyncup {

// MEDIAPLAYER_ASYNC_UPLOAD: unset or anything else = on; 0 / off / false / no (any case) =
// off, every copy on the render thread as before.
bool Enabled(const char* env);
bool Enabled();  // from the environment

// Staging slots for N frames in flight: one per frame the GPU may still be reading from, one
// being filled, one ready to take. At least 3, at most kMaxSlots.
constexpr int kMaxSlots = 12;
int SlotCount(uint32_t framesInFlight);

// Where a frame's planes go in a slot: rows packed, each plane at a kPlaneAlign offset.
constexpr size_t kPlaneAlign = 256;
struct Layout {
    int format = -1;  // VulkanRenderer::UploadYUV's: 0 I420, 1 NV12, 2 P016; 3 raw v210 (#238)
    int planes = 0;   // v210: one per eye, each eye's rows as they are (media/V210.h)
    uint32_t w[3] = {}, h[3] = {};  // v210: the eye's width in pixels
    uint32_t bpt[3] = {};  // bytes per texel (v210: 0, rows are RowBytes(w))
    size_t rowBytes[3] = {}, offset[3] = {};
    size_t total = 0;      // bytes the slot needs
};
// UploadYUV's chroma: (w+1)/2 x (h+1)/2. False for an unknown format or an empty frame.
bool LayoutFor(int format, uint32_t w, uint32_t h, Layout& out);
// A ring frame. False for a zero-copy (gpu) frame, an RGBA one, or planes shorter than the
// layout: the render thread uploads those itself, as before. A v210 frame (format 3) only with
// `v210`, i.e. when the renderer decodes it on the GPU: else its rows are no use in staging
// (the CPU fallback reads the ring frame, not write-combined staging).
bool LayoutFor(const FrameRing::Frame& f, Layout& out, bool v210 = true);
// UploadYUV's format number for a ring frame (-1: not a YUV frame; 3: raw v210).
int UploadFormat(PixFormat f);

enum class SlotState : uint8_t { Free, Filling, Ready, InFlight };

// One fill of one slot: the generation tells two fills of the same slot apart.
struct Ticket {
    int slot = -1;
    uint32_t gen = 0;
    bool Valid() const { return slot >= 0; }
};

// The slots' state machine (see the file comment). Not thread-safe: the Uploader locks it.
class SlotTable {
public:
    explicit SlotTable(int count);
    int Count() const { return count_; }
    int FreeCount() const;
    SlotState State(int slot) const;

    // Free -> Filling for frame `serial` of `source`. Invalid when no slot is free.
    Ticket Reserve(const void* source, int64_t serial, bool ahead);
    // The copy ended: Filling -> Ready, or Free when it failed or the fill was dropped
    // meanwhile (then it counts as skipped). True when it ended Ready.
    bool Finish(Ticket t, bool ok);
    // The live fill (Filling or Ready, not dropped) of frame `serial` of `source`.
    Ticket Find(const void* source, int64_t serial) const;
    // The render thread will record this fill: no drop touches it until Record or Cancel.
    void Claim(Ticket t);
    bool Filling(Ticket t) const;  // the copy is still running
    bool Ready(Ticket t) const;    // copied, and not dropped
    bool Ahead(Ticket t) const;    // filled from the queue before the take
    // Ready -> InFlight(frame): GPU frame `frame` copies from this slot.
    bool Record(Ticket t, uint64_t frame);
    // A claimed fill the render thread will not record after all: Ready -> Free now, a
    // running copy -> Free when it ends. Counts as skipped.
    void Cancel(Ticket t);
    // InFlight slots whose frame is <= completedFrame: the GPU is done with them -> Free.
    int Retire(uint64_t completedFrame);
    // Unclaimed fills nobody will take: another source's, or `source`'s below `serial` (the
    // ring dropped or trimmed those frames). Returns how many; each counts as skipped.
    int DropStale(const void* source, int64_t serial);
    // Unclaimed fills of every source but `source` (nullptr: all of them).
    int DropOtherSources(const void* source);

    uint64_t Skipped() const { return skipped_; }  // fills dropped unrecorded, since start
    uint64_t Fills() const { return fills_; }      // Reserves, since start
    uint64_t LastFill(int slot) const;             // Fills() at the slot's last Reserve
    uint64_t Frame(int slot) const;                // InFlight: the frame reading it

private:
    struct Slot {
        SlotState state = SlotState::Free;
        const void* source = nullptr;
        int64_t serial = 0;
        uint32_t gen = 0;
        bool claimed = false;  // the render thread holds a ticket for it
        bool dropped = false;  // Filling: free it when the copy ends
        bool ahead = false;
        uint64_t frame = 0;
        uint64_t lastFill = 0;
    };
    bool Match(Ticket t) const;
    void Drop(Slot& s);
    Slot slots_[kMaxSlots];
    int count_ = 0;
    uint64_t skipped_ = 0, fills_ = 0;
};

// What the upload thread did since the last TakeTally() (the render thread drains it into
// its perf window every frame).
using Tally = AsyncUploadTally;

class Uploader {
public:
    // Mapped memory of at least `bytes` for slot `slot` (reallocated when too small, or much
    // too big); nullptr on failure. Called on the upload thread, only for a slot it is
    // filling, which no submitted GPU frame reads.
    using EnsureFn = std::function<void*(int slot, size_t bytes)>;
    // `copyThreads`: the copy's threads, the upload thread included (bandcopy::UploadThreads).
    Uploader(int slots, int copyThreads, EnsureFn ensure);
    // Lets a copy in progress finish, unpins, removes the publish hook, joins the thread.
    ~Uploader();
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    bool Running() const { return running_; }  // false: the thread did not start
    // Stage v210 frames (the renderer's GPU decode is up, #238). Off by default: before any
    // source is set.
    void SetV210(bool on) { v210_ = on; }
    int Slots() const { return table_.Count(); }
    int CopyThreads() const { return copyThreads_; }

    // --- The render thread ---
    // The ring whose waiting frames to copy ahead (queue mode only; any other ring, or
    // nullptr, copies on demand only). Fills of any other source are dropped.
    void SetSource(FrameRing* ring);
    // The frame the render thread just took from `ring`, which it holds until its next
    // AcquireLatest. Returns the fill to record (copied ahead, being copied, or queued for an
    // on-demand copy now), or an invalid ticket: copy it yourself (not a staged format, no
    // free slot). A valid ticket MUST be waited (Wait) before `ring` is acquired again.
    Ticket Claim(const FrameRing* ring, const FrameRing::Frame& f);
    struct Done {
        bool ok = false;     // Ready: record it (Record), else copy the frame yourself
        bool ahead = false;  // it was copied before the take
        Layout layout;
        double waitMs = 0.0;  // how long this call blocked for the copy
        double copyMs = 0.0;  // the copy, on the upload thread
        int threads = 0;
    };
    // Blocks while the ticket's copy is still running.
    Done Wait(Ticket t);
    void Record(Ticket t, uint64_t frame);  // GPU frame `frame` copies from it
    void Cancel(Ticket t);                  // not recorded after all (wait for it first)
    // GPU frames <= completedFrame have finished: their slots are free again.
    void Retire(uint64_t completedFrame);
    Tally TakeTally();
    // Calls release(slot) for each free slot no fill has used since the previous call, under
    // the lock (the upload thread cannot take it meanwhile).
    void TrimIdle(const std::function<void(int)>& release);

    // For tests: the slot's state and generation.
    SlotState StateOf(int slot);

private:
    struct Work {
        Ticket t;
        const FrameRing::Frame* frame = nullptr;
        Layout layout;
        FrameRing* pinned = nullptr;  // an ahead copy: the ring to Unpin when done
        bool ahead = false;
    };
    static void OnPublish(void* self);
    void Wake();
    void Loop();
    bool NextWorkLocked(Work& w);
    void Copy(const Work& w, bool& ok, double& ms, int& threads);

    const int copyThreads_;
    EnsureFn ensure_;
    std::unique_ptr<bandcopy::Pool> pool_;  // the upload thread's, made at the first big copy

    // The slot table, the work, the source: tm_. tcv_: a copy finished.
    std::mutex tm_;
    std::condition_variable tcv_;
    SlotTable table_;
    Layout layout_[kMaxSlots];
    double slotCopyMs_[kMaxSlots] = {};
    int slotThreads_[kMaxSlots] = {};
    Work job_;                  // the render thread's on-demand copy, not yet picked up
    FrameRing* src_ = nullptr;  // copy ahead from this ring (queue mode)
    int64_t lastAhead_ = 0;     // the newest serial taken for an ahead copy from src_
    Tally tally_;
    uint64_t skippedSeen_ = 0;
    uint64_t trimMark_ = 0;

    // The upload thread's wake-up: wm_ is a leaf lock (the ring's publish hook takes it under
    // the ring's lock; nothing is locked under it).
    std::mutex wm_;
    std::condition_variable wcv_;
    uint64_t wakeSeq_ = 0;
    bool quit_ = false;
    bool running_ = false;
    std::atomic<bool> v210_{false};
    std::thread thread_;
};

}  // namespace mp::asyncup
