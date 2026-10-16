# SPDX-License-Identifier: Apache-2.0
#
# tools/resolve/DisplayXR_Station_Panel.py against a fake Resolve (no Resolve needed):
# markers from station notes / alerts (timecode -> frame, de-duplication, outside the
# timeline, no timecode), SMPTE drop-frame, the playhead -> clip source time the station
# follows, the link and command files the station reads, and the status lines.
# Run: python tests/resolve_panel_test.py

import importlib.util
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "panel", os.path.join(HERE, "..", "tools", "resolve", "DisplayXR_Station_Panel.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class FakeMediaPoolItem:
    def __init__(self, path, fps):
        self.props = {"File Path": path, "FPS": fps}

    def GetClipProperty(self, k):
        return self.props.get(k, "")


class FakeItem:
    def __init__(self, start, src_start_s, path, fps=24):
        self.start, self.src_start_s, self.mpi = start, src_start_s, FakeMediaPoolItem(path, fps)

    def GetStart(self):
        return self.start

    def GetSourceStartTime(self):
        return self.src_start_s

    def GetLeftOffset(self):
        return int(self.src_start_s * 24)

    def GetMediaPoolItem(self):
        return self.mpi


class FakeTimeline:
    def __init__(self, start_tc, fps, length):
        self.start_tc, self.fps, self.length, self.markers = start_tc, fps, length, {}
        self.tc, self.item = start_tc, None

    def GetName(self):
        return "Reel 1"

    def GetSetting(self, k):
        return str(self.fps) if k == "timelineFrameRate" else ""

    def GetStartTimecode(self):
        return self.start_tc

    def GetStartFrame(self):
        return 86400

    def GetEndFrame(self):
        return 86400 + self.length

    def GetCurrentTimecode(self):
        return self.tc

    def SetCurrentTimecode(self, tc):
        self.tc = tc
        return True

    def GetCurrentVideoItem(self):
        return self.item

    def GetMarkerByCustomData(self, key):
        return {f: v for f, v in self.markers.items() if v["customData"] == key}

    def AddMarker(self, frame, color, name, note, duration, custom):
        if frame in self.markers:
            return False
        self.markers[frame] = {"color": color, "name": name, "note": note, "customData": custom}
        return True


class FakeResolve:
    def __init__(self, tl):
        self.tl = tl

    def GetProjectManager(self):
        return self

    def GetCurrentProject(self):
        return self

    def GetCurrentTimeline(self):
        return self.tl


failures = 0


def check(cond, msg):
    global failures
    if not cond:
        print("  FAIL:", msg)
        failures += 1


# --- timecode
check(m.tc_to_frames("01:00:00:00", 24) == 86400, "non-drop")
check(m.tc_to_frames("00:00:01;00", 29.97) == 30, "drop-frame: no drop inside the first minute")
check(m.tc_to_frames("00:01:00;02", 29.97) == 1800, "drop-frame: ;00 and ;01 skipped at minute 1")
check(m.tc_to_frames("00:10:00;00", 29.97) == 17982, "drop-frame: ten minutes = 17982 frames")
check(m.tc_to_frames("01:00:00;00", 59.94) == 215784, "59.94 drop: 4 skipped per minute")
for f in (0, 1799, 1800, 17981, 17982, 107891, 107892, 123456):
    check(m.tc_to_frames(m.frames_to_tc(f, 29.97, True), 29.97) == f, "drop-frame round trip %d" % f)
check(m.tc_to_frames("garbage", 24) is None and m.tc_to_frames("", 24) is None, "bad timecode")

with tempfile.TemporaryDirectory() as d:
    # --- markers (the old one-shot script's cases)
    def note(name, tc, text):
        with open(os.path.join(d, name), "w", encoding="utf-8") as f:
            json.dump({"note": text, "timecode": tc, "vertical_px": 0.3, "near_pct": -1.8, "far_pct": 0.9}, f)

    note("pro_20261015_143208_note.json", "01:00:10:12", "too close")
    note("pro_20261015_143551_note.json", "01:00:20:00", "check R")
    note("pro_20261015_150000_note.json", "02:00:00:00", "other reel")   # outside
    note("pro_20261015_150100_note.json", "", "no tc")                    # untimed
    with open(os.path.join(d, "station_alerts.jsonl"), "w", encoding="utf-8") as f:
        f.write(json.dumps({"what": "Foreground at -2.1% for 3 s", "timecode": "01:00:15:00", "time": "14:32:08"}) + "\n")
    os.environ["DISPLAYXR_STATION_DIR"] = d
    tl = FakeTimeline("01:00:00:00", 24, 24 * 60)
    m.resolve = FakeResolve(tl)  # what Resolve predefines
    check(m.main(["--once"]) == 0, "one-shot run")
    check(sorted(tl.markers) == [10 * 24 + 12, 15 * 24, 20 * 24], "markers at the notes' timecodes: %s" % sorted(tl.markers))
    check(tl.markers[15 * 24]["color"] == "Red" and tl.markers[20 * 24]["name"] == "check R", "colours + names")
    m.main(["--once"])
    check(len(tl.markers) == 3, "a second run adds nothing")

    # --- live: a note that arrives while the panel runs becomes a marker on the next sync
    link = m.Link(m.resolve, d)
    link.tick(now=100.0)
    note("pro_20261015_151000_note.json", "01:00:30:00", "late note")
    link.tick(now=100.5)
    check(30 * 24 not in tl.markers, "markers sync at most once a second")
    link.tick(now=101.1)
    check(30 * 24 in tl.markers and link.last_counts["added"] == 1, "a new note becomes a marker live")

    # --- playhead -> link file: clip 'a.mov' placed at timeline frame 86400+240 (01:00:10:00),
    # trimmed so its first frame on the timeline is 2.0 s into the file.
    tl.item = FakeItem(86400 + 240, 2.0, "D:/shots/a.mov")
    tl.tc = "01:00:13:00"  # 3 s into the clip
    link.tick(now=102.0)
    with open(os.path.join(d, "resolve_link.json"), encoding="utf-8") as f:
        j = json.load(f)
    check(j["playhead_tc"] == "01:00:13:00" and j["timeline"] == "Reel 1", "playhead written")
    check(j["clip"]["path"] == "D:/shots/a.mov", "clip under the playhead")
    check(abs(j["clip"]["seconds"] - 5.0) < 1e-9, "source time = 2.0 trim + 3.0 into the clip: %s" % j["clip"]["seconds"])
    check(j["follow"] is True and j["seq"] == link.seq, "follow + seq")
    seq = j["seq"]
    link.follow = False
    link.tick(now=102.2)
    with open(os.path.join(d, "resolve_link.json"), encoding="utf-8") as f:
        j = json.load(f)
    check(j["follow"] is False and j["seq"] == seq + 1, "follow off reaches the station, new seq")
    tl.item = None
    link.tick(now=102.4)
    with open(os.path.join(d, "resolve_link.json"), encoding="utf-8") as f:
        check("clip" not in json.load(f), "a gap: no clip")
    check(not [p for p in os.listdir(d) if p.endswith(".tmp")], "no temp files left behind")

    # --- commands
    link.command("snapshot", "from the edit bay")
    link.command("snapshot", "second")
    with open(os.path.join(d, "resolve_commands.jsonl"), encoding="utf-8") as f:
        lines = [json.loads(x) for x in f]
    check([x["note"] for x in lines] == ["from the edit bay", "second"], "commands appended in order")
    check(lines[1]["id"] > lines[0]["id"], "command ids increase")

    # --- station status -> panel
    status = {"input": "Rig A", "live": True, "timecode": "01:00:12:05",
              "rig": {"measured": True, "vertical_px": 0.3, "near_pct": -2.1, "far_pct": 0.9, "delta_e": 1.1,
                      "eye_sync_frames": 0, "alerts": [{"what": "Foreground at -2.1%", "open": True}]}}
    with open(os.path.join(d, "station_status.json"), "w", encoding="utf-8") as f:
        json.dump(status, f)
    head, rig = m.status_lines(m.read_status(d))
    check("Rig A" in head and "LIVE" in head and "01:00:12:05" in head, "status head: %s" % head)
    check("Vertical 0.3 px" in rig and "ALERT: Foreground" in rig, "status rig line: %s" % rig)
    check(m.status_lines(None)[0] == "Station: not running", "no station")
    check(link.goto_station(m.read_status(d)) and tl.tc == "01:00:12:05", "Go to station TC moves the playhead")

# --- review fixes
check(m.parse_fps("29.97 DF") == 29.97 and m.parse_fps("23.976") == 23.976, "drop-frame rate strings parse")
check(m.parse_fps(None, 25.0) == 25.0 and m.parse_fps("", 25.0) == 25.0, "missing rate falls back")
check(m.tc_on_timeline("10:00:00:26", 50, 25) == 36000 * 25 + 13, "50p station TC on a 25p timeline")
check(m.tc_on_timeline("10:00:00:12", 0, 25) == 36000 * 25 + 12, "no tc_fps: same rate")
with tempfile.TemporaryDirectory() as d2:
    tl2 = FakeTimeline("01:00:00:00", "29.97 DF", 30 * 600)
    tl2.GetSetting = lambda k: "29.97 DF" if k == "timelineFrameRate" else ""
    check(m.timeline_fps(tl2) == 29.97, "a DF timeline is not read as 24 fps")
    # Two notes on the SAME frame: Resolve keeps one; the other is refused once, not forever.
    for name in ("pro_a_note.json", "pro_b_note.json"):
        with open(os.path.join(d2, name), "w", encoding="utf-8") as f:
            json.dump({"note": name, "timecode": "01:00:01:00"}, f)
    calls = {"n": 0}
    orig = tl2.AddMarker

    def counting(*a):
        calls["n"] += 1
        return orig(*a)
    tl2.AddMarker = counting
    lk = m.Link(FakeResolve(tl2), d2)
    for k in range(5):
        lk.tick(now=200.0 + k * 1.1)
    check(len(tl2.markers) == 1 and calls["n"] == 2, "a clashing marker is tried once: %d calls" % calls["n"])
    check(lk.last_counts["skipped"] == 2, "settled keys are skipped without asking Resolve")
    # Go to a 50p station TC on the 29.97 DF timeline.
    ok = lk.goto_station({"timecode": "01:00:10:00", "tc_fps": 50})
    want = int(round(3610.0 * 29.97))  # 3610 s of 29.97 fps frames
    check(ok and ";" in tl2.tc and m.tc_to_frames(tl2.tc, 29.97) == want,
          "station TC converted to the timeline's rate: %s" % tl2.tc)

print("resolve_panel_test:", "all passed" if not failures else "%d failure(s)" % failures)
sys.exit(1 if failures else 0)
