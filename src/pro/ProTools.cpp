// SPDX-License-Identifier: Apache-2.0
//
// Media Player Pro agent tools (XR_DXR_mcp_tools; the plan's "MCP tools for automation"):
// the grade, the view, and the monitoring station. Registered and dispatched from the
// trunk's agent-tool code through its two #if MEDIAPLAYER_PRO hooks. An agent can send
// any JSON: a wrong-typed argument comes back as an error, never takes the app down.
#include "pro/ProController.h"
#include "pro/DisplayCalibration.h"

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
        "exposure in stops (-4..4, linear light) and bypass. HDR sources (HLG, PQ, scene-linear "
        "EXR): tone_map auto (roll off HLG / PQ, clip EXR at white) | on | off (clip) | raw (the "
        "signal unmapped, for a show LUT that expects PQ / HLG), peak_nits = the source's "
        "brightest (0 = from the stream, else 1000; for HLG the display peak it is rendered for), "
        "untagged = how a source with no HDR tag is taken: sdr (default) | hlg | pq. Arguments left out keep their "
        "current value. Returns the grade. A LUT loads in the background: the reply carries "
        "lut_loading, and get_grade shows the LUT (or lut_error) once it applies.",
        "{\"type\":\"object\",\"properties\":{\"lut\":{\"type\":\"string\"},\"cdl\":{\"type\":\"string\"},"
        "\"exposure\":{\"type\":\"number\"},\"bypass\":{\"type\":\"boolean\"},"
        "\"tone_map\":{\"type\":\"string\",\"enum\":[\"auto\",\"on\",\"off\",\"raw\"]},"
        "\"peak_nits\":{\"type\":\"number\"},"
        "\"untagged\":{\"type\":\"string\",\"enum\":[\"sdr\",\"hlg\",\"pq\"]}}}");
    reg("set_target_screen",
        "Judge the depth for the screen the picture is made for (#135): target cinema, tv, monitor, "
        "tablet or off, or a custom width_m + distance_m. With a target, get_rig_status reports near / "
        "far in its terms (mm, arcminutes, background divergence) and the rig check alerts when the "
        "background diverges past 0.5 deg there. preview (default true) shows the picture on the panel "
        "at the angular size it has on the target, so the depth reads as it will there (the picture "
        "crops or gets a border). Returns the target and the depth in its terms, for the picture as shown "
        "(convergence included: parallax_offset_pct). picture_scale in the reply is the last frame's; "
        "get_rig_status has the new one a frame later.",
        "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"enum\":[\"off\",\"cinema\",\"tv\",\"monitor\",\"tablet\"]},"
        "\"width_m\":{\"type\":\"number\",\"minimum\":0.1},\"distance_m\":{\"type\":\"number\",\"minimum\":0.2},"
        "\"preview\":{\"type\":\"boolean\"}}}");
    reg("set_display_calibration",
        "Set this panel's display calibration (#133): a .cube 3D LUT (domain 0..1) from a calibration "
        "tool (DisplayCAL, Calman, ColourSpace), applied after the grade and QC views. It is kept for "
        "this panel and loads automatically from then on (keep: false = this run only). clear: true "
        "removes it (and the kept file); bypass turns it off without removing it. The show grade's "
        "bypass (set_grade bypass, G) does not affect it. Returns the panel key and the calibration; "
        "the LUT loads in the background, so the reply may carry loading.",
        "{\"type\":\"object\",\"properties\":{\"cube\":{\"type\":\"string\"},\"keep\":{\"type\":\"boolean\"},"
        "\"clear\":{\"type\":\"boolean\"},\"bypass\":{\"type\":\"boolean\"}}}");
    reg("get_grade", "Read the grade: LUT (with lut_loading / lut_error), CDL (slope/offset/power/"
        "saturation), exposure, bypass, and the HDR mapping (tone_map, peak_nits, hdr = what "
        "the current source gets: transfer, raw, roll_off, bt2020, peak_nits).",
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
        "front of the screen), edge violations (edge_left_pct / edge_right_pct: the most in-front "
        "parallax along each frame edge, null = not measured), colour match between the eyes (CIE76 dE), eye sync (frames between "
        "the paired inputs, -1 = unknown), the depth budget, recent alerts with timecode, and the "
        "source timecode and capture-to-screen delay.",
        "{\"type\":\"object\"}");
    reg("set_depth_budget",
        "Set the limits the rig checks alert on: near / far parallax in % of eye width (near "
        "negative = in front), vertical error in px, colour match in dE, and edge_pct: the nearest "
        "parallax allowed at the left / right frame edge (an edge violation below it).",
        "{\"type\":\"object\",\"properties\":{\"near_pct\":{\"type\":\"number\"},\"far_pct\":{\"type\":\"number\"},"
        "\"vertical_px\":{\"type\":\"number\"},\"delta_e\":{\"type\":\"number\"},"
        "\"edge_pct\":{\"type\":\"number\"}}}");
    reg("set_depth_overlay", "Show or hide the depth overlay (the rig check's matches on the picture).",
        "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}},\"required\":[\"on\"]}");
    reg("snapshot",
        "Capture both eyes as shown (graded) to a PNG in Pictures/DisplayXR, with a JSON note "
        "beside it: the note text, timecode and rig numbers.",
        "{\"type\":\"object\",\"properties\":{\"note\":{\"type\":\"string\"}}}");
    reg("export_qc_report",
        "Write this session's QC report to a new Pictures/DisplayXR/qc_<time> folder: report.html "
        "(alerts, snapshots with both eyes, notes, rig numbers, depth budget; self-contained), "
        "markers.edl (DaVinci Resolve timeline markers) and markers.csv. Returns the paths at once "
        "(writing: true); the files are written in the background, report.html last, so the "
        "report is complete once report.html exists.",
        "{\"type\":\"object\"}");
    reg("list_inputs", "List the station's configured live inputs (name, spec) and which one is live.",
        "{\"type\":\"object\"}");
    reg("select_input", "Switch to a configured input, by index (from list_inputs) or name.",
        "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"},\"name\":{\"type\":\"string\"}}}");
    reg("set_ocio",
        "Bake an OpenColorIO transform into the grade's LUT: config (a path, an ocio:// built-in, or "
        "empty for $OCIO), src = the picture's colour space, then display + view (default: the "
        "config's) or dst = a target colour space; optional looks. Needs a build with OpenColorIO. "
        "Bakes in the background: poll get_grade until lut_loading is empty (lut or lut_error).",
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
        auto calibrationJson = [&]() {
            return json{{"panel", calKey_}, {"panel_key_source", calKeyFromRuntime_ ? "runtime" : "device_name"},
                        {"name", calLoaded_ ? calName_ : std::string()},
                        {"on", calLoaded_ && !calBypass_}, {"bypass", calBypass_},
                        {"loading", calLoading_}, {"error", calError_},
                        {"file", cal::CalibrationPathFor(calKey_)}};
        };
        auto HdrJson = [&]() {
            const VulkanRenderer::HdrState h = renderer_ ? renderer_->SourceHdr() : VulkanRenderer::HdrState{};
            return json{{"transfer", HdrTransferName(h.transfer)}, {"raw", h.raw}, {"roll_off", h.rollOff},
                        {"bt2020", h.bt2020}, {"peak_nits", h.peakNits}, {"ocio_unclipped", h.pqShaper},
                        {"forced", h.forced}, {"over_white", h.overWhite},
                        {"summary", HdrSummary(h)}};
        };
        auto gradeJson = [&]() {
            return json{{"lut", lutName_}, {"lut_on", grade_.lutOn},
                        {"lut_loading", lutLoading_}, {"lut_error", lutError_},
                        {"cdl", cdlName_}, {"cdl_on", grade_.cdlOn},
                        {"slope", {grade_.slope[0], grade_.slope[1], grade_.slope[2]}},
                        {"offset", {grade_.offset[0], grade_.offset[1], grade_.offset[2]}},
                        {"power", {grade_.power[0], grade_.power[1], grade_.power[2]}},
                        {"saturation", grade_.saturation}, {"exposure", grade_.exposure}, {"bypass", bypass_},
                        {"calibration", calibrationJson()},
                        {"tone_map", ToneMapName(grade_.toneMap)}, {"peak_nits", grade_.peakNits},
                        {"untagged", UntaggedName(grade_.forceTransfer)},
                        {"hdr", HdrJson()}};
        };
        if (tool == "set_grade") {
            std::string p;
            if (Str(a, "lut", p)) {
                LoadLut(p);  // asynchronous: get_grade's lut_loading / lut_error report it
                if (lutLoading_ != p) return err("LUT not loaded: " + lutError_);
            }
            if (Str(a, "cdl", p)) {
                LoadCdl(p);
                if (cdlPath_ != p) return err("CDL not loaded: " + p);
            }
            float ev;
            if (Num(a, "exposure", ev)) grade_.exposure = std::clamp(ev, -4.0f, 4.0f);
            if (a.contains("bypass") && a["bypass"].is_boolean()) bypass_ = a["bypass"].get<bool>();
            if (Str(a, "tone_map", p)) {
                const int m = ToneMapFromName(p, -1);
                if (m < 0) return err("tone_map must be auto, on, off or raw");
                grade_.toneMap = m;
            }
            float nits;
            if (Num(a, "peak_nits", nits)) grade_.peakNits = std::clamp(nits, 0.0f, 10000.0f);
            if (Str(a, "untagged", p)) {
                const int m = UntaggedFromName(p, -1);
                if (m < 0) return err("untagged must be sdr, hlg or pq");
                grade_.forceTransfer = m;
            }
            result = gradeJson().dump();
            return true;
        }
        if (tool == "set_target_screen") {
            std::string t;
            float w = 0.0f, d = 0.0f;
            if (Str(a, "target", t)) {
                target::Screen s;
                if (t == "off") SetTarget({});
                else if (target::PresetByName(t, s)) SetTarget(s);
                else return err("target must be off|cinema|tv|monitor|tablet");
            }
            if (Num(a, "width_m", w) | Num(a, "distance_m", d)) {
                if (w <= 0.0f) w = target_.On() ? target_.widthM : 0.0f;
                if (d <= 0.0f) d = target_.On() ? target_.distanceM : 0.0f;
                if (w < 0.1f || d < 0.2f) return err("a custom target needs width_m >= 0.1 and distance_m >= 0.2");
                SetTarget({"Custom", w, d});
            }
            if (a.contains("preview") && a["preview"].is_boolean()) {
                targetPreview_ = a["preview"].get<bool>();
                targetDirty_ = true;
            }
            ApplyTarget();
            const json rs = json::parse(station_.RigStatusJson(), nullptr, false);
            result = json{{"preview", targetPreview_ && target_.On()},
                          {"target", rs.is_object() && rs.contains("target") ? rs["target"] : json(nullptr)}}
                         .dump();
            return true;
        }
        if (tool == "set_display_calibration") {
            if (!calResolved_) ResolveCalibration();
            if (a.contains("clear") && a["clear"].is_boolean() && a["clear"].get<bool>()) ClearCalibration(true);
            std::string p;
            if (Str(a, "cube", p)) {
                const bool keep = !(a.contains("keep") && a["keep"].is_boolean() && !a["keep"].get<bool>());
                LoadCalibration(p, keep);
                if (calLoading_ != p) return err("calibration not loaded: " + calError_);
            }
            if (a.contains("bypass") && a["bypass"].is_boolean()) calBypass_ = a["bypass"].get<bool>();
            result = calibrationJson().dump();
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
            result = json{{"lut_loading", lutLoading_}}.dump();  // get_grade reports the result
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
