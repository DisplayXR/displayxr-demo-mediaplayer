// SPDX-License-Identifier: Apache-2.0
#include "pro/Station.h"

#include "Log.h"

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include "imgui.h"
#endif

namespace mp::pro {

namespace {

// "decklink:<dev>[+<right>][,sdi|hdmi][,dual|sbs|mono][,mode=Hp25]"
bool ParseDeckLinkSpec(const std::string& args, DeckLinkConfig& c) {
    c = DeckLinkConfig{};
    std::string head = args, rest;
    const size_t comma = args.find(',');
    if (comma != std::string::npos) {
        head = args.substr(0, comma);
        rest = args.substr(comma + 1);
    }
    const size_t plus = head.find('+');
    c.device = head.substr(0, plus);
    if (c.device.empty()) c.device = "0";
    if (plus != std::string::npos) {
        c.rightDevice = head.substr(plus + 1);
        c.stereo = DeckLinkConfig::Stereo::Pair;
    }
    size_t pos = 0;
    while (!rest.empty() && pos <= rest.size()) {
        const size_t e = rest.find(',', pos);
        const std::string t = rest.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (t == "sdi" || t == "hdmi") c.input = t;
        else if (t.rfind("mode=", 0) == 0) c.mode = t.substr(5);
        else if (!t.empty()) {
            DeckLinkConfig::Stereo st;
            if (!DeckLinkConfig::ParseStereo(t, st)) return false;
            c.stereo = st;
        }
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return true;
}

std::string Clock(char* stampOut, size_t stampN) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char clock[16];
    std::strftime(clock, sizeof(clock), "%H:%M:%S", &tm);
    if (stampOut) std::strftime(stampOut, stampN, "%Y%m%d_%H%M%S", &tm);
    return clock;
}

}  // namespace

Station::~Station() { Shutdown(); }

void Station::Init(StationHost host, const StationOptions& opt) {
    host_ = std::move(host);
    opt_ = opt;
    showTimecode_ = opt.timecode;
    rig_.SetBudget(opt.budget);
    for (const std::string& in : opt.inputs) {
        const size_t eq = in.find('=');
        Input d;
        d.name = eq == std::string::npos ? in : in.substr(0, eq);
        d.spec = eq == std::string::npos ? in : in.substr(eq + 1);
        inputs_.push_back(d);
    }
    if (!inputs_.empty() && opt.startFirstInput) SwitchInput(0);
    if (const char* f = std::getenv("MEDIAPLAYER_PRO_SNAPSHOT_FRAME")) testSnapshotFrame_ = std::atoi(f);
    if (const char* d = std::getenv("MEDIAPLAYER_PRO_DECKLINK_DIALOG"); d && *d && *d != '0') OpenDeckLinkDialog();
}

void Station::Shutdown() {
    decklink_.Stop();
    testSignal_.Stop();
}

bool Station::SwitchInput(int index) {
    if (inputs_.empty()) {
        if (host_.toast) host_.toast("No inputs configured (--input=\"Name=decklink:0+1\")");
        return false;
    }
    const int n = (int)inputs_.size();
    index = ((index % n) + n) % n;
    const Input& d = inputs_[(size_t)index];
    const size_t colon = d.spec.find(':');
    const std::string kind = d.spec.substr(0, colon);
    const std::string args = colon == std::string::npos ? std::string() : d.spec.substr(colon + 1);
    bool ok = false;
    if (kind == "decklink") {
        DeckLinkConfig c;
        if (!ParseDeckLinkSpec(args, c)) {
            if (host_.toast) host_.toast("Input '" + d.name + "': bad spec " + d.spec);
            return false;
        }
        // EnterLive stops the previous source only once the new one opened. Re-opening the
        // card that is live stops it first, so a failure there must leave live mode.
        const LiveState was = decklink_.GetState();
        const bool wasLive = was != LiveState::Closed && was != LiveState::Failed && was != LiveState::Denied;
        ok = decklink_.Open(c);
        if (ok && host_.enterLive) host_.enterLive(&decklink_, decklink_.DeviceName());
        if (!ok) {
            if (wasLive && host_.stopLive) host_.stopLive();
            if (host_.toast) host_.toast("DeckLink: " + decklink_.LastError());
        }
    } else if (kind == "test") {
        TestSignalConfig c = opt_.testCfg;
        if (!c.ParseSpec(args)) {
            if (host_.toast) host_.toast("Input '" + d.name + "': bad test spec " + args);
            return false;
        }
        ok = testSignal_.Open(c);
        if (ok && host_.enterLive) host_.enterLive(&testSignal_, testSignal_.DeviceName());
    } else if (kind == "camera") {
        ok = host_.openCamera && host_.openCamera(args);  // EnterLive stops a Pro source on success
    } else if (kind == "url") {
        ok = host_.openUrl && host_.openUrl(args);        // LoadUrl stops any live source
    } else {
        if (host_.toast) host_.toast("Input '" + d.name + "': unknown kind '" + kind + "'");
        return false;
    }
    if (ok) {
        inputIndex_ = index;
        rig_.Reset();  // a new input is a new rig: its history and alerts start over
        LOG_INFO("Station: input %d '%s' (%s)", index, d.name.c_str(), d.spec.c_str());
    }
    return ok;
}

bool Station::HandleKey(int key) {
    switch (key) {
        case SDLK_T:
            showTimecode_ = !showTimecode_;
            if (host_.toast) host_.toast(showTimecode_ ? "Timecode + delay on" : "Timecode + delay off");
            return true;
        case SDLK_R:
            showHistory_ = !showHistory_;
            if (host_.toast) host_.toast(showHistory_ ? "Rig history shown" : "Rig history hidden");
            return true;
        case SDLK_N:
            pendingSnapshot_ = true;  // done in Tick(), outside the SDL event hook
            pendingSnapshotNote_.clear();
            return true;
        case SDLK_J:
            pendingSwitch_ = true;
            return true;
        default:
            return false;
    }
}

void Station::Tick() {
    // Test scaffolding (screenshots): MEDIAPLAYER_PRO_SNAPSHOT_FRAME=N takes one snapshot
    // after N ticks, with MEDIAPLAYER_PRO_SNAPSHOT_NOTE as its note.
    ++testFrames_;
    if (testSnapshotFrame_ > 0 && testFrames_ == testSnapshotFrame_) {
        pendingSnapshot_ = true;
        const char* n = std::getenv("MEDIAPLAYER_PRO_SNAPSHOT_NOTE");
        pendingSnapshotNote_ = n ? n : "";
    }
    if (pendingSnapshot_) {
        pendingSnapshot_ = false;
        TakeSnapshot(pendingSnapshotNote_);
        pendingSnapshotNote_.clear();
    }
    // The runtime names the capture <prefix>_atlas<...>.png; point each new note at it.
    for (size_t i = 0; i < snapshots_.size(); ++i) {
        Snapshot& s = snapshots_[i];
        if (s.imageFound || s.prefix.empty()) continue;
        if (LiveProducer::NowSeconds() - s.takenSec > 5.0) {
            s.imageFound = true;  // give up; the note keeps the guess
            continue;
        }
        if (testFrames_ % 15 != 0) continue;
        const std::filesystem::path pre(s.prefix);
        const std::string stem = pre.filename().string() + "_atlas";
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(pre.parent_path(), ec)) {
            const std::string f = de.path().filename().string();
            if (f.rfind(stem, 0) == 0 && de.path().extension() == ".png") {
                s.image = de.path().string();
                s.imageFound = true;
                SaveNote(i);
                break;
            }
        }
    }
    if (!pendingDeckLinkSpec_.empty()) {
        const std::string spec = pendingDeckLinkSpec_;
        pendingDeckLinkSpec_.clear();
        OpenDeckLinkSpec(spec);
    }
    if (pendingSwitch_) {
        pendingSwitch_ = false;
        SwitchInput(inputIndex_ + 1);
    }
}

// The dialog's and the open_decklink tool's path: the "DeckLink" input gets this spec
// (added or replaced) and becomes the live input through SwitchInput, so a failure leaves
// live mode and the input list the same way a failed J does.
bool Station::OpenDeckLinkSpec(const std::string& spec) {
    int idx = -1;
    for (size_t i = 0; i < inputs_.size(); ++i)
        if (inputs_[i].name == "DeckLink") idx = (int)i;
    if (idx < 0) {
        inputs_.push_back({"DeckLink", "decklink:" + spec});
        idx = (int)inputs_.size() - 1;
    } else {
        inputs_[idx].spec = "decklink:" + spec;
    }
    return SwitchInput(idx);
}

void Station::OnFrame(const FrameRing::Frame& f, StereoLayout layout, bool live, int eyeOffsetFrames,
                      bool eyesSwapped) {
    LogNewAlerts();
    if (live) {
        timecode_ = f.timecode;
        if (f.captureSec > 0.0) {
            const double ms = (LiveProducer::NowSeconds() - f.captureSec) * 1000.0;
            delayMs_ = delayMs_ <= 0.0 ? ms : delayMs_ * 0.9 + ms * 0.1;  // capture -> upload
        }
    } else {
        timecode_.clear();
        delayMs_ = 0.0;
    }
    if (layout == StereoLayout::SbsFull || layout == StereoLayout::SbsHalf)
        rig_.Submit(f, layout, live ? eyeOffsetFrames : -1, f.timecode, eyesSwapped);
}

int Station::MeasuredEyeSync(int* lateEye) const {
    if (lateEye) *lateEye = 0;
    const std::vector<rig::Sample> h = rig_.History(2.0);
    for (auto it = h.rbegin(); it != h.rend(); ++it)
        if (it->eyeSyncMeasured >= 0) {
            if (lateEye) *lateEye = it->eyeSyncLateEye;
            return it->eyeSyncMeasured;
        }
    return -1;
}

bool Station::TakeSnapshot(const std::string& note) {
    char stamp[32];
    const std::string clock = Clock(stamp, sizeof(stamp));
    if (!host_.capturePrefix || !host_.captureAtlas) return false;
    const std::string prefix = host_.capturePrefix(std::string("pro_") + stamp);
    if (!host_.captureAtlas(prefix)) {
        if (host_.toast) host_.toast("Snapshot: the runtime cannot capture (no atlas capture)");
        return false;
    }
    Snapshot s;
    s.image = prefix + "_atlas.png";  // corrected in Tick once the runtime has written it
    s.prefix = prefix;
    s.takenSec = LiveProducer::NowSeconds();
    s.notePath = prefix + "_note.json";
    s.timecode = timecode_;
    s.clock = clock;
    s.rig = rig_.Last();
    {
        const std::vector<rig::Sample> h = rig_.History(1.0);
        s.eyeSync = h.empty() ? -1 : h.back().eyeOffsetFrames;
    }
    std::snprintf(s.note, sizeof(s.note), "%s", note.c_str());
    snapshots_.push_back(s);
    if (snapshots_.size() > 50) snapshots_.erase(snapshots_.begin());
    SaveNote(snapshots_.size() - 1);
    if (host_.toast) host_.toast("Snapshot " + clock + (timecode_.empty() ? "" : "  TC " + timecode_));
    return true;
}

void Station::SaveNote(size_t i) {
    const Snapshot& s = snapshots_[i];
    nlohmann::json j;
    j["image"] = s.image;
    j["note"] = std::string(s.note);
    j["timecode"] = s.timecode;
    j["time"] = s.clock;
    if (s.rig.ok) {
        j["vertical_px"] = s.rig.verticalPx;
        j["roll_deg"] = s.rig.rollDeg;
        j["near_pct"] = s.rig.nearPct;
        j["far_pct"] = s.rig.farPct;
        j["delta_e"] = s.rig.deltaE;
    }
    if (s.eyeSync >= 0) j["eye_sync_frames"] = s.eyeSync;
    if (s.rig.eyeSyncMeasuredFrames >= 0) j["eye_sync_measured_frames"] = s.rig.eyeSyncMeasuredFrames;
    const rig::Budget b = rig_.GetBudget();
    j["budget"] = {{"near_pct", b.nearPct}, {"far_pct", b.farPct}};
    std::ofstream(s.notePath) << j.dump(2) << "\n";
}

void Station::LogNewAlerts() {
    double newest = alertsLoggedUntil_;
    std::string dir;
    for (const rig::Alert& a : rig_.Alerts()) {
        if (a.end < 0 || a.end <= alertsLoggedUntil_) continue;
        if (dir.empty() && host_.capturePrefix)
            dir = std::filesystem::path(host_.capturePrefix("x")).parent_path().string();
        if (dir.empty()) break;
        nlohmann::json j;
        j["what"] = a.what;
        j["timecode"] = a.timecode;
        if (!a.endTimecode.empty()) j["end_timecode"] = a.endTimecode;
        j["time"] = a.wallClock;
        j["duration_s"] = a.end - a.start;
        std::ofstream((std::filesystem::path(dir) / "station_alerts.jsonl").string(), std::ios::app) << j.dump() << "\n";
        newest = std::max(newest, a.end);
    }
    alertsLoggedUntil_ = newest;
}

std::string Station::RigStatusJson() const {
    using json = nlohmann::json;
    const rig::Result r = rig_.Last();
    const rig::Budget b = rig_.GetBudget();
    const std::vector<rig::Sample> h = rig_.History(1.0);
    json out;
    out["measured"] = r.ok && rig_.Active(LiveProducer::NowSeconds());
    out["matches"] = r.matches;
    out["vertical_px"] = r.verticalPx;
    out["roll_deg"] = r.rollDeg;
    out["near_pct"] = r.nearPct;
    out["far_pct"] = r.farPct;
    out["delta_e"] = r.deltaE;
    out["eye_sync_frames"] = h.empty() ? -1 : h.back().eyeOffsetFrames;
    int late = 0;
    out["eye_sync_measured_frames"] = MeasuredEyeSync(&late);  // from the pixels, -1 = not measured
    out["eye_sync_late_eye"] = late > 0 ? "right" : late < 0 ? "left" : "none";
    out["budget"] = {{"near_pct", b.nearPct}, {"far_pct", b.farPct}, {"vertical_px", b.maxVerticalPx},
                     {"delta_e", b.maxDeltaE}};
    json alerts = json::array();
    for (const rig::Alert& al : rig_.Alerts())
        alerts.push_back({{"what", al.what}, {"timecode", al.timecode}, {"time", al.wallClock}, {"open", al.end < 0}});
    out["alerts"] = alerts;
    out["timecode"] = timecode_;
    out["delay_ms"] = delayMs_ > 0.0 ? delayMs_ + (host_.displayPeriodMs ? host_.displayPeriodMs() : 16.7) : 0.0;
    return out.dump();
}

std::string Station::Folder() const {
    return host_.capturePrefix ? std::filesystem::path(host_.capturePrefix("x")).parent_path().string()
                               : std::string();
}

// ---------------------------------------------------------------------------- agent tools

bool Station::DispatchTool(const std::string& tool, const std::string& argsJson, std::string& result,
                           bool& success) {
    using json = nlohmann::json;
    json a = json::parse(argsJson.empty() ? "{}" : argsJson, nullptr, false);
    if (a.is_discarded() || !a.is_object()) a = json::object();
    auto err = [&](const std::string& e) {
        success = false;
        result = json{{"error", e}}.dump();
        return true;
    };
    auto num = [&](const char* k, float& v) {
        if (!a.contains(k) || !a[k].is_number()) return false;
        v = a[k].get<float>();
        return true;
    };
    if (tool == "get_rig_status") {
        result = RigStatusJson();
        return true;
    }
    if (tool == "set_depth_budget") {
        rig::Budget b = rig_.GetBudget();
        num("near_pct", b.nearPct);
        num("far_pct", b.farPct);
        num("vertical_px", b.maxVerticalPx);
        num("delta_e", b.maxDeltaE);
        if (!(b.nearPct < b.farPct)) return err("near_pct must be below far_pct");
        rig_.SetBudget(b);
        result = json{{"near_pct", b.nearPct}, {"far_pct", b.farPct}, {"vertical_px", b.maxVerticalPx},
                      {"delta_e", b.maxDeltaE}}.dump();
        return true;
    }
    if (tool == "snapshot") {
        const std::string note = a.contains("note") && a["note"].is_string() ? a["note"].get<std::string>() : "";
        if (!TakeSnapshot(note)) return err("snapshot failed (the runtime has no atlas capture?)");
        const Snapshot& s = snapshots_.back();
        result = json{{"image", s.image}, {"note_file", s.notePath}, {"timecode", s.timecode}}.dump();
        return true;
    }
    if (tool == "list_inputs") {
        json arr = json::array();
        for (size_t i = 0; i < inputs_.size(); ++i)
            arr.push_back({{"index", i}, {"name", inputs_[i].name}, {"spec", inputs_[i].spec}});
        result = json{{"inputs", arr}, {"active", inputIndex_}}.dump();
        return true;
    }
    if (tool == "select_input") {
        int idx = a.contains("index") && a["index"].is_number_integer() ? a["index"].get<int>() : -1;
        if (idx < 0 && a.contains("name") && a["name"].is_string())
            for (size_t i = 0; i < inputs_.size(); ++i)
                if (inputs_[i].name == a["name"].get<std::string>()) idx = (int)i;
        if (idx < 0 || idx >= (int)inputs_.size() || !SwitchInput(idx)) return err("no such input, or it failed to open");
        result = json{{"active", inputIndex_}, {"name", inputs_[(size_t)inputIndex_].name}}.dump();
        return true;
    }
    if (tool == "open_decklink") {
        // Same path as the dialog: a "DeckLink" input with this spec, switched to.
        std::string spec = a.contains("device") && a["device"].is_string() ? a["device"].get<std::string>() : "0";
        if (a.contains("right") && a["right"].is_string()) spec += "+" + a["right"].get<std::string>();
        if (a.contains("input") && a["input"].is_string()) spec += "," + a["input"].get<std::string>();
        if (a.contains("stereo")) {
            DeckLinkConfig::Stereo st;
            if (!a["stereo"].is_string() || !DeckLinkConfig::ParseStereo(a["stereo"].get<std::string>(), st))
                return err("stereo must be auto|mono|sbs|dual|pair");
            if (!a["right"].is_string()) spec += "," + a["stereo"].get<std::string>();
        }
        if (a.contains("mode") && a["mode"].is_string()) spec += ",mode=" + a["mode"].get<std::string>();
        if (!OpenDeckLinkSpec(spec)) return err(decklink_.LastError().empty() ? "bad DeckLink spec: " + spec : decklink_.LastError());
        result = json{{"device", decklink_.DeviceName()}, {"state", LiveStateName(decklink_.GetState())},
                      {"spec", "decklink:" + spec}}.dump();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------- UI

#if defined(MEDIAPLAYER_WITH_IMGUI)
namespace {
const ImVec4 kOk(0.45f, 0.85f, 0.55f, 1.0f);
const ImVec4 kWarn(1.00f, 0.72f, 0.25f, 1.0f);
const ImVec4 kBad(1.00f, 0.38f, 0.35f, 1.0f);
const ImVec4 kAccent(0.20f, 0.65f, 1.00f, 1.0f);

void Graded(const char* label, const char* value, float magnitude, float limit) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.56f);
    const ImVec4 c = magnitude <= limit * 0.75f ? kOk : magnitude <= limit ? kWarn : kBad;
    ImGui::TextColored(c, "%s", value);
}
}  // namespace
#endif

void Station::BuildInputsSection(const std::string& liveName, bool live) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!ImGui::CollapsingHeader("Inputs  (J)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::SmallButton("DeckLink setup...")) OpenDeckLinkDialog();
    if (inputs_.empty()) {
        ImGui::TextDisabled("%s", live ? liveName.c_str() : "No inputs configured.");
        ImGui::TextDisabled("--input=\"Rig=decklink:0+1,sdi\"  --test-signal");
        if (!DeckLinkSource::Available()) ImGui::TextDisabled("(this build has no DeckLink SDK)");
        return;
    }
    for (size_t i = 0; i < inputs_.size(); ++i) {
        ImGui::PushID((int)i);
        if (ImGui::Selectable(inputs_[i].name.c_str(), (int)i == inputIndex_ && live)) SwitchInput((int)i);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.62f);
        ImGui::TextDisabled("%s", inputs_[i].spec.substr(0, inputs_[i].spec.find(':')).c_str());
        ImGui::PopID();
    }
#else
    (void)liveName;
    (void)live;
#endif
}

void Station::OpenDeckLinkDialog() {
    dlgDevices_ = DeckLinkSource::Available() ? DeckLinkSource::Enumerate() : std::vector<std::string>{};
    dlgReason_ = dlgDevices_.empty() ? DeckLinkSource::UnavailableReason() : std::string();
    dlgLeft_ = std::clamp(dlgLeft_, 0, std::max(0, (int)dlgDevices_.size() - 1));
    dlgRight_ = std::clamp(dlgRight_, 0, (int)dlgDevices_.size());
    dlgOpen_ = true;
}

void Station::BuildDeckLinkDialog() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!dlgOpen_) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::clamp(io.DisplaySize.x * 0.30f, 380.0f, 520.0f), 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);
    if (!ImGui::Begin("DeckLink input setup", &dlgOpen_, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const bool sdk = DeckLinkSource::Available();
    const bool any = !dlgDevices_.empty();
    if (!sdk) {
        ImGui::TextColored(kBad, "This build has no DeckLink SDK.");
        ImGui::TextDisabled("Rebuild with -DMP_DECKLINK_SDK_DIR=<Blackmagic DeckLink SDK>.");
    } else if (!any) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("No DeckLink devices found: %s.", dlgReason_.c_str());
        ImGui::PopStyleColor();
        ImGui::TextWrapped("Fit a Blackmagic DeckLink card (e.g. DeckLink Duo 2, 8K Pro G2, Mini Recorder 4K) and "
                           "install Blackmagic Desktop Video, then press Refresh.");
    } else {
        ImGui::TextColored(kOk, "%zu DeckLink input%s found.", dlgDevices_.size(), dlgDevices_.size() == 1 ? "" : "s");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) OpenDeckLinkDialog();
    ImGui::Separator();
    ImGui::BeginDisabled(!any);
    auto deviceCombo = [&](const char* label, int& sel, bool allowNone) {
        const int n = (int)dlgDevices_.size();
        const std::string cur = allowNone && sel == 0 ? std::string("none - one cable")
                                : n ? dlgDevices_[std::clamp(sel - (allowNone ? 1 : 0), 0, n - 1)]
                                    : std::string("-");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        if (ImGui::BeginCombo(label, cur.c_str())) {
            if (allowNone && ImGui::Selectable("none - one cable", sel == 0)) sel = 0;
            for (int i = 0; i < n; ++i)
                if (ImGui::Selectable(dlgDevices_[i].c_str(), sel == i + (allowNone ? 1 : 0))) sel = i + (allowNone ? 1 : 0);
            ImGui::EndCombo();
        }
    };
    deviceCombo("Left eye / input", dlgLeft_, false);
    deviceCombo("Right eye", dlgRight_, true);
    const bool pair = dlgRight_ > 0;
    const bool sameDevice = pair && dlgRight_ - 1 == dlgLeft_;
    if (sameDevice) ImGui::TextColored(kBad, "Left and right must be two different inputs.");
    else ImGui::TextDisabled(pair ? "Two cables: frames paired by SDI timecode (RP188); same card: by its clock." : "One cable: both eyes in one signal.");
    const char* conns[] = {"as set in Desktop Video", "SDI", "HDMI"};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::Combo("Connection", &dlgConn_, conns, 3);
    ImGui::BeginDisabled(pair);
    const char* packs[] = {"auto (3D flag / aspect)", "side by side", "dual-stream 3D (SDI 3G-B)", "mono"};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::Combo("Stereo", &dlgStereo_, packs, 4);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::InputTextWithHint("Start format", "Hp25 (auto-detect follows)", dlgMode_, sizeof(dlgMode_));
    ImGui::Separator();
    ImGui::BeginDisabled(sameDevice);
    if (ImGui::Button("Open")) {
        std::string spec = std::to_string(dlgLeft_);
        if (pair) spec += "+" + std::to_string(dlgRight_ - 1);
        if (dlgConn_ == 1) spec += ",sdi";
        if (dlgConn_ == 2) spec += ",hdmi";
        if (!pair) {
            const char* st[] = {"", "sbs", "dual", "mono"};
            if (*st[dlgStereo_]) spec += std::string(",") + st[dlgStereo_];
        }
        if (dlgMode_[0]) spec += std::string(",mode=") + dlgMode_;
        pendingDeckLinkSpec_ = spec;  // opened in Tick()
        dlgOpen_ = false;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) dlgOpen_ = false;
    ImGui::End();
#endif
}

void Station::BuildSignalLines(bool live) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!live || !showTimecode_) return;
    ImGui::Text("TC %s", timecode_.empty() ? "--:--:--:--" : timecode_.c_str());
    const double period = host_.displayPeriodMs ? host_.displayPeriodMs() : 16.7;
    ImGui::Text("Delay %.0f ms", delayMs_ + period);
    if (decklink_.GetState() != LiveState::Closed) {
        const int dark = decklink_.GetStats().noSignalEyes;
        if (dark & 1) ImGui::TextColored(kBad, decklink_.IsPair() ? "Left eye: NO SIGNAL" : "NO SIGNAL");
        if (dark & 2) ImGui::TextColored(kBad, "Right eye: NO SIGNAL");
    }
    if (decklink_.IsPair()) {
        const DeckLinkSource::PairStats ps = decklink_.GetPairStats();
        ImGui::Text("Eye skew %+.2f ms (max %.2f)  unpaired %llu", ps.lastSkewMs, ps.maxAbsSkewMs,
                    (unsigned long long)ps.unpaired);
        if (ps.timecode) ImGui::TextDisabled("paired by SMPTE timecode");
        else if (ps.hwClockPairing) ImGui::TextDisabled("no timecode: paired by the card's clock");
        else if (ps.separateCards) ImGui::TextColored(kWarn, "no timecode, two cards: newest frames paired - jam-sync both inputs");
        else if (ps.pairs) ImGui::TextDisabled("no timecode: newest frames paired");
    }
    ImGui::TextDisabled("capture to screen; camera + panel not included");
#else
    (void)live;
#endif
}

void Station::BuildRigSection(bool stereo) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!ImGui::CollapsingHeader("Rig check", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const rig::Result r = rig_.Last();
    const rig::Budget b = rig_.GetBudget();
    if (!stereo) {
        ImGui::TextDisabled("Needs a side-by-side stereo source.");
        return;
    }
    if (!r.ok || !rig_.Active(LiveProducer::NowSeconds())) {
        ImGui::TextDisabled("Measuring... (needs texture in the picture)");
        return;
    }
    char v[48];
    std::snprintf(v, sizeof(v), "%.1f px", r.verticalPx);
    Graded("Vertical error", v, std::fabs(r.verticalPx), b.maxVerticalPx);
    std::snprintf(v, sizeof(v), "%.2f deg", r.rollDeg);
    Graded("Roll", v, std::fabs(r.rollDeg), 0.5f);
    const std::vector<rig::Sample> h = rig_.History(1.0);
    const int sync = h.empty() ? -1 : h.back().eyeOffsetFrames;
    int late = 0;
    const int measured = MeasuredEyeSync(&late);
    if (measured >= 0) {  // measured from the pixels wins; the timecode figure is the source's word
        std::snprintf(v, sizeof(v), "%d frame%s%s", measured, measured == 1 ? "" : "s",
                      measured == 0 ? "" : late > 0 ? " (R late)" : " (L late)");
        Graded("Eye sync", v, (float)measured, 0.5f);
        if (sync >= 0 && sync != measured) ImGui::TextDisabled("timecode says %d", sync);
    } else if (sync >= 0) {
        std::snprintf(v, sizeof(v), "%d frame%s (TC)", sync, sync == 1 ? "" : "s");
        Graded("Eye sync", v, (float)sync, 0.5f);
    } else {
        ImGui::TextUnformatted("Eye sync");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.56f);
        ImGui::TextDisabled("no timecode / motion");
    }
    std::snprintf(v, sizeof(v), "dE %.1f", r.deltaE);
    Graded("Color match", v, r.deltaE, b.maxDeltaE);
    std::snprintf(v, sizeof(v), "%+.1f%% .. %+.1f%%", r.nearPct, r.farPct);
    const float over = std::max(b.nearPct - r.nearPct, r.farPct - b.farPct);
    Graded("Depth range", v, std::max(0.0f, over) + 0.01f, 0.05f);
    ImGui::TextDisabled("budget %+.1f%% .. %+.1f%%  (R: 60 s history)", b.nearPct, b.farPct);
    const std::vector<rig::Alert> alerts = rig_.Alerts();
    if (!alerts.empty()) {
        const rig::Alert& al = alerts.back();
        ImGui::PushStyleColor(ImGuiCol_Text, al.end < 0 ? kBad : kWarn);
        ImGui::TextWrapped("%s at %s%s%s.", al.what.c_str(), al.wallClock.c_str(),
                           al.timecode.empty() ? "" : ", TC ", al.timecode.c_str());
        ImGui::PopStyleColor();
    }
#else
    (void)stereo;
#endif
}

void Station::BuildSnapshotsSection() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    char head[48];
    std::snprintf(head, sizeof(head), "Snapshots - %zu  (N)###snaps", snapshots_.size());
    if (!ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::Button("Snapshot")) TakeSnapshot("");
    for (size_t k = snapshots_.size(); k-- > 0;) {
        Snapshot& s = snapshots_[k];
        ImGui::PushID((int)k);
        ImGui::TextDisabled("%s%s%s", s.clock.c_str(), s.timecode.empty() ? "" : "  TC ", s.timecode.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##note", "note", s.note, sizeof(s.note));
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveNote(k);
        ImGui::PopID();
    }
#endif
}

void Station::BuildHistoryStrip(float alpha, bool stereo) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!showHistory_ || !stereo || alpha < 0.01f) return;
    const std::vector<rig::Sample> hist = rig_.History(60.0);
    if (hist.empty()) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    // Between the two panels (ProController's panel width), along the bottom.
    const float panelW = std::clamp(W * 0.24f, 280.0f, 440.0f);
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.86f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(std::max(200.0f, W - 2.0f * panelW - 48.0f), H * 0.15f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##rig_history", nullptr, fl)) {
        const rig::Budget b = rig_.GetBudget();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float labelW = avail.x * 0.14f;
        const float x0 = p0.x + labelW, x1 = p0.x + avail.x;
        const float rowH = (avail.y - ImGui::GetTextLineHeight()) * 0.5f;
        const double now = hist.back().t;
        auto X = [&](double t) { return x0 + (float)((t - (now - 60.0)) / 60.0) * (x1 - x0); };
        const ImU32 grid = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f));
        const ImU32 limit = ImGui::GetColorU32(ImVec4(kBad.x, kBad.y, kBad.z, 0.8f));
        const ImU32 band = ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f));
        const ImU32 line = ImGui::GetColorU32(kAccent);
        const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        // depth band (near..far) against the budget
        {
            const float y0 = p0.y, y1 = p0.y + rowH - 4.0f;
            dl->AddText(ImVec2(p0.x, y0 + rowH * 0.3f), dim, "Depth");
            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), grid);
            float lo = std::min(b.nearPct, -0.5f) - 0.6f, hi = std::max(b.farPct, 0.5f) + 0.6f;
            for (const rig::Sample& s : hist)
                if (s.ok) {
                    lo = std::min(lo, s.nearPct - 0.2f);
                    hi = std::max(hi, s.farPct + 0.2f);
                }
            auto Y = [&](float pct) { return y1 - (pct - lo) / (hi - lo) * (y1 - y0); };
            for (float lim : {b.nearPct, b.farPct})
                for (float x = x0; x < x1; x += 10.0f) dl->AddLine(ImVec2(x, Y(lim)), ImVec2(std::min(x + 5.0f, x1), Y(lim)), limit, 1.5f);
            dl->AddLine(ImVec2(x0, Y(0.0f)), ImVec2(x1, Y(0.0f)), grid);
            for (size_t i = 1; i < hist.size(); ++i) {
                const rig::Sample& a = hist[i - 1];
                const rig::Sample& c = hist[i];
                if (!a.ok || !c.ok || c.t - a.t > 2.0) continue;
                dl->AddQuadFilled(ImVec2(X(a.t), Y(a.nearPct)), ImVec2(X(c.t), Y(c.nearPct)), ImVec2(X(c.t), Y(c.farPct)),
                                  ImVec2(X(a.t), Y(a.farPct)), band);
                const bool over = c.nearPct < b.nearPct || c.farPct > b.farPct;
                dl->AddLine(ImVec2(X(a.t), Y(a.nearPct)), ImVec2(X(c.t), Y(c.nearPct)), over ? limit : line, 2.0f);
                dl->AddLine(ImVec2(X(a.t), Y(a.farPct)), ImVec2(X(c.t), Y(c.farPct)), line, 1.5f);
            }
        }
        // vertical error
        {
            const float y0 = p0.y + rowH, y1 = p0.y + 2.0f * rowH - 4.0f;
            dl->AddText(ImVec2(p0.x, y0 + rowH * 0.3f), dim, "Vertical");
            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), grid);
            // Scale to the larger of twice the limit and the worst sample, so an error past
            // the limit shows as a line above the dashed limit, not one pinned to the frame.
            float range = std::max(b.maxVerticalPx * 2.0f, 2.0f);
            for (const rig::Sample& s : hist)
                if (s.ok) range = std::max(range, std::fabs(s.verticalPx) * 1.25f);
            auto Y = [&](float px) { return (y0 + y1) * 0.5f - std::clamp(px / range, -1.0f, 1.0f) * (y1 - y0) * 0.5f; };
            for (float lim : {b.maxVerticalPx, -b.maxVerticalPx})
                for (float x = x0; x < x1; x += 10.0f) dl->AddLine(ImVec2(x, Y(lim)), ImVec2(std::min(x + 5.0f, x1), Y(lim)), limit, 1.0f);
            for (size_t i = 1; i < hist.size(); ++i) {
                const rig::Sample& a = hist[i - 1];
                const rig::Sample& c = hist[i];
                if (!a.ok || !c.ok || c.t - a.t > 2.0) continue;
                dl->AddLine(ImVec2(X(a.t), Y(a.verticalPx)), ImVec2(X(c.t), Y(c.verticalPx)),
                            std::fabs(c.verticalPx) > b.maxVerticalPx ? limit : ImGui::GetColorU32(kOk), 2.0f);
            }
        }
        const char* ticks[] = {"-60 s", "-45 s", "-30 s", "-15 s", "now"};
        for (int i = 0; i < 5; ++i) {
            const float x = x0 + (x1 - x0) * (float)i / 4.0f;
            const ImVec2 ts = ImGui::CalcTextSize(ticks[i]);
            dl->AddText(ImVec2(std::min(x - ts.x * 0.5f, x1 - ts.x), p0.y + 2.0f * rowH), dim, ticks[i]);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
#else
    (void)alpha;
    (void)stereo;
#endif
}

}  // namespace mp::pro
