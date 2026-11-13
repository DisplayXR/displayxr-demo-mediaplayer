// SPDX-License-Identifier: Apache-2.0
//
// DirectPair - two-input stereo (DeckLink Stereo::Pair) converted straight into the ring.
//
// Why: each pair crossed system RAM once more than it had to. Each eye's lane converted its
// frame into a history slot (TimecodePairer's), then the pair was composed side by side into
// the ring frame: at 2x 2160p P016 that compose reads and writes 47.5 MB per pair, on the
// same memory bus as the card's DMA, the two converts and the upload copy. On the 8K station
// (two 2160p59.94 inputs, the render side at ~60 with #237's async upload) the capture side
// published ~45-53 pairs/s instead of 59.94, and FEWER upload threads published more: the bus
// was the limit, not the cores. (Pro 0.9.8.)
//
// How: when a lane knows, BEFORE converting a frame, the key it will pair by
// (TimecodePairer::DirectKey: the pairer is matching by key - timecode, or the card's clock -
// and the frame is newer than the last pair shown), it converts the frame straight into its
// half of a ring frame reserved for that key (FrameRing::ReserveBuffer). When the pairer then
// pairs the two halves of that one entry, the frame is published as it is: no compose.
// Anything else is composed as before, from wherever each eye's pixels are: no key, newest of
// each, two eyes in different entries, an eye in its history slot (counted as `composed`).
//
// The pairer stays the only judge of what pairs (matched pairs only included: an entry's two
// halves always carry the same key in the same epoch, so a whole entry is one instant). This
// table only knows where each history slot's pixels are - its own P016, a half of an entry,
// or gone - and owns the reserved ring frames. Pure (no SDK, no ring): the ring frames sit
// behind `Frames`. tests/direct_pair_test.cpp.
//
// An entry's life; every step is under the capture's pairing lock:
//   Claim    a lane takes its half of the entry for (key, epoch, generation), created with a
//            reserved ring frame if there is none: that half Empty -> Writing, and the lane's
//            history slot names it. The lane then writes that half alone, WITHOUT the lock.
//   Done     Writing -> Done, before the lane adds the frame to the pairer.
//   Publish  the pairer paired exactly this entry's two halves, both Done, and no compose is
//            reading it (Whole): the frame goes to the ring's queue as it is. The entry is
//            gone, and its slots' pixels with it (Gone): the ring may give the buffer to a
//            producer again once the renderer is done with it.
//   Retire   it can no longer be published whole: the pairer changed domain or jumped
//            (Epoch), showed this key or a newer one, a new signal came (generation), or one
//            of its slots was reused. No more claims; its Done halves stay readable for a
//            compose that a decision may still ask for. (While the pairer is not matching by
//            key - newest of each, a strict refusal - nothing new is claimed, but the entries
//            stay: the lagging eye may still match them when it catches up.)
//   Release  retired, nothing writing it (no Writing half), no compose reading it (no pin),
//            no slot naming it: back to the ring's free list, unpublished. Recycle is the same
//            for the oldest such entry still named by slots, when a new key needs a frame and
//            the ring has none to spare (those slots' pixels become Gone).
// So a reserved frame is written only by the one lane that claimed each half, while nobody
// else can see it; it reaches the ring's queue only with both halves Done; and once published
// or released, no lane, compose or entry touches it again.
#pragma once

#include "media/FrameRing.h"
#include "pro/ConvertPool.h"
#include "pro/TimecodePairer.h"

#include <cstdint>

namespace mp {

class DirectPairTable {
public:
    static constexpr int kEntries = FrameRing::kMaxReserved;  // one reserved ring frame each
    static constexpr int kSlots = TimecodePairer::kHistory;

    // The ring frames (DeckLinkSource: LiveSourceBase::ReservePairFrame / CancelPairFrame).
    class Frames {
    public:
        // A frame for two eyeW x eyeH eyes side by side, or nullptr when there is none to spare.
        virtual void* Reserve(int eyeW, int eyeH) = 0;
        virtual void Cancel(void* frame) = 0;  // back unpublished
    protected:
        ~Frames() = default;
    };

    enum class Half : uint8_t { Empty, Writing, Done };
    // Where a history slot's pixels are: its own P016, a half of an entry, or nowhere any more
    // (published whole, or recycled).
    enum class Where : uint8_t { Own, Half, Gone };
    // Why a claim went the old way: the entry's half for this eye is taken, the eyes differ in
    // size, or no ring frame (the ring's kMaxReserved are out, none recyclable).
    enum class Refused : uint8_t { None, Busy, Size, NoFrame };

    explicit DirectPairTable(Frames* frames);

    // A lane is about to put a new frame in `slot` of `eye` (TimecodePairer::NextSlot): what
    // the slot named is no longer reachable from it. An entry with a half there is retired.
    void SlotReused(int eye, int slot);
    // Every frame's size, direct or not: an entry is only created when the other eye's last
    // frame had the same size (two inputs that differ never fill one entry).
    void NoteSize(int eye, int eyeW, int eyeH);
    // The frame in `slot` of `eye` pairs by `key` (DirectKey) in `epoch` (the pairer's) for
    // signal `gen`: the entry whose `eye` half it goes into, now Writing and named by the slot;
    // -1 when it goes the old way (`why`). Call SlotReused(eye, slot) first.
    int Claim(int eye, int slot, int key, uint32_t epoch, uint32_t gen, int eyeW, int eyeH,
              Refused* why = nullptr);
    // The lane finished writing its half: Writing -> Done.
    void Done(int entry, int eye);

    Where Locate(int eye, int slot, int* entry = nullptr) const;
    // The decision paired slot0 (left) with slot1 (right): the entry to publish whole when they
    // are its two halves, both Done, and no compose reads it; else -1 (compose).
    int Whole(int slot0, int slot1) const;
    // Hands the entry's frame over to be published: the entry is gone, its slots Gone.
    void* TakeForPublish(int entry);
    // A compose reads a half of this entry (outside the lock): not published, released or
    // recycled meanwhile. The other half may still be written (disjoint memory).
    void Pin(int entry);
    void Unpin(int entry);
    // After each decision (and on a new signal): retire what can no longer be published whole
    // - another epoch or generation, or a key not newer than `lastKey` (the pairer's) - and
    // release what nothing needs any more.
    void Sweep(int lastKey, uint32_t epoch, uint32_t gen);
    // Capture over (no lane runs, no compose): every frame back to the ring.
    void ReleaseAll();

    void* Frame(int entry) const { return Valid(entry) ? e_[entry].frame : nullptr; }
    int EyeW(int entry) const { return Valid(entry) ? e_[entry].w : 0; }
    int EyeH(int entry) const { return Valid(entry) ? e_[entry].h : 0; }
    Half HalfState(int entry, int eye) const { return Valid(entry) ? e_[entry].half[eye & 1] : Half::Empty; }
    bool Retired(int entry) const { return Valid(entry) && e_[entry].retired; }
    int Pins(int entry) const { return Valid(entry) ? e_[entry].pins : 0; }
    int Held() const;  // entries holding a reserved frame

private:
    struct Entry {
        bool used = false;
        bool retired = false;
        int key = -1;
        uint32_t epoch = 0, gen = 0;
        uint64_t seq = 0;          // creation order: recycling takes the oldest
        void* frame = nullptr;
        int w = 0, h = 0;          // each eye's size
        Half half[2] = {Half::Empty, Half::Empty};
        int slot[2] = {-1, -1};    // the history slot naming each half (-1 none)
        int pins = 0;
    };
    static constexpr int kOwn = -1, kGone = -2;  // map_ values besides an entry index

    bool Valid(int i) const { return i >= 0 && i < kEntries && e_[i].used; }
    bool Releasable(const Entry& e) const {
        return e.retired && e.half[0] != Half::Writing && e.half[1] != Half::Writing && e.pins == 0;
    }
    void Drop(int i);        // its slots -> Gone, its frame cancelled, the row free
    void TryRelease(int i);  // Drop when releasable and no slot names it

    Frames* frames_;
    Entry e_[kEntries];
    int map_[2][kSlots];     // per history slot: an entry, kOwn or kGone
    int size_[2][2] = {};    // each eye's last frame size (w, h)
    uint64_t seq_ = 0;
};

namespace livepix {

// A side-by-side pair frame's planes as the converters' target (2*eyeW x eyeH): eye e's half
// starts at column e*eyeW (EyeJob::x0), as a dual-stream input's two eyes do in one frame.
inline P016Planes PairPlanes(FrameRing::Frame& f, int eyeW, int eyeH) {
    return {reinterpret_cast<uint16_t*>(f.plane[0].data()), reinterpret_cast<uint16_t*>(f.plane[1].data()), 2 * eyeW,
            eyeH};
}
// Eye e's half of it, to read: a compose pairing it with an eye from elsewhere.
inline EyeView PairHalf(const FrameRing::Frame& f, int eye, int eyeW, int eyeH) {
    const size_t x0 = (size_t)(eye & 1) * (size_t)eyeW;
    return {reinterpret_cast<const uint16_t*>(f.plane[0].data()) + x0,
            reinterpret_cast<const uint16_t*>(f.plane[1].data()) + x0, 2 * eyeW, eyeW, eyeH};
}

}  // namespace livepix
}  // namespace mp
