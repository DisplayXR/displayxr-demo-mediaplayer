// SPDX-License-Identifier: Apache-2.0
//
// pro::qc — the station's QC report (#137): one session's rig alerts, snapshots (both eyes,
// graded, with their notes and rig numbers) and the depth budget, as a self-contained HTML
// page a stereo supervisor can hand on, plus marker lists an editor can import:
//
//   report.html   the page; snapshot thumbnails are embedded (data: URIs), so the one file
//                 travels on its own
//   markers.edl   DaVinci Resolve timeline markers (Timeline > Import > Timeline Markers
//                 from EDL): alerts red, snapshots blue, at their SMPTE timecode
//   markers.csv   every row, with the rig numbers, for a spreadsheet or another NLE
//
// Pure text in, text out (the station gathers the session, encodes the thumbnails and
// writes the files). Timecodes are SMPTE as the station read them: "HH:MM:SS:FF", with
// ';' (or '.') before the frames for drop-frame. tests/qc_report_test.cpp.
#pragma once

#include <string>
#include <vector>

namespace mp::pro::qc {

struct Rig {  // the rig numbers with a snapshot (measured = false: none)
    bool measured = false;
    float verticalPx = 0, rollDeg = 0, nearPct = 0, farPct = 0, deltaE = 0;
    bool edgeMeasured[2] = {false, false};
    float edgePct[2] = {0, 0};  // left, right frame edge
};

struct Alert {
    std::string what;         // "Foreground at -2.1% for 3 s"
    std::string timecode;     // where it started ("" = no timecode)
    std::string endTimecode;  // where it ended ("" = unknown / still open)
    std::string clock;        // wall clock when it started, HH:MM:SS
    int tcFps = 0;            // the signal's rate when it was logged (0 = unknown)
    double durationS = 0.0;
    bool open = false;        // still over budget when the report was made
};

struct Snapshot {
    std::string image;        // the capture on disk (both eyes)
    std::string thumbJpeg;    // embedded thumbnail bytes (JPEG), "" = none
    std::string note, timecode, clock;
    int tcFps = 0;
    int eyeSyncFrames = -1;   // -1 = not measured
    Rig rig;
};

struct Session {
    std::string title;        // "QC 2026-10-29 14:05"
    std::string input;        // the live input / file name
    std::string started, exported;  // wall clock
    int tcFps = 0;            // the signal's rate (0 = unknown): the EDL's rate when rows have none
    float budgetNearPct = 0, budgetFarPct = 0, budgetVerticalPx = 0, budgetDeltaE = 0, budgetEdgePct = 0;
    std::vector<Alert> alerts;
    std::vector<Snapshot> snapshots;
};

// SMPTE timecode <-> a frame count at `fps` (nominal: 24, 25, 30, 50, 60; 29.97 / 59.94
// count as 30 / 60 with drop-frame when the text says so). False / "" when unparsable.
bool TimecodeToFrames(const std::string& tc, int fps, long long& frames, bool* dropFrame = nullptr);
std::string FramesToTimecode(long long frames, int fps, bool dropFrame);

std::string MakeEdl(const Session& s);   // rows without a timecode are left out
std::string MakeCsv(const Session& s);
std::string MakeHtml(const Session& s);
// Rows the EDL could not place (no timecode), for the report's note and the tool's reply.
int RowsWithoutTimecode(const Session& s);

}  // namespace mp::pro::qc
