// SPDX-License-Identifier: Apache-2.0
#include "pro/DirectPair.h"

namespace mp {

DirectPairTable::DirectPairTable(Frames* frames) : frames_(frames) {
    for (int e = 0; e < 2; ++e)
        for (int s = 0; s < kSlots; ++s) map_[e][s] = kOwn;
}

void DirectPairTable::SlotReused(int eye, int slot) {
    if (slot < 0 || slot >= kSlots) return;
    eye &= 1;
    const int i = map_[eye][slot];
    map_[eye][slot] = kOwn;
    if (!Valid(i)) return;
    Entry& e = e_[i];
    e.slot[eye] = -1;
    e.retired = true;  // that half can no longer be named by a decision: never whole now
    TryRelease(i);
}

void DirectPairTable::NoteSize(int eye, int eyeW, int eyeH) {
    size_[eye & 1][0] = eyeW;
    size_[eye & 1][1] = eyeH;
}

int DirectPairTable::Claim(int eye, int slot, int key, uint32_t epoch, uint32_t gen, int eyeW, int eyeH,
                           Refused* why) {
    Refused dummy;
    Refused& r = why ? *why : dummy;
    r = Refused::None;
    eye &= 1;
    if (slot < 0 || slot >= kSlots || key < 0 || eyeW <= 0 || eyeH <= 0) {
        r = Refused::Busy;
        return -1;
    }
    int at = -1;
    for (int i = 0; i < kEntries; ++i) {
        const Entry& e = e_[i];
        if (e.used && !e.retired && e.key == key && e.epoch == epoch && e.gen == gen) {
            at = i;
            break;
        }
    }
    if (at >= 0) {  // the other eye's frame of this instant is (being) converted: join it
        const Entry& e = e_[at];
        if (e.half[eye] != Half::Empty) {
            r = Refused::Busy;  // (two frames of one eye with one key: the second goes the old way)
            return -1;
        }
        if (e.w != eyeW || e.h != eyeH) {
            r = Refused::Size;
            return -1;
        }
    } else {
        const int other = 1 - eye;
        if (size_[other][0] > 0 && (size_[other][0] != eyeW || size_[other][1] != eyeH)) {
            r = Refused::Size;  // the inputs differ in size: the old compose crops them
            return -1;
        }
        int row = -1;
        for (int i = 0; i < kEntries && row < 0; ++i)
            if (!e_[i].used) row = i;
        void* frame = row >= 0 ? frames_->Reserve(eyeW, eyeH) : nullptr;
        if (!frame) {
            // Recycle the oldest retired entry nothing writes or reads (its slots' pixels go).
            int old = -1;
            for (int i = 0; i < kEntries; ++i)
                if (e_[i].used && Releasable(e_[i]) && (old < 0 || e_[i].seq < e_[old].seq)) old = i;
            if (old >= 0) {
                Drop(old);
                if (row < 0) row = old;
                frame = frames_->Reserve(eyeW, eyeH);
            }
        }
        if (!frame) {
            r = Refused::NoFrame;
            return -1;
        }
        Entry& e = e_[row];
        e = Entry{};
        e.used = true;
        e.key = key;
        e.epoch = epoch;
        e.gen = gen;
        e.seq = ++seq_;
        e.frame = frame;
        e.w = eyeW;
        e.h = eyeH;
        at = row;
    }
    Entry& e = e_[at];
    e.half[eye] = Half::Writing;
    e.slot[eye] = slot;
    map_[eye][slot] = at;
    return at;
}

void DirectPairTable::Done(int entry, int eye) {
    if (!Valid(entry)) return;
    Half& h = e_[entry].half[eye & 1];
    if (h == Half::Writing) h = Half::Done;
}

DirectPairTable::Where DirectPairTable::Locate(int eye, int slot, int* entry) const {
    if (entry) *entry = -1;
    if (slot < 0 || slot >= kSlots) return Where::Gone;
    const int m = map_[eye & 1][slot];
    if (m == kGone) return Where::Gone;
    if (m < 0) return Where::Own;
    if (entry) *entry = m;
    return Where::Half;
}

int DirectPairTable::Whole(int slot0, int slot1) const {
    if (slot0 < 0 || slot0 >= kSlots || slot1 < 0 || slot1 >= kSlots) return -1;
    const int i = map_[0][slot0];
    if (!Valid(i) || map_[1][slot1] != i) return -1;
    const Entry& e = e_[i];
    return e.half[0] == Half::Done && e.half[1] == Half::Done && e.pins == 0 ? i : -1;
}

void* DirectPairTable::TakeForPublish(int entry) {
    if (!Valid(entry)) return nullptr;
    Entry& e = e_[entry];
    void* frame = e.frame;
    for (int eye = 0; eye < 2; ++eye)
        if (e.slot[eye] >= 0) map_[eye][e.slot[eye]] = kGone;  // its pixels are the renderer's now
    e = Entry{};
    return frame;
}

void DirectPairTable::Pin(int entry) {
    if (Valid(entry)) ++e_[entry].pins;
}

void DirectPairTable::Unpin(int entry) {
    if (!Valid(entry) || e_[entry].pins <= 0) return;
    --e_[entry].pins;
    TryRelease(entry);
}

void DirectPairTable::Sweep(int lastKey, uint32_t epoch, uint32_t gen) {
    for (int i = 0; i < kEntries; ++i) {
        Entry& e = e_[i];
        if (!e.used) continue;
        // (Not "the pairer is not matching by key right now": no new claims then (DirectKey),
        // but the lagging eye may still match these keys when it catches up.)
        if (!e.retired && (e.epoch != epoch || e.gen != gen || e.key <= lastKey)) e.retired = true;
        TryRelease(i);
    }
}

void DirectPairTable::ReleaseAll() {
    for (int i = 0; i < kEntries; ++i)
        if (e_[i].used) Drop(i);
}

int DirectPairTable::Held() const {
    int n = 0;
    for (const Entry& e : e_) n += e.used ? 1 : 0;
    return n;
}

void DirectPairTable::Drop(int i) {
    Entry& e = e_[i];
    for (int eye = 0; eye < 2; ++eye)
        if (e.slot[eye] >= 0) map_[eye][e.slot[eye]] = kGone;
    void* frame = e.frame;
    e = Entry{};
    if (frame) frames_->Cancel(frame);
}

void DirectPairTable::TryRelease(int i) {
    const Entry& e = e_[i];
    if (e.used && Releasable(e) && e.slot[0] < 0 && e.slot[1] < 0) Drop(i);
}

}  // namespace mp
