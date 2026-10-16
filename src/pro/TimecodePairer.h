// SPDX-License-Identifier: Apache-2.0
//
// TimecodePairer — which left and right frames to show together when a stereo rig comes
// in on two separate inputs (DeckLinkSource, Stereo::Pair). Pure decision logic; the
// caller keeps the pixels in the slots it names. tests/timecode_pairer_test.cpp.
//
// With timecode on both inputs the newest instant both eyes have is shown, and a lagging
// eye is waited for while its matching frame can still arrive (it is in flight up to
// kHistory-1 frames behind). Timecode that holds still or jumps back (a camera not
// rolling, a deck rewinding, midnight) restarts the matching instead of freezing it.
// Without usable timecode - none, or the two inputs' clocks far apart - the newest
// frame of each is shown, paced by the left eye, and the offset reported.
#pragma once

#include <cstddef>

namespace mp {

// One SMPTE timecode as a capture card reports it.
struct TimecodeFields {
    int hh = 0, mm = 0, ss = 0, ff = 0;
    bool fieldMark = false;      // 50/60p: the second frame of a frame pair (ST 12-1)
    bool dropFrame = false;      // 29.97 / 59.94 drop-frame counting
    bool highFrameRate = false;  // an HFRTC (ST 12-3): ff already counts every frame
    bool fullCount = false;      // this source counts every frame (caller-latched, see below)
};

// Timecode -> frames since midnight at the signal's FULL frame rate `fpsNominal` (25, 30,
// 50, 60 ...). Above 30 fps, ordinary RP188 / VITC timecode counts frame PAIRS (ff runs
// 0..24 or 0..29, twice each) and the field mark says which frame of the pair this is;
// both frames of a pair would otherwise get the same number and two-input pairing could
// match the wrong ones. A source that already counts every frame is taken as it is: an
// HFRTC, or `fullCount` - which the CALLER latches per input once it has seen ff reach
// half the rate (TimecodeCountsEveryFrame), and resets when the format changes. Deciding
// that per frame would double ff 0..29 of every second and read ff 30..59 as-is. Drop-frame is counted the SMPTE way, so consecutive
// frames are consecutive numbers across the minute.
int TimecodeFrames(const TimecodeFields& t, int fpsNominal);
// "HH:MM:SS:FF" with FF at the full rate (what a 50/60p Resolve timeline shows); ';'
// before the frames for drop-frame.
void FormatTimecode(const TimecodeFields& t, int fpsNominal, char* out, size_t n);
// True when this frame proves its source counts every frame (ff past half the rate).
bool TimecodeCountsEveryFrame(const TimecodeFields& t, int fpsNominal);

class TimecodePairer {
public:
    // Frames kept per eye (the caller's slots). 6, not 4: with conversion slower than a
    // frame (4K60) the leading eye overwrites a slot before the lagging eye's match lands,
    // so a 3-frame lag needs the slack. ~25 MB a slot for a 4K 10-bit eye.
    static constexpr int kHistory = 6;

    struct Decision {
        bool publish = false;
        int slot[2] = {-1, -1};  // the slots to show, left and right
        int offsetFrames = -1;   // how far apart the eyes' newest frames are (-1 unknown)
        bool unsynced = false;   // paired without a matching timecode
    };

    void Reset();
    // The slot the next frame of `eye` goes into (the oldest). Mark it busy first, fill it,
    // then call Add.
    int NextSlot(int eye) const { return next_[eye]; }
    void MarkBusy(int eye, int slot) { valid_[eye][slot] = false; }
    // A frame of `eye` landed in `slot` with timecode `key` (frames, -1 = none) at time
    // `arrived` (seconds). `period` = one frame in seconds.
    Decision Add(int eye, int slot, int key, double arrived, double period);

private:
    int key_[2][kHistory] = {};
    double arrived_[2][kHistory] = {};
    bool valid_[2][kHistory] = {};
    int next_[2] = {0, 0};
    int newest_[2] = {-1, -1};
    int lastKey_ = -1;
    int sincePublish_ = 0;  // left-eye frames since the last publish (safety valve)
};

}  // namespace mp
