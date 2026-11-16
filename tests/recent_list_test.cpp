// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the desktop Recent menu's list (#252 phase 0): newest first, no
// duplicates (case and slash insensitive on Windows), the capacity, the text round trip,
// Save/Load through a temp file, and the menu labels. No UI.

#include "media/RecentList.h"

#include <cstdio>
#include <filesystem>
#include <string>

using mp::RecentLabel;
using mp::RecentList;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

void TestOrderAndDedupe() {
    RecentList r(3);
    CHECK(r.Add("a.mp4"), "add a");
    CHECK(r.Add("b.mp4"), "add b");
    CHECK(!r.Add("b.mp4"), "re-adding the front is no change");
    CHECK(r.Add("a.mp4"), "re-adding an older one moves it");
    CHECK(r.Items().size() == 2 && r.Items()[0] == "a.mp4" && r.Items()[1] == "b.mp4", "a moved to front");
    r.Add("c.mp4");
    r.Add("d.mp4");
    CHECK(r.Items().size() == 3, "capacity 3");
    CHECK(r.Items()[0] == "d.mp4" && r.Items()[2] == "a.mp4", "oldest (b) dropped");
    CHECK(!r.Add(""), "empty path ignored");
    CHECK(r.Remove("c.mp4") && r.Items().size() == 2, "remove");
    CHECK(!r.Remove("zzz"), "remove missing is no change");
}

void TestPathCompare() {
#if defined(_WIN32)
    CHECK(RecentList::SamePath("C:\\Media\\Clip.MP4", "c:/media/clip.mp4"), "windows: case + slash insensitive");
    RecentList r;
    r.Add("C:\\Media\\Clip.mp4");
    r.Add("c:/media/clip.mp4");
    CHECK(r.Items().size() == 1 && r.Items()[0] == "c:/media/clip.mp4", "windows: one entry, latest spelling");
#else
    CHECK(!RecentList::SamePath("/a/Clip.mp4", "/a/clip.mp4"), "posix: case sensitive");
#endif
    CHECK(!RecentList::SamePath("a.mp4", "a.mp"), "different lengths differ");
}

void TestText() {
    RecentList r(4);
    r.SetFromText("  one.jpg \r\n\r\ntwo.mp4\none.jpg\nthree.lif\nfour.mpo\nfive.mp4\n");
    CHECK(r.Items().size() == 4, "blank + duplicate lines skipped, capacity applied");
    CHECK(r.Items()[0] == "one.jpg" && r.Items()[3] == "four.mpo", "trimmed, file order kept");
    RecentList back(4);
    back.SetFromText(r.ToText());
    CHECK(back.Items() == r.Items(), "text round trip");
}

void TestFile() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "mp_recent_list_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string file = (dir / "sub" / "recent.txt").u8string();
    RecentList r;
    r.Add("x.mp4");
    r.Add("y.jpg");
    CHECK(r.Save(file), "save creates the folders");
    RecentList back;
    CHECK(back.Load(file), "load");
    CHECK(back.Items() == r.Items(), "file round trip");
    CHECK(!fs::exists(file + ".tmp"), "no temp file left");
    RecentList none;
    CHECK(!none.Load((dir / "missing.txt").u8string()), "missing file -> false");
    CHECK(!none.Save(""), "empty path -> false");
    fs::remove_all(dir, ec);
}

void TestLabels() {
#if defined(_WIN32)
    CHECK(RecentLabel("D:\\Media\\Videos\\trailer.mp4", false) == "trailer.mp4   (D:\\Media\\Videos)", "file label");
    CHECK(RecentLabel("D:\\Media\\Stereo\\", true) == "Stereo\\   (D:\\Media)", "folder label");
    CHECK(RecentLabel("D:\\clip.mp4", false) == "clip.mp4   (D:\\)", "drive root parent");
#else
    CHECK(RecentLabel("/home/u/Videos/trailer.mp4", false) == "trailer.mp4   (/home/u/Videos)", "file label");
    CHECK(RecentLabel("/home/u/Stereo/", true) == "Stereo/   (/home/u)", "folder label");
#endif
    CHECK(RecentLabel("clip.mp4", false) == "clip.mp4", "bare name");
}

} // namespace

int main() {
    TestOrderAndDedupe();
    TestPathCompare();
    TestText();
    TestFile();
    TestLabels();
    if (g_failures) {
        std::fprintf(stderr, "recent_list_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("recent_list_test: OK\n");
    return 0;
}
