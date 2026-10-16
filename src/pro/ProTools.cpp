// SPDX-License-Identifier: Apache-2.0
//
// Media Player Pro agent tools (XR_DXR_mcp_tools; the plan's "MCP tools for automation"):
// the grade, the view, and the monitoring station. Registered and dispatched from the
// trunk's agent-tool code through its two #if MEDIAPLAYER_PRO hooks. An agent can send
// any JSON: a wrong-typed argument comes back as an error, never takes the app down.
#include "pro/ProController.h"

#include "Log.h"

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
        "Set the QC view: 3d, left, right, difference (|left - right| x4) or anaglyph; optional "
        "zoom (1 = fit) applied to both eyes.",
        "{\"type\":\"object\",\"properties\":{\"view\":{\"type\":\"string\",\"enum\":[\"3d\",\"left\",\"right\","
        "\"difference\",\"anaglyph\"]},\"zoom\":{\"type\":\"number\",\"minimum\":1}}}");
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
                                                       : ViewMode::Count;
                if (m == ViewMode::Count) return err("view must be 3d|left|right|difference|anaglyph");
                SetMode(m, true);
            }
            float z;
            if (Num(a, "zoom", z)) SetZoom(std::max(1.0f, z));
            result = json{{"view", ViewModeName(mode_)}, {"zoom", zoom_}}.dump();
            return true;
        }
        if (tool == "set_depth_overlay") {
            if (!a.contains("on") || !a["on"].is_boolean()) return err("on (boolean) is required");
            depthOverlay_ = a["on"].get<bool>();
            result = json{{"depth_overlay", depthOverlay_}}.dump();
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
