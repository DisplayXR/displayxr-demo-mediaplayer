// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the station's QC report (#137): SMPTE timecode <-> frames (drop-frame
// included), the Resolve marker EDL, the CSV and the HTML page. Pure: no files, no GPU.

#include "pro/QcReport.h"

#include <cstdio>
#include <string>

using namespace mp::pro::qc;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void TimecodeMaths() {
    long long f = 0;
    bool drop = true;
    CHECK(TimecodeToFrames("01:00:00:00", 24, f, &drop) && f == 86400 && !drop, "24p: one hour");
    CHECK(TimecodeToFrames("00:00:01:12", 25, f) && f == 37, "25p: 1 s 12 f");
    CHECK(!TimecodeToFrames("00:00:01:25", 25, f), "a frame number >= fps is refused");
    CHECK(!TimecodeToFrames("", 25, f) && !TimecodeToFrames("garbage", 25, f), "unparsable");
    // 29.97 drop-frame: 00:01:00;02 is the first label after the skip = frame 1800
    CHECK(TimecodeToFrames("00:01:00;02", 30, f, &drop) && f == 1800 && drop, "DF: first label of minute 1");
    CHECK(TimecodeToFrames("00:10:00;00", 30, f) && f == 17982, "DF: ten minutes = 17982 frames");
    CHECK(FramesToTimecode(1800, 30, true) == "00:01:00;02", "DF: frame 1800 -> 00:01:00;02");
    CHECK(FramesToTimecode(1799, 30, true) == "00:00:59;29", "DF: frame 1799 -> 00:00:59;29");
    CHECK(FramesToTimecode(17982, 30, true) == "00:10:00;00", "DF: minute 10 keeps :00");
    CHECK(FramesToTimecode(86400, 24, false) == "01:00:00:00", "NDF round trip");
    // every DF frame of an hour round-trips through its label
    bool ok = true;
    for (long long i = 0; i < 30LL * 3600 && ok; i += 7) {
        long long back = -1;
        ok = TimecodeToFrames(FramesToTimecode(i, 30, true), 30, back) && back == i;
    }
    CHECK(ok, "DF: an hour of frames round-trips");
    for (long long i = 0; i < 60LL * 1200 && ok; i += 5) {
        long long back = -1;
        ok = TimecodeToFrames(FramesToTimecode(i, 60, true), 60, back) && back == i;
    }
    CHECK(ok, "59.94 DF: twenty minutes round-trip");
}

Session Sample() {
    Session s;
    s.title = "QC 2026-10-29 14:05";
    s.input = "Test rig";
    s.tcFps = 24;
    s.budgetNearPct = -2.0f;
    s.budgetFarPct = 1.0f;
    s.budgetVerticalPx = 1.0f;
    s.budgetDeltaE = 2.0f;
    s.budgetEdgePct = -0.3f;
    Alert a;
    a.what = "Vertical 4.0 px for 3 s";
    a.timecode = "01:00:10:00";
    a.endTimecode = "01:00:13:00";
    a.clock = "14:01:10";
    a.tcFps = 24;
    a.durationS = 3.0;
    s.alerts.push_back(a);
    Alert b;  // no timecode: CSV and HTML, not the EDL
    b.what = "Colour <dE 5> | right eye warm";
    b.clock = "14:02:00";
    b.durationS = 1.5;
    s.alerts.push_back(b);
    Snapshot sn;
    sn.note = "check the \"left\" edge, then cut";
    sn.timecode = "01:00:05:12";
    sn.clock = "14:01:05";
    sn.tcFps = 24;
    sn.image = "C:\\pics\\pro_x_atlas.png";
    sn.thumbJpeg = std::string("\xff\xd8\xff", 3);
    sn.rig.measured = true;
    sn.rig.verticalPx = 1.5f;  // over the 1 px budget
    sn.rig.nearPct = -1.0f;
    sn.rig.farPct = 0.5f;
    sn.rig.deltaE = 0.8f;
    sn.eyeSyncFrames = 0;
    s.snapshots.push_back(sn);
    return s;
}

void Edl() {
    const std::string e = MakeEdl(Sample());
    CHECK(Has(e, "TITLE: QC 2026-10-29 14:05\nFCM: NON-DROP FRAME\n"), "EDL header");
    // sorted by timecode: the snapshot (01:00:05:12) first, then the alert (01:00:10:00)
    const size_t snap = e.find("01:00:05:12 01:00:05:13 01:00:05:12 01:00:05:13"), alert = e.find("01:00:10:00 01:00:10:01");
    CHECK(snap != std::string::npos && alert != std::string::npos && snap < alert, "events in timecode order");
    CHECK(Has(e, "001  001      V     C        01:00:05:12"), "event 001 is the snapshot");
    CHECK(Has(e, " |C:ResolveColorBlue |M:check the \"left\" edge, then cut |D:1\n"), "snapshot marker: blue, note, 1 frame");
    CHECK(Has(e, " |C:ResolveColorRed |M:Vertical 4.0 px for 3 s |D:72\n"), "alert marker: red, 3 s at 24 fps = 72 frames");
    CHECK(!Has(e, "right eye warm"), "a row with no timecode is left out");
    CHECK(RowsWithoutTimecode(Sample()) == 1, "one untimed row");
    Session d;
    Alert a;
    a.what = "x";
    a.timecode = "00:01:00;02";
    a.tcFps = 30;
    a.durationS = 1.0;
    d.alerts.push_back(a);
    const std::string de = MakeEdl(d);
    CHECK(Has(de, "FCM: DROP FRAME") && Has(de, "00:01:00;02 00:01:00;03") && Has(de, "|D:30"), "drop-frame EDL");
}

void CsvAndHtml() {
    const std::string c = MakeCsv(Sample());
    CHECK(c.rfind("kind,timecode,end_timecode,duration_frames,", 0) == 0, "CSV header");
    CHECK(Has(c, "alert,01:00:10:00,01:00:13:00,72,24,14:01:10,Vertical 4.0 px for 3 s,"), "CSV alert row");
    CHECK(Has(c, ",\"check the \"\"left\"\" edge, then cut\","), "CSV quotes a note with commas and quotes");
    CHECK(Has(c, ",1.50,"), "CSV carries the rig numbers");
    const std::string h = MakeHtml(Sample());
    CHECK(Has(h, "<!doctype html>") && Has(h, "</html>"), "a whole page");
    CHECK(Has(h, "Colour &lt;dE 5&gt; | right eye warm"), "HTML escapes text");
    CHECK(Has(h, "src=\"data:image/jpeg;base64,/9j/\""), "the thumbnail is embedded (base64 of FF D8 FF)");
    CHECK(Has(h, "<span class=bad>1.50 px</span>"), "a number over budget is flagged");
    CHECK(Has(h, "Depth -1.00%") && !Has(h, "<span class=bad>-1.00%"), "a number in budget is not");
    CHECK(Has(h, "1 row(s) have no source timecode"), "the untimed rows are called out");
    Session empty;
    const std::string he = MakeHtml(empty);
    CHECK(Has(he, "No rig check went over budget") && Has(he, "No snapshots"), "an empty session says so");
}

}  // namespace

int main() {
    TimecodeMaths();
    Edl();
    CsvAndHtml();
    if (g_failures) {
        std::fprintf(stderr, "qc_report_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("qc_report_test: all passed\n");
    return 0;
}
