// SPDX-License-Identifier: Apache-2.0
//
// Two-input stereo pairing (DeckLink Stereo::Pair). Simulates two SDI inputs delivering
// frames with SMPTE timecode in different interleavings and checks what is shown: every
// shown pair has matching timecode, each instant is shown once, nothing freezes - the
// cases a code review found broken (a steady 3-frame lag, timecode that stops or rewinds).

#include "pro/TimecodePairer.h"

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

}  // namespace

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
    if (g_failures) {
        std::fprintf(stderr, "timecode_pairer_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("timecode_pairer_test: all passed\n");
    return 0;
}
