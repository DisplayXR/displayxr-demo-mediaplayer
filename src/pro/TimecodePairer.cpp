// SPDX-License-Identifier: Apache-2.0
#include "pro/TimecodePairer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mp {

namespace {

// The frame number at the full rate: pairs (ff 0..29, field mark = second) unfolded.
int FullRateFrame(const TimecodeFields& t, int fps) {
    if (fps <= 30 || t.highFrameRate || t.fullCount) return t.ff;
    return t.ff * 2 + (t.fieldMark ? 1 : 0);
}

}  // namespace

int TimecodeFrames(const TimecodeFields& t, int fpsNominal) {
    const int fps = fpsNominal > 0 ? fpsNominal : 25;
    int frames = ((t.hh * 60 + t.mm) * 60 + t.ss) * fps + FullRateFrame(t, fps);
    if (t.dropFrame && (fps == 30 || fps == 60)) {
        const int drop = fps / 15;  // 2 at 29.97, 4 at 59.94
        const int minutes = t.hh * 60 + t.mm;
        frames -= drop * (minutes - minutes / 10);
    }
    return frames;
}

bool TimecodeCountsEveryFrame(const TimecodeFields& t, int fpsNominal) {
    return fpsNominal > 30 && t.ff >= (fpsNominal + 1) / 2;
}

void FormatTimecode(const TimecodeFields& t, int fpsNominal, char* out, size_t n) {
    const int fps = fpsNominal > 0 ? fpsNominal : 25;
    std::snprintf(out, n, "%02d:%02d:%02d%c%02d", t.hh, t.mm, t.ss, t.dropFrame ? ';' : ':', FullRateFrame(t, fps));
}

void TimecodePairer::Reset() { *this = TimecodePairer{}; }

TimecodePairer::Decision TimecodePairer::Add(int eye, int slot, int key, double arrived, double period) {
    Decision d;
    key_[eye][slot] = key;
    arrived_[eye][slot] = arrived;
    valid_[eye][slot] = true;
    newest_[eye] = slot;
    next_[eye] = (slot + 1) % kHistory;
    if (eye == 0) ++sincePublish_;
    if (key >= 0 && lastKey_ >= 0 && key < lastKey_) {
        // This input went back (a deck rewind, midnight): the frames remembered from before
        // the jump would match "newer" keys and be shown again. Forget every frame keyed
        // after the new timecode, on both inputs, and wait for the other one to follow.
        for (int e2 = 0; e2 < 2; ++e2)
            for (int a = 0; a < kHistory; ++a)
                if (valid_[e2][a] && key_[e2][a] > key) valid_[e2][a] = false;
        const int other = 1 - eye;
        if (newest_[other] >= 0 && !valid_[other][newest_[other]]) newest_[other] = -1;
        lastKey_ = -1;
    }
    if (newest_[0] < 0 || newest_[1] < 0) return d;
    const int n0 = key_[0][newest_[0]], n1 = key_[1][newest_[1]];

    auto newestOfEach = [&](bool unsynced) {
        d.publish = true;
        d.slot[0] = newest_[0];
        d.slot[1] = newest_[1];
        d.unsynced = unsynced;
        sincePublish_ = 0;
    };

    if (n0 >= 0 && n1 >= 0) {
        d.offsetFrames = std::abs(n0 - n1);
        if (n0 <= lastKey_ && n1 <= lastKey_) {
            // Timecode held still (a camera not rolling): it says nothing about which frames
            // belong together, so pair the newest of each once per left frame, as without
            // timecode. Gone back (a rewind, midnight): start matching again.
            if (n0 == lastKey_ && n1 == lastKey_) {
                if (eye == 0) newestOfEach(false);
                return d;
            }
            lastKey_ = -1;
        }
        // The newest instant both eyes have, newer than the last one shown.
        int best = -1, s0 = -1, s1 = -1;
        for (int a = 0; a < kHistory; ++a) {
            if (!valid_[0][a] || key_[0][a] <= lastKey_ || key_[0][a] <= best) continue;
            for (int b = 0; b < kHistory; ++b)
                if (valid_[1][b] && key_[1][b] == key_[0][a]) {
                    best = key_[0][a];
                    s0 = a;
                    s1 = b;
                }
        }
        if (best >= 0) {
            d.publish = true;
            d.slot[0] = s0;
            d.slot[1] = s1;
            lastKey_ = best;
            sincePublish_ = 0;
            return d;
        }
        // No common instant yet. Wait only while the lagging eye's NEXT frame can still find
        // its match when it lands: the leading eye keeps kHistory frames, so after a
        // leading-eye arrival a lag of up to kHistory can still match, and after a
        // lagging-eye arrival (the leading eye moves on before the next one) kHistory-1.
        const int lagEye = n0 < n1 ? 0 : 1;
        const int lag = n0 < n1 ? n1 - n0 : n0 - n1;
        const int reach = (eye == lagEye) ? kHistory - 1 : kHistory;
        if (lag <= reach && sincePublish_ <= lag + 1) return d;  // no longer than the lag explains
        // The inputs' clocks are too far apart to pair: newest of each, paced by the left.
        if (eye == 0) newestOfEach(true);
        return d;
    }

    // No timecode: newest of each, paced by the left eye's arrivals.
    if (eye != 0) return d;
    if (period > 0.0) {
        const double dt = std::fabs(arrived_[0][newest_[0]] - arrived_[1][newest_[1]]);
        d.offsetFrames = (int)std::lround(dt / period);
    }
    newestOfEach(false);
    return d;
}

}  // namespace mp
