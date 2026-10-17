// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for LiftClient ("Convert to 3D" over XR_DXR_lift spec v4) against a fake
// entry-point table: availability (hidden / disabled / enabled), the Off / Requested /
// Warming / Active / Unavailable machine, frame-id matching, the latest-wins one-in-flight
// cadence, result size changes, BGRA exports, FEATURE_UNSUPPORTED -> Unavailable, events,
// and the CPU frame preparation helpers. No runtime, no GPU.

#include "media/LiftClient.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mp::lift;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

constexpr XrFlags64 Bit(uint32_t v) { return (XrFlags64)1 << v; }

// ---- the fake runtime ------------------------------------------------------------------

XrSession const kSession = (XrSession)(uintptr_t)0x5e55;

struct Fake {
    std::vector<XrLiftTaskPropertiesDXR> tasks;
    XrResult enumResult = XR_SUCCESS;
    XrResult createResult = XR_SUCCESS;
    XrResult submitResult = XR_SUCCESS;
    int enumCalls = 0, creates = 0, createCalls = 0, destroys = 0, submits = 0;
    XrLiftStreamCreateInfoDXR lastCreate{};
    XrLiftTaskStreamCreateInfoDXR lastTask{};
    bool lastTaskChained = false;
    uint64_t nextFrameId = 1;
    uint32_t lastSubmitW = 0, lastSubmitH = 0;
    XrLiftImageKindDXR lastSubmitKind = XR_LIFT_IMAGE_KIND_PLATFORM_DEFAULT_DXR;
    int64_t lastSourceTime = 0;
    // The result the next acquire hands out (0 = NOT_READY).
    uint64_t readyId = 0;
    uint32_t readyW = 0, readyH = 0;
    std::vector<uint8_t> bytes;
    XrLiftPixelFormatDXR fmt = XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR;
    XrLiftImageKindDXR kind = XR_LIFT_IMAGE_KIND_CPU_DXR;

    void Finish(uint64_t id, uint32_t w, uint32_t h, uint8_t fill = 0x40) {
        readyId = id;
        readyW = w;
        readyH = h;
        bytes.assign((size_t)w * h * 4, fill);
    }
};
Fake g;

XrLiftTaskPropertiesDXR MakeTask(XrLiftTaskStateDXR state) {
    XrLiftTaskPropertiesDXR t{};
    t.type = XR_TYPE_LIFT_TASK_PROPERTIES_DXR;
    t.task = XR_LIFT_TASK_VIEWS_MONO_DXR;
    t.state = state;
    t.progress = -1.0f;
    t.lanes = XR_LIFT_LANE_STREAM_BIT_DXR;
    t.inputKinds = Bit(XR_LIFT_INPUT_KIND_MONO_DXR);
    t.imageKinds = Bit(XR_LIFT_IMAGE_KIND_D3D11_NT_DXR) | Bit(XR_LIFT_IMAGE_KIND_CPU_DXR);
    t.exportKinds = Bit(XR_LIFT_IMAGE_KIND_D3D11_NT_DXR) | Bit(XR_LIFT_IMAGE_KIND_CPU_DXR);
    t.maxStreams = 4;
    t.maxViews = 8;
    return t;
}

XrLiftTaskPropertiesDXR DepthTask() {
    XrLiftTaskPropertiesDXR t = MakeTask(XR_LIFT_TASK_STATE_READY_DXR);
    t.task = XR_LIFT_TASK_DEPTH_MONO_DXR;
    return t;
}

XRAPI_ATTR XrResult XRAPI_CALL FakeEnumerate(XrSession s, uint32_t cap, uint32_t* count,
                                             XrLiftTaskPropertiesDXR* out) {
    ++g.enumCalls;
    if (s != kSession) return XR_ERROR_HANDLE_INVALID;
    if (XR_FAILED(g.enumResult)) return g.enumResult;
    *count = (uint32_t)g.tasks.size();
    if (cap == 0) return XR_SUCCESS;
    if (cap < g.tasks.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    for (size_t i = 0; i < g.tasks.size(); ++i) {
        if (out[i].type != XR_TYPE_LIFT_TASK_PROPERTIES_DXR) return XR_ERROR_VALIDATION_FAILURE;
        out[i] = g.tasks[i];
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL FakeCreate(XrSession s, const XrLiftStreamCreateInfoDXR* ci,
                                          XrLiftStreamDXR* stream) {
    ++g.createCalls;
    if (s != kSession) return XR_ERROR_HANDLE_INVALID;
    g.lastCreate = *ci;
    g.lastTaskChained = false;
    for (auto* n = static_cast<const XrBaseInStructure*>(ci->next); n; n = n->next) {
        if (n->type == XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR) {
            g.lastTask = *reinterpret_cast<const XrLiftTaskStreamCreateInfoDXR*>(n);
            g.lastTaskChained = true;
        }
    }
    if (XR_FAILED(g.createResult)) return g.createResult;
    ++g.creates;
    g.nextFrameId = 1;  // per stream, from 1
    *stream = (XrLiftStreamDXR)(uintptr_t)(0x1000 + g.creates);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL FakeDestroy(XrLiftStreamDXR) {
    ++g.destroys;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL FakeSubmit(XrLiftStreamDXR, const XrLiftFrameSubmitInfoDXR* si,
                                          uint64_t* frameId) {
    if (XR_FAILED(g.submitResult)) return g.submitResult;
    const XrLiftFrameImagesDXR* imgs = nullptr;
    for (auto* n = static_cast<const XrBaseInStructure*>(si->next); n; n = n->next)
        if (n->type == XR_TYPE_LIFT_FRAME_IMAGES_DXR)
            imgs = reinterpret_cast<const XrLiftFrameImagesDXR*>(n);
    if (!imgs || imgs->imageCount != 1 || !imgs->images[0].cpuBytes)
        return XR_ERROR_VALIDATION_FAILURE;
    g.lastSubmitKind = imgs->images[0].kind;
    g.lastSubmitW = (uint32_t)imgs->images[0].region.extent.width;
    g.lastSubmitH = (uint32_t)imgs->images[0].region.extent.height;
    g.lastSourceTime = si->sourceTime;
    ++g.submits;
    *frameId = g.nextFrameId++;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL FakeAcquire(XrLiftStreamDXR, XrLiftResultDXR* r) {
    if (g.readyId == 0) return XR_LIFT_NOT_READY_DXR;
    r->frameId = g.readyId;
    r->sourceTime = (XrTime)(g.readyId * 1000);
    r->outputTexture = g.bytes.data();
    r->fence = nullptr;
    r->extent = {(int32_t)g.readyW, (int32_t)g.readyH};
    r->format = 0;
    r->viewCount = 2;
    for (auto* n = static_cast<XrBaseOutStructure*>(r->next); n; n = n->next) {
        if (n->type == XR_TYPE_LIFT_RESULT_EXPORT_DXR) {
            auto* ex = reinterpret_cast<XrLiftResultExportDXR*>(n);
            ex->kind = g.kind;
            ex->format = g.fmt;
            ex->colorSpace = XR_LIFT_COLOR_SPACE_SRGB_DXR;
            ex->syncKind = XR_LIFT_SYNC_KIND_CPU_COMPLETE_DXR;
        }
    }
    g.readyId = 0;
    return XR_SUCCESS;
}

Api FakeApi() {
    Api a;
    a.enumerateTasks = FakeEnumerate;
    a.createStream = FakeCreate;
    a.destroyStream = FakeDestroy;
    a.submitFrame = FakeSubmit;
    a.acquireResult = FakeAcquire;
    return a;
}

void ResetFake(std::vector<XrLiftTaskPropertiesDXR> tasks) {
    g = Fake{};
    g.tasks = std::move(tasks);
}

std::vector<uint8_t> Frame(uint32_t w, uint32_t h) {
    return std::vector<uint8_t>((size_t)w * h * 4, 0x80);
}

bool SubmitOne(LiftClient& c, double t) {
    std::vector<uint8_t> f = Frame(16, 8);
    return c.Submit(f.data(), 16, 8, 16 * 4, (int64_t)(t * 1e9), t);
}

XrEventDataLiftTaskStateChangedDXR StateEvent(XrLiftTaskStateDXR from, XrLiftTaskStateDXR to) {
    XrEventDataLiftTaskStateChangedDXR e{};
    e.type = XR_TYPE_EVENT_DATA_LIFT_TASK_STATE_CHANGED_DXR;
    e.session = kSession;
    e.task = XR_LIFT_TASK_VIEWS_MONO_DXR;
    e.previousState = from;
    e.state = to;
    e.progress = -1.0f;
    return e;
}

const XrEventDataBaseHeader* Hdr(const void* e) {
    return static_cast<const XrEventDataBaseHeader*>(e);
}

// ---- tests -------------------------------------------------------------------------------

void TestHidden() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    LiftClient c;
    CHECK(c.GetAvailability() == Availability::Hidden, "unattached client is hidden");
    Api partial = FakeApi();
    partial.acquireResult = nullptr;  // an entry point did not resolve
    c.Attach(partial, kSession);
    CHECK(c.GetAvailability() == Availability::Hidden, "incomplete api is hidden");
    CHECK(!c.Start(false), "a hidden client cannot start");
    CHECK(c.GetState() == State::Off, "hidden start leaves it off");
    CHECK(g.createCalls == 0, "hidden: no stream created");
}

void TestDisabledReasons() {
    struct Case {
        const char* name;
        XrLiftTaskStateDXR state;
        bool present;
        void (*mutate)(XrLiftTaskPropertiesDXR&);
    };
    const Case cases[] = {
        {"absent", XR_LIFT_TASK_STATE_READY_DXR, false, nullptr},
        {"incompatible", XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR, true, nullptr},
        {"unlicensed", XR_LIFT_TASK_STATE_UNLICENSED_DXR, true, nullptr},
        {"failed", XR_LIFT_TASK_STATE_FAILED_DXR, true, nullptr},
        {"disabled", XR_LIFT_TASK_STATE_DISABLED_DXR, true, nullptr},
        {"no input kinds", XR_LIFT_TASK_STATE_READY_DXR, true,
         [](XrLiftTaskPropertiesDXR& t) { t.inputKinds = 0; }},
        {"no CPU import", XR_LIFT_TASK_STATE_READY_DXR, true,
         [](XrLiftTaskPropertiesDXR& t) { t.imageKinds = Bit(XR_LIFT_IMAGE_KIND_D3D11_NT_DXR); }},
        {"no CPU export", XR_LIFT_TASK_STATE_READY_DXR, true,
         [](XrLiftTaskPropertiesDXR& t) { t.exportKinds = 0; }},
        {"no stream lane", XR_LIFT_TASK_STATE_READY_DXR, true,
         [](XrLiftTaskPropertiesDXR& t) { t.lanes = XR_LIFT_LANE_JOB_BIT_DXR; }},
    };
    for (const Case& k : cases) {
        std::vector<XrLiftTaskPropertiesDXR> tasks{DepthTask()};
        if (k.present) {
            XrLiftTaskPropertiesDXR t = MakeTask(k.state);
            if (k.mutate) k.mutate(t);
            tasks.push_back(t);
        }
        ResetFake(tasks);
        LiftClient c;
        c.Attach(FakeApi(), kSession);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s -> disabled", k.name);
        CHECK(c.GetAvailability() == Availability::Disabled, msg);
        std::snprintf(msg, sizeof(msg), "%s -> has a reason", k.name);
        CHECK(!c.Reason().empty(), msg);
        std::snprintf(msg, sizeof(msg), "%s -> start refused, unavailable", k.name);
        CHECK(!c.Start(true) && c.GetState() == State::Unavailable, msg);
        CHECK(g.createCalls == 0, "disabled: no create attempted");
        c.Stop();
        CHECK(c.GetState() == State::Off, "stop after a refused start is off");
    }
    // The reason never names a provider: it is built only from the task contract.
    ResetFake({});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    CHECK(c.Reason() == "No 2D->3D converter is installed", "absent reason text");
}

void TestEnumerateUnsupported() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    g.enumResult = XR_ERROR_FEATURE_UNSUPPORTED;  // e.g. an in-process session
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    CHECK(c.GetAvailability() == Availability::Disabled,
          "enumerate FEATURE_UNSUPPORTED -> disabled");
    CHECK(!c.Reason().empty(), "enumerate unsupported has a reason");
}

void TestStartReadyAndCreateInfo() {
    ResetFake({DepthTask(), MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    CHECK(c.GetAvailability() == Availability::Enabled, "ready task -> enabled");
    CHECK(c.GetState() == State::Off, "off by default");
    CHECK(c.Start(/*video=*/true), "start succeeds");
    CHECK(c.GetState() == State::Requested, "ready + no result -> requested");
    CHECK(g.lastTaskChained, "task create info chained");
    CHECK(g.lastTask.task == XR_LIFT_TASK_VIEWS_MONO_DXR, "task = VIEWS_MONO");
    CHECK(g.lastTask.inputKind == XR_LIFT_INPUT_KIND_MONO_DXR, "input = MONO");
    CHECK(g.lastTask.viewCount == 2, "two views");
    CHECK(g.lastTask.exportKind == XR_LIFT_IMAGE_KIND_CPU_DXR, "CPU export");
    CHECK(g.lastCreate.mode == XR_LIFT_MODE_SBS_DXR, "legacy mode = SBS (graceful on spec 3)");
    CHECK(g.lastCreate.contentHint == XR_LIFT_CONTENT_HINT_VIDEO_DXR, "video hint");
    CHECK(c.Start(true) && g.creates == 1, "start while on is idempotent");
    c.Stop();
    CHECK(g.destroys == 1 && c.GetState() == State::Off, "stop destroys the stream");
    CHECK(c.Start(false) && g.lastCreate.contentHint == XR_LIFT_CONTENT_HINT_PHOTO_DXR,
          "photo hint");
    c.Detach();
    CHECK(g.destroys == 2, "detach destroys the stream");
}

void TestWarming() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_WARMING_DXR)});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    CHECK(c.GetAvailability() == Availability::Enabled, "warming -> enabled");
    CHECK(c.TaskWaiting(), "warming is a waiting state");
    CHECK(c.Start(true) && c.GetState() == State::Warming, "start while warming -> warming");
    CHECK(c.WantsFrame(0.0), "frames queue while warming");
    const auto e = StateEvent(XR_LIFT_TASK_STATE_WARMING_DXR, XR_LIFT_TASK_STATE_READY_DXR);
    CHECK(c.OnEvent(Hdr(&e)), "state event consumed");
    CHECK(c.GetState() == State::Requested, "ready event -> requested");
    // INSTALLED (cold) is enabled too, and also waiting.
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_INSTALLED_DXR)});
    LiftClient d;
    d.Attach(FakeApi(), kSession);
    CHECK(d.GetAvailability() == Availability::Enabled && d.TaskWaiting(),
          "installed -> enabled, waiting");
}

void TestCadenceAndFrameIds() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    Result r;
    CHECK(!c.WantsFrame(0.0) && !c.Poll(r), "off: no frames, no results");
    c.Start(true);
    CHECK(c.WantsFrame(0.0), "nothing in flight -> wants a frame");
    CHECK(SubmitOne(c, 0.0), "submit 1");
    CHECK(g.lastSubmitKind == XR_LIFT_IMAGE_KIND_CPU_DXR, "CPU image kind");
    CHECK(g.lastSubmitW == 16 && g.lastSubmitH == 8, "submitted region = frame extent");
    CHECK(!c.WantsFrame(0.1), "one in flight -> no second submit");
    CHECK(!c.Poll(r), "NOT_READY -> no result");
    g.Finish(1, 32, 8);
    CHECK(c.Poll(r), "result 1 lands");
    CHECK(r.frameId == 1 && r.width == 32 && r.height == 8 && r.rgba != nullptr, "result 1 fields");
    CHECK(c.GetState() == State::Active, "first result -> active");
    CHECK(c.WantsFrame(0.2), "result matched the in-flight frame -> wants the next");
    CHECK(!c.Poll(r), "no newer result");
    // A repeated / older frame id is never shown twice.
    g.Finish(1, 32, 8);
    CHECK(!c.Poll(r), "stale frame id rejected");

    // Latest-wins: frame 2 goes stale (provider declined it), frame 3 replaces it.
    CHECK(SubmitOne(c, 1.0), "submit 2");
    CHECK(!c.WantsFrame(1.5), "in flight < stale period -> wait");
    CHECK(c.WantsFrame(1.0 + LiftClient::kStaleSeconds + 0.01), "stale in-flight frame unblocks");
    CHECK(SubmitOne(c, 2.1), "submit 3");
    g.Finish(2, 32, 8);  // the late result of frame 2 still shows (newest finished)...
    CHECK(c.Poll(r) && r.frameId == 2, "late result 2 is shown");
    CHECK(!c.WantsFrame(2.2), "...but frame 3 is still in flight");
    g.Finish(3, 32, 8);
    CHECK(c.Poll(r) && r.frameId == 3 && c.WantsFrame(2.3), "result 3 clears the in-flight frame");
    CHECK(c.FramesSubmitted() == 3 && c.FramesConverted() == 3, "counters");

    // A failed submit (RUNTIME_FAILURE = retry next frame) leaves nothing in flight.
    g.submitResult = XR_ERROR_RUNTIME_FAILURE;
    CHECK(!SubmitOne(c, 3.0) && c.WantsFrame(3.0), "failed submit -> still wants a frame");
}

void TestSizeChange() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    c.Start(false);
    Result r;
    SubmitOne(c, 0.0);
    g.Finish(1, 8, 2);
    CHECK(c.Poll(r) && r.sizeChanged, "first result is a size change");
    SubmitOne(c, 0.1);
    g.Finish(2, 8, 2);
    CHECK(c.Poll(r) && !r.sizeChanged, "same extent -> no size change");
    SubmitOne(c, 0.2);
    g.Finish(3, 16, 4);
    CHECK(c.Poll(r) && r.sizeChanged && r.width == 16 && r.height == 4,
          "new extent -> size change");
}

void TestBgraAndUnreadable() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    c.Start(false);
    Result r;
    SubmitOne(c, 0.0);
    g.Finish(1, 2, 1);
    g.fmt = XR_LIFT_PIXEL_FORMAT_BGRA8_UNORM_DXR;
    const uint8_t bgra[8] = {10, 20, 30, 255, 40, 50, 60, 128};
    std::memcpy(g.bytes.data(), bgra, 8);
    CHECK(c.Poll(r), "BGRA result accepted");
    CHECK(r.rgba[0] == 30 && r.rgba[1] == 20 && r.rgba[2] == 10 && r.rgba[3] == 255 &&
              r.rgba[4] == 60 && r.rgba[7] == 128,
          "BGRA swizzled to RGBA");
    SubmitOne(c, 0.1);
    g.Finish(2, 2, 1);
    g.kind = XR_LIFT_IMAGE_KIND_D3D11_NT_DXR;  // a GPU handle we cannot read on the CPU path
    CHECK(!c.Poll(r), "non-CPU export is not handed out");
}

void TestCreateUnsupported() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    g.createResult = XR_ERROR_FEATURE_UNSUPPORTED;
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    CHECK(c.GetAvailability() == Availability::Enabled, "enabled before trying");
    CHECK(!c.Start(true), "create FEATURE_UNSUPPORTED -> start fails");
    CHECK(c.GetState() == State::Unavailable, "-> unavailable");
    CHECK(!c.Reason().empty(), "unavailable has a reason");
    CHECK(c.GetAvailability() == Availability::Disabled, "-> the toggle is disabled (latched)");
    c.Stop();
    const int calls = g.createCalls;
    CHECK(!c.Start(true) && g.createCalls == calls, "latched: no second create attempt");
    c.Stop();
    // A provider change may bring a provider that serves this session.
    g.createResult = XR_SUCCESS;
    XrEventDataLiftProvidersChangedDXR pe{};
    pe.type = XR_TYPE_EVENT_DATA_LIFT_PROVIDERS_CHANGED_DXR;
    pe.session = kSession;
    CHECK(c.OnEvent(Hdr(&pe)), "providers event consumed");
    CHECK(c.GetAvailability() == Availability::Enabled, "providers changed clears the latch");
    CHECK(c.Start(true) && c.GetState() == State::Requested, "start works afterwards");
}

void TestLimitReachedNotLatched() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    g.createResult = XR_ERROR_LIMIT_REACHED;
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    CHECK(!c.Start(true) && c.GetState() == State::Unavailable, "limit reached -> unavailable");
    CHECK(c.GetAvailability() == Availability::Enabled, "limit reached is not latched");
    g.createResult = XR_SUCCESS;
    c.Tick(10.0);  // the toggle is still on: the periodic retry makes the stream
    CHECK(c.GetState() == State::Requested, "periodic retry recovers");
}

void TestTerminalWhileOnAndRecovery() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    c.Start(true);
    Result r;
    SubmitOne(c, 0.0);
    g.Finish(1, 8, 2);
    c.Poll(r);
    auto e = StateEvent(XR_LIFT_TASK_STATE_READY_DXR, XR_LIFT_TASK_STATE_FAILED_DXR);
    e.failureReason = XR_LIFT_FAILURE_REASON_RESOURCE_DXR;
    c.OnEvent(Hdr(&e));
    CHECK(c.GetState() == State::Unavailable, "terminal task while on -> unavailable");
    CHECK(c.On(), "the toggle stays on (the user turns it off)");
    CHECK(c.Reason().find("memory") != std::string::npos, "failure reason surfaces");
    CHECK(g.destroys == 0, "the stream is kept (the runtime may move it)");
    e = StateEvent(XR_LIFT_TASK_STATE_FAILED_DXR, XR_LIFT_TASK_STATE_READY_DXR);
    c.OnEvent(Hdr(&e));
    CHECK(c.GetState() == State::Active, "recovered -> active again");

    // Started while ABSENT; the task appears later (event) -> the stream is made then.
    ResetFake({});
    LiftClient d;
    d.Attach(FakeApi(), kSession);
    CHECK(!d.Start(true) && d.GetState() == State::Unavailable, "absent start -> unavailable");
    g.tasks = {MakeTask(XR_LIFT_TASK_STATE_WARMING_DXR)};
    e = StateEvent(XR_LIFT_TASK_STATE_ABSENT_DXR, XR_LIFT_TASK_STATE_WARMING_DXR);
    d.OnEvent(Hdr(&e));
    CHECK(d.GetState() == State::Warming && g.creates == 1,
          "task appeared -> stream made, warming");

    // Events for another session or another task are ignored.
    auto other = StateEvent(XR_LIFT_TASK_STATE_WARMING_DXR, XR_LIFT_TASK_STATE_FAILED_DXR);
    other.task = XR_LIFT_TASK_DEPTH_MONO_DXR;
    CHECK(d.OnEvent(Hdr(&other)) && d.GetState() == State::Warming, "other task ignored");
    XrEventDataBuffer unrelated{};
    unrelated.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    CHECK(!d.OnEvent(Hdr(&unrelated)), "non-lift event not consumed");
}

void TestIdlePoll() {
    // An in-process session can enumerate nothing until its lift link connects: the first
    // answer is not final, so Tick re-reads it (2 s while off) and the toggle enables.
    ResetFake({});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    CHECK(c.GetAvailability() == Availability::Disabled, "nothing known yet -> disabled");
    const int calls = g.enumCalls;
    c.Tick(0.0);
    CHECK(g.enumCalls > calls, "unsettled -> re-enumerates");
    g.tasks = {MakeTask(XR_LIFT_TASK_STATE_READY_DXR)};
    const int calls2 = g.enumCalls;
    c.Tick(1.0);
    CHECK(g.enumCalls == calls2, "idle poll period respected");
    c.Tick(2.5);
    CHECK(c.GetAvailability() == Availability::Enabled, "task appeared on the poll");
    const int calls3 = g.enumCalls;
    c.Tick(10.0);
    CHECK(g.enumCalls == calls3, "settled READY -> no more polling");
}

void TestReset() {
    ResetFake({MakeTask(XR_LIFT_TASK_STATE_READY_DXR)});
    LiftClient c;
    c.Attach(FakeApi(), kSession);
    c.Reset();
    CHECK(g.createCalls == 0, "reset while off does nothing");
    c.Start(true);
    SubmitOne(c, 0.0);
    CHECK(!c.WantsFrame(0.1), "in flight before the seek");
    c.Reset();  // seek
    CHECK(g.destroys == 1 && g.creates == 2, "reset recreates the stream");
    CHECK(c.WantsFrame(0.1), "the new stream has nothing in flight");
    Result r;
    g.Finish(1, 8, 2);  // frame 1 of the NEW stream
    SubmitOne(c, 0.2);
    CHECK(c.Poll(r) && r.frameId == 1, "ids restart per stream");
}

void TestFrameHelpers() {
    uint32_t w = 0, h = 0;
    FitExtent(3840, 2160, 1920, w, h);
    CHECK(w == 1920 && h == 1080, "4K -> 1920x1080");
    FitExtent(1000, 500, 1920, w, h);
    CHECK(w == 1000 && h == 500, "small frames are not upscaled");
    FitExtent(1080, 4000, 1920, w, h);
    CHECK(h == 1920 && w == 518, "portrait long edge");

    // Box downscale: a 4x2 of two flat halves -> 2x1.
    std::vector<uint8_t> src(4 * 2 * 4, 0), out;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            for (int c = 0; c < 4; ++c) src[(y * 4 + x) * 4 + c] = x < 2 ? 100 : 200;
    ScaleRgba(src.data(), 4, 2, 16, 2, 1, out);
    CHECK(out.size() == 8 && out[0] == 100 && out[4] == 200, "box downscale keeps the halves");
    ScaleRgba(src.data(), 4, 2, 16, 4, 2, out);
    CHECK(out == src, "same size copies");

    // NV12 limited range: white / black / grey, and a saturated red.
    const uint32_t W = 4, H = 2;
    std::vector<uint8_t> Y(W * H), UV(W);  // chroma 2x1, interleaved
    auto nv12 = [&](uint8_t y, uint8_t u, uint8_t v, bool full) {
        std::fill(Y.begin(), Y.end(), y);
        for (uint32_t i = 0; i < W / 2; ++i) {
            UV[i * 2] = u;
            UV[i * 2 + 1] = v;
        }
        std::vector<uint8_t> o;
        ConvertYuvToRgba(Y.data(), UV.data(), nullptr, W, H, 1, full, W, H, o);
        return o;
    };
    std::vector<uint8_t> o = nv12(235, 128, 128, false);
    CHECK(o[0] == 255 && o[1] == 255 && o[2] == 255 && o[3] == 255, "limited white");
    o = nv12(16, 128, 128, false);
    CHECK(o[0] == 0 && o[1] == 0 && o[2] == 0, "limited black");
    o = nv12(255, 128, 128, true);
    CHECK(o[0] == 255 && o[1] == 255 && o[2] == 255, "full white");
    o = nv12(63, 102, 240, false);  // BT.709 limited red
    CHECK(o[0] >= 250 && o[1] <= 5 && o[2] <= 5, "limited red");
    // I420 and P016 agree with NV12 on the same picture.
    std::vector<uint8_t> U(W / 2, 102), V(W / 2, 240), oi;
    std::fill(Y.begin(), Y.end(), 63);
    ConvertYuvToRgba(Y.data(), U.data(), V.data(), W, H, 0, false, W, H, oi);
    CHECK(oi == o, "I420 == NV12");
    std::vector<uint8_t> Y16(W * H * 2), UV16(W * 2), op;
    for (uint32_t i = 0; i < W * H; ++i) {
        Y16[i * 2] = 0;
        Y16[i * 2 + 1] = 63;
    }
    for (uint32_t i = 0; i < W / 2; ++i) {
        UV16[i * 4 + 1] = 102;
        UV16[i * 4 + 3] = 240;
    }
    ConvertYuvToRgba(Y16.data(), UV16.data(), nullptr, W, H, 2, false, W, H, op);
    CHECK(op == o, "P016 == NV12");
    CHECK(!ConvertYuvToRgba(Y.data(), nullptr, nullptr, W, H, 1, false, W, H, op),
          "missing plane rejected");
}

}  // namespace

int main() {
    TestHidden();
    TestDisabledReasons();
    TestEnumerateUnsupported();
    TestStartReadyAndCreateInfo();
    TestWarming();
    TestCadenceAndFrameIds();
    TestSizeChange();
    TestBgraAndUnreadable();
    TestCreateUnsupported();
    TestLimitReachedNotLatched();
    TestTerminalWhileOnAndRecovery();
    TestIdlePoll();
    TestReset();
    TestFrameHelpers();
    if (g_failures) {
        std::fprintf(stderr, "lift_client_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("lift_client_test: all passed\n");
    return EXIT_SUCCESS;
}
