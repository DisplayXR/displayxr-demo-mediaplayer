// SPDX-License-Identifier: Apache-2.0
//
// pro::ResolveLink — the station's half of the live Resolve panel (docs/PRO.md, Review
// tools: "Resolve panel: timecode follow, markers for notes and depth warnings").
//
// The panel (tools/resolve/DisplayXR Station Panel.py) runs inside DaVinci Resolve. The two
// sides talk through small files in the station folder (Pictures/DisplayXR), so neither
// needs a port, a server or the other to be running first:
//
//   station_status.json      station -> panel, about 4x a second: input, timecode, delay,
//                            rig numbers, open alert, the file playing and its position.
//   resolve_link.json        panel -> station, about 5x a second: timeline, playhead
//                            timecode, and the clip under the playhead (file + source time).
//   resolve_commands.jsonl   panel -> station, appended: {"cmd":"snapshot","note":...}.
//
// Notes and alerts reach Resolve as markers through the files the station already writes
// (*_note.json, station_alerts.jsonl); the panel adds them as they appear.
//
// Follow (station follows Resolve's playhead when it plays the same media as a file):
// Resolve's scripting API reports the playhead but not whether it is playing, so the
// decision below infers it from how the playhead moves between samples. Pure, unit-tested
// in tests/resolve_link_test.cpp.
#pragma once

#include <string>

namespace mp::pro::resolve {

// One sample of resolve_link.json.
struct LinkSample {
    bool ok = false;           // parsed and carried a playhead
    long long seq = 0;         // the panel's counter (a new sample = a new seq)
    std::string timeline;
    std::string playheadTc;    // record timecode, "HH:MM:SS:FF"
    double timelineFps = 0.0;
    std::string clipPath;      // media file under the playhead ("" = none / a generator)
    double clipSeconds = -1.0; // source time inside clipPath (-1 = unknown)
    double clipFps = 0.0;
    bool follow = true;        // the panel's "Station follows Resolve" box
};

// Parse resolve_link.json text; `ok` false when it is not one.
LinkSample ParseLink(const std::string& text);

// Command lines in resolve_commands.jsonl.
struct Command {
    std::string cmd;   // "snapshot", "goto" ... ("" = unparsable)
    std::string note;
    long long id = 0;  // the panel's counter, so a command runs once
};
Command ParseCommand(const std::string& line);

// The follow decision. Feed it every new sample with the station's clock; it says what the
// station's player should do.
struct Action {
    enum Kind { None, Open, Seek, Play, Pause } kind = None;
    std::string path;      // Open
    double seconds = 0.0;  // Open / Seek target
    bool paused = true;    // Open / Seek: leave paused after
};

class Follower {
public:
    // How far the station may drift from Resolve before it seeks. While Resolve plays, the
    // station plays on its own clock and only corrects drift beyond this (seeking at the
    // panel's 5 Hz poll would stutter); while Resolve is parked, the station lands on the
    // exact frame.
    double playingTolerance = 0.5;  // s
    // The playhead counts as moving when it advanced this fraction of the wall time since
    // the previous sample (1.0 = real time; scrubbing and J/K/L shuttle also count).
    double movingRatio = 0.5;
    // A sample older than this (the panel stopped / Resolve quit) means "not following".
    double staleAfter = 2.0;  // s

    // `now` = station clock (s). `localPath` / `localSeconds` / `localPaused` = what the
    // station's player shows (localSeconds < 0 = not a video). `fileExists` lets the caller
    // say whether clipPath is reachable from this machine.
    Action Update(const LinkSample& s, double now, const std::string& localPath, double localSeconds,
                  bool localPaused, bool fileExists);

    // True while the last sample is fresh (the panel is up and following).
    bool Active(double now) const { return lastSeen_ >= 0.0 && now - lastSeen_ <= staleAfter; }
    bool ResolvePlaying() const { return playing_; }
    void Reset() { *this = Follower(); }

private:
    long long lastSeq_ = -1;
    double lastSeen_ = -1.0;      // station clock when the last NEW sample arrived
    double lastClipSec_ = -1.0;   // its clipSeconds
    std::string lastClip_;
    bool playing_ = false;
};

// "C:/a/b.mov" vs "c:\a\B.mov": the same file to Windows. Plain compare elsewhere.
bool SamePath(const std::string& a, const std::string& b);

}  // namespace mp::pro::resolve
