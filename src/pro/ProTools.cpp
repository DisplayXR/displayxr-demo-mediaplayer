// SPDX-License-Identifier: Apache-2.0
//
// Media Player Pro agent tools (XR_DXR_mcp_tools; the plan's "MCP tools for automation"):
// the grade, the view, and the monitoring station. Registered and dispatched from the
// trunk's agent-tool code through its two #if MEDIAPLAYER_PRO hooks. An agent can send
// any JSON: a wrong-typed argument comes back as an error, never takes the app down.
#include "pro/ProController.h"

#include "Log.h"
#include "media/LiveProducer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace mp::pro {

using json = nlohmann::json;

void ProController::RegisterTools(const RegisterFn& reg) {
    reg("set_grade",
        "Set Media Player Pro's grade: a .cube 3D LUT path, an ASC CDL file (.cc/.ccc/.cdl), "
        "exposure in stops (-4..4, linear light) and bypass. Arguments left out keep their "
        "current value. Returns the grade.",
        "{\"type\":\"object\",\"properties\":{\"lut\":{\"type\":\"string\"},\"cdl\":{\"type\":\"string\"},"
        "\"exposure\":{\"type\":\"number\"},\"bypass\":{\"type\":\"boolean\"}}}");
    reg("get_grade", "Read the grade: LUT, CDL (slope/offset/power/saturation), exposure, bypass.",
        "{\"type\":\"object\"}");
    reg("set_view",
        "Set the QC view: 3d, left, right, difference (|left - right| x4), anaglyph or false_colour; "
        "optional zoom (1 = fit) applied to both eyes; channel isolation (rgb, red, green, blue, "
        "alpha, luma); display gamma (0.25..4, 1 = none); a wipe (off, grade = source | graded, "
        "eyes = left eye | right eye, clips = this clip | the set_compare B clip) at wipe_pos (0..1 across "
        "the picture).",
        "{\"type\":\"object\",\"properties\":{\"view\":{\"type\":\"string\",\"enum\":[\"3d\",\"left\",\"right\","
        "\"difference\",\"anaglyph\",\"false_colour\"]},\"zoom\":{\"type\":\"number\",\"minimum\":1},"
        "\"channel\":{\"type\":\"string\",\"enum\":[\"rgb\",\"red\",\"green\",\"blue\",\"alpha\",\"luma\"]},"
        "\"gamma\":{\"type\":\"number\",\"minimum\":0.25,\"maximum\":4},"
        "\"wipe\":{\"type\":\"string\",\"enum\":[\"off\",\"grade\",\"eyes\",\"clips\"]},"
        "\"wipe_pos\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1}}}");
    reg("get_rig_status",
        "Read the live rig checks for a side-by-side stereo source: vertical error (full-res px, + = "
        "right eye lower), roll (degrees), near / far screen parallax (% of eye width, negative = in "
        "front of the screen), colour match between the eyes (CIE76 dE), eye sync (frames between "
        "the paired inputs, -1 = unknown), the depth budget, recent alerts with timecode, and the "
        "source timecode and capture-to-screen delay.",
        "{\"type\":\"object\"}");
    reg("set_depth_budget",
        "Set the limits the rig checks alert on: near / far parallax in % of eye width (near "
        "negative = in front), vertical error in px, colour match in dE.",
        "{\"type\":\"object\",\"properties\":{\"near_pct\":{\"type\":\"number\"},\"far_pct\":{\"type\":\"number\"},"
        "\"vertical_px\":{\"type\":\"number\"},\"delta_e\":{\"type\":\"number\"}}}");
    reg("set_depth_overlay", "Show or hide the depth overlay (the rig check's matches on the picture).",
        "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}},\"required\":[\"on\"]}");
    reg("snapshot",
        "Capture both eyes as shown (graded) to a PNG in Pictures/DisplayXR, with a JSON note "
        "beside it: the note text, timecode and rig numbers.",
        "{\"type\":\"object\",\"properties\":{\"note\":{\"type\":\"string\"}}}");
    reg("list_inputs", "List the station's configured live inputs (name, spec) and which one is live.",
        "{\"type\":\"object\"}");
    reg("select_input", "Switch to a configured input, by index (from list_inputs) or name.",
        "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"},\"name\":{\"type\":\"string\"}}}");
    reg("set_ocio",
        "Bake an OpenColorIO transform into the grade's LUT: config (a path, an ocio:// built-in, or "
        "empty for $OCIO), src = the picture's colour space, then display + view (default: the "
        "config's) or dst = a target colour space; optional looks. Needs a build with OpenColorIO.",
        "{\"type\":\"object\",\"properties\":{\"config\":{\"type\":\"string\"},\"src\":{\"type\":\"string\"},"
        "\"display\":{\"type\":\"string\"},\"view\":{\"type\":\"string\"},\"dst\":{\"type\":\"string\"},"
        "\"looks\":{\"type\":\"string\"}},\"required\":[\"src\"]}");
    reg("describe_ocio", "List an OpenColorIO config's colour spaces, displays and the default display's views.",
        "{\"type\":\"object\",\"properties\":{\"config\":{\"type\":\"string\"}}}");
    reg("get_resolve_link",
        "Read the live Resolve panel link: whether the panel is running, Resolve's timeline, playhead "
        "timecode and the clip under it, whether the station follows it, and whether Resolve is "
        "playing (inferred from the playhead).",
        "{\"type\":\"object\"}");
    reg("set_resolve_follow", "Make the station follow (or stop following) Resolve's playhead.",
        "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}},\"required\":[\"on\"]}");
    reg("set_compare",
        "A/B compare: set the B clip (a file path); toggle=true (or no path) swaps to the other clip "
        "at the current position.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"toggle\":{\"type\":\"boolean\"}}}");
    reg("open_decklink",
        "Open a Blackmagic DeckLink SDI/HDMI input as the live source. device = index or part of "
        "the name; right = the right eye's device (two inputs paired by timecode); stereo = "
        "auto|mono|sbs|dual|pair; input = sdi|hdmi; mode = starting format FourCC (e.g. Hp25).",
        "{\"type\":\"object\",\"properties\":{\"device\":{\"type\":\"string\"},\"right\":{\"type\":\"string\"},"
        "\"stereo\":{\"type\":\"string\"},\"input\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"}}}");
}

namespace {
bool Str(const json& a, const char* k, std::string& out) {
    if (!a.contains(k) || !a[k].is_string()) return false;
    out = a[k].get<std::string>();
    return true;
}
bool Num(const json& a, const char* k, float& out) {
    if (!a.contains(k) || !a[k].is_number()) return false;
    out = a[k].get<float>();
    return true;
}
}  // namespace

bool ProController::DispatchTool(const std::string& tool, const std::string& argsJson, std::string& result,
                                 bool& success) {
    try {
        json a = json::parse(argsJson.empty() ? "{}" : argsJson, nullptr, false);
        if (a.is_discarded() || !a.is_object()) a = json::object();
        auto err = [&](const std::string& e) {
            success = false;
            result = json{{"error", e}}.dump();
            return true;
        };
        auto gradeJson = [&]() {
            return json{{"lut", lutName_}, {"lut_on", grade_.lutOn},
                        {"cdl", cdlName_}, {"cdl_on", grade_.cdlOn},
                        {"slope", {grade_.slope[0], grade_.slope[1], grade_.slope[2]}},
                        {"offset", {grade_.offset[0], grade_.offset[1], grade_.offset[2]}},
                        {"power", {grade_.power[0], grade_.power[1], grade_.power[2]}},
                        {"saturation", grade_.saturation}, {"exposure", grade_.exposure}, {"bypass", bypass_}};
        };
        if (tool == "set_grade") {
            std::string p;
            if (Str(a, "lut", p)) {
                LoadLut(p);
                if (lutPath_ != p) return err("LUT not loaded: " + p);
            }
            if (Str(a, "cdl", p)) {
                LoadCdl(p);
                if (cdlPath_ != p) return err("CDL not loaded: " + p);
            }
            float ev;
            if (Num(a, "exposure", ev)) grade_.exposure = std::clamp(ev, -4.0f, 4.0f);
            if (a.contains("bypass") && a["bypass"].is_boolean()) bypass_ = a["bypass"].get<bool>();
            result = gradeJson().dump();
            return true;
        }
        if (tool == "get_grade") {
            result = gradeJson().dump();
            return true;
        }
        if (tool == "set_view") {
            std::string v;
            if (Str(a, "view", v)) {
                const ViewMode m = v == "3d"          ? ViewMode::Stereo3D
                                   : v == "left"       ? ViewMode::LeftOnly
                                   : v == "right"      ? ViewMode::RightOnly
                                   : v == "difference" ? ViewMode::Difference
                                   : v == "anaglyph"   ? ViewMode::Anaglyph
                                   : v == "false_colour" ? ViewMode::FalseColour
                                                       : ViewMode::Count;
                if (m == ViewMode::Count) return err("view must be 3d|left|right|difference|anaglyph|false_colour");
                SetMode(m, true);
            }
            float z;
            if (Num(a, "zoom", z)) SetZoom(std::max(1.0f, z));
            std::string ch;
            if (Str(a, "channel", ch)) {
                const char* names[] = {"rgb", "red", "green", "blue", "alpha", "luma"};
                int found = -1;
                for (int i = 0; i < (int)Channel::Count; ++i)
                    if (ch == names[i]) found = i;
                if (found < 0) return err("channel must be rgb|red|green|blue|alpha|luma");
                channel_ = (Channel)found;
            }
            float g;
            if (Num(a, "gamma", g)) gamma_ = std::clamp(g, 0.25f, 4.0f);
            std::string w;
            if (Str(a, "wipe", w)) {
                const Wipe m = w == "off"     ? Wipe::Off
                               : w == "grade" ? Wipe::Grade
                               : w == "eyes"  ? Wipe::Eyes
                               : w == "clips" ? Wipe::Clips
                                              : Wipe::Count;
                if (m == Wipe::Count) return err("wipe must be off|grade|eyes|clips");
                SetWipe(m, true);
            }
            float wp;
            if (Num(a, "wipe_pos", wp)) wipePos_ = std::clamp(wp, 0.0f, 1.0f);
            result = json{{"view", ViewModeName(mode_)}, {"zoom", zoom_}, {"channel", ChannelName(channel_)},
                          {"gamma", gamma_}, {"wipe", WipeName(wipe_)}, {"wipe_pos", wipePos_}}.dump();
            return true;
        }
        if (tool == "set_ocio") {
            ocio::Request req;
            Str(a, "config", req.config);
            Str(a, "src", req.src);
            Str(a, "display", req.display);
            Str(a, "view", req.view);
            Str(a, "dst", req.dst);
            Str(a, "looks", req.looks);
            std::string e;
            if (!LoadOcio(req, &e)) return err(e);
            result = json{{"lut", lutName_}, {"lut_size", lut_.size}}.dump();
            return true;
        }
        if (tool == "describe_ocio") {
            std::string cfg, e;
            Str(a, "config", cfg);
            ocio::ConfigInfo info;
            if (!ocio::Describe(cfg, info, e)) return err(e);
            result = json{{"name", info.name}, {"color_spaces", info.colorSpaces}, {"displays", info.displays},
                          {"default_display", info.defaultDisplay}, {"default_view", info.defaultView},
                          {"views", info.views}}.dump();
            return true;
        }
        if (tool == "set_compare") {
            std::string path;
            const bool hasPath = Str(a, "path", path);
            if (hasPath) compare_[1] = path;
            const bool toggle = a.contains("toggle") && a["toggle"].is_boolean() && a["toggle"].get<bool>();
            if (!hasPath || toggle) ToggleCompare();
            result = json{{"a", compare_[0]}, {"b", compare_[1]}, {"showing", compareSide_ ? "B" : "A"}}.dump();
            return true;
        }
        if (tool == "set_depth_overlay") {
            if (!a.contains("on") || !a["on"].is_boolean()) return err("on (boolean) is required");
            depthOverlay_ = a["on"].get<bool>();
            result = json{{"depth_overlay", depthOverlay_}}.dump();
            return true;
        }
        if (tool == "get_resolve_link") {
            const double now = LiveProducer::NowSeconds();
            result = json{{"panel_running", follower_.Active(now)},
                          {"timeline", link_.timeline},
                          {"playhead_tc", link_.playheadTc},
                          {"clip", link_.clipPath},
                          {"clip_seconds", link_.clipSeconds},
                          {"follow", followResolve_},
                          {"panel_follow", link_.follow},
                          {"resolve_playing", follower_.ResolvePlaying()}}.dump();
            return true;
        }
        if (tool == "set_resolve_follow") {
            if (!a.contains("on") || !a["on"].is_boolean()) return err("on (boolean) is required");
            followResolve_ = a["on"].get<bool>();
            result = json{{"follow", followResolve_}}.dump();
            return true;
        }
        return station_.DispatchTool(tool, a.dump(), result, success);
    } catch (const std::exception& e) {
        success = false;
        result = json{{"error", std::string("bad arguments: ") + e.what()}}.dump();
        return true;
    }
}

}  // namespace mp::pro
