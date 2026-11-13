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
    // 7. Reserved buffers (Pro's DeckLink direct pairing). Latest-wins has none; queue mode
    //    hands out at most kMaxReserved, never one the consumer, a pin or the queue holds,
    //    and never so many that Publish() finds no free buffer.
    {
        FrameRing lw;
        CHECK(lw.ReserveBuffer() == nullptr);

        FrameRing ring;
        ring.SetQueueDepth(FrameRing::kMaxQueueDepth);  // the deepest queue: the tightest count
        for (int k = 0; k < FrameRing::kMaxQueueDepth; ++k) {  // fill the queue
            ring.WriteBuffer().plane[0].assign(1, (uint8_t)(10 + k));
            ring.Publish();
        }
        const FrameRing::Frame* taken = ring.AcquireLatest();  // the consumer holds one
        const FrameRing::Frame* pin = ring.PinNextQueued(taken->serial);  // the upload thread pins one
        CHECK(taken && pin);
        FrameRing::Frame* res[FrameRing::kMaxReserved + 1] = {};
        for (int i = 0; i <= FrameRing::kMaxReserved; ++i) res[i] = ring.ReserveBuffer();
        for (int i = 0; i < FrameRing::kMaxReserved; ++i) {
            CHECK(res[i] != nullptr);
            CHECK(res[i] != taken && res[i] != pin && res[i] != &ring.WriteBuffer());
            for (int j = 0; j < i; ++j) CHECK(res[i] != res[j]);
        }
        CHECK(res[FrameRing::kMaxReserved] == nullptr);  // kMaxReserved at most
        CHECK(ring.ReservedCount() == FrameRing::kMaxReserved);
        // A reserved buffer is in no queue: the consumer and the pin never see it.
        for (int i = 0; i < FrameRing::kMaxReserved; ++i) res[i]->plane[0].assign(1, (uint8_t)(100 + i));
        int seen = 0;
        while (const FrameRing::Frame* f = ring.AcquireLatest()) {
            for (int i = 0; i < FrameRing::kMaxReserved; ++i) CHECK(f != res[i]);
            ++seen;
        }
        CHECK(seen == FrameRing::kMaxQueueDepth - 1);
        // With every reservation out, a full queue and a parked pin, Publish() still has a
        // free buffer, every time (the count kMaxBuffers is built from).
        ring.Unpin(pin);
        for (int k = 0; k < FrameRing::kMaxQueueDepth; ++k) {
            ring.WriteBuffer();
            ring.Publish();
        }
        const FrameRing::Frame* pin2 = ring.PinNextQueued(0);  // pinned at the head of a full queue...
        CHECK(pin2 != nullptr);
        for (int k = 0; k < 50; ++k) {  // ...dropped and parked by the publishes that follow
            FrameRing::Frame& w = ring.WriteBuffer();
            for (int i = 0; i < FrameRing::kMaxReserved; ++i) CHECK(&w != res[i]);
            CHECK(&w != pin2);
            ring.Publish();
        }
        for (int i = 0; i < FrameRing::kMaxReserved; ++i) CHECK(res[i]->plane[0][0] == (uint8_t)(100 + i));
        // Publishing a reserved buffer: a new serial, drop-oldest on a full queue, in order.
        const bool dropped = ring.PublishReserved(res[0]);
        CHECK(dropped);  // the queue was full
        CHECK(ring.ReservedCount() == FrameRing::kMaxReserved - 1);
        CHECK(!ring.PublishReserved(res[0]));  // not reserved any more: nothing
        const FrameRing::Frame* last = nullptr;
        while (const FrameRing::Frame* f = ring.AcquireLatest()) last = f;
        CHECK(last == res[0] && last->plane[0][0] == 100);  // the newest, taken last
        // Cancel: back to the free list, never queued.
        ring.CancelReserved(res[1]);
        ring.CancelReserved(res[1]);  // twice: nothing
        CHECK(ring.ReservedCount() == FrameRing::kMaxReserved - 2);
        CHECK(ring.AcquireLatest() == nullptr);
        ring.Unpin(pin2);
        ring.CancelReserved(res[2]);
        CHECK(ring.ReservedCount() == 0);
    }
    // 8. A ring that publishes reserved buffers and its twin that publishes WriteBuffer() see
    //    the same cadence: drops, trims, repeats and the frames taken, under a pin.
    {
        std::mt19937 rng(99);
        FrameRing a, b;
        a.SetQueueDepth(3);
        b.SetQueueDepth(3);
        long dropsA = 0, dropsB = 0, k = 0;
        // reservations kept out meanwhile (a lane's half still converting): up to all but one
        FrameRing::Frame* held[FrameRing::kMaxReserved - 1] = {};
        const FrameRing::Frame* pin = nullptr;
        for (int step = 0; step < 40000; ++step) {
            const unsigned r = rng() % 100;
            if (r < 47) {  // a frame: through a reserved buffer on a, the old way on b
                ++k;
                FrameRing::Frame* f = a.ReserveBuffer();
                CHECK(f != nullptr);
                if (!f) break;
                f->plane[0].assign(1, (uint8_t)k);
                b.WriteBuffer().plane[0].assign(1, (uint8_t)k);
                dropsA += a.PublishReserved(f) ? 1 : 0;
                dropsB += b.Publish() ? 1 : 0;
                for (FrameRing::Frame*& h : held)
                    if (!h && rng() % 4 == 0) {
                        h = a.ReserveBuffer();
                        CHECK(h != nullptr);
                        break;
                    }
            } else if (r < 94) {
                const FrameRing::Frame* fa = a.AcquireLatest();
                const FrameRing::Frame* fb = b.AcquireLatest();
                CHECK((fa == nullptr) == (fb == nullptr));
                if (fa && fb) CHECK(fa->serial == fb->serial && fa->plane[0][0] == fb->plane[0][0]);
                for (FrameRing::Frame* h : held) CHECK(!h || fa != h);
            } else if (r < 97) {
                if (!pin) pin = a.PinNextQueued(0);
                else {
                    a.Unpin(pin);
                    pin = nullptr;
                }
            } else {
                for (FrameRing::Frame*& h : held)
                    if (h) {
                        a.CancelReserved(h);
                        h = nullptr;
                    }
            }
        }
        if (pin) a.Unpin(pin);
        CHECK(dropsA == dropsB);
        CHECK(a.TrimmedFrames() == b.TrimmedFrames());
        CHECK(a.RepeatedFrames() == b.RepeatedFrames());
        std::printf("reserved twin: %ld frames, drops %ld, trims %llu, repeats %llu (same on both)\n", k, dropsA,
                    (unsigned long long)a.TrimmedFrames(), (unsigned long long)a.RepeatedFrames());
    }
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("frame_ring_queue: all passed\n");
    return 0;
}
