// SPDX-License-Identifier: Apache-2.0
//
// Two-input stereo pairing (DeckLink Stereo::Pair). Simulates two SDI inputs delivering
// frames with SMPTE timecode in different interleavings and checks what is shown: every
// shown pair has matching timecode, each instant is shown once, nothing freezes - the
// cases a code review found broken (a steady 3-frame lag, timecode that stops or rewinds).

#include "pro/TimecodePairer.h"

#include <cstdio>
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

}  // namespace

int main() {
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
