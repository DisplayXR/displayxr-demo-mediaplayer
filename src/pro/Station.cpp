// SPDX-License-Identifier: Apache-2.0
#include "pro/Station.h"

#include "Log.h"

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    if (!inputs_.empty()) SwitchInput(0);
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
        testSignal_.Stop();
        ok = decklink_.Open(c);
        if (ok && host_.enterLive) host_.enterLive(&decklink_, decklink_.DeviceName());
        if (!ok && host_.toast) host_.toast("DeckLink: " + decklink_.LastError());
    } else if (kind == "test") {
        TestSignalConfig c = opt_.testCfg;
        if (!c.ParseSpec(args)) {
            if (host_.toast) host_.toast("Input '" + d.name + "': bad test spec " + args);
            return false;
        }
        decklink_.Stop();
        ok = testSignal_.Open(c);
        if (ok && host_.enterLive) host_.enterLive(&testSignal_, testSignal_.DeviceName());
    } else if (kind == "camera") {
        decklink_.Stop();
        testSignal_.Stop();
        ok = host_.openCamera && host_.openCamera(args);
    } else if (kind == "url") {
        decklink_.Stop();
        testSignal_.Stop();
        ok = host_.openUrl && host_.openUrl(args);
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
            TakeSnapshot("");
            return true;
        case SDLK_J:
            SwitchInput(inputIndex_ + 1);
            return true;
        default:
            return false;
    }
}

void Station::OnFrame(const FrameRing::Frame& f, StereoLayout layout, bool live, int eyeOffsetFrames) {
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
        rig_.Submit(f, layout, live ? eyeOffsetFrames : -1, f.timecode);
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
    s.image = prefix + "_atlas.png";
    s.notePath = prefix + "_note.json";
    s.timecode = timecode_;
    s.clock = clock;
    s.rig = rig_.Last();
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
        j["time"] = a.wallClock;
        j["duration_s"] = a.end - a.start;
        std::ofstream((std::filesystem::path(dir) / "station_alerts.jsonl").string(), std::ios::app) << j.dump() << "\n";
        newest = std::max(newest, a.end);
    }
    alertsLoggedUntil_ = newest;
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
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.48f);
    const ImVec4 c = magnitude <= limit * 0.75f ? kOk : magnitude <= limit ? kWarn : kBad;
    ImGui::TextColored(c, "%s", value);
}
}  // namespace
#endif

void Station::BuildInputsSection(const std::string& liveName, bool live) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!ImGui::CollapsingHeader("Inputs  (J)", ImGuiTreeNodeFlags_DefaultOpen)) return;
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

void Station::BuildSignalLines(bool live) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!live || !showTimecode_) return;
    ImGui::Text("TC %s", timecode_.empty() ? "--:--:--:--" : timecode_.c_str());
    const double period = host_.displayPeriodMs ? host_.displayPeriodMs() : 16.7;
    ImGui::Text("Delay %.0f ms", delayMs_ + period);
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
    if (sync >= 0) {
        std::snprintf(v, sizeof(v), "%d frame%s", sync, sync == 1 ? "" : "s");
        Graded("Eye sync", v, (float)sync, 0.5f);
    } else {
        ImGui::TextUnformatted("Eye sync");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.48f);
        ImGui::TextDisabled("no timecode");
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
    if (!ImGui::CollapsingHeader(head)) return;
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
    ImGui::SetNextWindowPos(ImVec2(W * 0.38f, H * 0.86f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.56f, H * 0.15f), ImGuiCond_Always);
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
            const float range = std::max(b.maxVerticalPx * 2.0f, 2.0f);
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
