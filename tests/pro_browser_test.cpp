// SPDX-License-Identifier: Apache-2.0
//
// The media browser in Media Player Pro (#267): a render's frames as one tile per sequence
// (SequenceTiles), and the capture gate that parks the thumbnail work (BackgroundGate).
// Pure: no files, no clock.

#include "browser/BackgroundGate.h"
#include "browser/SequenceTiles.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace mp::browser;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

void TestSequences() {
    std::vector<std::string> paths;
    // 240 frames, shuffled a little: the walk returns them in directory order.
    for (int n = 1240; n >= 1001; --n) paths.push_back("//server/plates/sh010/sh010_L.v002." + std::to_string(n) + ".exr");
    paths.push_back("//server/plates/sh010/sh010_L.v002.999.exr");   // other padding: not this sequence
    paths.push_back("//server/plates/sh010/other.1001.exr");          // a lone frame: left alone
    paths.push_back("//server/plates/sh010/sh010_L.v002.1005.dpx");   // other extension: left alone
    paths.push_back("//server/plates/sh010/plate.exr");               // no frame number
    paths.push_back("//server/plates/sh010/clip.0001.mov");           // not a sequence format
    paths.push_back("//server/plates/sh020/sh010_L.v002.1001.exr");   // same name, other folder
    paths.push_back("//server/plates/sh020/SH010_L.V002.1002.EXR");   // ... any case (Windows)
    paths.push_back("C:/renders/beauty_0001.tif");
    paths.push_back("C:/renders/beauty_0002.tif");

    const std::vector<SequenceTile> tiles = GroupSequences(paths);
    CHECK(tiles.size() == 3, "three sequences: sh010 EXR, sh020 EXR, the TIFF render");
    const SequenceTile* sh010 = nullptr;
    for (const SequenceTile& t : tiles)
        if (t.members.size() == 240) sh010 = &t;
    CHECK(sh010 != nullptr, "240 frames in one tile");
    if (sh010) {
        CHECK(sh010->firstFrame == 1001 && sh010->lastFrame == 1240, "frame range 1001-1240");
        CHECK(paths[sh010->first].find(".1001.exr") != std::string::npos, "the tile keeps the lowest frame");
        CHECK(SequenceTileName("sh010_L.v002.1001.exr", sh010->firstFrame, sh010->lastFrame) ==
                  "sh010_L.v002.[1001-1240].exr",
              "the tile's name shows the range and keeps the extension");
    }
    for (const SequenceTile& t : tiles) {
        for (size_t m : t.members) {
            CHECK(paths[m].find("other.") == std::string::npos, "a lone frame is not folded");
            CHECK(paths[m].find(".dpx") == std::string::npos, "another extension is not folded");
            CHECK(paths[m].find(".999.") == std::string::npos, "other padding is not folded");
        }
    }
    CHECK(SequenceTileName("plate.exr", 1, 2) == "plate.exr", "no frame number: the name as it is");
    CHECK(GroupSequences({}).empty(), "nothing: no tiles");
}

void TestGate() {
    BackgroundGate g(2.0);
    CHECK(!g.Paused(), "starts running");
    CHECK(!g.Update(false, 0.0), "no capture: no change");
    CHECK(g.Update(true, 1.0) && g.Paused(), "a capture pauses at once");
    CHECK(!g.Update(true, 5.0) && g.Paused(), "stays paused while it streams");
    CHECK(!g.Update(false, 6.0) && g.Paused(), "a drop does not resume at once");
    CHECK(!g.Update(true, 7.0) && g.Paused(), "the signal is back: still paused");
    CHECK(!g.Update(false, 8.0) && g.Paused(), "off again: the hold starts over");
    CHECK(!g.Update(false, 9.9) && g.Paused(), "1.9 s off: still paused");
    CHECK(g.Update(false, 10.0) && !g.Paused(), "2 s off: resumes");
    CHECK(g.Notice() != nullptr && *g.Notice(), "a notice to show");
}

}  // namespace

int main() {
    TestSequences();
    TestGate();
    if (g_failures) {
        std::fprintf(stderr, "pro_browser_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pro_browser_test: OK\n");
    return 0;
}
