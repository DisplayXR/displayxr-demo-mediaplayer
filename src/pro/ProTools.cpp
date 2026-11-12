// SPDX-License-Identifier: Apache-2.0
//
// Media Player Pro agent tools (XR_DXR_mcp_tools; the plan's "MCP tools for automation"):
// the grade, the view, and the monitoring station. Registered and dispatched from the
// trunk's agent-tool code through its two #if MEDIAPLAYER_PRO hooks; the tool table itself
// is ProToolList.cpp. An agent can send any JSON: a wrong-typed argument comes back as an
// error, never takes the app down.
#include "pro/ProController.h"
#include "pro/DisplayCalibration.h"
#include "pro/ProToolList.h"

#include "Log.h"
#include "media/LiveProducer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace mp::pro {

using json = nlohmann::json;

void ProController::RegisterTools(const RegisterFn& reg) { RegisterProAgentTools(reg); }

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
                LoadLut(p);  // asynchronous: set_grade {}'s lut_loading / lut_error report it
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
            if (a.contains("clear") && a["clear"].is_boolean() && a["clear"].get<bool>() && !ClearCalibration(true))
                return err("calibration not removed: " + calError_);  // #169: a failed delete is a failure
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
        if (tool == "set_qc_view") {  // #220: was set_view, which is the player's VR viewer tool
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
            result = json{{"lut_loading", lutLoading_}}.dump();  // set_grade {} reports the result
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
        if (tool == "set_resolve_follow") {  // also the read (#220: was get_resolve_link)
            if (a.contains("on")) {
                if (!a["on"].is_boolean()) return err("on must be a boolean");
                followResolve_ = a["on"].get<bool>();
            }
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
        return station_.DispatchTool(tool, a.dump(), result, success);
    } catch (const std::exception& e) {
        success = false;
        result = json{{"error", std::string("bad arguments: ") + e.what()}}.dump();
        return true;
    }
}

}  // namespace mp::pro
