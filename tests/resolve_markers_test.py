# SPDX-License-Identifier: Apache-2.0
#
# tools/resolve/DisplayXR_Station_Markers.py against a fake Resolve timeline (no Resolve
# needed): timecode -> marker frame, de-duplication on a second run, notes outside the
# timeline and notes without timecode. Run: python tests/resolve_markers_test.py

import importlib.util
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "markers", os.path.join(HERE, "..", "tools", "resolve", "DisplayXR_Station_Markers.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class FakeTimeline:
    def __init__(self, start_tc, fps, length):
        self.start_tc, self.fps, self.length, self.markers = start_tc, fps, length, {}

    def GetSetting(self, k):
        return str(self.fps) if k == "timelineFrameRate" else ""

    def GetStartTimecode(self):
        return self.start_tc

    def GetStartFrame(self):
        return 86400

    def GetEndFrame(self):
        return 86400 + self.length

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


with tempfile.TemporaryDirectory() as d:
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
    m.resolve = FakeResolve(tl)  # what Resolve predefines for menu scripts
    check(m.main() == 0, "runs")
    check(sorted(tl.markers) == [10 * 24 + 12, 15 * 24, 20 * 24], "markers at the notes' timecodes: %s" % sorted(tl.markers))
    check(tl.markers[15 * 24]["color"] == "Red" and tl.markers[20 * 24]["name"] == "check R", "colours + names")
    m.main()
    check(len(tl.markers) == 3, "a second run adds nothing")
    check(m.tc_to_frames("00:00:01;00", 29.97) == 30, "drop-frame counted nominally")

print("resolve_markers_test:", "all passed" if not failures else "%d failure(s)" % failures)
sys.exit(1 if failures else 0)
