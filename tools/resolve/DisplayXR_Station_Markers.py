# SPDX-License-Identifier: Apache-2.0
#
# Media Player Pro -> DaVinci Resolve markers (docs/PRO.md, Next 6: the Resolve panel, first step).
#
# The station watches Resolve's stereo output over SDI, so every snapshot note and
# every depth / alignment alert it records carries Resolve's own timecode (RP188 on the
# SDI signal). This script puts them on the current timeline as markers:
#
#   snapshot notes   (Pictures/DisplayXR/*_note.json)           -> Blue markers
#   rig alerts       (Pictures/DisplayXR/station_alerts.jsonl)  -> Red markers
#
# Install: copy this file to Resolve's Scripts folder, e.g.
#   Windows  %APPDATA%\Blackmagic Design\DaVinci Resolve\Support\Fusion\Scripts\Edit\
#   macOS    ~/Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Scripts/Edit/
# then run it from Workspace > Scripts > DisplayXR_Station_Markers. Running it again only
# adds what is new (each marker remembers its source in the marker's custom data).
# DISPLAYXR_STATION_DIR overrides the folder the station writes to.
#
# Needs DaVinci Resolve Studio (external scripting) or the free version's Workspace >
# Scripts menu; tested against the documented scripting API, not yet against a live
# Resolve session.

import glob
import json
import os
import sys


def get_resolve():
    try:
        return resolve  # noqa: F821 - predefined when run from Resolve's Scripts menu
    except NameError:
        pass
    try:
        return bmd.scriptapp("Resolve")  # noqa: F821
    except NameError:
        pass
    try:
        import DaVinciResolveScript as dvr
        return dvr.scriptapp("Resolve")
    except ImportError:
        return None


def station_dir():
    d = os.environ.get("DISPLAYXR_STATION_DIR")
    if d:
        return d
    home = os.path.expanduser("~")
    for pics in (os.path.join(home, "Pictures"), os.path.join(home, "OneDrive", "Pictures")):
        cand = os.path.join(pics, "DisplayXR")
        if os.path.isdir(cand):
            return cand
    return os.path.join(home, "Pictures", "DisplayXR")


def tc_to_frames(tc, fps):
    """'HH:MM:SS:FF' (or ;FF drop-frame, counted nominally) -> frames at the nominal rate."""
    if not tc:
        return None
    parts = tc.replace(";", ":").split(":")
    if len(parts) != 4:
        return None
    try:
        h, m, s, f = (int(p) for p in parts)
    except ValueError:
        return None
    nominal = int(round(fps))
    return ((h * 60 + m) * 60 + s) * nominal + f


def load_items(folder):
    items = []
    for path in sorted(glob.glob(os.path.join(folder, "*_note.json"))):
        try:
            with open(path, "r", encoding="utf-8") as f:
                j = json.load(f)
        except (OSError, ValueError):
            continue
        note = j.get("note") or "Station snapshot"
        detail = []
        if "vertical_px" in j:
            detail.append("vertical %.1f px" % j["vertical_px"])
        if "near_pct" in j and "far_pct" in j:
            detail.append("depth %+.1f%% .. %+.1f%%" % (j["near_pct"], j["far_pct"]))
        if j.get("look"):
            detail.append("look: %s" % j["look"])
        items.append({
            "tc": j.get("timecode", ""),
            "color": "Blue",
            "name": note[:60],
            "note": "; ".join(detail) + ("\n" + j.get("image", "") if j.get("image") else ""),
            "key": "dxr-note:" + os.path.basename(path),
        })
    alerts = os.path.join(folder, "station_alerts.jsonl")
    if os.path.isfile(alerts):
        with open(alerts, "r", encoding="utf-8") as f:
            for i, line in enumerate(f):
                try:
                    j = json.loads(line)
                except ValueError:
                    continue
                items.append({
                    "tc": j.get("timecode", ""),
                    "color": "Red",
                    "name": j.get("what", "Rig alert")[:60],
                    "note": "%s (%s)" % (j.get("what", ""), j.get("time", "")),
                    "key": "dxr-alert:%d:%s" % (i, j.get("timecode", "")),
                })
    return items


def main():
    r = get_resolve()
    if r is None:
        print("DisplayXR station: run this from DaVinci Resolve (Workspace > Scripts).")
        return 1
    project = r.GetProjectManager().GetCurrentProject()
    timeline = project.GetCurrentTimeline() if project else None
    if timeline is None:
        print("DisplayXR station: open a timeline first.")
        return 1
    fps = float(timeline.GetSetting("timelineFrameRate") or 24)
    start = tc_to_frames(timeline.GetStartTimecode(), fps) or 0
    length = timeline.GetEndFrame() - timeline.GetStartFrame()
    folder = station_dir()
    added = skipped = outside = untimed = 0
    for it in load_items(folder):
        frames = tc_to_frames(it["tc"], fps)
        if frames is None:
            untimed += 1  # a note taken with no timecode on the signal
            continue
        offset = frames - start
        if offset < 0 or offset > length:
            outside += 1
            continue
        if timeline.GetMarkerByCustomData(it["key"]):
            skipped += 1
            continue
        if timeline.AddMarker(offset, it["color"], it["name"], it["note"], 1, it["key"]):
            added += 1
    print("DisplayXR station (%s): %d marker(s) added, %d already there, %d outside this timeline, "
          "%d without timecode." % (folder, added, skipped, outside, untimed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
