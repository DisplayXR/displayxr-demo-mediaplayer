// SPDX-License-Identifier: Apache-2.0
// Pro's audio meters (media/AudioMeter.h, LOXR-871): levels follow what is HEARD, not what is
// queued; peak / RMS are right for float and the card's int32 samples; the display's
// ballistics fall, hold and latch a clip as documented.
#include "media/AudioMeter.h"
#include "media/AudioSolo.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                           \
        }                                                                       \
    } while (0)

using namespace mp;

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

int main() {
    // A constant 0.5 on L, -0.25 on R: peak and RMS are the magnitudes.
    {
        AudioMeter m;
        m.Watch();
        std::vector<float> s(2 * 100);
        for (int i = 0; i < 100; ++i) s[2 * i] = 0.5f, s[2 * i + 1] = -0.25f;
        m.AddFloat(s.data(), 100, 2, 0);
        CHECK(!m.Take().any);                // queued, not heard yet: nothing to show
        m.AddFloat(s.data(), 100, 2, 0);     // the queue drained before this: block 1 heard
        AudioMeter::Reading r = m.Take();
        CHECK(r.any && r.channels == 2);
        CHECK(Near(r.peak[0], 0.5f) && Near(r.peak[1], 0.25f));
        CHECK(Near(r.rms[0], 0.5f) && Near(r.rms[1], 0.25f));
        CHECK(!m.Take().any);                // each block is read once
    }
    // Timing: with 150 frames still queued ahead, a 100-frame block is not heard yet.
    {
        AudioMeter m;
        m.Watch();
        std::vector<float> loud(100, 1.0f), quiet(100, 0.0f);
        m.AddFloat(loud.data(), 100, 1, 0);     // pushed 100
        m.AddFloat(quiet.data(), 100, 1, 150);  // 150 queued > the 100 pushed: nothing heard
        CHECK(!m.Take().any);
        m.AddFloat(quiet.data(), 100, 1, 100);  // 200 pushed, 100 queued: block 1 heard
        AudioMeter::Reading r = m.Take();
        CHECK(r.any && Near(r.peak[0], 1.0f));
        m.AddFloat(quiet.data(), 100, 1, 1000);  // a larger queue never moves "heard" back
        CHECK(!m.Take().any);
    }
    // A sine at amplitude 1: RMS 1/sqrt(2).
    {
        AudioMeter m;
        m.Watch();
        const int n = 4800;
        std::vector<float> s(n);
        for (int i = 0; i < n; ++i) s[i] = (float)std::sin(2.0 * 3.14159265358979 * 1000.0 * i / 48000.0);
        m.AddFloat(s.data(), n, 1, 0);
        m.AddFloat(s.data(), 1, 1, 0);
        AudioMeter::Reading r = m.Take();
        CHECK(Near(r.rms[0], 0.7071f, 2e-3f) && Near(r.peak[0], 1.0f, 1e-3f));
    }
    // The card's int32 samples, 8 channels: channel k carries k/8 of full scale.
    {
        AudioMeter m;
        m.Watch();
        std::vector<int32_t> s(8 * 10);
        for (int i = 0; i < 10; ++i)
            for (int k = 0; k < 8; ++k) s[i * 8 + k] = (int32_t)(2147483647.0 * k / 8.0) * (i % 2 ? -1 : 1);
        m.AddInt32(s.data(), 10, 8, 0);
        m.AddInt32(s.data(), 10, 8, 0);
        AudioMeter::Reading r = m.Take();
        CHECK(r.channels == 8);
        for (int k = 0; k < 8; ++k) CHECK(Near(r.peak[k], k / 8.0f) && Near(r.rms[k], k / 8.0f));
    }
    // Clear (seek / resync): what was queued is never shown.
    {
        AudioMeter m;
        m.Watch();
        std::vector<float> s(10, 0.9f);
        m.AddFloat(s.data(), 10, 1, 0);
        m.Clear();
        CHECK(!m.Take().any);
        m.AddFloat(s.data(), 10, 1, 0);
        m.AddFloat(s.data(), 10, 1, 0);
        CHECK(m.Take().any);
    }
    // Bounded: nobody reading (panel closed) never grows past kMaxBlocks.
    {
        AudioMeter m;
        m.Watch();
        float x = 0.1f;
        for (int i = 0; i < 5000; ++i) m.AddFloat(&x, 1, 1, 0);
        AudioMeter::Reading r = m.Take();
        CHECK(r.any);
    }
    // Nobody looking (Audio section closed): Add does nothing, and blocks from before a
    // reopen are not shown.
    {
        AudioMeter m;  // never watched
        float x = 0.5f;
        CHECK(!m.Watching());
        m.AddFloat(&x, 1, 1, 0);
        m.AddFloat(&x, 1, 1, 0);
        CHECK(!m.Take().any);
        m.Watch();
        CHECK(m.Watching());
        float y = 0.25f;
        m.AddFloat(&y, 1, 1, 0);
        m.AddFloat(&y, 1, 1, 0);
        AudioMeter::Reading r = m.Take();
        CHECK(r.any && Near(r.peak[0], 0.25f));  // only what came after Watch()
    }
    // A/V sync (LOXR-873): + offset = the video runs ahead of the sound by that much.
    CHECK(ShiftedAudioClock(10.0, 0) == 10.0);
    CHECK(Near((float)ShiftedAudioClock(10.0, 250), 10.25f) && Near((float)ShiftedAudioClock(10.0, -250), 9.75f));
    CHECK(ShiftedAudioClock(0.1, -500) == 0.0);    // never negative ("audio gone" to the decoder)
    CHECK(ShiftedAudioClock(-1.0, 300) == -1.0);   // no audio clock stays "none"
    // Channel solo (LOXR-874).
    {
        float st[] = {0.1f, 0.2f, 0.3f, 0.4f};  // 2 frames of stereo
        SoloFloatInPlace(st, 2, 2, 1);          // R alone, on both sides
        CHECK(st[0] == 0.2f && st[1] == 0.2f && st[2] == 0.4f && st[3] == 0.4f);
        float six[] = {1, 2, 3, 4, 5, 6};      // one 5.1 frame: FL FR FC LFE BL BR
        SoloFloatInPlace(six, 1, 6, 2);         // the centre alone, on the front pair
        CHECK(six[0] == 3 && six[1] == 3 && six[2] == 0 && six[3] == 0 && six[4] == 0 && six[5] == 0);
        float keep[] = {0.5f, 0.6f};
        SoloFloatInPlace(keep, 1, 2, -1);       // no solo
        SoloFloatInPlace(keep, 1, 2, 2);        // out of range: no solo
        CHECK(keep[0] == 0.5f && keep[1] == 0.6f);
        int32_t card[3 * 8];                    // 3 frames of 8 SDI channels
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 8; ++k) card[i * 8 + k] = i * 100 + k;
        int32_t out[6] = {};
        CHECK(ExtractSoloStereo(card, 3, 8, 6, out));  // ch 7
        CHECK(out[0] == 6 && out[1] == 6 && out[2] == 106 && out[3] == 106 && out[4] == 206 && out[5] == 206);
        CHECK(!ExtractSoloStereo(card, 3, 8, 8, out) && !ExtractSoloStereo(card, 3, 8, -1, out));
    }
    CHECK(MeterDb(1.0f) == 0.0f && Near(MeterDb(0.5f), -6.0206f) && MeterDb(0.0f) == kMeterFloorDb);

    // Ballistics.
    {
        MeterDisplay d;
        AudioMeter::Reading r;
        r.any = true;
        r.channels = 2;
        r.peak[0] = 1.0f;  // full scale on L: 0 dB and a clip
        r.rms[0] = 0.5f;
        r.peak[1] = 0.1f;  // -20 dB on R
        r.rms[1] = 0.1f;
        d.Update(r, 0.02f);
        CHECK(d.channels == 2 && Near(d.peakDb[0], 0.0f) && d.Clipping(0) && !d.Clipping(1));
        CHECK(Near(d.holdDb[1], -20.0f));
        AudioMeter::Reading none;  // silence from here
        d.Update(none, 0.5f);
        CHECK(Near(d.peakDb[0], -12.0f, 0.01f));  // falls 24 dB/s
        CHECK(Near(d.holdDb[0], 0.0f));           // the hold stays for 2 s
        CHECK(d.Clipping(0));
        for (int i = 0; i < 4; ++i) d.Update(none, 0.5f);  // 2.5 s in
        CHECK(d.holdDb[0] < 0.0f);                // ...then falls
        CHECK(d.Clipping(0));                     // the clip light stays 3 s...
        d.Update(none, 0.6f);
        CHECK(!d.Clipping(0));                    // ...then goes out
        CHECK(d.channels == 2);                   // silence keeps the last channel count
        for (int i = 0; i < 20; ++i) d.Update(none, 0.25f);
        CHECK(d.peakDb[0] == kMeterFloorDb && d.rmsDb[0] < -90.0f);
    }

    if (g_fail) std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    else std::printf("audio meter: all checks passed\n");
    return g_fail ? 1 : 0;
}
