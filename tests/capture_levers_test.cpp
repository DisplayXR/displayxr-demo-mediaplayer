// SPDX-License-Identifier: Apache-2.0
//
// The capture path's A/B levers (docs/CAPTURE-LEVERS.md):
//  1. pro.env parsing (pro/CaptureLevers.h ParseEnvText): comments, blanks, CRLF, a BOM, quotes,
//     spaces, bad lines; and applying it: the process environment wins over the file.
//  2. The lever table: every name unique, read through a getenv, the "set" summary.
//  3. MEDIAPLAYER_CAPTURE_PIN / MEDIAPLAYER_UPLOAD_PIN parsing (platform/ThreadPin.h).
//  4. LiveSourceBase::PublishV210 (MEDIAPLAYER_DECKLINK_RAW_V210, #238 phase 2): the eyes' rows
//     reach the ring frame byte for byte, MOVED (the caller's vectors come back holding the
//     ring buffer's previous planes, so a steady stream allocates nothing), tagged as the
//     renderer's v210 path wants (v210::Shape), and a shape the renderer would refuse is
//     neither published nor swapped. Over many frames of a queue-mode ring, with the consumer
//     taking frames between publishes.
// Pure: no SDK, no card, no GPU.

#include "media/FrameRing.h"
#include "media/V210.h"
#include "platform/ThreadPin.h"
#include "pro/CaptureLevers.h"
#include "pro/LiveSourceBase.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

using namespace mp;
using namespace mp::pro;

void EnvFile() {
    std::vector<int> bad;
    const std::string text =
        "\xEF\xBB\xBF# a comment\r\n"
        "MEDIAPLAYER_GPU_V210=1\r\n"
        "\r\n"
        "  MEDIAPLAYER_DECKLINK_RAW_V210 = on  \n"
        "DXR_VK_HUB_DEFER_COMPOSE_WAIT=\"1\"\n"
        "MEDIAPLAYER_CAPTURE_PIN='0,1'\n"
        "not a line\n"
        "=novalue\n"
        "BAD-KEY=1\n"
        "EMPTY=\n"
        "LAST=x";  // no newline at the end
    const std::vector<EnvEntry> e = ParseEnvText(text, &bad);
    CHECK(e.size() == 6, "six entries");
    if (e.size() == 6) {
        CHECK(e[0].key == "MEDIAPLAYER_GPU_V210" && e[0].value == "1" && e[0].line == 2, "BOM + CRLF stripped");
        CHECK(e[1].key == "MEDIAPLAYER_DECKLINK_RAW_V210" && e[1].value == "on", "spaces trimmed");
        CHECK(e[2].value == "1", "double quotes removed");
        CHECK(e[3].value == "0,1", "single quotes removed");
        CHECK(e[4].key == "EMPTY" && e[4].value.empty(), "an empty value is an entry");
        CHECK(e[5].key == "LAST" && e[5].value == "x", "last line without a newline");
    }
    CHECK(bad == std::vector<int>({7, 8, 9}), "bad lines reported by number");
    CHECK(ParseEnvText("").empty(), "empty text");
    CHECK(ParseEnvText("\n\n# x\n").empty(), "only blanks and comments");

    // The environment wins over the file.
    std::map<std::string, std::string> env = {{"MEDIAPLAYER_GPU_V210", "0"}};
    const auto get = [&](const char* k) -> const char* {
        auto it = env.find(k);
        return it == env.end() ? nullptr : it->second.c_str();
    };
    const auto set = [&](const std::string& k, const std::string& v) { env[k] = v; };
    const std::vector<EnvApplied> a = ApplyEnvEntries(e, get, set);
    CHECK(a.size() == e.size(), "one result per entry");
    CHECK(env["MEDIAPLAYER_GPU_V210"] == "0" && !a[0].fromFile && a[0].value == "0", "environment kept");
    CHECK(env["MEDIAPLAYER_DECKLINK_RAW_V210"] == "on" && a[1].fromFile, "file value set");
    CHECK(env["LAST"] == "x", "last applied");
}

void LeverTable() {
    const std::vector<Lever> t = CaptureLeverTable();
    std::set<std::string> names;
    for (const Lever& l : t) {
        CHECK(names.insert(l.name).second, "lever names are unique");
        CHECK(l.what && *l.what && l.def && *l.def, "every lever says what it does and its default");
    }
    CHECK(names.count("MEDIAPLAYER_DECKLINK_RAW_V210") && names.count("MEDIAPLAYER_DECKLINK_ALLOCATOR") &&
              names.count("MEDIAPLAYER_DECKLINK_HOLD") && names.count("MEDIAPLAYER_CAPTURE_PIN") &&
              names.count("MEDIAPLAYER_UPLOAD_PIN") && names.count("DXR_VK_HUB_DEFER_COMPOSE_WAIT"),
          "the new levers and the runtime's are in the table");
    const std::map<std::string, std::string> env = {{"MEDIAPLAYER_GPU_V210", "1"}, {"MEDIAPLAYER_CAPTURE_PIN", "auto"}};
    const std::vector<Lever> r = ReadCaptureLevers([&](const char* k) -> const char* {
        auto it = env.find(k);
        return it == env.end() ? nullptr : it->second.c_str();
    });
    CHECK(SetLeversText(r) == "MEDIAPLAYER_GPU_V210=1, MEDIAPLAYER_CAPTURE_PIN=auto", "set levers, table order");
    for (const Lever& l : r)
        if (std::string(l.name) == "MEDIAPLAYER_DECKLINK_DIRECT_PAIR")
            CHECK(!l.Set() && l.Effective() == "1", "unset: the default is effective");
    CHECK(SetLeversText(ReadCaptureLevers([](const char*) -> const char* { return nullptr; })).empty(),
          "nothing set: empty");
}

void PinParse() {
    CHECK(pin::GroupFromEnv(nullptr, 0, 5) == -1, "unset");
    CHECK(pin::GroupFromEnv("", 1, 5) == -1, "empty");
    CHECK(pin::GroupFromEnv("off", 0, 5) == -1, "off");
    CHECK(pin::GroupFromEnv("auto", 1, 7) == 7, "auto = the caller's default");
    CHECK(pin::GroupFromEnv("3", 0, 0) == 3 && pin::GroupFromEnv("3", 1, 0) == 3, "one number for every slot");
    CHECK(pin::GroupFromEnv("0,2", 0, 9) == 0 && pin::GroupFromEnv("0,2", 1, 9) == 2, "a list per slot");
    CHECK(pin::GroupFromEnv("0,2", 4, 9) == 2, "a short list repeats its last");
    CHECK(pin::GroupFromEnv("-1", 0, 9) == -1, "negative = not pinned");
    CHECK(pin::GroupFromEnv("x", 0, 9) == -1 && pin::GroupFromEnv("1,x", 0, 9) == -1, "not a number = not pinned");
    CHECK(!pin::PinCurrentThread(-1), "-1 never pins");
#if defined(_WIN32)
    CHECK(!pin::CacheGroups().empty(), "Windows lists at least one last-level-cache group");
    for (const pin::CacheGroup& g : pin::CacheGroups()) CHECK(g.processors > 0 && g.mask != 0, "a group has processors");
#endif
}

class Src : public LiveSourceBase {
public:
    Src() { Ring().SetQueueDepth(3); }
    void Stop() override {}
    const char* Kind() const override { return "test"; }
    using LiveSourceBase::PublishV210;
};

// Fills `v` in place (as the capture lane refills what PublishV210 handed back: resize, then
// write; no allocation once it has the capacity).
void Fill(std::vector<uint8_t>& v, int eyeW, int h, uint8_t seed) {
    v.resize(v210::RowBytes(eyeW) * (size_t)h);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)(seed + i * 7);
}
std::vector<uint8_t> Rows(int eyeW, int h, uint8_t seed) {
    std::vector<uint8_t> v;
    Fill(v, eyeW, h, seed);
    return v;
}

void RawPublish() {
    Src src;
    FrameRing& ring = src.Ring();
    const int eyeW = 1926, h = 20;  // not a multiple of 6 or 48: padded rows
    const size_t bytes = v210::RowBytes(eyeW) * (size_t)h;
    std::vector<uint8_t> l, r;
    std::set<const uint8_t*> seenData;
    for (int i = 0; i < 40; ++i) {
        Fill(l, eyeW, h, (uint8_t)(2 * i));
        Fill(r, eyeW, h, (uint8_t)(2 * i + 1));
        const std::vector<uint8_t> lWant = l, rWant = r;
        const uint8_t* lData = l.data();
        std::vector<uint8_t>* eyes[2] = {&l, &r};
        const bool ok = src.PublishV210(eyes, 2, eyeW, h, ColorMatrix::BT2020, ColorTransfer::HLG, false, 1.5 + i,
                                        "01:00:00:00", 0);
        CHECK(ok, "published");
        const FrameRing::Frame* f = ring.AcquireLatest();
        CHECK(f != nullptr, "the consumer gets it");
        if (!f) continue;
        int eyes2 = 0, ew = 0;
        CHECK(v210::Shape(*f, eyes2, ew) && eyes2 == 2 && ew == eyeW, "the renderer's v210 shape");
        CHECK(f->width == 2 * eyeW && f->height == h && f->format == PixFormat::V210, "size + format");
        CHECK(f->plane[0] == lWant && f->plane[1] == rWant, "both eyes byte for byte");
        CHECK(f->plane[0].data() == lData, "moved, not copied (the left eye's own buffer)");
        CHECK(f->matrix == ColorMatrix::BT2020 && f->transfer == ColorTransfer::HLG && !f->fullRange, "tags");
        CHECK(std::strcmp(f->timecode, "01:00:00:00") == 0 && f->captureSec == 1.5 + i, "timecode + time");
        seenData.insert(f->plane[0].data());
        // What came back is a ring buffer's old plane: after warm-up it is already the right size.
        if (i >= 8) CHECK(l.capacity() >= bytes && r.capacity() >= bytes, "warm: the returned planes fit (no allocation)");
    }
    CHECK(seenData.size() <= 8, "a steady stream cycles a fixed set of buffers");

    // Refused shapes: nothing published, nothing swapped.
    std::vector<uint8_t> a = Rows(eyeW, h, 1), b = Rows(eyeW, h, 2);
    const std::vector<uint8_t> a0 = a;
    std::vector<uint8_t>* eyes[2] = {&a, &b};
    const LiveStats before = src.GetStats();
    CHECK(!src.PublishV210(eyes, 2, eyeW + 1, h, ColorMatrix::BT709, ColorTransfer::SDR, false, 0, nullptr, 0), "odd width");
    CHECK(!src.PublishV210(eyes, 2, eyeW, h + 1, ColorMatrix::BT709, ColorTransfer::SDR, false, 0, nullptr, 0), "odd height");
    CHECK(!src.PublishV210(eyes, 3, eyeW, h, ColorMatrix::BT709, ColorTransfer::SDR, false, 0, nullptr, 0), "three eyes");
    b.resize(bytes - 1);
    CHECK(!src.PublishV210(eyes, 2, eyeW, h, ColorMatrix::BT709, ColorTransfer::SDR, false, 0, nullptr, 0), "short plane");
    CHECK(a == a0, "a refused publish swaps nothing");
    CHECK(src.GetStats().published == before.published, "a refused publish counts nothing");

    // One eye (a mono v210 input).
    std::vector<uint8_t> m = Rows(eyeW, h, 9);
    const std::vector<uint8_t> mWant = m;
    std::vector<uint8_t>* one[2] = {&m, nullptr};
    CHECK(src.PublishV210(one, 1, eyeW, h, ColorMatrix::BT709, ColorTransfer::SDR, false, 0, nullptr, -1), "mono");
    const FrameRing::Frame* f = ring.AcquireLatest();
    int e1 = 0, w1 = 0;
    CHECK(f && v210::Shape(*f, e1, w1) && e1 == 1 && w1 == eyeW && f->plane[0] == mWant && f->plane[1].empty(),
          "mono: one eye plane, the other cleared");
}

}  // namespace

int main() {
    EnvFile();
    LeverTable();
    PinParse();
    RawPublish();
    if (g_failures) {
        std::fprintf(stderr, "capture_levers_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("capture_levers_test: all passed\n");
    return 0;
}
