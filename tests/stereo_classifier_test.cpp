// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the media browser's StereoClassifier (#160): the 3D badge needs positive
// evidence (file name, a 3D file such as a LIF, or a decided side-by-side content check on the
// thumbnail), and verdicts survive a restart through the cache file, keyed by id, date
// modified and size, except a 2D one reached without the file check it needed (#180).
// Synthetic thumbnails, no fixtures.

#include "browser/StereoClassifier.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using mp::StereoLayout;
using mp::browser::MediaItem;
using mp::browser::StereoClassifier;
using mp::browser::StereoVerdict;
using mp::browser::Thumbnail;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// Smooth aperiodic texture, as in stereo_detect_test.cpp: a shifted copy of it reads as a
// stereo pair, a continuous span of it as one mono scene.
float Hash(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}
float ValueNoise(float x, float y, float scale) {
    const float fx = x / scale, fy = y / scale;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float tx = fx - (float)x0, ty = fy - (float)y0;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const float a = Hash(x0, y0), b = Hash(x0 + 1, y0);
    const float c = Hash(x0, y0 + 1), d = Hash(x0 + 1, y0 + 1);
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}
uint8_t Texture(int x, int y) {
    float v = 20.0f + 120.0f * ValueNoise((float)x, (float)y, 37.0f) + 70.0f * ValueNoise((float)x, (float)y, 13.0f) +
              40.0f * ValueNoise((float)x, (float)y, 5.0f);
    return (uint8_t)(v > 255.0f ? 255.0f : v);
}

// A full side-by-side thumbnail (two 16:9 eyes, 3 px apart), or a mono one of the same size.
Thumbnail MakeThumbnail(int64_t id, bool sbs) {
    const int eyeW = 256, eyeH = 144;
    Thumbnail t;
    t.id = id;
    t.width = eyeW * 2;
    t.height = eyeH;
    t.rgba.resize((size_t)t.width * t.height * 4);
    for (int y = 0; y < eyeH; ++y) {
        for (int x = 0; x < eyeW * 2; ++x) {
            const int sx = sbs ? (x < eyeW ? x : x - eyeW + 3) : x;
            uint8_t* p = &t.rgba[((size_t)y * t.width + x) * 4];
            p[0] = p[1] = p[2] = Texture(sx, y);
            p[3] = 255;
        }
    }
    return t;
}

MediaItem Item(int64_t id, const std::string& name) {
    MediaItem item;
    item.id = id;
    item.name = name;
    item.modified = 1000 + id;
    item.bytes = 5000 + id;
    return item;
}

// Wait for the worker. Results come back in the order classified.
bool Result(StereoClassifier& c, Thumbnail& t, StereoVerdict& v) {
    for (int i = 0; i < 500; ++i) {
        if (c.TakeResult(t, v)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void TestEvidence(const std::string& cachePath) {
    StereoClassifier c;
    CHECK(c.Initialize(cachePath), "initialize");

    const StereoVerdict byName = c.Get(Item(1, "clip_half_2x1.mp4"));
    CHECK(byName.known && byName.stereo && byName.layout == StereoLayout::SbsHalf, "file name decides at once");
    CHECK(!c.Get(Item(2, "photo.jpg")).known, "a plain name is not known before its thumbnail");

    CHECK(c.NeedsFileCheck(Item(2, "photo.JPG")), "file check: an unknown JPEG may be a LIF");
    CHECK(c.NeedsFileCheck(Item(2, "photo.lif")), "file check: a .lif");
    CHECK(!c.NeedsFileCheck(Item(2, "photo.png")), "file check: a PNG is never a LIF");
    CHECK(!c.NeedsFileCheck(Item(1, "clip_half_2x1.mp4")), "file check: not when the name decides");
    MediaItem video = Item(7, "clip.mkv");
    video.type = mp::browser::MediaType::Video;
    CHECK(c.NeedsFileCheck(video), "file check: an unknown video may have two tracks");

    Thumbnail t;
    StereoVerdict v;
    Thumbnail lif = MakeThumbnail(3, false);
    lif.file3D = true;
    c.Classify(Item(3, "IMG_0003.jpg"), std::move(lif));
    CHECK(c.TakeResult(t, v), "a 3D file is decided without the worker");
    CHECK(t.id == 3 && v.stereo && v.layout == StereoLayout::Dual, "a 3D file is stereo, shown whole");

    c.Classify(Item(4, "beach.jpg"), MakeThumbnail(4, true));
    CHECK(Result(c, t, v), "side by side: result arrives");
    CHECK(t.id == 4 && t.width == 512 && v.known && v.stereo, "side by side content is stereo");
    CHECK(v.layout == StereoLayout::SbsFull, "two 16:9 eyes are full side by side");

    Thumbnail street = MakeThumbnail(5, false);
    street.fileChecked = true;  // read, and not a LIF
    c.Classify(Item(5, "street.jpg"), std::move(street));
    CHECK(Result(c, t, v), "mono: result arrives");
    CHECK(t.id == 5 && v.known && !v.stereo && v.layout == StereoLayout::Mono, "mono content is not stereo");

    Thumbnail none;
    none.id = 6;
    none.fileChecked = true;
    c.Classify(Item(6, "broken.jpg"), std::move(none));
    CHECK(Result(c, t, v), "no thumbnail: result arrives");
    CHECK(t.id == 6 && v.known && !v.stereo, "no thumbnail and no other evidence: not stereo");

    // The file did not open, so a LIF cannot be ruled out: 2D for now.
    c.Classify(Item(8, "IMG_0008.jpg"), MakeThumbnail(8, false));
    CHECK(Result(c, t, v), "unchecked file: result arrives");
    CHECK(t.id == 8 && v.known && !v.stereo, "unchecked file with mono content: 2D for now");
    CHECK(c.Get(Item(8, "IMG_0008.jpg")).known, "and for the rest of the run");

    CHECK(c.Get(Item(4, "beach.jpg")).stereo, "a content verdict is remembered");
    c.Shutdown();
}

void TestCache(const std::string& cachePath) {
    StereoClassifier c;
    c.Initialize(cachePath);
    const StereoVerdict beach = c.Get(Item(4, "beach.jpg"));
    CHECK(beach.known && beach.stereo && beach.layout == StereoLayout::SbsFull, "the cache survives a restart");
    const StereoVerdict street = c.Get(Item(5, "street.jpg"));
    CHECK(street.known && !street.stereo, "a mono verdict is cached too");
    CHECK(!c.Get(Item(8, "IMG_0008.jpg")).known, "but not one without the file check it needed");
    CHECK(c.NeedsFileCheck(Item(8, "IMG_0008.jpg")), "that file gets its check next run");
    MediaItem edited = Item(4, "beach.jpg");
    edited.modified += 1;
    CHECK(!c.Get(edited).known, "an edited file is worked out again");
    CHECK(!c.NeedsFileCheck(Item(5, "street.jpg")), "file check: not for a cached verdict");

    // A cached item still comes back through TakeResult, with its cached verdict.
    c.Classify(Item(4, "beach.jpg"), MakeThumbnail(4, false));
    Thumbnail t;
    StereoVerdict v;
    CHECK(c.TakeResult(t, v) && v.stereo, "a cached verdict skips the content check");
    c.Shutdown();
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string cachePath = (fs::temp_directory_path() / "mp_stereo_classifier_test.txt").string();
    std::error_code ec;
    fs::remove(cachePath, ec);
    TestEvidence(cachePath);
    TestCache(cachePath);
    fs::remove(cachePath, ec);
    if (g_failures) {
        std::fprintf(stderr, "stereo_classifier_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("stereo_classifier_test: all passed\n");
    return 0;
}
