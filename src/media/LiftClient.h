// SPDX-License-Identifier: Apache-2.0
//
// LiftClient — "Convert to 3D" over the runtime's XR_DXR_lift conversion service
// (spec v4, ADR-053). One VIEWS_MONO task stream turns the player's mono picture into a
// two-view side-by-side image; the runtime picks whatever converter is installed and
// this client never learns or shows which one (the UI says "2D→3D conversion").
//
// Data path v1 is CPU in, CPU out: the caller hands an RGBA8 frame (already downscaled
// to <= kMaxInputLongEdge), the runtime copies it before the submit returns, and a
// result arrives later as tightly packed RGBA8 bytes (2W x H) that stay valid until the
// next Poll(). Results are asynchronous and latest-wins: at most ONE frame is in flight,
// a new one is submitted only once the previous one produced a result (or went stale),
// and Poll() never blocks.
//
// The client is a pure state machine over a table of entry points (Api), so it unit-tests
// without a runtime (tests/lift_client_test.cpp drives it through fakes). XrSession
// resolves the real table; nothing here includes the loader or calls OpenXR directly.
//
//   Off ──Start()──► Warming / Requested ──first result──► Active
//    ▲                  │  (create refused, or the task went terminal)
//    └────Stop()────────┴────────────► Unavailable(reason)
//
// Availability (what the toggle shows) is separate from State (what the toggle did):
//   Hidden   — no XR_DXR_lift on this runtime (or the entry points did not resolve);
//   Disabled — the extension is there but the VIEWS_MONO task cannot serve this session
//              (absent / incompatible / unlicensed / failed / disabled, no mono input,
//              no CPU transport, or stream creation said XR_ERROR_FEATURE_UNSUPPORTED);
//   Enabled  — READY or a waiting state (WARMING / INSTALLED / ...): frames queue.
#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_lift.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mp::lift {

// The XR_DXR_lift entry points the client uses. XrSession fills it from
// xrGetInstanceProcAddr; tests fill it with fakes. requestWarm is optional.
struct Api {
    PFN_xrEnumerateLiftTasksDXR enumerateTasks = nullptr;
    PFN_xrRequestLiftTaskWarmDXR requestWarm = nullptr;
    PFN_xrCreateLiftStreamDXR createStream = nullptr;
    PFN_xrDestroyLiftStreamDXR destroyStream = nullptr;
    PFN_xrSubmitLiftFrameDXR submitFrame = nullptr;
    PFN_xrAcquireLiftResultDXR acquireResult = nullptr;

    bool Complete() const {
        return enumerateTasks && createStream && destroyStream && submitFrame && acquireResult;
    }
};

enum class State { Off, Requested, Warming, Active, Unavailable };
enum class Availability { Hidden, Disabled, Enabled };

const char* StateName(State s);  // "off" | "requested" | "warming" | "active" | "unavailable"
const char* AvailabilityName(Availability a);  // "hidden" | "disabled" | "enabled"
const char* TaskStateName(XrLiftTaskStateDXR s);

// One converted picture. `rgba` is tightly packed RGBA8 (width * 4 bytes per row),
// owned by the runtime (or the client's swizzle buffer) and valid until the next
// Poll / Reset / Stop / Detach. `width` covers all views side by side.
struct Result {
    const uint8_t* rgba = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t viewCount = 2;
    uint64_t frameId = 0;
    int64_t sourceTime = 0;
    bool sizeChanged = false;  // differs from the previous result's extent
};

class LiftClient {
public:
    // Longest edge of a submitted frame. Larger sources are downscaled by the caller
    // (ScaleRgba / ConvertYuvToRgba below) before Submit.
    static constexpr uint32_t kMaxInputLongEdge = 1920;
    // An in-flight frame with no result after this long no longer blocks the next
    // submit (a declined conversion produces nothing; the mailbox is latest-wins, so a
    // resubmit simply replaces it). Also the re-enumeration period while waiting.
    static constexpr double kStaleSeconds = 1.0;
    // Re-enumeration period while the toggle is off and the task is not settled READY.
    static constexpr double kIdlePollSeconds = 2.0;

    LiftClient() = default;
    ~LiftClient() { Detach(); }
    LiftClient(const LiftClient&) = delete;
    LiftClient& operator=(const LiftClient&) = delete;

    // Bind to a session. `api` incomplete => the client stays Hidden. Enumerates the
    // tasks once, which also opts the session into the task / provider events.
    // `::XrSession`: mp::XrSession (the app's session class) shadows the handle type.
    void Attach(const Api& api, ::XrSession session);
    // Destroy the stream (if any) and forget the session. Call before xrDestroySession.
    void Detach();
    bool Attached() const { return session_ != XR_NULL_HANDLE && api_.Complete(); }

    // Re-read the VIEWS_MONO task's properties (xrEnumerateLiftTasksDXR).
    void Refresh();
    // Feed every XrEventDataBuffer from xrPollEvent. Returns true if it was a lift event.
    bool OnEvent(const XrEventDataBaseHeader* ev);
    // Periodic work, call once per frame. While the task is not settled READY, re-read it
    // (the spec's consumer contract: poll at <= 2 Hz or follow events): every
    // kStaleSeconds while the toggle is on (and retry the stream), every kIdlePollSeconds
    // while it is off. An in-process session may enumerate NOTHING until its lift link has
    // connected, so the first answer is not final. A latched FEATURE_UNSUPPORTED is.
    void Tick(double nowSec);

    Availability GetAvailability() const;
    // Why the toggle is Disabled (or why the state is Unavailable), user-facing, never a
    // provider name. Empty otherwise.
    std::string Reason() const;
    // The task is in a waiting state (WARMING / INSTALLED / DOWNLOADING / EVICTED /
    // UNKNOWN): frames queue until it is READY.
    bool TaskWaiting() const;
    float Progress() const { return task_.progress; }  // [0,1], < 0 = unknown
    XrLiftTaskStateDXR TaskState() const {
        return taskPresent_ ? task_.state : XR_LIFT_TASK_STATE_ABSENT_DXR;
    }

    // The toggle. Start creates the stream (VIEWS_MONO, mono input, 2 views, CPU export);
    // `video` picks the content hint. Returns false when the stream could not be made
    // (State becomes Unavailable, Reason() says why). Stop destroys the stream.
    bool Start(bool video);
    void Stop();
    bool On() const { return want_; }
    // Discontinuity (seek, source switch, size change): recreate the stream so no
    // result from before the jump can land after it. No-op when off.
    void Reset();
    State GetState() const;

    // True when the caller should hand over a new frame now: the stream exists and no
    // frame is in flight (or the in-flight one went stale).
    bool WantsFrame(double nowSec) const;
    // Submit one RGBA8 frame (CPU, copied by the runtime before return). `rowPitch` in
    // bytes. Returns false on any failure (non-fatal: retry on a later frame).
    bool Submit(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t rowPitch,
                int64_t sourceTime, double nowSec);
    // Non-blocking: the newest finished result newer than the last one, or false.
    bool Poll(Result& out);

    uint64_t FramesSubmitted() const { return submitted_; }
    uint64_t FramesConverted() const { return converted_; }
    bool HaveResult() const { return haveResult_; }

private:
    bool CreateStream();
    void DestroyStream();
    void SetUnavailable(const std::string& why) { unavailableWhy_ = why; }
    static bool Terminal(XrLiftTaskStateDXR s);

    Api api_{};
    ::XrSession session_ = XR_NULL_HANDLE;

    // The VIEWS_MONO task as last enumerated (taskPresent_ false => ABSENT).
    XrLiftTaskPropertiesDXR task_{};
    bool taskPresent_ = false;
    bool enumerateUnsupported_ = false;  // the session reports FEATURE_UNSUPPORTED
    bool createUnsupported_ = false;     // stream creation said FEATURE_UNSUPPORTED (latched)
    double lastEnumSec_ = -1e9;

    bool want_ = false;   // the toggle is on
    bool video_ = false;  // content hint of the current stream
    XrLiftStreamDXR stream_ = XR_NULL_HANDLE;
    std::string unavailableWhy_;  // set while want_ && the stream could not be made / serve

    uint64_t inFlightId_ = 0;  // frameId of the submitted frame awaiting a result
    double inFlightSince_ = 0.0;
    uint64_t lastResultId_ = 0;  // per stream: results at or below it are stale
    bool haveResult_ = false;    // a result landed since Start()
    uint32_t lastW_ = 0, lastH_ = 0;
    std::vector<uint8_t> swizzle_;  // BGRA -> RGBA when the export is BGRA
    uint64_t submitted_ = 0, converted_ = 0;
    bool acquireErrLogged_ = false;
};

// ---- frame preparation (pure, unit-tested) --------------------------------------

// The extent a w x h frame is submitted at: aspect kept, long edge <= maxLong.
void FitExtent(uint32_t w, uint32_t h, uint32_t maxLong, uint32_t& outW, uint32_t& outH);

// Resample an RGBA8 image to (outW x outH) (box filter when shrinking), tightly packed.
void ScaleRgba(const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch, uint32_t outW,
               uint32_t outH, std::vector<uint8_t>& out);

// Decoded video frame -> RGBA8 at (outW x outH), BT.709 (the renderer's YUV convert).
// format: 0 = I420 (p1 = U, p2 = V), 1 = NV12 (p1 = interleaved UV), 2 = P016 (NV12
// layout, 16-bit little-endian MSB-aligned samples), 3 = packed RGBA (p0 only). Planes
// are tightly packed, chroma (w+1)/2 x (h+1)/2. Returns false on a bad argument.
bool ConvertYuvToRgba(const uint8_t* p0, const uint8_t* p1, const uint8_t* p2, uint32_t w,
                      uint32_t h, int format, bool fullRange, uint32_t outW, uint32_t outH,
                      std::vector<uint8_t>& out);

}  // namespace mp::lift
