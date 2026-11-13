// SPDX-License-Identifier: Apache-2.0
//
// Two-input stereo pairing (DeckLink Stereo::Pair). Simulates two SDI inputs delivering
// frames with SMPTE timecode in different interleavings and checks what is shown: every
// shown pair has matching timecode, each instant is shown once, nothing freezes - the
// cases a code review found broken (a steady 3-frame lag, timecode that stops or rewinds).

#include "pro/TimecodePairer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using mp::TimecodePairer;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

struct Sim {
    TimecodePairer p;
    int slotKey[2][TimecodePairer::kHistory] = {};
    std::vector<std::pair<int, int>> shown;  // (left key, right key)
    int unsynced = 0;
    void Frame(int eye, int key, double t = 0.0) {
        const int s = p.NextSlot(eye);
        p.MarkBusy(eye, s);
        slotKey[eye][s] = key;
        const TimecodePairer::Decision d = p.Add(eye, s, key, t, 1.0 / 25.0);
        if (d.publish) {
            shown.emplace_back(slotKey[0][d.slot[0]], slotKey[1][d.slot[1]]);
            if (d.unsynced) ++unsynced;
        }
    }
};

// Left leads by `lag` frames, both run at the same rate for n frames.
void SteadyLag(int lag, bool leftFirst) {
    Sim s;
    const int n = 40, start = 1000;
    for (int i = 0; i < n; ++i) {
        if (leftFirst) {
            s.Frame(0, start + i + lag);
            s.Frame(1, start + i);
        } else {
            s.Frame(1, start + i);
            s.Frame(0, start + i + lag);
        }
    }
    bool allMatch = true, increasing = true;
    for (size_t k = 0; k < s.shown.size(); ++k) {
        if (s.shown[k].first != s.shown[k].second) allMatch = false;
        if (k && s.shown[k].first <= s.shown[k - 1].first) increasing = false;
    }
    std::printf("  lag %d (%s first): %zu shown, unsynced %d\n", lag, leftFirst ? "left" : "right", s.shown.size(),
                s.unsynced);
    CHECK(allMatch, "every shown pair has matching timecode");
    CHECK(increasing, "each instant shown once, in order (no jumping back)");
    CHECK(s.unsynced == 0, "a lag inside the history is never treated as unsynced");
    CHECK((int)s.shown.size() >= n - lag - 1, "one shown pair per frame once the lag is absorbed");
}

// Lags beyond the history: never stall - one pair per frame, matched where the history
// still allows it, otherwise newest-of-each flagged unsynced. Nothing shown twice.
void LongLag(int lag, bool leftFirst) {
    Sim s;
    const int n = 60, start = 2000;
    for (int i = 0; i < n; ++i) {
        if (leftFirst) {
            s.Frame(0, start + i + lag);
            s.Frame(1, start + i);
        } else {
            s.Frame(1, start + i);
            s.Frame(0, start + i + lag);
        }
    }
    bool increasing = true;
    for (size_t k = 1; k < s.shown.size(); ++k)
        if (s.shown[k].first < s.shown[k - 1].first) increasing = false;
    std::printf("  lag %d (%s first): %zu shown, unsynced %d\n", lag, leftFirst ? "left" : "right", s.shown.size(),
                s.unsynced);
    CHECK((int)s.shown.size() >= n - 6, "a long lag still shows a picture every frame");
    CHECK((int)s.shown.size() <= n + 1, "...and not twice per frame");
    CHECK(increasing, "the left eye never goes back");
}

// Conversion slower than a frame: the leading eye claims its next slot (MarkBusy) before
// the lagging eye's matching frame is added - the review's stall case. Lag 3.
void SlowConversion() {
    TimecodePairer p;
    int key[2][TimecodePairer::kHistory] = {};
    int shown = 0, matched = 0;
    const int lag = 3, n = 60;
    int pendingSlot = p.NextSlot(0);
    p.MarkBusy(0, pendingSlot);
    for (int i = 0; i < n; ++i) {
        const int lead = 1000 + i + lag;
        // eye 0's frame `lead` finishes converting...
        key[0][pendingSlot] = lead;
        TimecodePairer::Decision d = p.Add(0, pendingSlot, lead, 0.0, 0.04);
        if (d.publish) {
            ++shown;
            if (key[0][d.slot[0]] == key[1][d.slot[1]]) ++matched;
        }
        // ...eye 0's NEXT frame starts converting before eye 1's frame lands
        pendingSlot = p.NextSlot(0);
        p.MarkBusy(0, pendingSlot);
        const int s1 = p.NextSlot(1);
        p.MarkBusy(1, s1);
        key[1][s1] = 1000 + i;
        d = p.Add(1, s1, 1000 + i, 0.0, 0.04);
        if (d.publish) {
            ++shown;
            if (key[0][d.slot[0]] == key[1][d.slot[1]]) ++matched;
        }
    }
    std::printf("  slow conversion, lag 3: %d shown, %d matched\n", shown, matched);
    CHECK(shown >= n - lag - 2, "no stall when the leading eye claims its slot early");
    CHECK(matched >= n - lag - 2, "and the pairs still match");
}

void RewindNoReplay() {
    Sim s;
    for (int i = 0; i < 4; ++i) {
        s.Frame(0, 100 + i);
        s.Frame(1, 100 + i);
    }
    const size_t before = s.shown.size();
    for (int i = 0; i < 8; ++i) {
        s.Frame(0, 50 + i);
        s.Frame(1, 50 + i);
    }
    bool replay = false;
    for (size_t k = before; k < s.shown.size(); ++k)
        if (s.shown[k].first >= 100 || s.shown[k].second >= 100) replay = true;
    CHECK(!replay, "after a rewind no frame from before the jump is shown again");
    CHECK(s.shown.size() - before >= 7 && s.shown.size() - before <= 9, "one pair per frame after a rewind");
}

void TimecodeStops() {
    Sim s;
    for (int i = 0; i < 10; ++i) {
        s.Frame(0, 500 + i);
        s.Frame(1, 500 + i);
    }
    const size_t before = s.shown.size();
    for (int i = 0; i < 10; ++i) {  // camera stops rolling: TC holds at 509
        s.Frame(0, 509);
        s.Frame(1, 509);
    }
    std::printf("  held timecode: %zu shown while held\n", s.shown.size() - before);
    CHECK(s.shown.size() - before >= 9, "held timecode keeps the picture moving");
    CHECK(s.shown.size() - before <= 11, "...once per frame, not once per eye");
}

void TimecodeRewinds() {
    Sim s;
    for (int i = 0; i < 10; ++i) {
        s.Frame(0, 90000 + i);
        s.Frame(1, 90000 + i);
    }
    const size_t before = s.shown.size();
    for (int i = 0; i < 10; ++i) {  // midnight wrap / deck rewind
        s.Frame(0, 10 + i);
        s.Frame(1, 10 + i);
    }
    CHECK(s.shown.size() - before >= 9, "a timecode jump back does not freeze the pairing");
    CHECK(s.shown.back().first == 19 && s.shown.back().second == 19, "and pairs the new timecode");
}

void Unsynced() {
    Sim s;
    for (int i = 0; i < 20; ++i) {
        s.Frame(0, 1000 + i);
        s.Frame(1, 70000 + i);  // two unrelated clocks
    }
    CHECK(s.unsynced >= 15, "unrelated clocks: newest of each, flagged unsynced");
    CHECK(s.shown.size() <= 20, "paced by the left eye: at most one per left frame");
}

void NoTimecode() {
    Sim s;
    for (int i = 0; i < 20; ++i) {
        s.Frame(1, -1, i / 25.0);
        s.Frame(0, -1, i / 25.0 + 0.001);
    }
    CHECK(s.shown.size() == 20 && s.unsynced == 0, "no timecode: one pair per left frame");
}

void OneEyeMissing() {
    Sim s;
    for (int i = 0; i < 10; ++i) s.Frame(0, 100 + i);
    CHECK(s.shown.empty(), "nothing to show until both eyes have a frame");
}

// RP188 above 30 fps counts frame pairs and marks the second frame with the field flag:
// the key must be distinct for every frame and in frame order, at 48, 50 and 60.
void HighRateTimecodeKeys() {
    using mp::TimecodeKey;
    for (int fps : {48, 50, 60}) {
        int prev = -1;
        bool strictlyIncreasing = true, consecutive = true;
        for (int ss = 0; ss < 3; ++ss)
            for (int ff = 0; ff < fps / 2; ++ff)
                for (int field = 0; field < 2; ++field) {
                    const int k = TimecodeKey(1, 2, ss, ff, field != 0, fps);
                    if (prev >= 0 && k != prev + 1) consecutive = false;
                    if (k <= prev) strictlyIncreasing = false;
                    prev = k;
                }
        CHECK(strictlyIncreasing && consecutive, "high-rate keys are consecutive and distinct per frame");
    }
    // Two frames of a pair at 60p: same components, field mark set on the second.
    CHECK(TimecodeKey(0, 0, 0, 7, false, 60) + 1 == TimecodeKey(0, 0, 0, 7, true, 60),
          "field mark separates the two frames of a pair");
    // A card that counts to fps-1 itself (ff >= fps/2 seen) is taken as full-rate.
    CHECK(TimecodeKey(0, 0, 0, 45, false, 60) == 45, "full-rate count at 60 is used as is");
    // At or below 30 fps nothing changes.
    CHECK(TimecodeKey(0, 0, 1, 3, false, 25) == 28, "25 fps key is seconds*25 + ff");
    CHECK(TimecodeKey(0, 0, 1, 3, true, 25) == 28, "field mark ignored at 25 fps");
    // The two frames of a 48p pair must pair with each other, not one frame apart.
    Sim s;
    for (int i = 0; i < 48; ++i) {
        const int k = TimecodeKey(0, 0, 0, i / 2, (i & 1) != 0, 48);
        s.Frame(1, k);
        s.Frame(0, k);
    }
    bool allMatch = true;
    for (const auto& pr : s.shown) allMatch = allMatch && pr.first == pr.second;
    CHECK(s.shown.size() == 48 && allMatch, "48p: every frame pairs with its own instant");
}

// The hardware-clock key uses the card's exact frame duration, so two genlocked inputs
// a little apart never drift onto different keys at 23.976 / 59.94 over a long run.
void HardwareClockKeys() {
    using mp::HwClockKey;
    // Nanoseconds, as the source asks the card for; 2.6 M frames = 12 h at 60 fps.
    for (double fps : {23.976, 29.97, 59.94, 24.0, 48.0}) {
        const int64_t dur = (int64_t)std::llround(1e9 / fps);
        const double exactDur = 1e9 / fps;
        const int64_t t0 = 123456789;
        int mismatches = 0, skips = 0, prev = -1;
        for (int i = 0; i < 2600000; ++i) {
            const int64_t tl = t0 + (int64_t)std::llround(i * exactDur);
            const int64_t tr = tl + dur / 4;  // right eye a quarter frame late (genlocked, skewed)
            const int kl = HwClockKey(tl, t0, dur), kr = HwClockKey(tr, t0, dur);
            if (kl != kr) ++mismatches;
            if (prev >= 0 && kl != prev + 1) ++skips;
            prev = kl;
        }
        CHECK(mismatches == 0, "quarter-frame skew never mispairs");
        CHECK(skips == 0, "no skipped or repeated keys over a 12-hour shift");
    }
    // Half a frame or more apart is a different frame.
    CHECK(HwClockKey(1000 + 600, 1000, 1000) == 1, "0.6 frame late is the next frame");
    // Rounded fps would drift: at 23.976 the integer-fps period is 0.1% short, so after
    // ~500 frames the quarter-frame skew lands on the other side of a rounding boundary.
    {
        const int64_t badDur = 1000000000 / 24;  // what the old code used for 23.976
        const double exactDur = 1e9 / 23.976;
        int mismatches = 0;
        for (int i = 0; i < 20000; ++i) {
            const int64_t tl = (int64_t)std::llround(i * exactDur), tr = tl + badDur / 4;
            if (HwClockKey(tl, 0, badDur) != HwClockKey(tr, 0, badDur)) ++mismatches;
        }
        CHECK(mismatches > 0, "sanity: a rounded period does mispair (the bug this guards)");
    }
}

// Timecode -> full-rate frames (RP188 at 50/60p: frame pairs + field mark; HFRTC;
// drop-frame), and two inputs at 50p pairing on it.
void FullRateTimecode() {
    using mp::TimecodeFields;
    using mp::TimecodeFrames;
    auto tf = [](int hh, int mm, int ss, int ff, bool fm = false, bool df = false, bool hfr = false) {
        TimecodeFields t;
        t.hh = hh; t.mm = mm; t.ss = ss; t.ff = ff;
        t.fieldMark = fm; t.dropFrame = df; t.highFrameRate = hfr;
        return t;
    };
    CHECK(TimecodeFrames(tf(0, 0, 1, 0), 25) == 25, "25p: plain count");
    CHECK(TimecodeFrames(tf(0, 0, 0, 12, false), 50) == 24, "50p pair 12, first frame -> 24");
    CHECK(TimecodeFrames(tf(0, 0, 0, 12, true), 50) == 25, "50p pair 12, field mark -> 25");
    CHECK(TimecodeFrames(tf(0, 0, 1, 0), 50) == 50, "50p: one second = 50 frames");
    CHECK(TimecodeFrames(tf(0, 0, 0, 37, false, false, true), 60) == 37, "HFRTC counts every frame");
    {
        TimecodeFields full = tf(0, 0, 0, 37);
        CHECK(mp::TimecodeCountsEveryFrame(full, 60), "ff past half the rate proves a full count");
        full.fullCount = true;
        CHECK(TimecodeFrames(full, 60) == 37, "a latched full-count source is taken as it is");
        // A full-count source across a second, latched as an input would latch it: from
        // the first second's ff 30 on, every frame is consecutive - including ff 0..29 of
        // the next second, which a per-frame rule would have doubled.
        bool latch = false, consec = true;
        int prevK = -1, seenAfterLatch = 0;
        for (int ss = 0; ss < 3; ++ss)
            for (int ff = 0; ff < 60; ++ff) {
                TimecodeFields x = tf(0, 0, ss, ff);
                if (mp::TimecodeCountsEveryFrame(x, 60)) latch = true;
                x.fullCount = latch;
                const int k = TimecodeFrames(x, 60);
                if (latch && prevK >= 0 && seenAfterLatch > 0 && k != prevK + 1) consec = false;
                if (latch) ++seenAfterLatch;
                prevK = k;
            }
        CHECK(consec, "latched full-count 60p timecode is consecutive across seconds");
    }
    CHECK(TimecodeFrames(tf(0, 10, 0, 0, false, true), 30) == 17982, "29.97 DF: ten minutes");
    CHECK(TimecodeFrames(tf(0, 1, 0, 2, false, true), 30) == 1800, "29.97 DF: ;00 ;01 skipped");
    // 59.94 DF from RP188: pairs + field mark + 4 numbers dropped a minute (2 pairs).
    CHECK(TimecodeFrames(tf(0, 1, 0, 2, false, true), 60) == 3600, "59.94 DF: pair 2 at 00:01:00 = frame 3600");
    // Every frame of a 60p minute boundary is consecutive.
    int prev = -1;
    bool consecutive = true;
    for (int ss = 58; ss < 60; ++ss)
        for (int ff = 0; ff < 30; ++ff)
            for (int fm = 0; fm < 2; ++fm) {
                const int k = TimecodeFrames(tf(0, 0, ss, ff, fm != 0, true), 60);
                if (prev >= 0 && k != prev + 1) consecutive = false;
                prev = k;
            }
    for (int ff = 2; ff < 30; ++ff)
        for (int fm = 0; fm < 2; ++fm) {
            const int k = TimecodeFrames(tf(0, 1, 0, ff, fm != 0, true), 60);
            if (k != prev + 1) consecutive = false;
            prev = k;
        }
    CHECK(consecutive, "59.94 DF: consecutive across the minute");
    char buf[16];
    mp::FormatTimecode(tf(10, 0, 0, 12, true), 50, buf, sizeof(buf));
    CHECK(std::string(buf) == "10:00:00:25", "formatted at the full rate");
    mp::FormatTimecode(tf(0, 1, 0, 2, false, true), 30, buf, sizeof(buf));
    CHECK(std::string(buf) == "00:01:00;02", "drop-frame separator");

    // Two 50p inputs, the right eye 1 frame behind, keyed from their RP188. Track the TRUE
    // frame index in each slot: keyed without the field mark both frames of a pair collide
    // (the old key) and the pairer can show frame N of one eye with N+1 of the other.
    TimecodePairer p;
    int slotFrame[2][TimecodePairer::kHistory] = {};
    int shown = 0, mismatched = 0, lastShown = -1, repeats = 0;
    auto feed = [&](int eye, int i) {
        const int pair = 100 + i / 2;
        const int key = TimecodeFrames(tf(0, 0, pair / 25, pair % 25, (i % 2) != 0), 50);
        const int s = p.NextSlot(eye);
        p.MarkBusy(eye, s);
        slotFrame[eye][s] = i;
        const TimecodePairer::Decision d = p.Add(eye, s, key, 0.0, 1.0 / 50.0);
        if (!d.publish) return;
        ++shown;
        const int l = slotFrame[0][d.slot[0]], r = slotFrame[1][d.slot[1]];
        if (l != r) ++mismatched;
        if (l <= lastShown) ++repeats;
        lastShown = l;
    };
    for (int i = 0; i < 40; ++i) {
        feed(0, i);
        if (i > 0) feed(1, i - 1);
    }
    CHECK(shown > 30, "50p pairs are shown");
    CHECK(mismatched == 0, "50p: every shown pair is the same frame on both eyes");
    CHECK(repeats == 0, "50p: each frame shown once, in order");
}

// Two inputs on one card, frame-locked, each frame with a clock key (its frame on the card's
// clock) and a timecode chosen by the case. The eyes' frames reach the pairer in a jittery
// order, as the two conversion lanes finish: sometimes left first, sometimes right, now
// and then one eye two frames ahead. `trueFrame` is what is really in each slot.
struct ClockSim {
    TimecodePairer p;
    int trueFrame[2][TimecodePairer::kHistory] = {};
    int shown = 0, mismatched = 0, repeats = 0, unsynced = 0, byClock = 0, lastShown = -1;
    bool useClock = true;
    bool orderMix = false;  // the run had each eye land first a fair share of the time
    void Frame(int eye, int i, int tc) {
        const int s = p.NextSlot(eye);
        p.MarkBusy(eye, s);
        trueFrame[eye][s] = i;
        const TimecodePairer::Decision d = p.Add(eye, s, tc, i / 59.94, 1.0 / 59.94, useClock ? i : -1);
        if (!d.publish) return;
        ++shown;
        const int l = trueFrame[0][d.slot[0]], r = trueFrame[1][d.slot[1]];
        if (l != r) ++mismatched;
        if (l <= lastShown) ++repeats;
        lastShown = l;
        if (d.unsynced) ++unsynced;
        if (d.byClock) ++byClock;
    }
    // n frames; tcOf(eye, i) gives each frame's timecode key.
    template <class TcOf>
    void Run(int first, int n, TcOf tcOf) {
        uint32_t rng = 12345;
        auto next = [&] { return (rng = rng * 1664525u + 1013904223u) >> 16; };
        int li = first, ri = first;
        const int end = first + n;
        int leftFirst = 0, rightFirst = 0;
        while (li < end || ri < end) {
            // A random eye next, never more than 2 frames ahead of the other.
            bool left = (next() & 1) != 0;
            if (li >= end || li - ri >= 2) left = false;
            if (ri >= end || ri - li >= 2) left = true;
            if (left) {
                if (li == ri) ++leftFirst;
                Frame(0, li, tcOf(0, li)), ++li;
            } else {
                if (li == ri) ++rightFirst;
                Frame(1, ri, tcOf(1, ri)), ++ri;
            }
        }
        orderMix = leftFirst > n / 5 && rightFirst > n / 5;
    }
};

// DaVinci Resolve's dual SDI stereo output, as on the 8K station: both eyes frame-locked,
// timecode on the first output only (the right eye carries 00:00:00:00). Paired by the
// card's clock, every shown pair is one frame; newest-of-each (no clock key) was not.
void ResolveDualOutput() {
    const int start = 409672;  // the left eye's timecode, 01:53:47:52 at 60
    auto tcOf = [&](int eye, int i) { return eye == 0 ? start + i : 0; };
    ClockSim c;
    c.Run(0, 600, tcOf);
    std::printf("  Resolve dual output, by clock: %d shown, %d mismatched, %d repeats, %d unsynced, %d by clock\n",
                c.shown, c.mismatched, c.repeats, c.unsynced, c.byClock);
    CHECK(c.orderMix, "the simulated arrivals put each eye first often (the jitter that broke it)");
    CHECK(c.mismatched == 0, "Resolve dual output: every shown pair is the same frame on both eyes");
    CHECK(c.repeats == 0, "...each frame shown once, in order");
    CHECK(c.shown >= 590, "...one pair per frame");
    CHECK(c.unsynced == 0, "...never newest-of-each");
    CHECK(c.byClock == c.shown, "...all by the card's clock");
    CHECK(c.p.ByClock(), "...and it stays on the clock (the timecodes never agree)");

    ClockSim old;  // the same frames without clock keys: the old newest-of-each fallback
    old.useClock = false;
    old.Run(0, 600, tcOf);
    std::printf("  Resolve dual output, no clock (old): %d shown, %d mismatched\n", old.shown, old.mismatched);
    CHECK(old.mismatched > 0, "sanity: newest-of-each does pair the eyes a frame apart (the bug)");
}

// One card, no timecode at all: by the clock from the first frame.
void NoTimecodeSameCard() {
    ClockSim c;
    c.Run(0, 300, [](int, int) { return -1; });
    CHECK(c.mismatched == 0 && c.repeats == 0 && c.shown >= 290, "no timecode, one card: exact pairs by the clock");
    CHECK(c.byClock == c.shown && c.unsynced == 0, "...all by the clock");
}

// One card, jam-synced timecode on both eyes: timecode pairs as before, the clock is unused.
void MatchingTimecodeSameCard() {
    ClockSim c;
    c.Run(0, 300, [](int, int i) { return 5000 + i; });
    CHECK(c.mismatched == 0 && c.shown >= 290, "matching timecode, one card: exact pairs");
    CHECK(c.byClock == 0 && !c.p.ByClock(), "...by timecode, not the clock");
}

// The right eye's timecode starts matching (Resolve fixed mid-session): the clock pairs
// until kTrustTimecodeAfter frames in a row agree, then timecode again; exact throughout.
void TimecodeTrustedAgain() {
    const int fixAt = 200;
    ClockSim c;
    c.Run(0, 500, [&](int eye, int i) { return eye == 0 || i >= fixAt ? 7000 + i : 0; });
    std::printf("  timecode fixed at frame %d: %d shown, %d mismatched, %d by clock, by clock at the end: %d\n",
                fixAt, c.shown, c.mismatched, c.byClock, (int)c.p.ByClock());
    CHECK(c.mismatched == 0 && c.repeats == 0, "switching back to timecode keeps every pair exact");
    CHECK(!c.p.ByClock(), "timecode is trusted again once both eyes agree");
    CHECK(c.byClock >= fixAt - 5 && c.byClock <= fixAt + TimecodePairer::kTrustTimecodeAfter + 10,
          "...after kTrustTimecodeAfter agreeing frames, not before");
}

// Two cards (no shared clock: no clock keys) with unrelated timecodes: unchanged, newest of
// each flagged unsynced (the Unsynced case, through the new argument).
void UnsyncedTwoCards() {
    Sim s;
    for (int i = 0; i < 20; ++i) {
        const int a = s.p.NextSlot(0);
        s.p.MarkBusy(0, a);
        s.slotKey[0][a] = 1000 + i;
        TimecodePairer::Decision d = s.p.Add(0, a, 1000 + i, 0.0, 0.04, -1);
        if (d.publish && d.unsynced) ++s.unsynced;
        CHECK(!d.byClock, "two cards: never by clock");
        const int b = s.p.NextSlot(1);
        s.p.MarkBusy(1, b);
        s.slotKey[1][b] = 70000 + i;
        d = s.p.Add(1, b, 70000 + i, 0.0, 0.04, -1);
        if (d.publish && d.unsynced) ++s.unsynced;
    }
    CHECK(s.unsynced >= 15, "two cards, unrelated timecodes: newest of each, flagged unsynced");
}

}  // namespace

// Matched pairs only (strict, one card): the right eye's lane falls 12 frames behind (it
// skips frames 100..111, as a busy lane does: newest wins), then resumes. Without strict
// the pairer showed the newest of each eye meanwhile: a left and a right from different
// instants (seen on the 8K station as eye sync 1-3 frames, max skew 133 ms). With strict
// every shown pair is one frame on both eyes, and the picture resumes once they match.
void StrictMatchedPairsOnly() {
    for (int strict = 0; strict <= 1; ++strict) {
        ClockSim c;
        c.p.SetStrict(strict != 0);
        const int resumeAt = 112;
        for (int i = 0; i < 300; ++i) {
            if (i >= 100 && i < resumeAt) {  // right lane behind: only the left eye lands
                c.Frame(0, i, 409672 + i);
                continue;
            }
            c.Frame(1, i, 0);  // Resolve: no usable timecode on the right eye
            c.Frame(0, i, 409672 + i);
        }
        std::printf("  right lane 12 frames behind, strict=%d: %d shown, %d mismatched, %d repeats\n", strict, c.shown,
                    c.mismatched, c.repeats);
        if (strict) {
            CHECK(c.mismatched == 0, "strict: no pair with a left and right from different instants, ever");
            CHECK(c.repeats == 0, "strict: nothing shown twice");
            CHECK(c.shown >= 280, "strict: pairs resume once the lagging eye catches up");
        } else {
            CHECK(c.mismatched > 0, "sanity: without strict, newest-of-each shows mismatched pairs (the bug)");
        }
    }
    // Strict survives Reset (a format change resets the pairing, not the policy).
    TimecodePairer p;
    p.SetStrict(true);
    p.Reset();
    CHECK(p.Strict(), "Reset keeps strict");
}

int main() {
    FullRateTimecode();
    for (int lag = 0; lag < TimecodePairer::kHistory; ++lag) {
        SteadyLag(lag, true);
        SteadyLag(lag, false);
    }
    SlowConversion();
    for (int lag = TimecodePairer::kHistory; lag <= TimecodePairer::kHistory + 2; ++lag) {
        LongLag(lag, true);
        LongLag(lag, false);
    }
    RewindNoReplay();
    TimecodeStops();
    TimecodeRewinds();
    Unsynced();
    NoTimecode();
    OneEyeMissing();
    HighRateTimecodeKeys();
    HardwareClockKeys();
    ResolveDualOutput();
    StrictMatchedPairsOnly();
    NoTimecodeSameCard();
    MatchingTimecodeSameCard();
    TimecodeTrustedAgain();
    UnsyncedTwoCards();
    if (g_failures) {
        std::fprintf(stderr, "timecode_pairer_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("timecode_pairer_test: all passed\n");
    return 0;
}
