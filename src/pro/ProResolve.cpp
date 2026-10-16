// SPDX-License-Identifier: Apache-2.0
//
// The station's side of the live Resolve panel (pro/ResolveLink.h has the file protocol):
// writes station_status.json, reads resolve_link.json and resolve_commands.jsonl, and
// applies the follow decision to the player. Runs from ProController::Tick on the main
// thread; all file work is throttled (status 4 Hz, link 10 Hz) and never blocks on Resolve.
#include "pro/ProController.h"

#include "Log.h"
#include "media/LiveProducer.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include "imgui.h"
#endif

namespace mp::pro {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Write via a temp file + rename, so the panel never reads a half-written file.
void WriteAtomically(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return;
        o << text;
    }
    fs::rename(tmp, path, ec);
    if (ec) {  // Windows: the panel holds the file open for a moment - try next tick
        fs::remove(tmp, ec);
    }
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

long long Stamp(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? -1 : (long long)t.time_since_epoch().count();
}

std::string Absolute(const std::string& p) {
    if (p.empty()) return p;
    std::error_code ec;
    const fs::path a = fs::absolute(fs::path(p), ec);
    return ec ? p : a.lexically_normal().generic_string();
}

}  // namespace

void ProController::TickResolve(const Status& st) {
    const std::string dirStr = station_.Folder();
    if (dirStr.empty()) return;
    const fs::path dir(dirStr);
    const double now = LiveProducer::NowSeconds();

    // Status -> panel.
    if (now >= nextStatusSec_) {
        nextStatusSec_ = now + 0.25;
        std::error_code ec;
        fs::create_directories(dir, ec);
        json s;
        s["app"] = ProductName();
        s["seq"] = ++statusSeq_;
        s["input"] = st.live ? station_.InputName() : std::string();
        s["source"] = st.sourceLabel;
        s["kind"] = st.sourceKind;
        s["live"] = st.live;
        s["timecode"] = station_.Timecode();
        s["rig"] = json::parse(station_.RigStatusJson(), nullptr, false);
        const std::string media = host_.currentMedia ? Absolute(host_.currentMedia()) : std::string();
        if (!media.empty()) {
            s["file"] = {{"path", media},
                         {"seconds", host_.mediaPosition ? host_.mediaPosition() : -1.0},
                         {"paused", host_.mediaPaused && host_.mediaPaused()}};
        }
        s["follow"] = followResolve_;
        s["following"] = followResolve_ && follower_.Active(now) && link_.follow;
        s["view"] = ViewModeName(mode_);
        s["look"] = lutName_;
        WriteAtomically(dir / "station_status.json", s.dump(2));
    }

    if (now < nextLinkPollSec_) return;
    nextLinkPollSec_ = now + 0.1;

    // Commands <- panel (appended lines; start at the end so old commands never replay).
    const fs::path cmdPath = dir / "resolve_commands.jsonl";
    std::error_code ec;
    const long long size = fs::exists(cmdPath, ec) ? (long long)fs::file_size(cmdPath, ec) : 0;
    if (commandsOffset_ < 0 || size < commandsOffset_) {
        commandsOffset_ = size;  // first look, or the file was cleared
    } else if (size > commandsOffset_) {
        std::ifstream in(cmdPath, std::ios::binary);
        in.seekg(commandsOffset_);
        std::string line;
        long long consumed = commandsOffset_;
        while (std::getline(in, line)) {
            if (in.eof()) break;  // a line still being written: read it next time
            consumed += (long long)line.size() + 1;
            const resolve::Command c = resolve::ParseCommand(line);
            if (c.cmd.empty() || (c.id != 0 && c.id <= lastCommandId_)) continue;
            if (c.id != 0) lastCommandId_ = c.id;
            if (c.cmd == "snapshot") {
                station_.TakeSnapshot(c.note.empty() ? std::string("From Resolve") : c.note);
            } else if (c.cmd == "follow_on" || c.cmd == "follow_off") {
                followResolve_ = c.cmd == "follow_on";
            } else {
                LOG_WARN("Resolve panel: unknown command '%s'", c.cmd.c_str());
            }
        }
        commandsOffset_ = consumed;
    }

    // Playhead <- panel.
    const fs::path linkPath = dir / "resolve_link.json";
    const long long stamp = Stamp(linkPath);
    if (stamp < 0 || stamp == linkFileStamp_) return;
    linkFileStamp_ = stamp;
    const resolve::LinkSample s = resolve::ParseLink(ReadAll(linkPath));
    if (!s.ok) return;  // torn or not ours: the next write will do
    link_ = s;
    if (!followResolve_ || !s.follow || st.live) {
        follower_.Update(s, now, "", -1.0, true, false);  // keep its clock, act on nothing
        return;
    }
    const std::string local = host_.currentMedia ? Absolute(host_.currentMedia()) : std::string();
    const std::string clip = Absolute(s.clipPath);
    resolve::LinkSample abs = s;
    abs.clipPath = clip;
    const bool exists = !clip.empty() && fs::exists(fs::path(clip), ec);
    const double pos = host_.mediaPosition ? host_.mediaPosition() : -1.0;
    const bool paused = host_.mediaPaused && host_.mediaPaused();
    const resolve::Action a = follower_.Update(abs, now, local, pos, paused, exists);
    switch (a.kind) {
    case resolve::Action::Open:
        if (host_.reloadMedia && host_.reloadMedia(a.path) && host_.seekMedia) host_.seekMedia(a.seconds, a.paused);
        if (!a.paused && host_.setMediaPaused) host_.setMediaPaused(false);
        if (host_.toast) host_.toast("Following Resolve: " + fs::path(a.path).filename().string());
        break;
    case resolve::Action::Seek:
        if (host_.seekMedia) host_.seekMedia(a.seconds, a.paused);
        if (!a.paused && host_.setMediaPaused) host_.setMediaPaused(false);
        break;
    case resolve::Action::Play:
        if (host_.setMediaPaused) host_.setMediaPaused(false);
        break;
    case resolve::Action::Pause:
        if (host_.setMediaPaused) host_.setMediaPaused(true);
        break;
    default: break;
    }
}

void ProController::BuildResolveLines() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    const double now = LiveProducer::NowSeconds();
    if (!follower_.Active(now)) {
        ImGui::TextDisabled("Resolve panel: not running");
        return;
    }
    ImGui::Text("Resolve %s  TC %s", link_.timeline.c_str(), link_.playheadTc.c_str());
    ImGui::Checkbox("Follow Resolve", &followResolve_);
    ImGui::SameLine();
    if (!link_.follow)
        ImGui::TextDisabled("(off in the panel)");
    else if (followResolve_)
        ImGui::TextDisabled(follower_.ResolvePlaying() ? "playing" : "parked");
#endif
}

}  // namespace mp::pro
