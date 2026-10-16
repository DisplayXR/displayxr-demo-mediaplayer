// SPDX-License-Identifier: Apache-2.0
//
// The station's side of the live Resolve panel (pro/ResolveLink): parsing what the panel
// writes, and the follow decision - Resolve plays, parks, scrubs, jumps to another clip,
// the panel stops - against a simulated station player.

#include "pro/ResolveLink.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace mp::pro::resolve;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// A station player: a file, a position that advances while playing.
struct Player {
    std::string path;
    double pos = -1.0;
    bool paused = true;
    int opens = 0, seeks = 0;
    void Advance(double dt) {
        if (!paused && pos >= 0.0) pos += dt;
    }
    void Apply(const Action& a) {
        switch (a.kind) {
        case Action::Open: path = a.path; pos = a.seconds; paused = a.paused; ++opens; break;
        case Action::Seek: pos = a.seconds; paused = a.paused; ++seeks; break;
        case Action::Play: paused = false; break;
        case Action::Pause: paused = true; break;
        default: break;
        }
    }
};

LinkSample Sample(long long seq, const std::string& clip, double sec) {
    LinkSample s;
    s.ok = true;
    s.seq = seq;
    s.playheadTc = "01:00:00:00";
    s.clipPath = clip;
    s.clipSeconds = sec;
    s.clipFps = 24.0;
    return s;
}

void TestParse() {
    const LinkSample s = ParseLink(R"({"seq":7,"timeline":"Reel 1","playhead_tc":"01:00:10:12",
        "timeline_fps":24,"clip":{"path":"D:/shots/a.mov","seconds":3.5,"fps":23.976}})");
    CHECK(s.ok && s.seq == 7, "link parses");
    CHECK(s.timeline == "Reel 1" && s.playheadTc == "01:00:10:12", "timeline + playhead");
    CHECK(s.clipPath == "D:/shots/a.mov" && std::fabs(s.clipSeconds - 3.5) < 1e-9, "clip");
    CHECK(!ParseLink("not json").ok, "garbage is not a sample");
    CHECK(!ParseLink(R"({"seq":1})").ok, "no playhead is not a sample");
    const LinkSample gap = ParseLink(R"({"seq":2,"playhead_tc":"01:00:00:00"})");
    CHECK(gap.ok && gap.clipPath.empty() && gap.clipSeconds < 0.0, "a gap: playhead, no clip");

    const Command c = ParseCommand(R"({"cmd":"snapshot","note":"too close","id":3})");
    CHECK(c.cmd == "snapshot" && c.note == "too close" && c.id == 3, "command parses");
    CHECK(ParseCommand("{oops").cmd.empty(), "a torn line is ignored");
}

void TestSamePath() {
#if defined(_WIN32)
    CHECK(SamePath("C:/Shots/A.mov", "c:\\shots\\a.MOV"), "Windows paths compare loosely");
#endif
    CHECK(SamePath("/a/b.mov", "/a/b.mov"), "same path");
    CHECK(!SamePath("/a/b.mov", "/a/c.mov"), "different file");
}

// Resolve plays for 4 s from 10 s; the panel samples at 5 Hz. The station should open the
// file, start playing, and then NOT seek every sample (it plays on its own clock).
void TestPlayFollows() {
    Follower f;
    Player p;
    double now = 0.0, rpos = 10.0;
    long long seq = 0;
    for (int i = 0; i < 20; ++i) {
        p.Apply(f.Update(Sample(++seq, "D:/a.mov", rpos), now, p.path, p.pos, p.paused, true));
        now += 0.2;
        rpos += 0.2;
        p.Advance(0.2);
    }
    CHECK(p.opens == 1, "the clip opens once");
    CHECK(!p.paused, "the station plays while Resolve plays");
    CHECK(f.ResolvePlaying(), "playing inferred from the playhead");
    CHECK(p.seeks <= 1, "no seek storm while playing in step");
    CHECK(std::fabs(p.pos - rpos) < 0.5, "in step at the end");
}

// Drift while playing (the station's decoder hiccups): corrected once it passes 0.5 s.
void TestDriftCorrected() {
    Follower f;
    Player p{"D:/a.mov", 0.0, false};
    double now = 0.0, rpos = 0.0;
    long long seq = 0;
    for (int i = 0; i < 10; ++i) {  // in step
        p.Apply(f.Update(Sample(++seq, "D:/a.mov", rpos), now, p.path, p.pos, p.paused, true));
        now += 0.2; rpos += 0.2; p.Advance(0.2);
    }
    const int before = p.seeks;
    p.pos -= 0.8;  // the station fell behind
    p.Apply(f.Update(Sample(++seq, "D:/a.mov", rpos), now, p.path, p.pos, p.paused, true));
    CHECK(p.seeks == before + 1 && std::fabs(p.pos - rpos) < 1e-9 && !p.paused, "drift > 0.5 s is corrected");
    now += 0.2; rpos += 0.2; p.Advance(0.2);
    p.pos -= 0.2;  // small drift: tolerated
    p.Apply(f.Update(Sample(++seq, "D:/a.mov", rpos), now, p.path, p.pos, p.paused, true));
    CHECK(p.seeks == before + 1, "drift under 0.5 s is left alone");
}

// Resolve stops: two equal samples -> parked -> the station pauses on the exact frame.
void TestParkLandsOnFrame() {
    Follower f;
    Player p{"D:/a.mov", 5.0, false};
    double now = 0.0;
    long long seq = 0;
    p.Apply(f.Update(Sample(++seq, "D:/a.mov", 5.0), now, p.path, p.pos, p.paused, true));
    now += 0.2; p.Advance(0.2);
    p.Apply(f.Update(Sample(++seq, "D:/a.mov", 5.0), now, p.path, p.pos, p.paused, true));
    CHECK(p.paused, "parked Resolve -> paused station");
    CHECK(std::fabs(p.pos - 5.0) < 1e-9, "on Resolve's frame");
    const int seeks = p.seeks;
    now += 0.2;
    p.Apply(f.Update(Sample(++seq, "D:/a.mov", 5.0), now, p.path, p.pos, p.paused, true));
    CHECK(p.seeks == seeks, "no repeated seeks while parked");
}

// A click elsewhere on the timeline (a jump much faster than real time) is a park + seek,
// not "playing".
void TestJumpIsNotPlay() {
    Follower f;
    Player p{"D:/a.mov", 1.0, true};
    long long seq = 0;
    p.Apply(f.Update(Sample(++seq, "D:/a.mov", 1.0), 0.0, p.path, p.pos, p.paused, true));
    p.Apply(f.Update(Sample(++seq, "D:/a.mov", 30.0), 0.2, p.path, p.pos, p.paused, true));
    CHECK(!f.ResolvePlaying(), "a jump is not playback");
    CHECK(p.paused && std::fabs(p.pos - 30.0) < 1e-9, "the station lands on the jump target, paused");
}

// The same sample (seq unchanged) twice does nothing; a missing file is never opened;
// a gap on the timeline leaves the station alone; a stale panel stops following.
void TestEdges() {
    Follower f;
    Player p{"D:/a.mov", 2.0, true};
    p.Apply(f.Update(Sample(1, "D:/a.mov", 4.0), 0.0, p.path, p.pos, p.paused, true));
    const double after = p.pos;
    p.pos = 9.0;
    p.Apply(f.Update(Sample(1, "D:/a.mov", 4.0), 0.1, p.path, p.pos, p.paused, true));
    CHECK(after == 4.0 && p.pos == 9.0, "a repeated sample is ignored");

    p.Apply(f.Update(Sample(2, "E:/unreachable.mov", 1.0), 0.2, p.path, p.pos, p.paused, false));
    CHECK(p.path == "D:/a.mov" && p.opens == 0, "a file the station cannot reach is not opened");

    LinkSample gap = Sample(3, "", -1.0);
    p.Apply(f.Update(gap, 0.4, p.path, p.pos, p.paused, true));
    CHECK(p.pos == 9.0, "a gap leaves the station alone");

    CHECK(f.Active(1.0), "fresh panel = following");
    CHECK(!f.Active(5.0), "silent panel = not following");

    Player still{"D:/a.jpg", -1.0, true};
    still.Apply(f.Update(Sample(4, "D:/a.jpg", 0.0), 5.0, still.path, still.pos, still.paused, true));
    CHECK(still.seeks == 0 && still.opens == 0, "a still is not seeked");
}

}  // namespace

int main() {
    TestParse();
    TestSamePath();
    TestPlayFollows();
    TestDriftCorrected();
    TestParkLandsOnFrame();
    TestJumpIsNotPlay();
    TestEdges();
    if (g_failures) {
        std::fprintf(stderr, "resolve_link_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("resolve_link_test: all passed\n");
    return 0;
}
