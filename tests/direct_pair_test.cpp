// SPDX-License-Identifier: Apache-2.0
//
// DeckLink direct pairing (Media Player Pro 0.9.8, pro/DirectPair.h): each eye's lane
// converts straight into its half of a reserved ring frame, and a pair of two such halves is
// published whole, without the 47.5 MB compose.
//
//  1. Bit-identical. The new way (FrameRing::ReserveBuffer -> each eye converted into its half
//     -> LiveSourceBase::PublishPairFrame) and today's (each eye converted into its own P016
//     -> LiveSourceBase::PublishPair's compose) publish the same ring frame byte for byte:
//     planes, size, format, range, matrix, transfer, capture time, timecode, eye offset. Every
//     raw format (UYVY, v210, BGRA, r210, and two different ones per pair), odd card sizes,
//     1 / 3 / 7 convert threads, the card's colorimetry or not, the two halves converted at
//     the same time on two threads into a frame first filled with garbage. Also the compose
//     fallback reading one eye from a half: the same as from its own P016.
//  2. The entry table's rules (claim, join, done, whole, retire, release, recycle, pins).
//  3. The lane protocol over a real ring: the pairer, the table and LiveSourceBase as
//     DeckLinkCapture::ProcessPairDirect / PublishDirect drive them (mirrored here: the
//     capture code needs the SDK), the two lanes' claims and finishes interleaved. Every
//     published frame carries exactly the pixels of the two frames the pairer decided;
//     Resolve's dual output, jam-synced timecode, dropped frames, an eye far behind (strict
//     matched pairs only), a rewind, timecode held still, inputs of different sizes, more
//     keys in flight than ring frames, a format change mid-frame, two unsynced cards (never
//     direct); every reserved frame comes back.
//  4. The same protocol on real threads: two lanes, a consumer, the upload thread's pin; no
//     frame the renderer or the pin sees is ever half written or changes under the pin.
// Pure: no SDK, no card, no GPU.

#include "media/FrameRing.h"
#include "pro/ConvertPool.h"
#include "pro/DirectPair.h"
#include "pro/LivePixels.h"
#include "pro/LiveSourceBase.h"
#include "pro/TimecodePairer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using mp::ColorMatrix;
using mp::ColorTransfer;
using mp::DirectPairTable;
using mp::FrameRing;
using mp::PixFormat;
using mp::TimecodePairer;
using mp::livepix::ConvertPool;
using mp::livepix::EyeJob;
using mp::livepix::P016;
using mp::livepix::RawFormat;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// A live source with the protected publish paths opened up.
class TestSource : public mp::LiveSourceBase {
public:
    explicit TestSource(int queueDepth = 3) { Ring().SetQueueDepth(queueDepth); }
    void Stop() override {}
    const char* Kind() const override { return "test"; }
    using LiveSourceBase::CancelPairFrame;
    using LiveSourceBase::PublishPair;
    using LiveSourceBase::PublishPairFrame;
    using LiveSourceBase::ReservePairFrame;
};

// ---------------------------------------------------------------------------
// 1. Bit-identical
// ---------------------------------------------------------------------------

const char* Name(RawFormat f) {
    switch (f) {
        case RawFormat::Uyvy: return "UYVY";
        case RawFormat::V210: return "v210";
        case RawFormat::Bgra: return "BGRA";
        case RawFormat::R210: return "r210";
    }
    return "?";
}

// Row bytes as the card lays them out (v210: 48-pixel blocks of 128 bytes; r210: 64-pixel
// blocks of 256), UYVY / BGRA with some padding.
int RowBytes(RawFormat f, int w) {
    switch (f) {
        case RawFormat::Uyvy: return 2 * (w + 1) + 64;
        case RawFormat::V210: return ((w + 47) / 48) * 128;
        case RawFormat::Bgra: return 4 * w + 64;
        case RawFormat::R210: return ((w + 63) / 64) * 256;
    }
    return 0;
}

// One eye as the card hands it to a lane, and the frame's metadata (DeckLinkCapture::RawFrame).
struct Eye {
    RawFormat fmt = RawFormat::V210;
    int w = 0, h = 0, rowBytes = 0;  // the card's size (odd allowed); the lane converts w&~1 x h&~1
    std::vector<uint8_t> bytes;
    bool hasMatrix = false;
    ColorMatrix matrix = ColorMatrix::BT709;
    ColorTransfer transfer = ColorTransfer::SDR;
    double arrived = 0.0;
    char tc[16] = {0};
};

Eye RandomEye(RawFormat f, int w, int h, uint32_t seed) {
    Eye e;
    e.fmt = f;
    e.w = w;
    e.h = h;
    e.rowBytes = RowBytes(f, w);
    e.bytes.resize((size_t)e.rowBytes * h);
    for (uint8_t& b : e.bytes) {
        seed = seed * 1664525u + 1013904223u;
        b = (uint8_t)(seed >> 24);
    }
    return e;
}

// DeckLinkSource's RawJob: the job for one eye, before its destination is set.
EyeJob JobFor(const Eye& e, int x0) {
    EyeJob j;
    j.fmt = e.fmt;
    j.src = e.bytes.data();
    j.srcRowBytes = e.rowBytes;
    j.w = e.w & ~1;
    j.h = e.h & ~1;
    j.x0 = x0;
    j.rgbMatrix = e.hasMatrix ? e.matrix : mp::DefaultMatrixForHeight(j.h);
    return j;
}

// What the consumer sees of a published frame, all of it.
struct Shot {
    bool got = false;
    std::vector<uint8_t> y, uv;
    int width = 0, height = 0;
    PixFormat format = PixFormat::I420;
    bool fullRange = true, gpu = true, primaries2020 = true;
    ColorMatrix matrix = ColorMatrix::BT601;
    ColorTransfer transfer = ColorTransfer::LinearPq;
    float peakNits = -1.0f;
    double captureSec = -1.0;
    char timecode[16] = {0};
    int eyeOffsetFrames = -2, statW = 0, statH = 0;
    uint64_t published = 0;
};

Shot Take(TestSource& src) {
    Shot s;
    const FrameRing::Frame* f = src.Ring().AcquireLatest();
    if (!f) return s;
    s.got = true;
    s.y = f->plane[0];
    s.uv = f->plane[1];
    s.width = f->width;
    s.height = f->height;
    s.format = f->format;
    s.fullRange = f->fullRange;
    s.gpu = f->gpu || f->gpuSharedTexture || f->gpuSharedHandle;
    s.primaries2020 = f->primaries2020;
    s.matrix = f->matrix;
    s.transfer = f->transfer;
    s.peakNits = f->peakNits;
    s.captureSec = f->captureSec;
    std::memcpy(s.timecode, f->timecode, sizeof(s.timecode));
    const mp::LiveStats st = src.GetStats();
    s.eyeOffsetFrames = st.eyeOffsetFrames;
    s.statW = st.w;
    s.statH = st.h;
    s.published = st.published;
    return s;
}

bool Same(const Shot& a, const Shot& b) {
    return a.got && b.got && a.y == b.y && a.uv == b.uv && a.width == b.width && a.height == b.height &&
           a.format == b.format && a.fullRange == b.fullRange && a.gpu == b.gpu && a.primaries2020 == b.primaries2020 &&
           a.matrix == b.matrix && a.transfer == b.transfer && a.peakNits == b.peakNits &&
           a.captureSec == b.captureSec && std::memcmp(a.timecode, b.timecode, sizeof(a.timecode)) == 0 &&
           a.eyeOffsetFrames == b.eyeOffsetFrames && a.statW == b.statW && a.statH == b.statH &&
           a.published == b.published;
}

// Today: each eye into its own P016 (the lane's history slot), tags as the lane sets them,
// then PublishPair composes the two into the ring.
Shot OldWay(const Eye (&eye)[2], ConvertPool* pool, int offsetFrames) {
    TestSource src;
    P016 px[2];
    for (int e = 0; e < 2; ++e) {
        px[e].Resize(eye[e].w & ~1, eye[e].h & ~1);
        EyeJob j = JobFor(eye[e], 0);
        j.dst = &px[e];
        mp::livepix::ToP016Banded(pool, &j, 1);
        if (eye[e].hasMatrix) px[e].matrix = eye[e].matrix;
        px[e].transfer = eye[e].transfer;
    }
    const double captureSec = std::min(eye[0].arrived, eye[1].arrived);
    src.PublishPair(px[0], px[1], /*fullRange=*/false, captureSec, eye[0].tc[0] ? eye[0].tc : nullptr, offsetFrames,
                    pool);
    return Take(src);
}

// New: a reserved ring frame (first filled with garbage, as a reused buffer is), each eye
// converted into its half by its own lane thread at the same time, tags as the lane sets them,
// then PublishPairFrame.
Shot NewWay(const Eye (&eye)[2], ConvertPool* pools[2], int offsetFrames) {
    TestSource src;
    const int w = eye[0].w & ~1, h = eye[0].h & ~1;
    FrameRing::Frame* f = src.ReservePairFrame(w, h);
    CHECK(f != nullptr, "a pair frame is reserved");
    if (!f) return {};
    uint32_t g = 0xC0FFEEu;
    for (std::vector<uint8_t>* p : {&f->plane[0], &f->plane[1]})
        for (uint8_t& b : *p) b = (uint8_t)((g = g * 1103515245u + 12345u) >> 24);
    f->matrix = ColorMatrix::BT601;  // stale tags too
    f->transfer = ColorTransfer::HLG;
    std::memcpy(f->timecode, "99:99:99:99", 12);
    ColorMatrix m[2] = {ColorMatrix::BT601, ColorMatrix::BT601};
    ColorTransfer t[2] = {ColorTransfer::HLG, ColorTransfer::HLG};
    auto lane = [&](int e) {
        EyeJob j = JobFor(eye[e], e * w);
        j.planes = mp::livepix::PairPlanes(*f, w, h);
        j.matrixOut = &m[e];
        mp::livepix::ToP016Banded(pools[e], &j, 1);
        if (eye[e].hasMatrix) m[e] = eye[e].matrix;
        t[e] = eye[e].transfer;
    };
    std::thread right(lane, 1);
    lane(0);
    right.join();
    const double captureSec = std::min(eye[0].arrived, eye[1].arrived);
    src.PublishPairFrame(f, w, h, m[0], t[0], /*fullRange=*/false, captureSec, eye[0].tc[0] ? eye[0].tc : nullptr,
                         offsetFrames);
    return Take(src);
}

// The compose fallback when one eye is in a half of a reserved frame and the other in its own
// P016 (a decision that pairs two different places): PublishPair over the views.
Shot MixedWay(const Eye (&eye)[2], ConvertPool* pool, int offsetFrames) {
    TestSource src;
    const int w = eye[0].w & ~1, h = eye[0].h & ~1;
    FrameRing::Frame* half = src.ReservePairFrame(w, h);  // the left eye is in this one's left half
    if (!half) return {};
    std::fill(half->plane[0].begin(), half->plane[0].end(), (uint8_t)0x5A);
    std::fill(half->plane[1].begin(), half->plane[1].end(), (uint8_t)0xA5);
    ColorMatrix lm = ColorMatrix::BT601;
    EyeJob jl = JobFor(eye[0], 0);
    jl.planes = mp::livepix::PairPlanes(*half, w, h);
    jl.matrixOut = &lm;
    mp::livepix::ToP016Banded(pool, &jl, 1);
    if (eye[0].hasMatrix) lm = eye[0].matrix;
    P016 r;
    r.Resize(eye[1].w & ~1, eye[1].h & ~1);
    EyeJob jr = JobFor(eye[1], 0);
    jr.dst = &r;
    mp::livepix::ToP016Banded(pool, &jr, 1);
    const double captureSec = std::min(eye[0].arrived, eye[1].arrived);
    src.PublishPair(mp::livepix::PairHalf(*half, 0, w, h), mp::livepix::EyeOf(r), lm, eye[0].transfer,
                    /*fullRange=*/false, captureSec, eye[0].tc[0] ? eye[0].tc : nullptr, offsetFrames, pool);
    src.CancelPairFrame(half);
    return Take(src);
}

void BitIdentical() {
    struct Size {
        int w, h;
    };
    const Size sizes[] = {{1920, 1080}, {1283, 721}, {98, 66}, {6, 2}, {50, 35}, {722, 3}};
    const int threads[] = {1, 3, 7};
    std::vector<std::unique_ptr<ConvertPool>> pools;
    for (int t : threads) pools.push_back(std::make_unique<ConvertPool>(t));
    std::vector<std::unique_ptr<ConvertPool>> pools2;
    for (int t : threads) pools2.push_back(std::make_unique<ConvertPool>(t));
    const RawFormat formats[] = {RawFormat::Uyvy, RawFormat::V210, RawFormat::Bgra, RawFormat::R210};
    int cases = 0, same = 0, mixedSame = 0;
    uint32_t seed = 1;
    for (int fi = 0; fi < 5; ++fi) {  // the four formats on both eyes, then v210 left + r210 right
        const RawFormat lf = fi < 4 ? formats[fi] : RawFormat::V210;
        const RawFormat rf = fi < 4 ? formats[fi] : RawFormat::R210;
        for (const Size& s : sizes) {
            for (size_t ti = 0; ti < pools.size(); ++ti) {
                for (int tags = 0; tags < 3; ++tags) {
                    Eye eye[2] = {RandomEye(lf, s.w, s.h, ++seed), RandomEye(rf, s.w, s.h, ++seed)};
                    for (int e = 0; e < 2; ++e) {
                        // 0: no colorimetry from the card; 1: BT.2020 PQ; 2: BT.601 HLG, the right eye BT.709
                        eye[e].hasMatrix = tags != 0;
                        eye[e].matrix = tags == 1 ? ColorMatrix::BT2020 : e ? ColorMatrix::BT709 : ColorMatrix::BT601;
                        eye[e].transfer = tags == 1 ? ColorTransfer::PQ : tags == 2 ? ColorTransfer::HLG
                                                                                    : ColorTransfer::SDR;
                        eye[e].arrived = 1000.0 + 0.001 * (double)(seed % 17) + (e ? 0.0004 : 0.0);
                    }
                    if (tags != 0) std::snprintf(eye[0].tc, sizeof(eye[0].tc), "01:53:47:%02d", (int)(seed % 60));
                    std::snprintf(eye[1].tc, sizeof(eye[1].tc), "00:00:00:00");  // never the pair's
                    const int offset = (int)(seed % 3) - 1;
                    ConvertPool* lanePools[2] = {pools[ti].get(), pools2[ti].get()};
                    const Shot a = OldWay(eye, pools[ti].get(), offset);
                    const Shot b = NewWay(eye, lanePools, offset);
                    const Shot c = MixedWay(eye, pools[ti].get(), offset);
                    ++cases;
                    const bool ok = Same(a, b);
                    if (ok) ++same;
                    if (Same(a, c)) ++mixedSame;
                    if (!ok && g_failures < 20)
                        std::fprintf(stderr, "  differs: %s+%s %dx%d, %d threads, tags %d\n", Name(lf), Name(rf), s.w,
                                     s.h, threads[ti], tags);
                    CHECK(ok, "direct: the ring frame is today's, byte for byte (pixels, size, tags, timecode, offset)");
                    CHECK(Same(a, c), "compose from a half: the same as from the eye's own frame");
                }
            }
        }
    }
    std::printf("  bit-identical: %d / %d pairs direct == composed, %d / %d with one eye read from a half\n", same,
                cases, mixedSame, cases);

    // 2160p v210, both eyes, 8 threads per lane: the station's shape (one case, as above).
    ConvertPool p8a(8), p8b(8);
    Eye eye[2] = {RandomEye(RawFormat::V210, 3840, 2160, 77), RandomEye(RawFormat::V210, 3840, 2160, 78)};
    eye[0].hasMatrix = eye[1].hasMatrix = true;
    std::snprintf(eye[0].tc, sizeof(eye[0].tc), "01:53:47:52");
    ConvertPool* lanePools[2] = {&p8a, &p8b};
    const Shot a = OldWay(eye, &p8a, 0);
    const Shot b = NewWay(eye, lanePools, 0);
    CHECK(Same(a, b), "2160p v210 pair: direct == composed, byte for byte");
    CHECK(a.width == 7680 && a.height == 2160 && a.y.size() == (size_t)7680 * 2160 * 2, "7680x2160 P016");
    std::printf("  2160p v210 pair (%.1f MB): %s\n", (double)(a.y.size() + a.uv.size()) / (1 << 20),
                Same(a, b) ? "identical" : "DIFFERS");
}

// ---------------------------------------------------------------------------
// 2. The entry table
// ---------------------------------------------------------------------------

// Plain frames standing in for the ring: counts reserves and cancels, and refuses a cancel
// of a frame it did not hand out.
struct FakeFrames final : DirectPairTable::Frames {
    int cap = DirectPairTable::kEntries;
    int out = 0, reserves = 0, cancels = 0, badCancels = 0;
    int storage[16] = {};
    bool reserved[16] = {};
    void* Reserve(int, int) override {
        if (out >= cap) return nullptr;
        for (int i = 0; i < 16; ++i)
            if (!reserved[i]) {
                reserved[i] = true;
                ++out;
                ++reserves;
                return &storage[i];
            }
        return nullptr;
    }
    int IndexOf(void* f) const {
        for (int i = 0; i < 16; ++i)
            if (f == &storage[i]) return reserved[i] ? i : -1;
        return -1;
    }
    void Cancel(void* f) override {
        const int i = IndexOf(f);
        if (i < 0) {
            ++badCancels;
            return;
        }
        reserved[i] = false;
        --out;
        ++cancels;
    }
    // A frame the table handed over for publishing: the ring's now (back here, not cancelled).
    void Published(void* f) {
        const int i = IndexOf(f);
        if (i >= 0) {
            reserved[i] = false;
            --out;
        }
    }
};

void TableRules() {
    using W = DirectPairTable::Where;
    using R = DirectPairTable::Refused;
    using H = DirectPairTable::Half;
    {  // join, done, whole, publish
        FakeFrames ff;
        DirectPairTable t(&ff);
        R why;
        t.SlotReused(0, 0);
        const int e = t.Claim(0, 0, 10, 1, 1, 64, 32, &why);
        CHECK(e >= 0 && ff.out == 1 && t.HalfState(e, 0) == H::Writing, "a new key: an entry with a reserved frame");
        CHECK(t.Locate(0, 0) == W::Half, "the slot names the half");
        CHECK(t.Whole(0, 3) == -1, "not whole: the right half is missing");
        t.SlotReused(1, 3);
        CHECK(t.Claim(1, 3, 10, 1, 1, 64, 32, &why) == e, "the other eye of the same key joins it");
        CHECK(t.Claim(1, 4, 10, 1, 1, 64, 32, &why) == -1 && why == R::Busy, "a second right frame of the key: old way");
        t.Done(e, 0);
        CHECK(t.Whole(0, 3) == -1, "not whole while the right half is being written");
        t.Done(e, 1);
        CHECK(t.Whole(0, 3) == e, "both halves done: whole");
        CHECK(t.Whole(1, 3) == -1, "another left slot: not this entry");
        t.Pin(e);
        CHECK(t.Whole(0, 3) == -1, "a compose reads it: never published meanwhile");
        t.Unpin(e);
        void* f = t.TakeForPublish(e);
        CHECK(f != nullptr && t.Held() == 0, "published: the entry is gone");
        CHECK(t.Locate(0, 0) == W::Gone && t.Locate(1, 3) == W::Gone, "...and its slots' pixels with it");
        ff.Published(f);
        t.SlotReused(0, 0);
        CHECK(t.Locate(0, 0) == W::Own, "a reused slot is its own again");
        CHECK(ff.badCancels == 0 && ff.cancels == 0, "a published frame is never cancelled");
    }
    {  // epoch, generation and size keep entries apart
        FakeFrames ff;
        DirectPairTable t(&ff);
        R why;
        const int a = t.Claim(0, 0, 10, 1, 1, 64, 32, &why);
        const int b = t.Claim(1, 0, 10, 2, 1, 64, 32, &why);
        CHECK(b >= 0 && b != a, "another epoch: another entry");
        const int c = t.Claim(1, 1, 10, 1, 2, 64, 32, &why);
        CHECK(c >= 0 && c != a && c != b, "another generation: another entry");
        CHECK(t.Claim(0, 2, 12, 1, 1, 64, 32, &why) == -1 && why == R::NoFrame && ff.out == 3,
              "a fourth key with three frames out and none retired: the old way");
        t.NoteSize(0, 64, 32);
        t.NoteSize(1, 48, 32);
        CHECK(t.Claim(0, 1, 11, 1, 1, 64, 32, &why) == -1 && why == R::Size, "the eyes differ in size: no new entry");
        CHECK(t.Claim(1, 2, 10, 1, 1, 48, 32, &why) == -1 && why == R::Size, "...nor a join of a different size");
    }
    {  // retire and release: never while written or read, never while a slot names it
        FakeFrames ff;
        DirectPairTable t(&ff);
        R why;
        const int e = t.Claim(0, 2, 20, 1, 1, 64, 32, &why);
        t.Sweep(-1, /*epoch=*/2, 1);  // the pairer changed domain
        CHECK(t.Retired(e) && ff.out == 1, "retired while the lane writes: kept");
        CHECK(t.Claim(1, 2, 20, 1, 1, 64, 32, &why) != e, "a retired entry takes no new half");
        t.Done(e, 0);
        t.Sweep(-1, 2, 1);
        CHECK(ff.out == 2 && t.Locate(0, 2) == W::Half, "done but still named by its slot: readable, kept");
        t.Pin(e);
        t.SlotReused(0, 2);
        CHECK(ff.cancels == 0, "a pinned entry is not released");
        t.Unpin(e);
        CHECK(ff.cancels == 1 && ff.badCancels == 0, "unpinned, retired, unnamed: back to the ring");
    }
    {  // a newer key shown retires older ones; recycling when the ring has nothing to spare
        FakeFrames ff;
        DirectPairTable t(&ff);
        R why;
        int e[3];
        for (int k = 0; k < 3; ++k) {
            e[k] = t.Claim(0, k, 100 + k, 1, 1, 64, 32, &why);
            t.Done(e[k], 0);
        }
        CHECK(t.Held() == 3 && ff.out == 3, "three keys in flight: three frames");
        CHECK(t.Claim(0, 3, 103, 1, 1, 64, 32, &why) == -1 && why == R::NoFrame, "a fourth: none to spare");
        t.Sweep(/*lastKey=*/101, 1, 1);  // key 101 was shown (by another path)
        CHECK(t.Retired(e[0]) && t.Retired(e[1]) && !t.Retired(e[2]), "keys up to the last shown retire");
        CHECK(ff.out == 3, "...but stay readable while their slots name them");
        t.Pin(e[0]);
        const int n = t.Claim(0, 3, 103, 1, 1, 64, 32, &why);
        CHECK(n >= 0 && t.Locate(0, 1) == W::Gone && t.Locate(0, 0) == W::Half,
              "a new key recycles the oldest retired entry nothing reads (not the pinned one)");
        CHECK(ff.out == 3 && ff.badCancels == 0, "never more than kEntries frames out");
        t.Unpin(e[0]);
        t.ReleaseAll();
        CHECK(ff.out == 0 && t.Held() == 0, "ReleaseAll: every frame back");
    }
}

// ---------------------------------------------------------------------------
// 3 + 4. The lane protocol (DeckLinkCapture::ProcessPairDirect / PublishDirect, mirrored)
// ---------------------------------------------------------------------------

constexpr int kEyeW = 96, kEyeH = 8;  // small: the rig runs thousands of frames

// A UYVY eye whose every pixel says which frame it is: luma = id's low byte, Cb = its high
// byte (so a half that is half one frame and half another shows at once).
Eye IdEye(int id, int w = kEyeW, int h = kEyeH) {
    Eye e;
    e.fmt = RawFormat::Uyvy;
    e.w = w;
    e.h = h;
    e.rowBytes = RowBytes(e.fmt, w);
    e.bytes.assign((size_t)e.rowBytes * h, 0);
    for (int r = 0; r < h; ++r)
        for (int x = 0; x < w / 2; ++x) {
            uint8_t* p = &e.bytes[(size_t)r * e.rowBytes + 4 * x];
            p[0] = (uint8_t)((id >> 8) & 0xff);  // Cb
            p[1] = (uint8_t)(id & 0xff);         // Y
            p[2] = 128;                          // Cr
            p[3] = (uint8_t)(id & 0xff);         // Y
        }
    return e;
}

// The frame id in each half of a published side-by-side frame; -1 when the half is torn.
// (fw x h: the frame's size; a reserved frame is only tagged with it when it is published)
void ReadIds(const FrameRing::Frame& f, int fw, int h, int ids[2]) {
    const int w = fw / 2;
    const uint16_t* y = reinterpret_cast<const uint16_t*>(f.plane[0].data());
    const uint16_t* uv = reinterpret_cast<const uint16_t*>(f.plane[1].data());
    for (int e = 0; e < 2; ++e) {
        bool ok = w > 0 && (size_t)fw * h * 2 == f.plane[0].size() && (size_t)fw * (h / 2) * 2 == f.plane[1].size();
        const int lo = ok ? y[(size_t)e * w] >> 8 : 0, hi = ok ? uv[(size_t)e * w] >> 8 : 0;
        for (int r = 0; r < h && ok; ++r)
            for (int x = 0; x < w; ++x)
                if ((y[(size_t)r * fw + e * w + x] >> 8) != lo) ok = false;
        for (int r = 0; r < h / 2 && ok; ++r)
            for (int x = 0; x < w; x += 2)
                if ((uv[(size_t)r * fw + e * w + x] >> 8) != hi) ok = false;
        ids[e] = ok ? lo | (hi << 8) : -1;
    }
}
void ReadIds(const FrameRing::Frame& f, int ids[2]) { ReadIds(f, f.width, f.height, ids); }

// DeckLinkCapture's pairing state and its two lanes' critical sections, around the real
// pairer, table, converters and LiveSourceBase. Start = the claim (m held), Convert = the
// convert (no lock), Finish = done + decide + publish + sweep (m held, dropped for a compose).
struct Rig {
    explicit Rig(bool direct, bool strict, int queueDepth = 3) : src(queueDepth), frames(this), table(&frames),
                                                                 directPair(direct) {
        pairer.SetStrict(strict);
    }
    TestSource src;
    struct RingFrames final : DirectPairTable::Frames {
        explicit RingFrames(Rig* r) : rig(r) {}
        Rig* rig;
        void* Reserve(int w, int h) override {
            FrameRing::Frame* f = rig->src.ReservePairFrame(w, h);
            if (f) {
                ++rig->reserves;
                std::fill(f->plane[0].begin(), f->plane[0].end(), (uint8_t)0xEE);  // a reused buffer's garbage
            }
            return f;
        }
        void Cancel(void* f) override { rig->src.CancelPairFrame(static_cast<FrameRing::Frame*>(f)); }
    } frames;
    DirectPairTable table;
    TimecodePairer pairer;
    bool directPair;
    std::mutex m, publishM;
    std::condition_variable pinCv;
    struct EyeFrame {
        P016 px;
        int id = -1;  // the frame really in this slot (the test's truth)
        ColorMatrix matrix = ColorMatrix::BT709;
        ColorTransfer transfer = ColorTransfer::SDR;
        int w = 0, h = 0;
        double arrived = 0.0;
        char tc[16] = {0};
    };
    EyeFrame hist[2][TimecodePairer::kHistory];
    int pins[2][TimecodePairer::kHistory] = {};
    uint64_t framesIn[2] = {0, 0};
    uint32_t generation = 1;
    // counts (m held)
    int whole = 0, composed = 0, skipped = 0, noFrame = 0, refused = 0, mismatched = 0, wrongPixels = 0;
    int reserves = 0, maxHeld = 0;
    std::vector<int> shownIds;  // the left id of every published pair, in publish order
    std::vector<std::pair<int, int>> shownPairs;  // (left id, right id) of every published pair

    struct Ticket {
        int eye = 0, id = 0, tc = -1, clock = -1, slot = -1, entry = -1;
        FrameRing::Frame* frame = nullptr;
        Eye raw;
    };

    void Start(Ticket& t) {
        std::unique_lock<std::mutex> lk(m);
        const int eye = t.eye;
        pinCv.wait(lk, [&] { return pins[eye][pairer.NextSlot(eye)] == 0; });
        t.slot = pairer.NextSlot(eye);
        pairer.MarkBusy(eye, t.slot);
        if (!directPair) return;
        table.SlotReused(eye, t.slot);
        const int w = t.raw.w & ~1, h = t.raw.h & ~1;
        table.NoteSize(eye, w, h);
        const int key = pairer.DirectKey(t.tc, t.clock);
        if (key >= 0) {
            DirectPairTable::Refused why = DirectPairTable::Refused::None;
            t.entry = table.Claim(eye, t.slot, key, pairer.Epoch(), generation, w, h, &why);
            if (t.entry >= 0) t.frame = static_cast<FrameRing::Frame*>(table.Frame(t.entry));
            else if (why == DirectPairTable::Refused::NoFrame) ++noFrame;
        }
        maxHeld = std::max(maxHeld, table.Held());
    }

    void Convert(Ticket& t, ConvertPool* pool) {
        EyeFrame& ef = hist[t.eye][t.slot];
        const int w = t.raw.w & ~1, h = t.raw.h & ~1;
        if (t.frame) {
            EyeJob j = JobFor(t.raw, t.eye * w);
            j.planes = mp::livepix::PairPlanes(*t.frame, w, h);
            j.matrixOut = &ef.matrix;
            mp::livepix::ToP016Banded(pool, &j, 1);
            ef.transfer = t.raw.transfer;
        } else {
            ef.px.Resize(w, h);
            EyeJob j = JobFor(t.raw, 0);
            j.dst = &ef.px;
            mp::livepix::ToP016Banded(pool, &j, 1);
            ef.px.transfer = t.raw.transfer;
            ef.matrix = ef.px.matrix;
            ef.transfer = ef.px.transfer;
        }
        ef.w = w;
        ef.h = h;
        ef.id = t.id;
        ef.arrived = t.id / 59.94;
    }

    void Finish(Ticket& t, ConvertPool* pool) {
        std::unique_lock<std::mutex> lk(m);
        if (t.entry >= 0) table.Done(t.entry, t.eye);
        ++framesIn[t.eye];
        const TimecodePairer::Decision d = pairer.Add(t.eye, t.slot, t.tc, t.id / 59.94, 1.0 / 59.94, t.clock);
        if (d.refused) ++refused;
        if (d.publish) Publish(d, pool, lk);
        if (directPair) table.Sweep(pairer.LastKey(), pairer.Epoch(), generation);
        lk.unlock();
        pinCv.notify_all();
    }

    // PublishDirect (and, with directPair off, today's compose).
    void Publish(const TimecodePairer::Decision& d, ConvertPool* pool, std::unique_lock<std::mutex>& lk) {
        const int s[2] = {d.slot[0], d.slot[1]};
        const int truth[2] = {hist[0][s[0]].id, hist[1][s[1]].id};
        int ent[2] = {-1, -1};
        DirectPairTable::Where where[2] = {DirectPairTable::Where::Own, DirectPairTable::Where::Own};
        if (directPair) {
            where[0] = table.Locate(0, s[0], &ent[0]);
            where[1] = table.Locate(1, s[1], &ent[1]);
            if (where[0] == DirectPairTable::Where::Gone || where[1] == DirectPairTable::Where::Gone) {
                ++skipped;
                return;
            }
        }
        if (truth[0] != truth[1]) ++mismatched;
        shownIds.push_back(truth[0]);
        shownPairs.emplace_back(truth[0], truth[1]);  // in publish order: publishM is taken before m drops
        const EyeFrame& l = hist[0][s[0]];
        const EyeFrame& r = hist[1][s[1]];
        const int wholeEntry = directPair ? table.Whole(s[0], s[1]) : -1;
        if (wholeEntry >= 0) {
            ++whole;
            const int ew = table.EyeW(wholeEntry), eh = table.EyeH(wholeEntry);
            FrameRing::Frame* f = static_cast<FrameRing::Frame*>(table.TakeForPublish(wholeEntry));
            std::lock_guard<std::mutex> pub(publishM);
            int ids[2];
            ReadIds(*f, 2 * ew, eh, ids);  // before it is queued: still ours
            if (ids[0] != truth[0] || ids[1] != truth[1]) ++wrongPixels;
            src.PublishPairFrame(f, ew, eh, l.matrix, l.transfer, false, std::min(l.arrived, r.arrived), nullptr,
                                 d.offsetFrames);
            return;
        }
        ++composed;
        mp::livepix::EyeView view[2];
        for (int e = 0; e < 2; ++e) {
            if (where[e] == DirectPairTable::Where::Half) {
                const FrameRing::Frame* f = static_cast<const FrameRing::Frame*>(table.Frame(ent[e]));
                view[e] = mp::livepix::PairHalf(*f, e, table.EyeW(ent[e]), table.EyeH(ent[e]));
                table.Pin(ent[e]);
            } else {
                view[e] = mp::livepix::EyeOf(hist[e][s[e]].px);
                ++pins[e][s[e]];
            }
        }
        const ColorMatrix matrix = l.matrix;
        const ColorTransfer transfer = l.transfer;
        const double captureSec = std::min(l.arrived, r.arrived);
        std::unique_lock<std::mutex> pub(publishM);
        lk.unlock();
        src.PublishPair(view[0], view[1], matrix, transfer, false, captureSec, nullptr, d.offsetFrames, pool);
        pub.unlock();
        lk.lock();
        for (int e = 0; e < 2; ++e) {
            if (where[e] == DirectPairTable::Where::Half) table.Unpin(ent[e]);
            else --pins[e][s[e]];
        }
    }

    // DeckLinkCapture::ResetPairing: a format change.
    void Reset() {
        std::lock_guard<std::mutex> lk(m);
        ++generation;
        pairer.Reset();
        framesIn[0] = framesIn[1] = 0;
        if (directPair) table.Sweep(pairer.LastKey(), pairer.Epoch(), generation);
    }

    void End() {  // the Worker's last step
        std::lock_guard<std::mutex> lk(m);
        table.ReleaseAll();
    }
};

// One scenario on one thread: the two lanes' Start / Convert+Finish interleaved at random
// (a lane holds at most one frame), as the two lanes' threads interleave.
struct Scenario {
    const char* name = "";
    int frames = 600;
    bool clock = true;               // one card
    bool strict = true;
    int maxLead = 2;                 // how far one lane may run ahead
    int (*tc)(int eye, int i) = nullptr;
    bool (*dropped)(int eye, int i) = nullptr;  // a frame the lane never got (late)
    int rightW = kEyeW;              // the right input's width (differs: never direct)
    int resetAt = -1;                // a format change when the left lane is at this frame
    int minWholePct = 90;            // pairs published whole at least (0: not checked)
};

struct Result {
    int whole = 0, composed = 0, skipped = 0, noFrame = 0, refused = 0, mismatched = 0, wrongPixels = 0;
    int consumedTorn = 0, consumedMismatch = 0, consumedWrong = 0, consumed = 0, maxHeld = 0, reservedAtEnd = -1;
    int reserves = 0;
    int shown = 0;
};

Result RunScenario(const Scenario& sc, bool direct, uint32_t seed) {
    Rig rig(direct, sc.strict);
    Result res;
    std::mt19937 rng(seed);
    int next[2] = {0, 0};
    Rig::Ticket held[2];
    bool busy[2] = {false, false};
    size_t seenUpTo = 0;  // the renderer's frames are the decided pairs, in order (drops allowed)
    auto consume = [&] {
        while (const FrameRing::Frame* f = rig.src.Ring().AcquireLatest()) {
            int ids[2];
            ReadIds(*f, ids);
            ++res.consumed;
            if (ids[0] < 0 || ids[1] < 0) {
                ++res.consumedTorn;
                continue;
            }
            if (sc.rightW == kEyeW && ids[0] != ids[1] && sc.strict) ++res.consumedMismatch;
            size_t k = seenUpTo;
            while (k < rig.shownPairs.size() && rig.shownPairs[k] != std::make_pair(ids[0], ids[1])) ++k;
            if (k == rig.shownPairs.size()) ++res.consumedWrong;
            else seenUpTo = k + 1;
        }
    };
    bool resetDone = false;
    for (int guard = 0; guard < 100000 && (next[0] < sc.frames || next[1] < sc.frames || busy[0] || busy[1]);
         ++guard) {
        const int eye = (int)(rng() % 2);
        if (!busy[eye]) {
            if (next[eye] >= sc.frames || next[eye] - next[1 - eye] >= sc.maxLead) continue;
            int i = next[eye]++;
            while (sc.dropped && sc.dropped(eye, i) && next[eye] < sc.frames) i = next[eye]++;
            Rig::Ticket& t = held[eye];
            t = Rig::Ticket{};
            t.eye = eye;
            t.id = i;
            t.tc = sc.tc ? sc.tc(eye, i) : -1;
            t.clock = sc.clock ? i : -1;
            t.raw = IdEye(i, eye ? sc.rightW : kEyeW);
            rig.Start(t);
            busy[eye] = true;
        } else {
            rig.Convert(held[eye], nullptr);
            rig.Finish(held[eye], nullptr);
            busy[eye] = false;
            if (sc.resetAt >= 0 && !resetDone && next[0] >= sc.resetAt && busy[1 - eye]) {
                rig.Reset();  // the other lane is mid-frame (its claim is from the old signal)
                resetDone = true;
            }
        }
        if (rng() % 3 == 0) consume();
    }
    consume();
    rig.End();
    res.whole = rig.whole;
    res.composed = rig.composed;
    res.skipped = rig.skipped;
    res.noFrame = rig.noFrame;
    res.refused = rig.refused;
    res.mismatched = rig.mismatched;
    res.wrongPixels = rig.wrongPixels;
    res.maxHeld = rig.maxHeld;
    res.reserves = rig.reserves;
    res.reservedAtEnd = rig.src.Ring().ReservedCount();
    res.shown = (int)rig.shownIds.size();
    return res;
}

void Report(const char* name, bool direct, const Result& r) {
    std::printf("  %-34s %-7s shown %4d: whole %4d composed %4d skipped %d no-frame %d refused %d mismatched %d | "
                "consumed %d torn %d, max frames held %d\n",
                name, direct ? "direct" : "old", r.shown, r.whole, r.composed, r.skipped, r.noFrame, r.refused,
                r.mismatched, r.consumed, r.consumedTorn, r.maxHeld);
}

void LaneProtocol() {
    std::vector<Scenario> all;
    {
        Scenario s;
        s.name = "Resolve dual output (clock)";
        s.tc = [](int eye, int i) { return eye == 0 ? 409672 + i : 0; };
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "jam-synced timecode, one card";
        s.tc = [](int, int i) { return 5000 + i; };
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "Resolve + late frames on both";
        s.tc = [](int eye, int i) { return eye == 0 ? 409672 + i : 0; };
        s.dropped = [](int eye, int i) { return (i * 7 + eye * 3) % 23 == 0; };
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "right eye 12 behind (strict)";
        s.tc = [](int eye, int i) { return eye == 0 ? 409672 + i : 0; };
        s.dropped = [](int eye, int i) { return eye == 1 && i >= 100 && i < 112; };
        s.maxLead = 14;
        s.minWholePct = 0;  // the leading eye runs out of spare frames: composed meanwhile
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "left lane 4 ahead (no frame spare)";
        s.tc = [](int, int i) { return 5000 + i; };
        s.maxLead = 5;
        s.minWholePct = 0;
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "rewind + held timecode";
        s.tc = [](int, int i) { return i < 200 ? 100 + i : i < 260 ? 20 + i : i < 300 ? 279 : 100 + i; };
        s.clock = false;
        s.strict = false;
        s.minWholePct = 75;  // 40 frames of held timecode are newest-of-each: composed, as before
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "inputs of different sizes";
        s.tc = [](int, int i) { return 5000 + i; };
        s.rightW = kEyeW - 16;
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "format change mid-frame";
        s.tc = [](int eye, int i) { return eye == 0 ? 409672 + i : 0; };
        s.resetAt = 300;
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "two cards, unsynced timecode";
        s.tc = [](int eye, int i) { return eye == 0 ? 1000 + i : 70000 + i; };
        s.clock = false;
        s.strict = false;
        all.push_back(s);
    }
    {
        Scenario s;
        s.name = "two cards, no timecode";
        s.clock = false;
        s.strict = false;
        all.push_back(s);
    }
    for (const Scenario& sc : all) {
        for (uint32_t seed = 1; seed <= 3; ++seed) {
            const Result o = RunScenario(sc, false, seed);
            const Result n = RunScenario(sc, true, seed);
            const int failuresBefore = g_failures;
            CHECK(n.wrongPixels == 0, "a whole frame carries exactly the two frames the pairer decided");
            CHECK(n.consumedTorn == 0 && o.consumedTorn == 0, "the renderer never sees a half-written frame");
            CHECK(n.consumedWrong == 0 && o.consumedWrong == 0,
                  "every frame the renderer takes is a decided pair's pixels, in decision order");
            CHECK(n.reservedAtEnd == 0, "every reserved frame comes back");
            CHECK(n.maxHeld <= FrameRing::kMaxReserved, "never more than kMaxReserved frames held");
            CHECK(n.shown + n.skipped == o.shown, "the same decisions: shown or (rarely) skipped");
            CHECK(n.skipped <= 4, "skips only at a transition");
            CHECK(n.mismatched <= o.mismatched, "never a mismatched pair the old path did not show");
            if (sc.strict) CHECK(n.mismatched == 0 && n.consumedMismatch == 0, "strict: matched pairs only, direct too");
            const bool keyedCase = sc.clock || (sc.tc && sc.tc(0, 5) == sc.tc(1, 5));
            if (keyedCase && sc.rightW == kEyeW && sc.minWholePct > 0)
                CHECK(n.whole * 100 >= n.shown * sc.minWholePct, "matching by key: (nearly) every pair published whole");
            if (!keyedCase) CHECK(n.whole == 0 && n.reserves == 0, "nothing to match on: today's path, no frame reserved");
            if (sc.rightW != kEyeW) CHECK(n.whole == 0, "inputs of different sizes: composed, as before");
            if (sc.maxLead > 4 && !sc.dropped) CHECK(n.noFrame > 0 && n.whole > 0, "keys beyond the spare frames: old way, counted");
            if (seed == 1 || g_failures != failuresBefore) {
                if (seed != 1) std::printf("  (seed %u)\n", seed);
                Report(sc.name, false, o);
                Report(sc.name, true, n);
            }
        }
    }
}

// Busy-waits `us` microseconds (a Windows sleep is a millisecond or more).
void SpinFor(int us) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
}

// The same protocol on real threads: a card clock delivering frame i to both inputs at once,
// two lanes (their own convert pools) that each take the newest frame that has arrived (a lane
// that falls behind skips, as DeckLinkCapture::Enqueue's newest-wins), the render thread taking
// a frame per "refresh", and the upload thread pinning the next waiting frame and checking it
// does not change until it unpins. Resolve's dual output, strict, frames dropped now and then.
void ThreadedSoak(bool direct) {
    Rig rig(direct, /*strict=*/true);
    std::atomic<bool> done{false};
    std::atomic<int> card{0};  // frames 0..card-1 have arrived on both inputs
    std::atomic<int> torn{0}, mismatched{0}, consumed{0}, pinChanged{0}, pinned{0}, late{0}, backwards{0};
    const int n = 4000;
    auto lane = [&](int eye) {
        ConvertPool pool(2);
        std::mt19937 rng(eye + 11u);
        int i = 0;
        while (i < n) {
            const int arrived = card.load();
            if (arrived <= i) {
                std::this_thread::yield();
                continue;
            }
            if (arrived - i > 1) {  // fallen behind: the newest arrived frame wins
                late += arrived - 1 - i;
                i = arrived - 1;
            }
            if (rng() % 29 == 0) {  // a frame this input never delivered
                ++i;
                continue;
            }
            Rig::Ticket t;
            t.eye = eye;
            t.id = i;
            t.tc = eye == 0 ? 409672 + i : 0;
            t.clock = i;
            t.raw = IdEye(i);
            rig.Start(t);
            if (rng() % 4 == 0) std::this_thread::yield();
            rig.Convert(t, &pool);
            rig.Finish(t, &pool);
            if (rng() % 16 == 0) SpinFor((int)(rng() % 300));
            ++i;
        }
    };
    std::thread clockThread([&] {
        for (int i = 1; i <= n; ++i) {
            SpinFor(150);
            card = i;
        }
    });
    std::thread consumer([&] {
        int last = -1;
        while (!done) {
            if (const FrameRing::Frame* f = rig.src.Ring().AcquireLatest()) {
                int ids[2];
                ReadIds(*f, ids);
                ++consumed;
                if (ids[0] < 0 || ids[1] < 0) ++torn;
                else if (ids[0] != ids[1]) ++mismatched;
                if (ids[0] >= 0 && ids[0] <= last) ++backwards;
                if (ids[0] >= 0) last = ids[0];
            }
            SpinFor(150);
        }
    });
    std::thread uploader([&] {
        int64_t last = 0;
        while (!done) {
            const FrameRing::Frame* p = rig.src.Ring().PinNextQueued(last);
            if (!p) {
                std::this_thread::yield();
                continue;
            }
            last = p->serial;
            ++pinned;
            int a[2], b[2];
            ReadIds(*p, a);
            SpinFor(100);  // the staging copy
            ReadIds(*p, b);
            if (a[0] != b[0] || a[1] != b[1] || a[0] < 0) ++pinChanged;
            rig.src.Ring().Unpin(p);
        }
    });
    std::thread left(lane, 0), right(lane, 1);
    left.join();
    right.join();
    clockThread.join();
    done = true;
    consumer.join();
    uploader.join();
    rig.End();
    std::printf("  threaded (%s): %zu shown (whole %d, composed %d, skipped %d, no-frame %d, refused %d), late %d, "
                "consumed %d, pinned %d, torn %d, mismatched %d, backwards %d, pin changed %d\n",
                direct ? "direct" : "old", rig.shownIds.size(), rig.whole, rig.composed, rig.skipped, rig.noFrame,
                rig.refused, late.load(), consumed.load(), pinned.load(), torn.load(), mismatched.load(),
                backwards.load(), pinChanged.load());
    CHECK(torn == 0, "threads: the renderer never sees a half-written frame");
    CHECK(pinChanged == 0, "threads: a pinned frame never changes under the upload thread");
    CHECK(mismatched == 0 && rig.mismatched == 0, "threads: matched pairs only");
    CHECK(backwards == 0, "threads: the renderer's frames only go forward");
    CHECK(rig.src.Ring().ReservedCount() == 0, "threads: every reserved frame comes back");
    CHECK(consumed > 100 && pinned > 100 && rig.shownIds.size() > (size_t)n / 4, "threads: it really ran");
    if (direct) CHECK(rig.whole > (int)rig.shownIds.size() / 2, "threads: most pairs whole");
}

}  // namespace

int main() {
    std::printf("bit-identical (direct vs compose):\n");
    BitIdentical();
    std::printf("entry table:\n");
    TableRules();
    std::printf("lane protocol over a real ring:\n");
    LaneProtocol();
    std::printf("threads:\n");
    ThreadedSoak(false);
    ThreadedSoak(true);
    if (g_failures) {
        std::fprintf(stderr, "direct_pair_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("direct_pair_test: all passed\n");
    return 0;
}
