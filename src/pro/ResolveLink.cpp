// SPDX-License-Identifier: Apache-2.0
#include "pro/ResolveLink.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace mp::pro::resolve {

using json = nlohmann::json;

namespace {

std::string Str(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double Num(const json& j, const char* k, double def) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : def;
}

}  // namespace

LinkSample ParseLink(const std::string& text) {
    LinkSample s;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return s;
    s.seq = (long long)Num(j, "seq", 0.0);
    s.timeline = Str(j, "timeline");
    s.playheadTc = Str(j, "playhead_tc");
    s.timelineFps = Num(j, "timeline_fps", 0.0);
    auto clip = j.find("clip");
    if (clip != j.end() && clip->is_object()) {
        s.clipPath = Str(*clip, "path");
        s.clipSeconds = Num(*clip, "seconds", -1.0);
        s.clipFps = Num(*clip, "fps", 0.0);
    }
    s.written = Num(j, "written", -1.0);
    auto f = j.find("follow");
    s.follow = f == j.end() || !f->is_boolean() || f->get<bool>();
    s.ok = !s.playheadTc.empty();
    return s;
}

Command ParseCommand(const std::string& line) {
    Command c;
    const json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return c;
    c.cmd = Str(j, "cmd");
    c.note = Str(j, "note");
    c.id = (long long)Num(j, "id", 0.0);
    return c;
}

bool SamePath(const std::string& a, const std::string& b) {
#if defined(_WIN32)
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = (char)std::tolower((unsigned char)a[i]), y = (char)std::tolower((unsigned char)b[i]);
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x != y) return false;
    }
    return true;
#else
    return a == b;
#endif
}

Action Follower::Update(const LinkSample& s, double now, const std::string& localPath, double localSeconds,
                        bool localPaused, bool fileExists) {
    Action a;
    if (!s.ok || s.seq == lastSeq_) return a;

    // Is Resolve playing? Compare how far the source time moved with how much wall time
    // passed since the previous sample of the same clip. Forward at roughly real time =
    // playing; still, backwards, or a jump (much faster than real time: a click on the
    // timeline, scrubbing) = parked, so the station lands on the frame instead.
    const bool sameClip = !lastClip_.empty() && SamePath(s.clipPath, lastClip_);
    // Movement is timed on the panel's clock when it says when it wrote the sample: the
    // station polls at 10 Hz and Resolve's UI timer jitters, so arrival times would make
    // 200 ms of playback look like 100 or 300 ms and flip playing <-> parked.
    const double stamp = s.written > 0.0 ? s.written : now;
    const double dt = lastStamp_ >= 0.0 ? stamp - lastStamp_ : 0.0;
    if (sameClip && dt > 0.0 && dt <= staleAfter && lastClipSec_ >= 0.0 && s.clipSeconds >= 0.0) {
        const double moved = s.clipSeconds - lastClipSec_;
        playing_ = moved >= movingRatio * dt && moved <= 3.0 * dt + 0.1;
    } else {
        playing_ = false;
    }
    lastSeq_ = s.seq;
    lastSeen_ = now;
    lastStamp_ = stamp;
    lastClipSec_ = s.clipSeconds;
    if (!sameClip) lastSeekTarget_ = -1.0;
    lastClip_ = s.clipPath;
    if (!failed_.empty() && !SamePath(s.clipPath, failed_)) failed_.clear();  // Resolve moved on

    if (s.clipPath.empty() || s.clipSeconds < 0.0) return a;  // a gap, a title, a generator
    if (!SamePath(s.clipPath, localPath)) {
        if (!fileExists) return a;  // Resolve's media is not reachable from the station
        if (!failed_.empty()) return a;  // the station already failed to open this one
        a.kind = Action::Open;
        a.path = s.clipPath;
        a.seconds = s.clipSeconds;
        a.paused = !playing_;
        return a;
    }
    if (localSeconds < 0.0) return a;  // the same file, but not a video (a still)
    const double drift = localSeconds - s.clipSeconds;
    if (playing_) {
        lastSeekTarget_ = -1.0;  // once Resolve parks again, the station lands afresh
        if (std::fabs(drift) > playingTolerance) {
            a.kind = Action::Seek;
            a.seconds = s.clipSeconds;
            a.paused = false;
        } else if (localPaused) {
            a.kind = Action::Play;
        }
        return a;
    }
    const double tol = parkedToleranceFrames / (s.clipFps > 1.0 ? s.clipFps : 24.0);
    const bool sameTarget = lastSeekTarget_ >= 0.0 && std::fabs(s.clipSeconds - lastSeekTarget_) < 1e-6;
    if (std::fabs(drift) > tol && !sameTarget) {
        a.kind = Action::Seek;
        a.seconds = s.clipSeconds;
        a.paused = true;
        lastSeekTarget_ = s.clipSeconds;  // sent once: never re-seek to an unchanged target
    } else if (!localPaused) {
        a.kind = Action::Pause;
    }
    return a;
}

}  // namespace mp::pro::resolve
