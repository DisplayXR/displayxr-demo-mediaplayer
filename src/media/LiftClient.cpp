// SPDX-License-Identifier: Apache-2.0
//
// LiftClient — see LiftClient.h. Pure state machine over the XR_DXR_lift entry-point
// table; no loader, no graphics, no provider names.

#include "media/LiftClient.h"

#include "Log.h"

#include <algorithm>
#include <cstring>

namespace mp::lift {

namespace {

constexpr XrFlags64 Bit(uint32_t v) { return (XrFlags64)1 << v; }

const char* FailureText(XrLiftFailureReasonDXR r) {
    switch (r) {
        case XR_LIFT_FAILURE_REASON_LICENCE_DXR: return "its licence is missing or expired";
        case XR_LIFT_FAILURE_REASON_LOAD_DXR: return "it did not load";
        case XR_LIFT_FAILURE_REASON_PLATFORM_DXR: return "this GPU or OS is not supported";
        case XR_LIFT_FAILURE_REASON_NETWORK_DXR: return "its download failed";
        case XR_LIFT_FAILURE_REASON_VERSION_DXR: return "its version is not supported";
        case XR_LIFT_FAILURE_REASON_RESOURCE_DXR: return "not enough memory";
        case XR_LIFT_FAILURE_REASON_MODEL_DXR: return "its model is missing or damaged";
        case XR_LIFT_FAILURE_REASON_DEVICE_LOST_DXR: return "the GPU was reset";
        case XR_LIFT_FAILURE_REASON_CRASHED_DXR: return "it keeps crashing";
        default: return nullptr;
    }
}

inline uint8_t Clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

}  // namespace

const char* StateName(State s) {
    switch (s) {
        case State::Off: return "off";
        case State::Requested: return "requested";
        case State::Warming: return "warming";
        case State::Active: return "active";
        case State::Unavailable: return "unavailable";
    }
    return "off";
}

const char* AvailabilityName(Availability a) {
    switch (a) {
        case Availability::Hidden: return "hidden";
        case Availability::Disabled: return "disabled";
        case Availability::Enabled: return "enabled";
    }
    return "hidden";
}

const char* TaskStateName(XrLiftTaskStateDXR s) {
    switch (s) {
        case XR_LIFT_TASK_STATE_UNKNOWN_DXR: return "unknown";
        case XR_LIFT_TASK_STATE_READY_DXR: return "ready";
        case XR_LIFT_TASK_STATE_WARMING_DXR: return "warming";
        case XR_LIFT_TASK_STATE_ABSENT_DXR: return "absent";
        case XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR: return "incompatible";
        case XR_LIFT_TASK_STATE_UNLICENSED_DXR: return "unlicensed";
        case XR_LIFT_TASK_STATE_FAILED_DXR: return "failed";
        case XR_LIFT_TASK_STATE_DISABLED_DXR: return "disabled";
        case XR_LIFT_TASK_STATE_INSTALLED_DXR: return "installed";
        case XR_LIFT_TASK_STATE_DOWNLOADING_DXR: return "downloading";
        case XR_LIFT_TASK_STATE_EVICTED_DXR: return "evicted";
        default: return "unknown";
    }
}

bool Candidate::Terminal() const {
    return state == XR_LIFT_TASK_STATE_ABSENT_DXR || state == XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR ||
           state == XR_LIFT_TASK_STATE_UNLICENSED_DXR || state == XR_LIFT_TASK_STATE_FAILED_DXR ||
           state == XR_LIFT_TASK_STATE_DISABLED_DXR;
}

bool LiftClient::Terminal(XrLiftTaskStateDXR s) {
    return s == XR_LIFT_TASK_STATE_ABSENT_DXR || s == XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR ||
           s == XR_LIFT_TASK_STATE_UNLICENSED_DXR || s == XR_LIFT_TASK_STATE_FAILED_DXR ||
           s == XR_LIFT_TASK_STATE_DISABLED_DXR;
}

// ---- session binding ---------------------------------------------------------------

void LiftClient::Attach(const Api& api, ::XrSession session) {
    Detach();
    api_ = api;
    session_ = session;
    if (!Attached()) return;
    Refresh();
    LOG_INFO("2D→3D conversion: VIEWS_MONO task %s (%s)",
             taskPresent_ ? TaskStateName(task_.state) : "absent",
             AvailabilityName(GetAvailability()));
}

void LiftClient::Detach() {
    DestroyStream();
    want_ = false;
    haveResult_ = false;
    unavailableWhy_.clear();
    session_ = XR_NULL_HANDLE;
    taskPresent_ = false;
    enumerateUnsupported_ = createUnsupported_ = false;
    candidates_.clear();
    preferred_.clear();
}

void LiftClient::Refresh() {
    if (!Attached()) return;
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint32_t n = 0;
        XrResult r = api_.enumerateTasks(session_, 0, &n, nullptr);
        if (r == XR_ERROR_FEATURE_UNSUPPORTED) {
            enumerateUnsupported_ = true;
            taskPresent_ = false;
            return;
        }
        if (XR_FAILED(r)) {
            LOG_WARN("xrEnumerateLiftTasksDXR failed: XrResult=%d", (int)r);
            return;  // keep what we knew
        }
        enumerateUnsupported_ = false;
        std::vector<XrLiftTaskPropertiesDXR> tasks(n);
        for (auto& t : tasks) {
            t = XrLiftTaskPropertiesDXR{};
            t.type = XR_TYPE_LIFT_TASK_PROPERTIES_DXR;
        }
        if (n > 0) {
            r = api_.enumerateTasks(session_, n, &n, tasks.data());
            if (r == XR_ERROR_SIZE_INSUFFICIENT) continue;  // the set grew in between
            if (XR_FAILED(r)) {
                LOG_WARN("xrEnumerateLiftTasksDXR failed: XrResult=%d", (int)r);
                return;
            }
        }
        taskPresent_ = false;
        for (uint32_t i = 0; i < n && i < tasks.size(); ++i) {
            if (tasks[i].task == XR_LIFT_TASK_VIEWS_MONO_DXR) {
                task_ = tasks[i];
                task_.next = nullptr;
                taskPresent_ = true;
            }
        }
        RefreshCandidates();
        return;
    }
}

void LiftClient::RefreshCandidates() {
    if (!api_.enumerateCandidates || session_ == XR_NULL_HANDLE) {
        candidates_.clear();
        return;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint32_t n = 0;
        XrResult r =
            api_.enumerateCandidates(session_, XR_LIFT_TASK_VIEWS_MONO_DXR, 0, &n, nullptr);
        if (XR_FAILED(r)) {
            candidates_.clear();  // not offered here: no selector
            return;
        }
        std::vector<XrLiftTaskCandidateDXR> raw(n);
        for (auto& c : raw) {
            c = XrLiftTaskCandidateDXR{};
            c.type = XR_TYPE_LIFT_TASK_CANDIDATE_DXR;
        }
        if (n > 0) {
            r = api_.enumerateCandidates(session_, XR_LIFT_TASK_VIEWS_MONO_DXR, n, &n, raw.data());
            if (r == XR_ERROR_SIZE_INSUFFICIENT) continue;
            if (XR_FAILED(r)) {
                candidates_.clear();
                return;
            }
        }
        std::vector<Candidate> before;
        before.swap(candidates_);
        for (uint32_t i = 0; i < n && i < raw.size(); ++i) {
            Candidate c;
            c.id.assign(raw[i].providerId, strnlen(raw[i].providerId, sizeof(raw[i].providerId)));
            c.name.assign(raw[i].displayName,
                          strnlen(raw[i].displayName, sizeof(raw[i].displayName)));
            if (c.name.empty()) c.name = c.id;
            c.state = raw[i].state;
            c.failureReason = raw[i].failureReason;
            c.hosting = raw[i].hosting;
            c.isUserDefault = raw[i].isUserDefault == XR_TRUE;
            c.isSelected = raw[i].isSelected == XR_TRUE;
            if (!c.id.empty()) candidates_.push_back(std::move(c));
        }
        // Log the list when it changes (lifecycle, not per frame). The strings are the
        // runtime's, verbatim.
        bool changed = before.size() != candidates_.size();
        for (size_t i = 0; !changed && i < before.size(); ++i)
            changed = before[i].id != candidates_[i].id || before[i].state != candidates_[i].state ||
                      before[i].isUserDefault != candidates_[i].isUserDefault ||
                      before[i].isSelected != candidates_[i].isSelected;
        if (changed) {
            LOG_INFO("2D→3D conversion: %zu provider candidate(s)", candidates_.size());
            for (const Candidate& c : candidates_)
                LOG_INFO("  candidate '%s' (id %s): %s%s%s%s", c.name.c_str(), c.id.c_str(),
                         TaskStateName(c.state),
                         c.hosting == XR_LIFT_PROVIDER_HOSTING_REMOTE_DXR ? ", remote" : "",
                         c.isUserDefault ? ", user default" : "",
                         c.isSelected ? ", selected" : "");
        }
        // A pick that is no longer a candidate would be a silent no-op hint: drop it.
        if (!preferred_.empty()) {
            bool found = false;
            for (const Candidate& c : candidates_) found = found || c.id == preferred_;
            if (!found) {
                LOG_INFO(
                    "2D→3D conversion: picked provider is no longer a candidate; default again");
                preferred_.clear();
            }
        }
        return;
    }
}

int LiftClient::EffectiveCandidate() const {
    int def = -1, sel = -1;
    for (size_t i = 0; i < candidates_.size(); ++i) {
        if (!preferred_.empty() && candidates_[i].id == preferred_) return (int)i;
        if (def < 0 && candidates_[i].isUserDefault) def = (int)i;
        if (sel < 0 && candidates_[i].isSelected) sel = (int)i;
    }
    return def >= 0 ? def : sel;
}

std::string LiftClient::SetPreferred(const std::string& id) {
    if (!api_.enumerateCandidates) return "this runtime offers no provider choice";
    std::string want = id;
    if (!want.empty()) {
        const Candidate* hit = nullptr;
        for (const Candidate& c : candidates_)
            if (c.id == want) hit = &c;
        if (!hit) return "no such 2D->3D provider: " + id;
        if (hit->Terminal()) return hit->name + " is " + TaskStateName(hit->state);
        if (hit->isUserDefault) want.clear();  // the default needs no hint
    }
    if (want == preferred_) return std::string();
    preferred_ = want;
    LOG_INFO("2D→3D conversion: provider %s", preferred_.empty() ? "default" : "picked");
    if (want_) Reset();  // a live stream does not move on its own: re-create it with the hint
    return std::string();
}

bool LiftClient::OnEvent(const XrEventDataBaseHeader* ev) {
    if (!ev) return false;
    if (ev->type == XR_TYPE_EVENT_DATA_LIFT_TASK_STATE_CHANGED_DXR) {
        const auto* e = reinterpret_cast<const XrEventDataLiftTaskStateChangedDXR*>(ev);
        if (e->session != session_ || e->task != XR_LIFT_TASK_VIEWS_MONO_DXR) return true;
        if (e->state == XR_LIFT_TASK_STATE_ABSENT_DXR) {
            taskPresent_ = false;
        } else if (!taskPresent_) {
            Refresh();  // first sighting: take the whole contract, not just the state
        } else {
            task_.state = e->state;
            task_.failureReason = e->failureReason;
            task_.progress = e->progress;
        }
        LOG_INFO("2D→3D conversion: task %s -> %s", TaskStateName(e->previousState),
                 TaskStateName(e->state));
    } else if (ev->type == XR_TYPE_EVENT_DATA_LIFT_PROVIDERS_CHANGED_DXR) {
        const auto* e = reinterpret_cast<const XrEventDataLiftProvidersChangedDXR*>(ev);
        if (e->session != session_) return true;
        createUnsupported_ = false;  // a new provider set may serve the task now
        Refresh();
        LOG_INFO("2D→3D conversion: providers changed; task %s",
                 taskPresent_ ? TaskStateName(task_.state) : "absent");
    } else {
        return false;
    }
    // The toggle is on but the stream could not be made: try again now that the
    // task's state moved (a terminal state is never retried).
    if (want_ && stream_ == XR_NULL_HANDLE && GetAvailability() == Availability::Enabled)
        CreateStream();
    return true;
}

void LiftClient::Tick(double nowSec) {
    if (!Attached() || enumerateUnsupported_ || createUnsupported_) return;
    const bool settled = GetAvailability() == Availability::Enabled && !TaskWaiting() &&
                         (!want_ || stream_ != XR_NULL_HANDLE);
    if (settled) return;
    if (nowSec - lastEnumSec_ < (want_ ? kStaleSeconds : kIdlePollSeconds)) return;
    lastEnumSec_ = nowSec;
    Refresh();
    if (want_ && stream_ == XR_NULL_HANDLE && GetAvailability() == Availability::Enabled)
        CreateStream();
}

// ---- availability --------------------------------------------------------------------

Availability LiftClient::GetAvailability() const {
    if (!Attached()) return Availability::Hidden;
    if (enumerateUnsupported_ || createUnsupported_ || !taskPresent_) return Availability::Disabled;
    if (Terminal(task_.state)) return Availability::Disabled;
    if ((task_.inputKinds & Bit(XR_LIFT_INPUT_KIND_MONO_DXR)) == 0) return Availability::Disabled;
    if ((task_.imageKinds & Bit(XR_LIFT_IMAGE_KIND_CPU_DXR)) == 0 ||
        (task_.exportKinds & Bit(XR_LIFT_IMAGE_KIND_CPU_DXR)) == 0)
        return Availability::Disabled;
    if ((task_.lanes & XR_LIFT_LANE_STREAM_BIT_DXR) == 0) return Availability::Disabled;
    return Availability::Enabled;
}

bool LiftClient::TaskWaiting() const {
    if (!taskPresent_) return false;
    switch (task_.state) {
        case XR_LIFT_TASK_STATE_UNKNOWN_DXR:
        case XR_LIFT_TASK_STATE_WARMING_DXR:
        case XR_LIFT_TASK_STATE_INSTALLED_DXR:
        case XR_LIFT_TASK_STATE_DOWNLOADING_DXR:
        case XR_LIFT_TASK_STATE_EVICTED_DXR: return true;
        default: return false;
    }
}

std::string LiftClient::Reason() const {
    if (want_ && !unavailableWhy_.empty()) return unavailableWhy_;
    if (!Attached()) return std::string();
    if (enumerateUnsupported_ || createUnsupported_)
        return "2D->3D conversion is not available for this session";
    if (!taskPresent_) return "No 2D->3D converter is installed";
    switch (task_.state) {
        case XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR:
            return "The 2D->3D converter cannot run on this system";
        case XR_LIFT_TASK_STATE_UNLICENSED_DXR: return "The 2D->3D converter is not licensed";
        case XR_LIFT_TASK_STATE_DISABLED_DXR: return "2D->3D conversion is switched off";
        case XR_LIFT_TASK_STATE_FAILED_DXR: {
            const char* why = FailureText(task_.failureReason);
            return why ? std::string("The 2D->3D converter failed: ") + why
                       : std::string("The 2D->3D converter failed");
        }
        default: break;
    }
    if ((task_.inputKinds & Bit(XR_LIFT_INPUT_KIND_MONO_DXR)) == 0)
        return "The 2D->3D converter takes no single-picture input here";
    if ((task_.imageKinds & Bit(XR_LIFT_IMAGE_KIND_CPU_DXR)) == 0 ||
        (task_.exportKinds & Bit(XR_LIFT_IMAGE_KIND_CPU_DXR)) == 0)
        return "The 2D->3D converter cannot exchange pictures with this player yet";
    if ((task_.lanes & XR_LIFT_LANE_STREAM_BIT_DXR) == 0)
        return "The 2D->3D converter does not convert continuously";
    return std::string();
}

State LiftClient::GetState() const {
    if (!want_) return State::Off;
    if (stream_ == XR_NULL_HANDLE) return State::Unavailable;
    if (!taskPresent_ || Terminal(task_.state)) return State::Unavailable;
    if (haveResult_) return State::Active;
    return TaskWaiting() ? State::Warming : State::Requested;
}

// ---- the toggle ----------------------------------------------------------------------

bool LiftClient::Start(bool video) {
    if (!Attached()) return false;
    if (want_ && stream_ != XR_NULL_HANDLE && video == video_) return true;
    DestroyStream();
    want_ = true;
    video_ = video;
    haveResult_ = false;
    lastW_ = lastH_ = 0;
    unavailableWhy_.clear();
    if (GetAvailability() != Availability::Enabled) {
        SetUnavailable(Reason());
        return false;
    }
    return CreateStream();
}

void LiftClient::Stop() {
    DestroyStream();
    want_ = false;
    haveResult_ = false;
    unavailableWhy_.clear();
}

void LiftClient::Reset() {
    if (!want_) return;
    DestroyStream();
    if (GetAvailability() == Availability::Enabled) CreateStream();
}

bool LiftClient::CreateStream() {
    if (!Attached() || stream_ != XR_NULL_HANDLE) return stream_ != XR_NULL_HANDLE;
    XrLiftTaskStreamCreateInfoDXR task{};
    task.type = XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR;
    task.task = XR_LIFT_TASK_VIEWS_MONO_DXR;
    task.inputKind = XR_LIFT_INPUT_KIND_MONO_DXR;
    task.qualityHint = video_ ? XR_LIFT_QUALITY_HINT_DEFAULT_DXR : XR_LIFT_QUALITY_HINT_QUALITY_DXR;
    task.viewCount = 2;  // one side-by-side pair: the player's own stereo path shows it
    task.requiredOutputs = 0;
    task.exportKind = XR_LIFT_IMAGE_KIND_CPU_DXR;

    XrLiftStreamCreateInfoDXR ci{};
    ci.type = XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR;
    ci.next = &task;
    ci.mode = XR_LIFT_MODE_SBS_DXR;  // the legacy mode matching VIEWS_MONO with 2 views
    ci.contentHint = video_ ? XR_LIFT_CONTENT_HINT_VIDEO_DXR : XR_LIFT_CONTENT_HINT_PHOTO_DXR;
    ci.inputScale = 0.0f;  // = 1.0 (native)

    // Spec 5: the player's pick rides on THIS create only (a spec-4 runtime ignores it).
    XrLiftProviderPreferenceDXR pref{};
    if (!preferred_.empty()) {
        pref.type = XR_TYPE_LIFT_PROVIDER_PREFERENCE_DXR;
        pref.providerId = preferred_.c_str();
        task.next = &pref;
    }

    XrLiftStreamDXR s = XR_NULL_HANDLE;
    const XrResult r = api_.createStream(session_, &ci, &s);
    if (XR_SUCCEEDED(r) && s != XR_NULL_HANDLE) {
        stream_ = s;
        inFlightId_ = 0;
        lastResultId_ = 0;
        unavailableWhy_.clear();
        LOG_INFO("2D→3D conversion: stream created (%s, task %s)", video_ ? "video" : "photo",
                 TaskStateName(task_.state));
        return true;
    }
    switch (r) {
        case XR_ERROR_FEATURE_UNSUPPORTED:
            createUnsupported_ = true;  // latched until the provider set changes
            SetUnavailable("2D->3D conversion is not available for this session");
            break;
        case XR_ERROR_LIMIT_REACHED:
            SetUnavailable("The 2D->3D converter is busy with other apps");
            break;
        case XR_ERROR_VALIDATION_FAILURE:
            SetUnavailable("The 2D->3D converter refused this player's request");
            break;
        default: SetUnavailable("2D->3D conversion could not start"); break;
    }
    LOG_WARN("2D→3D conversion: xrCreateLiftStreamDXR failed: XrResult=%d (%s)", (int)r,
             unavailableWhy_.c_str());
    return false;
}

void LiftClient::DestroyStream() {
    if (stream_ != XR_NULL_HANDLE && api_.destroyStream) api_.destroyStream(stream_);
    stream_ = XR_NULL_HANDLE;
    inFlightId_ = 0;
    lastResultId_ = 0;
}

// ---- frames --------------------------------------------------------------------------

bool LiftClient::WantsFrame(double nowSec) const {
    if (!want_ || stream_ == XR_NULL_HANDLE) return false;
    return inFlightId_ == 0 || nowSec - inFlightSince_ >= kStaleSeconds;
}

bool LiftClient::Submit(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t rowPitch,
                        int64_t sourceTime, double nowSec) {
    if (!want_ || stream_ == XR_NULL_HANDLE || !rgba || width == 0 || height == 0 ||
        rowPitch < width * 4u)
        return false;
    XrLiftImageDXR img{};
    img.type = XR_TYPE_LIFT_IMAGE_DXR;
    img.role = XR_LIFT_IMAGE_ROLE_COLOR_DXR;
    img.kind = XR_LIFT_IMAGE_KIND_CPU_DXR;
    img.handle = nullptr;
    img.cpuBytes = rgba;
    img.cpuRowPitch = rowPitch;
    img.format = XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR;  // display-referred, sRGB-encoded bytes
    img.colorSpace = XR_LIFT_COLOR_SPACE_SRGB_DXR;
    img.region.offset = {0, 0};
    img.region.extent = {(int32_t)width, (int32_t)height};
    img.syncKind = XR_LIFT_SYNC_KIND_CPU_COMPLETE_DXR;

    XrLiftFrameImagesDXR images{};
    images.type = XR_TYPE_LIFT_FRAME_IMAGES_DXR;
    images.imageCount = 1;
    images.images = &img;

    XrLiftFrameSubmitInfoDXR submit{};
    submit.type = XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR;
    submit.next = &images;
    submit.inputTexture = nullptr;  // replaced by XrLiftFrameImagesDXR (spec 4)
    submit.extent = {(int32_t)width, (int32_t)height};
    submit.sourceTime = (XrTime)sourceTime;

    uint64_t frameId = 0;
    const XrResult r = api_.submitFrame(stream_, &submit, &frameId);
    if (XR_FAILED(r)) {
        // RUNTIME_FAILURE is the spec's "retry next frame"; anything else is logged once
        // per stream through the in-flight state staying empty.
        if (r != XR_ERROR_RUNTIME_FAILURE)
            LOG_WARN("2D→3D conversion: xrSubmitLiftFrameDXR failed: XrResult=%d", (int)r);
        return false;
    }
    inFlightId_ = frameId ? frameId : inFlightId_ + 1;
    inFlightSince_ = nowSec;
    ++submitted_;
    return true;
}

bool LiftClient::Poll(Result& out) {
    if (!want_ || stream_ == XR_NULL_HANDLE) return false;
    XrLiftResultExportDXR ex{};
    ex.type = XR_TYPE_LIFT_RESULT_EXPORT_DXR;
    XrLiftResultDXR r{};
    r.type = XR_TYPE_LIFT_RESULT_DXR;
    r.next = &ex;
    const XrResult res = api_.acquireResult(stream_, &r);
    if (res == XR_LIFT_NOT_READY_DXR) return false;
    if (XR_FAILED(res)) {
        if (!acquireErrLogged_) {
            LOG_WARN("2D→3D conversion: xrAcquireLiftResultDXR failed: XrResult=%d", (int)res);
            acquireErrLogged_ = true;
        }
        return false;
    }
    if (r.frameId == 0 || r.frameId <= lastResultId_) return false;  // nothing newer
    lastResultId_ = r.frameId;
    if (inFlightId_ != 0 && r.frameId >= inFlightId_) inFlightId_ = 0;  // ours landed

    if (ex.kind != XR_LIFT_IMAGE_KIND_CPU_DXR || r.outputTexture == nullptr ||
        r.extent.width <= 0 || r.extent.height <= 0) {
        LOG_WARN("2D→3D conversion: result %llu is not a readable CPU image (kind %d)",
                 (unsigned long long)r.frameId, (int)ex.kind);
        return false;
    }
    const uint32_t w = (uint32_t)r.extent.width, h = (uint32_t)r.extent.height;
    const uint8_t* px = static_cast<const uint8_t*>(r.outputTexture);
    switch (ex.format) {
        case XR_LIFT_PIXEL_FORMAT_RGBA8_UNORM_DXR:
        case XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR: break;
        case XR_LIFT_PIXEL_FORMAT_BGRA8_UNORM_DXR:
        case XR_LIFT_PIXEL_FORMAT_BGRA8_SRGB_DXR: {
            swizzle_.resize((size_t)w * h * 4);
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
                swizzle_[i * 4 + 0] = px[i * 4 + 2];
                swizzle_[i * 4 + 1] = px[i * 4 + 1];
                swizzle_[i * 4 + 2] = px[i * 4 + 0];
                swizzle_[i * 4 + 3] = px[i * 4 + 3];
            }
            px = swizzle_.data();
            break;
        }
        default:
            LOG_WARN("2D→3D conversion: result pixel format %d is not RGBA8 / BGRA8",
                     (int)ex.format);
            return false;
    }
    out.rgba = px;
    out.width = w;
    out.height = h;
    out.viewCount = r.viewCount ? r.viewCount : 2;
    out.frameId = r.frameId;
    out.sourceTime = (int64_t)r.sourceTime;
    out.sizeChanged = (w != lastW_ || h != lastH_);
    lastW_ = w;
    lastH_ = h;
    if (!haveResult_)
        LOG_INFO("2D→3D conversion: first result %ux%u (%u views)", w, h, out.viewCount);
    haveResult_ = true;
    ++converted_;
    return true;
}

// ---- frame preparation ---------------------------------------------------------------

void FitExtent(uint32_t w, uint32_t h, uint32_t maxLong, uint32_t& outW, uint32_t& outH) {
    outW = w;
    outH = h;
    const uint32_t longEdge = std::max(w, h);
    if (w == 0 || h == 0 || maxLong == 0 || longEdge <= maxLong) return;
    if (w >= h) {
        outW = maxLong;
        outH = std::max<uint32_t>(1, (uint32_t)(((uint64_t)h * maxLong + w / 2) / w));
    } else {
        outH = maxLong;
        outW = std::max<uint32_t>(1, (uint32_t)(((uint64_t)w * maxLong + h / 2) / h));
    }
}

void ScaleRgba(const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch, uint32_t outW,
               uint32_t outH, std::vector<uint8_t>& out) {
    out.resize((size_t)outW * outH * 4);
    if (!src || w == 0 || h == 0 || outW == 0 || outH == 0) return;
    if (outW == w && outH == h) {
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&out[(size_t)y * w * 4], src + (size_t)y * pitch, (size_t)w * 4);
        return;
    }
    for (uint32_t oy = 0; oy < outH; ++oy) {
        uint32_t y0 = (uint32_t)((uint64_t)oy * h / outH);
        uint32_t y1 = std::max(y0 + 1, (uint32_t)((uint64_t)(oy + 1) * h / outH));
        y1 = std::min(y1, h);
        for (uint32_t ox = 0; ox < outW; ++ox) {
            uint32_t x0 = (uint32_t)((uint64_t)ox * w / outW);
            uint32_t x1 = std::max(x0 + 1, (uint32_t)((uint64_t)(ox + 1) * w / outW));
            x1 = std::min(x1, w);
            uint32_t acc[4] = {0, 0, 0, 0};
            for (uint32_t y = y0; y < y1; ++y) {
                const uint8_t* row = src + (size_t)y * pitch;
                for (uint32_t x = x0; x < x1; ++x)
                    for (int c = 0; c < 4; ++c) acc[c] += row[x * 4 + c];
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            uint8_t* d = &out[((size_t)oy * outW + ox) * 4];
            for (int c = 0; c < 4; ++c) d[c] = (uint8_t)((acc[c] + n / 2) / n);
        }
    }
}

bool ConvertYuvToRgba(const uint8_t* p0, const uint8_t* p1, const uint8_t* p2, uint32_t w,
                      uint32_t h, int format, bool fullRange, uint32_t outW, uint32_t outH,
                      std::vector<uint8_t>& out) {
    if (!p0 || w == 0 || h == 0 || outW == 0 || outH == 0) return false;
    if (format == 3) {
        ScaleRgba(p0, w, h, w * 4, outW, outH, out);
        return true;
    }
    if (format < 0 || format > 2 || !p1 || (format == 0 && !p2)) return false;
    out.resize((size_t)outW * outH * 4);
    const uint32_t cw = (w + 1) / 2;
    const bool p16 = format == 2;
    // BT.709, 10-bit fixed point. Limited: Y' 16..235, C 16..240; full: 0..255.
    const int ky = fullRange ? 1024 : 1192;
    const int kRv = fullRange ? 1613 : 1836;
    const int kGu = fullRange ? 192 : 218;
    const int kGv = fullRange ? 479 : 546;
    const int kBu = fullRange ? 1900 : 2163;
    const int yOff = fullRange ? 0 : 16;
    for (uint32_t oy = 0; oy < outH; ++oy) {
        const uint32_t sy = (uint32_t)(((uint64_t)oy * 2 + 1) * h / (outH * 2ull));
        const uint32_t cy = sy / 2;
        uint8_t* d = &out[(size_t)oy * outW * 4];
        for (uint32_t ox = 0; ox < outW; ++ox, d += 4) {
            const uint32_t sx = (uint32_t)(((uint64_t)ox * 2 + 1) * w / (outW * 2ull));
            const uint32_t cx = sx / 2;
            int Y, U, V;
            if (p16) {  // little-endian 16-bit, MSB-aligned: the high byte is the 8-bit value
                Y = p0[((size_t)sy * w + sx) * 2 + 1];
                U = p1[((size_t)cy * cw + cx) * 4 + 1];
                V = p1[((size_t)cy * cw + cx) * 4 + 3];
            } else if (format == 1) {
                Y = p0[(size_t)sy * w + sx];
                U = p1[((size_t)cy * cw + cx) * 2 + 0];
                V = p1[((size_t)cy * cw + cx) * 2 + 1];
            } else {
                Y = p0[(size_t)sy * w + sx];
                U = p1[(size_t)cy * cw + cx];
                V = p2[(size_t)cy * cw + cx];
            }
            const int c = ky * (Y - yOff);
            const int u = U - 128, v = V - 128;
            d[0] = Clamp8((c + kRv * v + 512) >> 10);
            d[1] = Clamp8((c - kGu * u - kGv * v + 512) >> 10);
            d[2] = Clamp8((c + kBu * u + 512) >> 10);
            d[3] = 255;
        }
    }
    return true;
}

}  // namespace mp::lift
