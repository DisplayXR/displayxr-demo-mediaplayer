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
//
// When both inputs are on one card the caller also gives each frame a clock key (its
// place on the card's hardware clock, HwClockKey). Then a missing timecode, or two
// timecodes that don't belong together, pairs by the card's clock instead: the same
// matching, on clock keys, so a frame-locked source pairs exactly. DaVinci Resolve's dual
// SDI stereo output is that case: both eyes frame-locked, timecode on the first output
// only. Newest-of-each there paired the eyes a frame apart whenever one eye's frame
// landed first. Timecode is trusted again once kTrustTimecodeAfter clock-paired frames
// in a row carry the same timecode on both eyes.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mp {

// A frame's pairing key from SMPTE timecode components: frames since midnight. Above
// 30 fps (47.95/48/50/59.94/60) RP188 counts frame PAIRS, 0..fps/2-1, and the field
// mark tells the two frames of a pair apart - without it every other pair of frames
// shares a key and the eyes pair one frame apart. A card that already counts to fps-1
// (ff >= fps/2 seen) is taken as full-rate.
inline int TimecodeKey(int hh, int mm, int ss, int ff, bool fieldMark, int fpsInt) {
    const int fps = std::max(1, fpsInt);
    const int base = ((hh * 60 + mm) * 60 + ss) * fps;
    if (fps > 30 && ff < fps / 2) return base + 2 * ff + (fieldMark ? 1 : 0);
    return base + ff;
}

// A frame's pairing key from the card's hardware reference clock, when both inputs
// share one card (one clock): frames since t0 at the card's reported frame duration,
// both in nanoseconds. Using the exact duration at that resolution, not a rounded fps
// or microseconds, keeps 23.976 / 29.97 / 59.94 from drifting across a rounding
// boundary and mispairing (a 1 us rounding drifts half a frame in 7 min at 59.94).
inline int HwClockKey(int64_t tNs, int64_t t0Ns, int64_t durNs) {
    return (int)std::llround((double)(tNs - t0Ns) / (double)std::max<int64_t>(1, durNs));
}

// The full form, which the DeckLink input uses: one SMPTE timecode as a capture card
// reports it, so an HFRTC and drop-frame are counted too and the text is at the full rate.
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
    // Clock-paired frames in a row whose timecodes agree before timecode is trusted again.
    static constexpr int kTrustTimecodeAfter = 60;

    struct Decision {
        bool publish = false;
        int slot[2] = {-1, -1};  // the slots to show, left and right
        int offsetFrames = -1;   // how far apart the eyes' newest frames are (-1 unknown)
        bool unsynced = false;   // paired without a matching timecode or clock (newest of each)
        bool byClock = false;    // paired by the card's clock (clock keys), not by timecode
        int tcOffsetFrames = -1; // the eyes' newest timecodes apart, frames (-1 = not both)
        bool refused = false;    // strict: a newest-of-each pair was due and was NOT shown
    };

    void Reset();  // keeps the strict setting
    // Strict: matched pairs only. When both eyes have keys but no common one can arrive in
    // time, nothing is published (the picture holds the last matched pair) instead of the
    // newest frame of each eye, which shows a left and a right from different instants.
    // On for two inputs on one card (the clock always matches eventually); see DeckLinkSource.
    void SetStrict(bool on) { strict_ = on; }
    bool Strict() const { return strict_; }
    // The slot the next frame of `eye` goes into (the oldest). Mark it busy first, fill it,
    // then call Add.
    int NextSlot(int eye) const { return next_[eye]; }
    void MarkBusy(int eye, int slot) { valid_[eye][slot] = false; }
    // A frame of `eye` landed in `slot` with timecode `key` (frames, -1 = none) at time
    // `arrived` (seconds). `period` = one frame in seconds. `clockKey` = its frame on the
    // card's clock when both inputs share one card (HwClockKey), else -1.
    Decision Add(int eye, int slot, int key, double arrived, double period, int clockKey = -1);
    bool ByClock() const { return byClock_; }

private:
    void Match(int eye, double period, Decision& d);
    void UseClock(bool on);  // re-key every remembered frame in the other domain
    int tc_[2][kHistory] = {};
    int clock_[2][kHistory] = {};
    bool byClock_ = false;
    bool strict_ = false;
    int tcAgree_ = 0;
    int lastShownTc_ = -1, lastShownClock_ = -1;  // the last shown left frame's keys
    int key_[2][kHistory] = {};  // the active domain's key: tc_ or clock_
    double arrived_[2][kHistory] = {};
    bool valid_[2][kHistory] = {};
    int next_[2] = {0, 0};
    int newest_[2] = {-1, -1};
    int lastKey_ = -1;
    int sincePublish_ = 0;  // left-eye frames since the last publish (safety valve)
};

}  // namespace mp
