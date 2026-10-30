// SPDX-License-Identifier: Apache-2.0
#include "pro/QcReport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mp::pro::qc {

namespace {

int DropPerMinute(int fps) { return fps >= 60 ? 4 : 2; }  // frame numbers skipped per minute (DF)

std::string Base64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
    }
    if (i < in.size()) {
        const bool two = i + 1 < in.size();
        const unsigned v = ((unsigned char)in[i] << 16) | (two ? ((unsigned char)in[i + 1] << 8) : 0);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += two ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string Html(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string Csv(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + "\"";
}

// EDL marker text: one line, no '|' (the field separator).
std::string EdlText(std::string s) {
    for (char& c : s)
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string Num(float v, int decimals) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", decimals, v);
    return b;
}

// An open alert says so once (the rig's own text for one still running ends "(now)").
std::string AlertText(const Alert& a) {
    return a.open && a.what.find("(now)") == std::string::npos ? a.what + " (still open)" : a.what;
}

int RateOf(int rowFps, const Session& s) { return rowFps > 0 ? rowFps : (s.tcFps > 0 ? s.tcFps : 25); }

// One marker: its record in and duration in frames. False when it has no usable timecode.
struct Marker {
    long long in = 0, frames = 1;
    int fps = 25;
    bool drop = false;
    std::string colour, name, note;
};

bool AlertMarker(const Alert& a, const Session& s, Marker& m) {
    m.fps = RateOf(a.tcFps, s);
    if (!TimecodeToFrames(a.timecode, m.fps, m.in, &m.drop)) return false;
    long long out = 0;
    if (!a.endTimecode.empty() && TimecodeToFrames(a.endTimecode, m.fps, out) && out > m.in)
        m.frames = out - m.in;
    else
        m.frames = std::max(1LL, (long long)std::llround(a.durationS * m.fps));
    m.colour = "ResolveColorRed";
    m.name = AlertText(a);
    return true;
}

bool SnapshotMarker(const Snapshot& sn, const Session& s, Marker& m) {
    m.fps = RateOf(sn.tcFps, s);
    if (!TimecodeToFrames(sn.timecode, m.fps, m.in, &m.drop)) return false;
    m.frames = 1;
    m.colour = "ResolveColorBlue";
    m.name = sn.note.empty() ? "Snapshot " + sn.clock : sn.note;
    return true;
}

std::vector<Marker> Markers(const Session& s) {
    std::vector<Marker> v;
    Marker m;
    for (const Alert& a : s.alerts)
        if (AlertMarker(a, s, m)) v.push_back(m);
    for (const Snapshot& sn : s.snapshots)
        if (SnapshotMarker(sn, s, m)) v.push_back(m);
    std::stable_sort(v.begin(), v.end(), [](const Marker& a, const Marker& b) { return a.in < b.in; });
    return v;
}

}  // namespace

bool TimecodeToFrames(const std::string& tc, int fps, long long& frames, bool* dropFrame) {
    if (fps <= 0) return false;
    int h = 0, m = 0, s = 0, f = 0;
    char sep = 0;
    if (std::sscanf(tc.c_str(), "%d:%d:%d%c%d", &h, &m, &s, &sep, &f) != 5) return false;
    if (sep != ':' && sep != ';' && sep != '.' && sep != ',') return false;
    if (h < 0 || m < 0 || m > 59 || s < 0 || s > 59 || f < 0 || f >= fps) return false;
    const bool drop = (sep == ';' || sep == '.' || sep == ',') && (fps == 30 || fps == 60);
    if (dropFrame) *dropFrame = drop;
    frames = ((long long)h * 3600 + m * 60 + s) * fps + f;
    if (drop) {
        const long long minutes = (long long)h * 60 + m;
        frames -= DropPerMinute(fps) * (minutes - minutes / 10);
    }
    return true;
}

std::string FramesToTimecode(long long frames, int fps, bool dropFrame) {
    if (fps <= 0 || frames < 0) return "";
    const bool drop = dropFrame && (fps == 30 || fps == 60);
    if (drop) {  // SMPTE 12M: put the skipped frame numbers back
        const long long d = DropPerMinute(fps);
        const long long per10 = (long long)fps * 600 - d * 9, perMin = (long long)fps * 60 - d;
        const long long tens = frames / per10, rem = frames % per10;
        frames += d * 9 * tens + (rem > d ? d * ((rem - d) / perMin) : 0);
    }
    const long long f = frames % fps, sTotal = frames / fps;
    char b[24];
    std::snprintf(b, sizeof(b), "%02lld:%02lld:%02lld%c%02lld", (sTotal / 3600) % 24, (sTotal / 60) % 60,
                  sTotal % 60, drop ? ';' : ':', f);
    return b;
}

int RowsWithoutTimecode(const Session& s) {
    int n = 0;
    Marker m;
    for (const Alert& a : s.alerts) n += AlertMarker(a, s, m) ? 0 : 1;
    for (const Snapshot& sn : s.snapshots) n += SnapshotMarker(sn, s, m) ? 0 : 1;
    return n;
}

std::string MakeEdl(const Session& s) {
    const std::vector<Marker> ms = Markers(s);
    const bool drop = !ms.empty() && ms.front().drop;
    std::string o = "TITLE: " + EdlText(s.title.empty() ? "QC report" : s.title) + "\n";
    o += drop ? "FCM: DROP FRAME\n\n" : "FCM: NON-DROP FRAME\n\n";
    int n = 0;
    for (const Marker& m : ms) {
        const std::string in = FramesToTimecode(m.in, m.fps, m.drop), out = FramesToTimecode(m.in + 1, m.fps, m.drop);
        char head[128];
        std::snprintf(head, sizeof(head), "%03d  001      V     C        %s %s %s %s  \n", ++n, in.c_str(), out.c_str(),
                      in.c_str(), out.c_str());
        o += head;
        o += " |C:" + m.colour + " |M:" + EdlText(m.name) + " |D:" + std::to_string(m.frames) + "\n\n";
    }
    return o;
}

std::string MakeCsv(const Session& s) {
    std::string o =
        "kind,timecode,end_timecode,duration_frames,tc_fps,clock,description,note,vertical_px,roll_deg,"
        "near_pct,far_pct,delta_e,edge_left_pct,edge_right_pct,eye_sync_frames,image\n";
    Marker m;
    for (const Alert& a : s.alerts) {
        const bool ok = AlertMarker(a, s, m);
        o += "alert," + Csv(a.timecode) + "," + Csv(a.endTimecode) + "," + (ok ? std::to_string(m.frames) : "") + "," +
             (a.tcFps > 0 ? std::to_string(a.tcFps) : "") + "," + Csv(a.clock) + "," +
             Csv(AlertText(a)) + ",,,,,,,,,,\n";
    }
    for (const Snapshot& sn : s.snapshots) {
        const Rig& r = sn.rig;
        auto rig = [&](float v, int d) { return r.measured ? Num(v, d) : std::string(); };
        o += "snapshot," + Csv(sn.timecode) + ",,1," + (sn.tcFps > 0 ? std::to_string(sn.tcFps) : "") + "," +
             Csv(sn.clock) + ",Snapshot," + Csv(sn.note) + "," + rig(r.verticalPx, 2) + "," + rig(r.rollDeg, 2) + "," +
             rig(r.nearPct, 2) + "," + rig(r.farPct, 2) + "," + rig(r.deltaE, 2) + "," +
             (r.edgeMeasured[0] ? Num(r.edgePct[0], 2) : "") + "," + (r.edgeMeasured[1] ? Num(r.edgePct[1], 2) : "") +
             "," + (sn.eyeSyncFrames >= 0 ? std::to_string(sn.eyeSyncFrames) : "") + "," + Csv(sn.image) + "\n";
    }
    return o;
}

std::string MakeHtml(const Session& s) {
    auto over = [](bool bad, const std::string& text) {
        return bad ? "<span class=bad>" + text + "</span>" : text;
    };
    std::string o;
    o += "<!doctype html>\n<html lang=en><head><meta charset=utf-8>"
         "<meta name=viewport content=\"width=device-width,initial-scale=1\">\n<title>" +
         Html(s.title) + "</title>\n<style>\n"
         ":root{--bg:#fff;--fg:#1b1b1f;--mute:#5c5f66;--line:#d9dbe0;--card:#f6f7f9;--bad:#c62828;--ok:#2e7d32}\n"
         "@media (prefers-color-scheme:dark){:root{--bg:#141518;--fg:#e8e9ec;--mute:#9a9da5;--line:#30333a;"
         "--card:#1c1e22;--bad:#ef6b6b;--ok:#6bc47a}}\n"
         "body{background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif;margin:0 auto;"
         "max-width:1100px;padding:24px 16px}\n"
         "h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:28px 0 8px}.mute{color:var(--mute)}\n"
         "table{border-collapse:collapse;width:100%;font-size:14px}th,td{text-align:left;padding:6px 8px;"
         "border-bottom:1px solid var(--line);vertical-align:top}th{color:var(--mute);font-weight:600}\n"
         ".tc{font-family:ui-monospace,Consolas,monospace;white-space:nowrap}.bad{color:var(--bad);font-weight:600}\n"
         ".snap{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;margin:12px 0}\n"
         ".snap img{width:100%;height:auto;border-radius:4px;display:block}.kv{display:flex;flex-wrap:wrap;gap:4px 18px;"
         "margin-top:8px;font-size:14px}\n.wrap{overflow-x:auto}\n</style></head><body>\n";
    o += "<h1>" + Html(s.title) + "</h1>\n<p class=mute>Input: " + Html(s.input.empty() ? "-" : s.input) +
         " &middot; session from " + Html(s.started) + " &middot; exported " + Html(s.exported) +
         (s.tcFps > 0 ? " &middot; " + std::to_string(s.tcFps) + " fps" : "") + "</p>\n";
    o += "<p>" + std::to_string(s.alerts.size()) + " alert" + (s.alerts.size() == 1 ? "" : "s") + ", " +
         std::to_string(s.snapshots.size()) + " snapshot" + (s.snapshots.size() == 1 ? "" : "s") +
         ". Depth budget " + Num(s.budgetNearPct, 1) + "% to " + Num(s.budgetFarPct, 1) +
         "% of the eye width; vertical &le; " + Num(s.budgetVerticalPx, 1) + " px; colour &Delta;E &le; " +
         Num(s.budgetDeltaE, 1) + "; nothing nearer than " + Num(s.budgetEdgePct, 1) + "% at the frame edges.</p>\n";
    const int untimed = RowsWithoutTimecode(s);
    if (untimed)
        o += "<p class=mute>" + std::to_string(untimed) +
             " row(s) have no source timecode and are not in markers.edl.</p>\n";

    o += "<h2>Alerts</h2>\n";
    if (s.alerts.empty()) {
        o += "<p class=mute>No rig check went over budget in this session.</p>\n";
    } else {
        o += "<div class=wrap><table><tr><th>Timecode</th><th>To</th><th>Clock</th><th>Duration</th><th>What</th></tr>\n";
        for (const Alert& a : s.alerts)
            o += "<tr><td class=tc>" + Html(a.timecode.empty() ? "-" : a.timecode) + "</td><td class=tc>" +
                 Html(a.endTimecode.empty() ? (a.open ? "open" : "-") : a.endTimecode) + "</td><td class=tc>" +
                 Html(a.clock) + "</td><td>" + Num((float)a.durationS, 1) + " s</td><td class=bad>" + Html(AlertText(a)) +
                 "</td></tr>\n";
        o += "</table></div>\n";
    }

    o += "<h2>Snapshots</h2>\n";
    if (s.snapshots.empty()) o += "<p class=mute>No snapshots in this session.</p>\n";
    for (const Snapshot& sn : s.snapshots) {
        o += "<div class=snap>";
        if (!sn.thumbJpeg.empty())
            o += "<img alt=\"both eyes at " + Html(sn.timecode) + "\" src=\"data:image/jpeg;base64," +
                 Base64(sn.thumbJpeg) + "\">";
        o += "<div class=kv><span class=tc>TC " + Html(sn.timecode.empty() ? "-" : sn.timecode) +
             "</span><span class=tc>" + Html(sn.clock) + "</span>";
        if (!sn.note.empty()) o += "<span><b>" + Html(sn.note) + "</b></span>";
        o += "</div>";
        const Rig& r = sn.rig;
        if (r.measured) {
            o += "<div class=kv>";
            o += "<span>Vertical " + over(std::fabs(r.verticalPx) > s.budgetVerticalPx, Num(r.verticalPx, 2) + " px") +
                 "</span>";
            o += "<span>Roll " + Num(r.rollDeg, 2) + "&deg;</span>";
            o += "<span>Depth " + over(r.nearPct < s.budgetNearPct, Num(r.nearPct, 2) + "%") + " to " +
                 over(r.farPct > s.budgetFarPct, Num(r.farPct, 2) + "%") + "</span>";
            o += "<span>Colour &Delta;E " + over(r.deltaE > s.budgetDeltaE, Num(r.deltaE, 2)) + "</span>";
            for (int e = 0; e < 2; ++e)
                if (r.edgeMeasured[e])
                    o += std::string("<span>") + (e == 0 ? "Left" : "Right") + " edge " +
                         over(r.edgePct[e] < s.budgetEdgePct, Num(r.edgePct[e], 2) + "%") + "</span>";
            if (sn.eyeSyncFrames >= 0)
                o += "<span>Eye sync " + over(sn.eyeSyncFrames != 0, std::to_string(sn.eyeSyncFrames) + " frames") +
                     "</span>";
            o += "</div>";
        } else {
            o += "<div class=kv mute>No rig measurement at this snapshot.</div>";
        }
        o += "<div class=\"kv mute\">" + Html(sn.image) + "</div></div>\n";
    }
    o += "<p class=mute>Media Player Pro QC report. Marker lists beside this file: markers.edl (DaVinci Resolve: "
         "Timeline &gt; Import &gt; Timeline Markers from EDL) and markers.csv.</p>\n</body></html>\n";
    return o;
}

}  // namespace mp::pro::qc
