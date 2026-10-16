# SPDX-License-Identifier: Apache-2.0
#
# DisplayXR Station panel for DaVinci Resolve (docs/PRO.md: the Resolve panel).
#
# A small window inside Resolve that keeps Resolve and Media Player Pro in step:
#
#   markers      station snapshot notes -> Blue markers, rig alerts (depth, alignment,
#                colour) -> Red markers, on the current timeline at their timecodes, as
#                they happen. Each marker remembers its source in its custom data, so
#                nothing is added twice.
#   follow       Resolve's playhead (and the clip under it) goes to the station; when the
#                station can reach the same media file it opens it and follows: plays when
#                Resolve plays, lands on the frame when Resolve parks.
#   station      the station's input, timecode, rig numbers and open alert, live; "Go to
#                station TC" moves Resolve's playhead to the station's timecode; "Snapshot"
#                takes a snapshot at the station with a note typed here.
#
# The two sides talk through small files in the station folder (Pictures/DisplayXR,
# DISPLAYXR_STATION_DIR overrides): station_status.json (station -> here),
# resolve_link.json and resolve_commands.jsonl (here -> station). See src/pro/ResolveLink.h.
#
# Install (Resolve Studio): copy this file to
#   Windows  %PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Workflow Integration Plugins\
#   macOS    /Library/Application Support/Blackmagic Design/DaVinci Resolve/Workflow Integration Plugins/
# and open it from Workspace > Workflow Integrations > DisplayXR_Station_Panel. It also runs
# from Workspace > Scripts (copy it to ...\Fusion\Scripts\Edit\), the free version's route.
# Without a UI (run outside Resolve's UI, or with --once) it adds the markers once and exits.

import glob
import json
import os
import sys
import time

WIN_ID = "com.displayxr.station.panel"
POLL_MS = 200


# ----------------------------------------------------------------------------- Resolve / paths

def get_resolve():
    for name in ("resolve",):
        r = globals().get(name)
        if r is not None:
            return r  # predefined for Workflow Integration and Scripts-menu scripts
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


# ----------------------------------------------------------------------------- timecode

def _is_drop(tc, fps):
    return ";" in tc and abs(fps - round(fps)) > 0.001 and round(fps) in (30, 60)


def tc_to_frames(tc, fps):
    """'HH:MM:SS:FF' -> frame count. Drop-frame ('HH:MM:SS;FF' at 29.97 / 59.94) is counted
    the SMPTE way (2 or 4 frame numbers skipped every minute except each tenth), so a frame
    count difference is the real number of frames between two timecodes."""
    if not tc:
        return None
    parts = tc.replace(";", ":").replace(".", ":").split(":")
    if len(parts) != 4:
        return None
    try:
        h, m, s, f = (int(p) for p in parts)
    except ValueError:
        return None
    nominal = int(round(fps)) or 24
    frames = ((h * 60 + m) * 60 + s) * nominal + f
    if _is_drop(tc, fps):
        drop = 2 * nominal // 30
        total_minutes = h * 60 + m
        frames -= drop * (total_minutes - total_minutes // 10)
    return frames


def frames_to_tc(frames, fps, drop=False):
    nominal = int(round(fps)) or 24
    if drop and round(fps) in (30, 60):
        d = 2 * nominal // 30
        per10 = nominal * 600 - d * 9
        per1 = nominal * 60 - d
        tens, rem = divmod(frames, per10)
        if rem > d:
            frames += d * 9 * tens + d * ((rem - d) // per1)
        else:
            frames += d * 9 * tens
    f = frames % nominal
    s = frames // nominal % 60
    m = frames // (nominal * 60) % 60
    h = frames // (nominal * 3600) % 24
    return "%02d:%02d:%02d%s%02d" % (h, m, s, ";" if drop else ":", f)


# ----------------------------------------------------------------------------- markers

def load_items(folder):
    """Station notes and rig alerts as marker items: tc, color, name, note, key."""
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
            "kind": "note",
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
                    "kind": "alert",
                })
    return items


def timeline_fps(timeline):
    try:
        return float(timeline.GetSetting("timelineFrameRate") or 24)
    except (TypeError, ValueError):
        return 24.0


def sync_markers(timeline, folder):
    """Add the station's notes and alerts that are not on `timeline` yet. Returns counts."""
    fps = timeline_fps(timeline)
    start = tc_to_frames(timeline.GetStartTimecode(), fps) or 0
    length = timeline.GetEndFrame() - timeline.GetStartFrame()
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
    return {"added": added, "skipped": skipped, "outside": outside, "untimed": untimed}


# ----------------------------------------------------------------------------- link files

def write_atomically(path, text):
    """Temp file + rename, so the station never reads half a file. False when the station
    held the file open at that moment (Windows); the next tick writes again."""
    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)
        return True
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False


def read_status(folder):
    try:
        with open(os.path.join(folder, "station_status.json"), "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def playhead(timeline):
    """Resolve's playhead: record timecode, and the clip under it with its source time.
    Source time = where the clip starts in its media + how far the playhead is into the
    clip (speed changes are not followed)."""
    fps = timeline_fps(timeline)
    tc = timeline.GetCurrentTimecode() or ""
    out = {"timeline": timeline.GetName(), "playhead_tc": tc, "timeline_fps": fps}
    try:
        item = timeline.GetCurrentVideoItem()
    except Exception:  # noqa: BLE001 - Resolve raises plain errors off the Edit page
        item = None
    if not item or not tc:
        return out
    try:
        mpi = item.GetMediaPoolItem()
        path = mpi.GetClipProperty("File Path") if mpi else ""
    except Exception:  # noqa: BLE001
        path = ""
    if not path:
        return out  # a title, a generator, a compound clip
    cur = tc_to_frames(tc, fps)
    start_tc = tc_to_frames(timeline.GetStartTimecode(), fps) or 0
    if cur is None:
        return out
    cur_frame = cur - start_tc + timeline.GetStartFrame()  # timeline frame numbers, as GetStart()
    into = (cur_frame - item.GetStart()) / fps
    src_start = None
    if hasattr(item, "GetSourceStartTime"):
        try:
            src_start = float(item.GetSourceStartTime())
        except (TypeError, ValueError):
            src_start = None
    if src_start is None:
        src_start = float(item.GetLeftOffset()) / fps
    try:
        clip_fps = float(mpi.GetClipProperty("FPS") or fps)
    except (TypeError, ValueError):
        clip_fps = fps
    out["clip"] = {"path": path, "seconds": max(0.0, src_start + into), "fps": clip_fps}
    return out


class Link:
    """The panel's state between ticks: writes the link file, appends commands, adds
    markers as they appear. UI-free, so tests drive it with a fake Resolve."""

    def __init__(self, resolve, folder):
        self.resolve = resolve
        self.folder = folder
        self.seq = 0
        self.cmd_id = int(time.time() * 1000)  # unique across panel restarts
        self.follow = True
        self.auto_markers = True
        self.next_marker_sync = 0.0
        self.last_counts = None

    def timeline(self):
        try:
            project = self.resolve.GetProjectManager().GetCurrentProject()
            return project.GetCurrentTimeline() if project else None
        except Exception:  # noqa: BLE001
            return None

    def tick(self, now=None):
        now = time.time() if now is None else now
        tl = self.timeline()
        os.makedirs(self.folder, exist_ok=True)
        if tl is not None:
            ph = playhead(tl)
            self.seq += 1
            ph["seq"] = self.seq
            ph["follow"] = self.follow
            ph["written"] = now
            write_atomically(os.path.join(self.folder, "resolve_link.json"), json.dumps(ph))
            if self.auto_markers and now >= self.next_marker_sync:
                self.next_marker_sync = now + 1.0
                self.last_counts = sync_markers(tl, self.folder)
        return tl

    def command(self, cmd, note=""):
        self.cmd_id += 1
        line = json.dumps({"cmd": cmd, "note": note, "id": self.cmd_id})
        with open(os.path.join(self.folder, "resolve_commands.jsonl"), "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def goto_station(self, status):
        tl = self.timeline()
        tc = (status or {}).get("timecode") or ""
        if tl is None or not tc:
            return False
        return bool(tl.SetCurrentTimecode(tc))


def status_lines(status, now=None):
    """The station's state as two short lines for the panel."""
    now = time.time() if now is None else now
    if not status:
        return "Station: not running", ""
    src = status.get("input") or status.get("source") or "-"
    tc = status.get("timecode") or "--:--:--:--"
    head = "Station: %s  %s  TC %s" % (src, "LIVE" if status.get("live") else "file", tc)
    rig = status.get("rig") or {}
    if not rig.get("measured"):
        return head, "Rig check: measuring..."
    line = "Vertical %.1f px   Depth %+.1f%% .. %+.1f%%   dE %.1f" % (
        rig.get("vertical_px", 0.0), rig.get("near_pct", 0.0), rig.get("far_pct", 0.0), rig.get("delta_e", 0.0))
    if rig.get("eye_sync_frames", -1) >= 0:
        line += "   Sync %d" % rig["eye_sync_frames"]
    open_alerts = [a for a in rig.get("alerts", []) if a.get("open")]
    if open_alerts:
        line += "\nALERT: " + open_alerts[-1].get("what", "")
    return head, line


# ----------------------------------------------------------------------------- UI

def run_ui(resolve, folder):
    ui = fusion.UIManager  # noqa: F821
    disp = bmd.UIDispatcher(ui)  # noqa: F821
    existing = ui.FindWindow(WIN_ID)
    if existing:
        existing.Show()
        existing.Raise()
        return 0
    link = Link(resolve, folder)
    win = disp.AddWindow(
        {"ID": WIN_ID, "WindowTitle": "DisplayXR Station", "Geometry": [120, 120, 460, 420]},
        ui.VGroup([
            ui.Label({"ID": "Head", "Text": "Station: ...", "Weight": 0}),
            ui.Label({"ID": "Rig", "Text": "", "Weight": 0, "WordWrap": True}),
            ui.Label({"ID": "Resolve", "Text": "", "Weight": 0}),
            ui.HGroup({"Weight": 0}, [
                ui.CheckBox({"ID": "Follow", "Text": "Station follows Resolve", "Checked": True}),
                ui.CheckBox({"ID": "Auto", "Text": "Add markers as they arrive", "Checked": True}),
            ]),
            ui.HGroup({"Weight": 0}, [
                ui.LineEdit({"ID": "Note", "PlaceholderText": "Note for a station snapshot"}),
                ui.Button({"ID": "Snap", "Text": "Snapshot"}),
            ]),
            ui.HGroup({"Weight": 0}, [
                ui.Button({"ID": "GoTo", "Text": "Go to station TC"}),
                ui.Button({"ID": "Sync", "Text": "Add markers now"}),
            ]),
            ui.Tree({"ID": "List", "ColumnCount": 3, "HeaderHidden": False, "RootIsDecorated": False}),
            ui.Label({"ID": "Foot", "Text": folder, "Weight": 0}),
        ]))
    it = win.GetItems()
    it["List"].SetHeaderLabels(["TC", "", "What"])
    it["List"].ColumnWidth[0] = 90
    it["List"].ColumnWidth[1] = 40
    state = {"status": None, "listed": None}

    def refresh_list():
        items = load_items(folder)
        sig = [i["key"] for i in items]
        if sig == state["listed"]:
            return
        state["listed"] = sig
        it["List"].Clear()
        for i in reversed(items[-50:]):
            row = it["List"].NewItem()
            row.Text[0] = i["tc"] or "--"
            row.Text[1] = "note" if i["kind"] == "note" else "alert"
            row.Text[2] = i["name"]
            it["List"].AddTopLevelItem(row)

    def on_tick(ev):
        tl = link.tick()
        state["status"] = read_status(folder)
        head, rig = status_lines(state["status"])
        it["Head"].Text = head
        it["Rig"].Text = rig
        if tl is None:
            it["Resolve"].Text = "Resolve: open a timeline on the Edit or Color page"
        else:
            c = link.last_counts or {}
            it["Resolve"].Text = "Resolve: %s  TC %s   markers +%d" % (
                tl.GetName(), tl.GetCurrentTimecode() or "-", c.get("added", 0))
        refresh_list()

    def on_follow(ev):
        link.follow = bool(it["Follow"].Checked)

    def on_auto(ev):
        link.auto_markers = bool(it["Auto"].Checked)

    def on_snap(ev):
        link.command("snapshot", it["Note"].Text or "")
        it["Note"].Text = ""

    def on_goto(ev):
        link.goto_station(state["status"])

    def on_sync(ev):
        tl = link.timeline()
        if tl is not None:
            link.last_counts = sync_markers(tl, folder)

    def on_item(ev):
        tc = ev["item"].Text[0]
        tl = link.timeline()
        if tl is not None and tc and tc != "--":
            tl.SetCurrentTimecode(tc)

    def on_close(ev):
        disp.ExitLoop()

    win.On[WIN_ID].Close = on_close
    win.On.Follow.Clicked = on_follow
    win.On.Auto.Clicked = on_auto
    win.On.Snap.Clicked = on_snap
    win.On.Note.ReturnPressed = on_snap
    win.On.GoTo.Clicked = on_goto
    win.On.Sync.Clicked = on_sync
    win.On.List.ItemDoubleClicked = on_item

    timer = ui.Timer({"ID": "DXRTick", "Interval": POLL_MS})
    disp.On.DXRTick.Timeout = on_tick
    timer.Start()
    on_tick(None)
    win.Show()
    disp.RunLoop()
    timer.Stop()
    win.Hide()
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    r = get_resolve()
    if r is None:
        print("DisplayXR station: run this from DaVinci Resolve (Workspace > Workflow Integrations "
              "or Workspace > Scripts).")
        return 1
    folder = station_dir()
    has_ui = "fusion" in globals() and "bmd" in globals()
    if has_ui and "--once" not in argv:
        return run_ui(r, folder)
    link = Link(r, folder)
    tl = link.timeline()
    if tl is None:
        print("DisplayXR station: open a timeline first.")
        return 1
    c = sync_markers(tl, folder)
    print("DisplayXR station (%s): %d marker(s) added, %d already there, %d outside this timeline, "
          "%d without timecode." % (folder, c["added"], c["skipped"], c["outside"], c["untimed"]))
    return 0


# Resolve runs this file with `resolve` (and, with a UI, `fusion` / `bmd`) predefined; it does
# not promise __name__ == "__main__". Tests load it as a module with neither, so nothing runs.
if __name__ == "__main__" or "resolve" in globals():
    _rc = main()
    if __name__ == "__main__":
        sys.exit(_rc)
