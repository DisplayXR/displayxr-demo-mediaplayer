// SPDX-License-Identifier: Apache-2.0
//
// FrameRing queue mode (live sources): one frame per refresh, a one-frame cushion,
// and a clock running a few hundred ppm off the display costs single drops/repeats,
// not bursts. Simulates producer and display clocks with jitter on one thread.
#include "media/FrameRing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using mp::FrameRing;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

struct Result {
    long drops = 0, repeats = 0, shown = 0, published = 0;
    long maxSkip = 0;  // largest serial jump between consecutive shown frames
    bool inOrder = true;
};

// Producer at srcHz, display at dispHz, both with +-jitterMs uniform timing noise, for
// `seconds`. Events are merged in time order; a frame arriving at the same instant as a
// refresh is the worst case latest-wins suffers from.
static Result Run(int queueDepth, double srcHz, double dispHz, double jitterMs, double seconds,
                  double phaseMs = 0.0) {
    FrameRing ring;
    ring.SetQueueDepth(queueDepth);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> j(-jitterMs / 1000.0, jitterMs / 1000.0);
    Result r;
    long nSrc = 0, nDisp = 0;
    int64_t lastSerial = 0;
    double tSrc = phaseMs / 1000.0 + j(rng), tDisp = j(rng);
    while (tSrc < seconds || tDisp < seconds) {
        if (tSrc <= tDisp) {
            ring.WriteBuffer();
            if (ring.Publish()) ++r.drops;
            ++r.published;
            ++nSrc;
            tSrc = phaseMs / 1000.0 + nSrc / srcHz + j(rng);
        } else {
            if (const FrameRing::Frame* f = ring.AcquireLatest()) {
                ++r.shown;
                if (lastSerial && f->serial <= lastSerial) r.inOrder = false;
                if (lastSerial) r.maxSkip = std::max<long>(r.maxSkip, (long)(f->serial - lastSerial));
                lastSerial = f->serial;
            } else if (r.published) {
                ++r.repeats;
            }
            ++nDisp;
            tDisp = nDisp / dispHz + j(rng);
        }
    }
    if (queueDepth > 0) r.drops += (long)ring.TrimmedFrames();
    return r;
}

int main() {
    // 1. Locked clocks, frames landing right on the refresh with 2 ms jitter: latest-wins
    //    drops and repeats all the time; the queue shows every frame once.
    {
        const Result lw = Run(0, 60.0, 60.0, 2.0, 60.0);
        const Result q = Run(3, 60.0, 60.0, 2.0, 60.0);
        std::printf("locked, on-phase: latest-wins drops %ld repeats %ld | queue drops %ld repeats %ld\n",
                    lw.drops, lw.repeats, q.drops, q.repeats);
        CHECK(lw.drops > 100);  // the burst the soak saw
        CHECK(q.drops == 0);
        CHECK(q.repeats <= 2);  // only the start-up cushion
        CHECK(q.inOrder);
        CHECK(q.maxSkip == 1);
    }
    // 2. Source 245 ppm FAST (Suki's box at 60.000 vs a 59.985 panel), 10 min: it gains
    //    ~8.8 frames, so ~9 single drops and nothing else; never two frames skipped at once.
    {
        const double disp = 60.0, src = disp * (1.0 + 245e-6);
        const Result lw = Run(0, src, disp, 2.0, 600.0);
        const Result q = Run(3, src, disp, 2.0, 600.0);
        const double gained = 600.0 * (src - disp);
        std::printf("source +245 ppm: gained %.1f frames; latest-wins drops %ld | queue drops %ld repeats %ld\n",
                    gained, lw.drops, q.drops, q.repeats);
        CHECK(q.drops >= (long)std::floor(gained) - 1 && q.drops <= (long)std::ceil(gained) + 2);
        CHECK(q.repeats <= 2);
        CHECK(lw.drops > 5 * q.drops);
        CHECK(q.maxSkip <= 2);
        CHECK(q.inOrder);
    }
    // 3. Source 245 ppm SLOW: the display runs out ~9 times -> ~9 repeats, no drops.
    {
        const double disp = 60.0, src = disp * (1.0 - 245e-6);
        const Result q = Run(3, src, disp, 2.0, 600.0);
        const double lost = 600.0 * (disp - src);
        std::printf("source -245 ppm: short %.1f frames; queue drops %ld repeats %ld\n", lost, q.drops, q.repeats);
        CHECK(q.drops == 0);
        CHECK(q.repeats >= (long)std::floor(lost) - 1 && q.repeats <= (long)std::ceil(lost) + 3);
        CHECK(q.maxSkip == 1);
    }
    // 4. A real rate mismatch (59.94 source on a 60 Hz panel): one repeat per 1001 frames,
    //    nothing else; 50 Hz on 60 Hz: the queue must not grow (repeats, no drops).
    {
        const Result q = Run(3, 59.94, 60.0, 2.0, 120.0);
        std::printf("59.94 on 60: queue drops %ld repeats %ld\n", q.drops, q.repeats);
        CHECK(q.drops == 0);
        CHECK(q.repeats >= 6 && q.repeats <= 10);
        const Result q50 = Run(3, 50.0, 60.0, 2.0, 60.0);
        CHECK(q50.drops == 0);
        CHECK(q50.repeats >= 590 && q50.repeats <= 610);
    }
    // 5. A source much faster than the display (60 on 30 Hz): the queue keeps up by
    //    dropping, latency stays bounded (depth), order kept.
    {
        const Result q = Run(3, 60.0, 30.0, 1.0, 60.0);
        std::printf("60 on 30: queue drops %ld shown %ld\n", q.drops, q.shown);
        CHECK(q.drops >= 1700);
        CHECK(q.inOrder);
    }
    // 6. Latest-wins mode is untouched: no counters, plain swap semantics.
    {
        FrameRing ring;
        CHECK(!ring.QueueMode());
        ring.WriteBuffer();
        CHECK(!ring.Publish());
        ring.WriteBuffer();
        CHECK(ring.Publish());  // replaced an unacquired frame
        const FrameRing::Frame* f = ring.AcquireLatest();
        CHECK(f && f->serial == 2);
        CHECK(ring.AcquireLatest() == nullptr);
        CHECK(ring.RepeatedFrames() == 0);
    }
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("frame_ring_queue: all passed\n");
    return 0;
}
