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

#include <cstddef>
#include <string>
#include <vector>

namespace mp::pro::qc {

struct Rig {  // the rig numbers with a snapshot (measured = false: none)
    bool measured = false;
    float verticalPx = 0, rollDeg = 0, nearPct = 0, farPct = 0, deltaE = 0;
    bool edgeMeasured[2] = {false, false};
    float edgePct[2] = {0, 0};  // left, right frame edge
};

// The display calibration (#133) in effect: snapshots and the report show the picture with
// it baked in, so each records which one (#169 / David's call 2). Recorded with every
// snapshot (what that capture has in it) and once for the session (what was on at export).
struct Calibration {
    std::string state;        // "on", "bypassed" or "none"; "" = not recorded
    std::string name;         // the LUT's TITLE, else its file name
    std::string panel;        // the panel key it is keyed by (pro/DisplayCalibration.h)
    std::string panelSource;  // "runtime" (stable key) or "device_name" (older runtime)
    std::string file;         // the .cube it was loaded from
    bool kept = false;        // the panel's kept file; false = this run only
    int lutSize = 0;          // N of the N^3 LUT
    std::string modified;     // the file's last change, "YYYY-MM-DD HH:MM:SS" local ("" unknown)
    std::string fingerprint;  // 16 hex digits over the LUT's values: the same LUT reads the same

    bool operator==(const Calibration& o) const {
        return state == o.state && name == o.name && panel == o.panel && panelSource == o.panelSource &&
               file == o.file && kept == o.kept && lutSize == o.lutSize && modified == o.modified &&
               fingerprint == o.fingerprint;
    }
    bool operator!=(const Calibration& o) const { return !(*this == o); }
};

// One line for a person: "PanelCal v3 (33^3, on, fingerprint 0123abcd4567ef89, file ...,
// changed ...)", "bypassed: ..." or "none". "" when not recorded.
std::string CalibrationText(const Calibration& c);

// FNV-1a 64 over `n` floats as bytes, as 16 lower-case hex digits: Calibration::fingerprint.
std::string LutFingerprint(const float* values, size_t n);

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
    Calibration calibration;  // the display calibration in this capture
};

struct Session {
    std::string title;        // "QC 2026-10-29 14:05"
    std::string input;        // the live input / file name
    std::string started, exported;  // wall clock
    int tcFps = 0;            // the signal's rate (0 = unknown): the EDL's rate when rows have none
    Calibration calibration;  // the display calibration when the report was made
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
