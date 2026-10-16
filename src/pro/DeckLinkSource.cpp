// SPDX-License-Identifier: Apache-2.0
#include "pro/DeckLinkSource.h"

#include "Log.h"
#include "pro/TimecodePairer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#if defined(MEDIAPLAYER_WITH_DECKLINK)
#ifndef NOMINMAX
#define NOMINMAX  // std::min / std::max below, not the windows.h macros
#endif
#include <windows.h>

#include <DeckLinkAPI.h>
#endif

namespace mp {

bool DeckLinkConfig::ParseStereo(const std::string& s, Stereo& out) {
    if (s.empty() || s == "auto") out = Stereo::Auto;
    else if (s == "mono") out = Stereo::Mono;
    else if (s == "sbs") out = Stereo::Sbs;
    else if (s == "dual") out = Stereo::Dual;
    else if (s == "pair") out = Stereo::Pair;
    else return false;
    return true;
}

const char* DeckLinkConfig::StereoName(Stereo s) {
    switch (s) {
        case Stereo::Auto: return "auto";
        case Stereo::Mono: return "mono";
        case Stereo::Sbs: return "sbs";
        case Stereo::Dual: return "dual";
        case Stereo::Pair: return "pair";
    }
    return "?";
}

DeckLinkSource::~DeckLinkSource() { Stop(); }

int DeckLinkSource::LayoutHint() const {
    if (cfg_.stereo == DeckLinkConfig::Stereo::Pair || cfg_.stereo == DeckLinkConfig::Stereo::Dual ||
        dualActive_)
        return 1;  // StereoLayout::SbsFull: the two eyes composed side by side
    if (cfg_.stereo == DeckLinkConfig::Stereo::Mono) return 0;  // StereoLayout::Mono
    return -1;
}

#if defined(MEDIAPLAYER_WITH_DECKLINK)
namespace {
int Pick(const std::vector<std::string>& names, const std::string& sel);  // below
}  // namespace
#endif

bool DeckLinkSource::Open(const DeckLinkConfig& cfg) {
    Stop();
    cfg_ = cfg;
    dualActive_ = false;
    name_ = "DeckLink " + cfg.device;
    if (cfg.stereo == DeckLinkConfig::Stereo::Pair) name_ += " + " + cfg.rightDevice;
    ResetStats(0, 0.0);
    if (!Available()) {
        Fail(LiveState::Failed, "this build has no Blackmagic DeckLink support");
        return false;
    }
    if (cfg.stereo == DeckLinkConfig::Stereo::Pair && cfg.rightDevice.empty()) {
        Fail(LiveState::Failed, "two-input stereo needs the right eye's input (--decklink-right)");
        return false;
    }
#if defined(MEDIAPLAYER_WITH_DECKLINK)
    // Resolve the display name now, on this thread: the UI reads it every frame.
    {
        const std::vector<std::string> devs = Enumerate();  // "i: name"
        auto nameOf = [&](const std::string& sel) {
            std::vector<std::string> names;
            for (const std::string& d : devs) names.push_back(d.substr(d.find(": ") + 2));
            const int i = Pick(names, sel);
            return i >= 0 ? names[(size_t)i] : "DeckLink " + sel;
        };
        name_ = nameOf(cfg.device);
        if (cfg.stereo == DeckLinkConfig::Stereo::Pair) name_ += " + " + nameOf(cfg.rightDevice);
    }
#endif
    SetState(LiveState::Opening);
    stop_ = false;
    worker_ = std::thread([this] { Worker(); });
    return true;
}

void DeckLinkSource::Stop() {
    if (worker_.joinable()) {
        stop_ = true;
        worker_.join();
        LOG_INFO("decklink: closed '%s'", name_.c_str());
    }
    stop_ = false;
    SetState(LiveState::Closed);
}

void DeckLinkSource::PrintDeviceList(FILE* out) {
    if (!Available()) {
        std::fprintf(out, "This build has no Blackmagic DeckLink support.\n");
        return;
    }
    const std::vector<std::string> devs = Enumerate();
    if (devs.empty()) {
        std::fprintf(out, "No Blackmagic devices (is Blackmagic Desktop Video installed and the "
                          "card connected?)\n");
        return;
    }
    std::fprintf(out, "Blackmagic devices (use the number or part of the name):\n");
    for (const std::string& d : devs) std::fprintf(out, "  %s\n", d.c_str());
}

#if defined(MEDIAPLAYER_WITH_DECKLINK)

bool DeckLinkSource::Available() { return true; }

namespace {

std::string FromBSTR(BSTR b) {
    if (!b) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, b, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, b, -1, s.data(), n, nullptr, nullptr);
    SysFreeString(b);
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

BMDDisplayMode FourCC(const std::string& s) {
    if (s.size() != 4) return bmdModeUnknown;
    return (BMDDisplayMode)(((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
                            ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3]);
}

template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// All DeckLink devices, in the driver's order, with their display names.
std::vector<IDeckLink*> AllDevices(std::vector<std::string>& names, bool* driver = nullptr) {
    std::vector<IDeckLink*> out;
    IDeckLinkIterator* it = nullptr;
    const bool ok = SUCCEEDED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator,
                                               (void**)&it)) && it;
    if (driver) *driver = ok;  // the iterator is registered by Blackmagic Desktop Video
    if (!ok) return out;
    IDeckLink* dl = nullptr;
    while (it->Next(&dl) == S_OK) {
        BSTR b = nullptr;
        dl->GetDisplayName(&b);
        names.push_back(FromBSTR(b));
        out.push_back(dl);
    }
    it->Release();
    return out;
}

// The device named by sel: an index ("0", "1", ...) or part of its name; -1 if none.
int Pick(const std::vector<std::string>& names, const std::string& sel) {
    char* end = nullptr;
    const long index = std::strtol(sel.c_str(), &end, 10);
    if (!sel.empty() && end && *end == '\0')
        return (index >= 0 && index < (long)names.size()) ? (int)index : -1;
    for (size_t i = 0; i < names.size(); ++i)
        if (Lower(names[i]).find(Lower(sel)) != std::string::npos) return (int)i;
    return -1;
}

std::string ListText(const std::vector<std::string>& names) {
    std::string s;
    for (size_t i = 0; i < names.size(); ++i)
        s += (s.empty() ? "" : ", ") + std::to_string(i) + ": " + names[i];
    return s;
}

// One eye's frame -> P016 at column x0 of dst (10-bit kept; 8-bit and RGB widened).
// False when the frame cannot be read.
bool ConvertEye(IDeckLinkVideoFrame* f, livepix::P016& dst, int x0) {
    IDeckLinkVideoBuffer* buf = nullptr;
    if (FAILED(f->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&buf)) || !buf) return false;
    void* bytes = nullptr;
    bool ok = SUCCEEDED(buf->StartAccess(bmdBufferAccessRead)) && SUCCEEDED(buf->GetBytes(&bytes)) &&
              bytes;
    if (ok) {
        const int w = (int)f->GetWidth(), h = (int)f->GetHeight();
        const int rowBytes = (int)f->GetRowBytes();
        const BMDPixelFormat pf = f->GetPixelFormat();
        if (pf == bmdFormat10BitYUV)
            livepix::V210ToP016((const uint8_t*)bytes, rowBytes, w & ~1, h & ~1, dst, x0);
        else if (pf == bmdFormat8BitBGRA)
            livepix::BgraToP016((const uint8_t*)bytes, rowBytes, w & ~1, h & ~1, dst, x0);
        else
            livepix::UyvyToP016((const uint8_t*)bytes, rowBytes, w & ~1, h & ~1, dst, x0);
        buf->EndAccess(bmdBufferAccessRead);
    }
    buf->Release();
    return ok;
}

// The frame's timecode as text and as a frame count at the full frame rate (-1 when it
// has none). A high-frame-rate timecode (HFRTC, counts every frame above 30 fps) is
// preferred when the card has one; otherwise RP188 (VITC1 / VITC2 / LTC) or VITC, whose
// frame pairs at 50/60p are unfolded with the field mark (TimecodeFrames).
int ReadTimecode(IDeckLinkVideoInputFrame* f, int fpsInt, char* text, size_t n) {
    text[0] = 0;
    IDeckLinkTimecode* tc = nullptr;
    TimecodeFields t;
    if (SUCCEEDED(f->GetTimecode(bmdTimecodeRP188HighFrameRate, &tc)) && tc) {
        t.highFrameRate = true;
    } else {
        Release(tc);
        tc = nullptr;
        if (FAILED(f->GetTimecode(bmdTimecodeRP188Any, &tc)) || !tc) {
            Release(tc);
            tc = nullptr;
            if (FAILED(f->GetTimecode(bmdTimecodeVITC, &tc)) || !tc) {
                Release(tc);
                return -1;
            }
        }
    }
    unsigned char hh = 0, mm = 0, ss = 0, ff = 0;
    int key = -1;
    if (SUCCEEDED(tc->GetComponents(&hh, &mm, &ss, &ff))) {
        const BMDTimecodeFlags flags = tc->GetFlags();
        t.hh = hh;
        t.mm = mm;
        t.ss = ss;
        t.ff = ff;
        t.fieldMark = (flags & bmdTimecodeFieldMark) != 0;
        t.dropFrame = (flags & bmdTimecodeIsDropFrame) != 0;
        // A source counting every frame without an HFRTC shows as ff past half the rate.
        FormatTimecode(t, std::max(1, fpsInt), text, n);
        key = TimecodeFrames(t, std::max(1, fpsInt));
    }
    tc->Release();
    return key;
}

}  // namespace

// Everything the card's callback threads share for one capture session.
struct DeckLinkCapture {
    DeckLinkSource* src = nullptr;
    bool pair = false;     // two inputs, eye 0 and eye 1
    bool dual = false;     // one input in the card's dual-stream 3D mode
    std::atomic<int> fpsInt{25};
    std::atomic<bool> noSignal[2]{};

    std::mutex m;  // the frames below + the ring's single-producer side
    std::atomic<bool> alive{true};  // cleared before the inputs stop: late callbacks do nothing
    livepix::P016 out;

    // Pair: the last few frames of each eye; TimecodePairer decides which two to show.
    TimecodePairer pairer;
    struct EyeFrame {
        livepix::P016 px;
        double arrived = 0.0;
        int64_t hwUs = -1;  // the card's hardware reference clock, microseconds (-1 unknown)
        char tc[16] = {0};
    };
    // No timecode: the hardware reference clock stands in for it (#108's pairing). Frame
    // index = round((t - t0) / period) with t0 from the first frame, so two genlocked
    // inputs less than half a frame apart get the same index and pair exactly.
    std::atomic<int64_t> hwT0{INT64_MIN};
    uint64_t framesIn[2] = {0, 0};
    EyeFrame hist[2][TimecodePairer::kHistory];
    bool warnedUnsynced = false;
    bool warnedSize = false;

    void SetFps(double fps) { src->SetFps(fps); }

    void OnFrame(int eye, IDeckLinkVideoInputFrame* f) {
        if (!alive) return;
        const int w = (int)f->GetWidth() & ~1, h = (int)f->GetHeight() & ~1;
        const double now = LiveProducer::NowSeconds();
        char tc[16];
        int key = ReadTimecode(f, fpsInt, tc, sizeof(tc));
        BMDTimeValue hwTime = 0, hwDur = 0;
        int64_t hwUs = -1;
        if (SUCCEEDED(f->GetHardwareReferenceTimestamp(1000000, &hwTime, &hwDur))) hwUs = (int64_t)hwTime;
        if (!pair) {
            // one input: only this card thread touches `out`; the lock keeps Stop() simple
            std::lock_guard<std::mutex> lk(m);
            out.Resize(dual ? 2 * w : w, h);
            if (!ConvertEye(f, out, 0)) return;
            if (dual) {
                IDeckLinkVideoFrame3DExtensions* ext = nullptr;
                IDeckLinkVideoFrame* right = nullptr;
                if (SUCCEEDED(f->QueryInterface(IID_IDeckLinkVideoFrame3DExtensions, (void**)&ext)) &&
                    SUCCEEDED(ext->GetFrameForRightEye(&right)) && right)
                    ConvertEye(right, out, w);
                else
                    livepix::CopyEye(out, 0, w, w);  // no right eye this frame: keep the pair valid
                Release(right);
                Release(ext);
            }
            if (alive) src->Publish(out, /*fullRange=*/false, now, tc[0] ? tc : nullptr, dual ? 0 : -1);
            return;
        }
        // Pair: claim this eye's oldest slot, convert into it WITHOUT the lock (the two
        // card threads convert in parallel; a busy slot is never chosen), then decide.
        int slot;
        {
            std::lock_guard<std::mutex> lk(m);
            slot = pairer.NextSlot(eye);
            pairer.MarkBusy(eye, slot);
        }
        EyeFrame& ef = hist[eye][slot];
        ef.px.Resize(w, h);
        if (!ConvertEye(f, ef.px, 0)) return;
        ef.arrived = now;
        ef.hwUs = hwUs;
        bool hwKey = false;
        if (key < 0 && hwUs >= 0) {
            int64_t t0 = hwT0.load();
            if (t0 == INT64_MIN) {
                hwT0.compare_exchange_strong(t0, hwUs);
                t0 = hwT0.load();
            }
            const double periodUs = 1e6 / std::max(1, fpsInt.load());
            key = (int)std::llround((double)(hwUs - t0) / periodUs);
            hwKey = true;
        }
        std::memcpy(ef.tc, tc, sizeof(tc));
        std::lock_guard<std::mutex> lk(m);
        ++framesIn[eye];
        const TimecodePairer::Decision d = pairer.Add(eye, slot, key, now, 1.0 / std::max(1, fpsInt.load()));
        if (!d.publish || !alive) return;
        {
            const EyeFrame& lf = hist[0][d.slot[0]];
            const EyeFrame& rf = hist[1][d.slot[1]];
            std::lock_guard<std::mutex> ps(src->pairMutex_);
            DeckLinkSource::PairStats& st = src->pairStats_;
            ++st.pairs;
            const uint64_t in = framesIn[0] + framesIn[1];
            st.unpaired = in > 2 * st.pairs ? in - 2 * st.pairs : 0;
            st.hwClockPairing = hwKey;
            if (lf.hwUs >= 0 && rf.hwUs >= 0) {
                st.lastSkewMs = (double)(rf.hwUs - lf.hwUs) / 1000.0;
                st.maxAbsSkewMs = std::max(st.maxAbsSkewMs, std::fabs(st.lastSkewMs));
            }
        }
        if (d.unsynced && !warnedUnsynced) {
            LOG_WARN("decklink: the two inputs' timecodes are %d frames apart - not jam-synced; "
                     "pairing the newest frames instead", d.offsetFrames);
            warnedUnsynced = true;
        }
        const EyeFrame& l = hist[0][d.slot[0]];
        const EyeFrame& r = hist[1][d.slot[1]];
        const int cw = std::min(l.px.width, r.px.width), ch = std::min(l.px.height, r.px.height);
        if ((l.px.width != r.px.width || l.px.height != r.px.height) && !warnedSize) {
            LOG_WARN("decklink: the two inputs differ in size (%dx%d vs %dx%d)", l.px.width, l.px.height,
                     r.px.width, r.px.height);
            warnedSize = true;
        }
        out.Resize(2 * cw, ch);
        for (int row = 0; row < ch; ++row) {
            std::memcpy(out.y.data() + (size_t)row * out.width, l.px.y.data() + (size_t)row * l.px.width, (size_t)cw * 2);
            std::memcpy(out.y.data() + (size_t)row * out.width + cw, r.px.y.data() + (size_t)row * r.px.width, (size_t)cw * 2);
        }
        for (int row = 0; row < ch / 2; ++row) {
            std::memcpy(out.uv.data() + (size_t)row * out.width, l.px.uv.data() + (size_t)row * l.px.width, (size_t)cw * 2);
            std::memcpy(out.uv.data() + (size_t)row * out.width + cw, r.px.uv.data() + (size_t)row * r.px.width, (size_t)cw * 2);
        }
        src->Publish(out, /*fullRange=*/false, std::min(l.arrived, r.arrived), l.tc[0] ? l.tc : nullptr,
                     d.offsetFrames);
    }
};

namespace {

// Receives one input's frames on the card's thread.
class Callback : public IDeckLinkInputCallback {
public:
    Callback(std::shared_ptr<DeckLinkCapture> cap, int eye, IDeckLinkInput* input, BMDVideoInputFlags flags,
             bool autoDual, std::atomic<bool>& dualActive, BMDPixelFormat yuvFormat)
        : cap_(std::move(cap)), eye_(eye), input_(input), flags_(flags), pixelFormat_(yuvFormat),
          yuvFormat_(yuvFormat), autoDual_(autoDual), dualActive_(dualActive) {
        input_->AddRef();  // our own reference: a late callback never uses a released input
    }
    ~Callback() { input_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override {
        if (iid == IID_IUnknown || iid == IID_IDeckLinkInputCallback) {
            *ppv = static_cast<IDeckLinkInputCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // The signal changed (resolution, rate, 422/444, 3D): restart in the new format.
    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events,
                                                      IDeckLinkDisplayMode* mode,
                                                      BMDDetectedVideoInputFormatFlags detected) override {
        const BMDPixelFormat pf = (detected & bmdDetectedVideoInputRGB444) ? bmdFormat8BitBGRA : yuvFormat_;
        if (!cap_->alive) return S_OK;
        bool flagsChanged = false;
        if ((detected & bmdDetectedVideoInputDualStream3D) && autoDual_ && !dualActive_ && !dualRefused_) {
            flags_ |= bmdVideoInputDualStream3D;  // support is per mode: try it, fall back below
            flagsChanged = true;
        } else if (!(detected & bmdDetectedVideoInputDualStream3D) && autoDual_ && dualActive_) {
            // the signal went back to 2D: leave dual-stream capture
            flags_ &= ~(BMDVideoInputFlags)bmdVideoInputDualStream3D;
            flagsChanged = true;
            std::lock_guard<std::mutex> lk(cap_->m);
            cap_->dual = false;
            dualActive_ = false;
            LOG_INFO("decklink: the signal is 2D again; left dual-stream 3D capture");
        }
        if (!(events & (bmdVideoInputDisplayModeChanged | bmdVideoInputColorspaceChanged)) &&
            pf == pixelFormat_ && !flagsChanged)
            return S_OK;
        BSTR name = nullptr;
        mode->GetName(&name);
        BMDTimeValue dur = 0;
        BMDTimeScale scale = 0;
        if (SUCCEEDED(mode->GetFrameRate(&dur, &scale)) && dur) {
            const double fps = (double)scale / (double)dur;
            cap_->fpsInt = (int)std::lround(fps);
            cap_->SetFps(fps);
        }
        LOG_INFO("decklink: input %d signal is now %s, %s%s", eye_, FromBSTR(name).c_str(),
                 (detected & bmdDetectedVideoInputRGB444) ? "RGB 4:4:4" : "YUV 4:2:2",
                 (detected & bmdDetectedVideoInputDualStream3D) ? ", dual-stream 3D" : "");
        pixelFormat_ = pf;
        // Restart under the capture lock, and only while capture is still on: the worker
        // clears `alive` (taking the same lock) before it stops and disables the input.
        std::lock_guard<std::mutex> restartLock(cap_->m);
        if (!cap_->alive) return S_OK;
        input_->PauseStreams();
        HRESULT r = input_->EnableVideoInput(mode->GetDisplayMode(), pf, flags_);
        if (FAILED(r) && (flags_ & bmdVideoInputDualStream3D) && !dualActive_) {
            LOG_WARN("decklink: the signal is dual-stream 3D but this card cannot capture it in this "
                     "mode; showing one eye");
            flags_ &= ~(BMDVideoInputFlags)bmdVideoInputDualStream3D;
            dualRefused_ = true;
            r = input_->EnableVideoInput(mode->GetDisplayMode(), pf, flags_);
        } else if (SUCCEEDED(r) && (flags_ & bmdVideoInputDualStream3D) && !dualActive_) {
            LOG_INFO("decklink: switched to dual-stream 3D capture");
            cap_->dual = true;  // (cap_->m is held: restartLock)
            dualActive_ = true;
        }
        if (FAILED(r) && pf == bmdFormat10BitYUV) {  // refused 10-bit in this format: 8-bit
            yuvFormat_ = pixelFormat_ = bmdFormat8BitYUV;
            r = input_->EnableVideoInput(mode->GetDisplayMode(), bmdFormat8BitYUV, flags_);
            if (SUCCEEDED(r)) LOG_INFO("decklink: input %d: 10-bit refused in this format, using 8-bit", eye_);
        }
        if (FAILED(r)) LOG_WARN("decklink: input %d cannot restart in the new format", eye_);
        input_->FlushStreams();
        input_->StartStreams();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame,
                                                     IDeckLinkAudioInputPacket*) override {
        if (!frame || !cap_->alive) return S_OK;
        if (frame->GetFlags() & bmdFrameHasNoInputSource) {
            if (!cap_->noSignal[eye_]) LOG_WARN("decklink: input %d has no signal", eye_);
            cap_->noSignal[eye_] = true;
            return S_OK;  // keep showing the last picture; the stall watchdog flags it
        }
        if (cap_->noSignal[eye_]) LOG_INFO("decklink: input %d signal back", eye_);
        cap_->noSignal[eye_] = false;
        cap_->OnFrame(eye_, frame);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<DeckLinkCapture> cap_;  // outlives any callback the SDK still delivers
    int eye_;
    IDeckLinkInput* input_;
    BMDVideoInputFlags flags_;
    BMDPixelFormat pixelFormat_;
    BMDPixelFormat yuvFormat_;  // 10-bit; 8-bit when the card refused 10-bit
    bool autoDual_;
    bool dualRefused_ = false;
    std::atomic<bool>& dualActive_;
};

}  // namespace

std::string DeckLinkSource::UnavailableReason() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<std::string> names;
    bool driver = false;
    for (IDeckLink* d : AllDevices(names, &driver)) d->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    if (!driver) return "Blackmagic Desktop Video driver not installed";
    if (names.empty()) return "no DeckLink card found (Desktop Video is installed)";
    return "";
}

DeckLinkSource::PairStats DeckLinkSource::GetPairStats() const {
    std::lock_guard<std::mutex> lk(pairMutex_);
    return pairStats_;
}

std::vector<std::string> DeckLinkSource::Enumerate() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<std::string> names;
    for (IDeckLink* d : AllDevices(names)) d->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    std::vector<std::string> out;
    for (size_t i = 0; i < names.size(); ++i) out.push_back(std::to_string(i) + ": " + names[i]);
    return out;
}

void DeckLinkSource::Worker() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<std::string> names;
    bool driver = false;
    std::vector<IDeckLink*> devices = AllDevices(names, &driver);
    const bool pair = cfg_.stereo == DeckLinkConfig::Stereo::Pair;
    {
        std::lock_guard<std::mutex> lk(pairMutex_);
        pairStats_ = PairStats{};
    }
    const int nInputs = pair ? 2 : 1;

    auto capPtr = std::make_shared<DeckLinkCapture>();
    DeckLinkCapture& cap = *capPtr;
    cap.src = this;
    cap.pair = pair;
    cap.dual = cfg_.stereo == DeckLinkConfig::Stereo::Dual;

    IDeckLinkInput* inputs[2] = {nullptr, nullptr};
    Callback* callbacks[2] = {nullptr, nullptr};
    int started = 0;
    std::string err;

    if (!driver) {
        err = "Blackmagic Desktop Video driver not installed";
    } else if (devices.empty()) {
        err = "no DeckLink card found (Desktop Video is installed; is the card seated?)";
    } else {
        const std::string sel[2] = {cfg_.device, cfg_.rightDevice};
        for (int e = 0; e < nInputs && err.empty(); ++e) {
            const int idx = Pick(names, sel[e]);
            if (idx < 0) {
                err = "no Blackmagic device matches \"" + sel[e] + "\" (found " + ListText(names) + ")";
                break;
            }
            IDeckLink* dev = devices[(size_t)idx];  // (name_ was resolved in Open: no race with the UI)
            IDeckLinkProfileAttributes* attrs = nullptr;
            IDeckLinkConfiguration* config = nullptr;
            dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attrs);
            dev->QueryInterface(IID_IDeckLinkConfiguration, (void**)&config);
            if (FAILED(dev->QueryInterface(IID_IDeckLinkInput, (void**)&inputs[e])) || !inputs[e]) {
                err = names[(size_t)idx] + " has no video input";
            } else {
                if (config && !cfg_.input.empty()) {
                    const std::string in = Lower(cfg_.input);
                    const LONGLONG conn = in == "hdmi" ? bmdVideoConnectionHDMI : in == "sdi" ? bmdVideoConnectionSDI : 0;
                    if (conn && FAILED(config->SetInt(bmdDeckLinkConfigVideoInputConnection, conn)))
                        LOG_WARN("decklink: %s: cannot select input %s", names[(size_t)idx].c_str(),
                                 cfg_.input.c_str());
                }
                BOOL detection = FALSE;
                if (attrs) attrs->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &detection);
                BMDVideoInputFlags flags = bmdVideoInputFlagDefault;
                if (detection) flags |= bmdVideoInputEnableFormatDetection;
                if (cap.dual) flags |= bmdVideoInputDualStream3D;
                BMDDisplayMode mode = FourCC(cfg_.mode);
                if (mode == bmdModeUnknown) mode = bmdModeHD1080p25;  // detection follows the signal
                if (!detection && cfg_.mode.empty())
                    LOG_WARN("decklink: %s cannot detect the input format: assuming 1080p25, set "
                             "--decklink-mode=<FourCC> for others", names[(size_t)idx].c_str());
                // 10-bit 4:2:2 (v210) so the look grades at full precision; 8-bit if refused
                BMDPixelFormat yuvFmt = bmdFormat10BitYUV;
                HRESULT r = inputs[e]->EnableVideoInput(mode, yuvFmt, flags);
                if (FAILED(r)) {
                    yuvFmt = bmdFormat8BitYUV;
                    r = inputs[e]->EnableVideoInput(mode, yuvFmt, flags);
                    if (SUCCEEDED(r))
                        LOG_INFO("decklink: %s: 10-bit capture refused, using 8-bit", names[(size_t)idx].c_str());
                }
                if (FAILED(r)) {
                    err = cap.dual ? names[(size_t)idx] + " does not support dual-stream 3D input in this mode"
                                   : names[(size_t)idx] + ": cannot start the video input (in use by another application?)";
                } else {
                    IDeckLinkDisplayMode* dm = nullptr;
                    BMDTimeValue dur = 0;
                    BMDTimeScale scale = 0;
                    if (SUCCEEDED(inputs[e]->GetDisplayMode(mode, &dm)) && dm &&
                        SUCCEEDED(dm->GetFrameRate(&dur, &scale)) && dur) {
                        cap.fpsInt = (int)std::lround((double)scale / (double)dur);
                        SetFps((double)scale / (double)dur);
                    }
                    Release(dm);
                    const bool autoDual = cfg_.stereo == DeckLinkConfig::Stereo::Auto && !pair;
                    callbacks[e] = new Callback(capPtr, e, inputs[e], flags, autoDual, dualActive_, yuvFmt);
                    inputs[e]->SetCallback(callbacks[e]);
                    if (FAILED(inputs[e]->StartStreams())) {
                        err = names[(size_t)idx] + ": cannot start capture";
                        inputs[e]->SetCallback(nullptr);
                        inputs[e]->DisableVideoInput();
                    } else {
                        ++started;
                        LOG_INFO("decklink: capturing from %s%s%s", names[(size_t)idx].c_str(),
                                 detection ? " (format auto-detected)" : "",
                                 cap.dual ? ", dual-stream 3D" : pair ? (e == 0 ? ", left eye" : ", right eye") : "");
                    }
                }
            }
            Release(config);
            Release(attrs);
        }
    }

    if (!err.empty()) {
        LOG_WARN("decklink: %s", err.c_str());
        Fail(LiveState::Failed, err);
    } else {
        while (!stop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CheckStall(2.0);
        }
    }

    {
        std::lock_guard<std::mutex> lk(cap.m);  // no callback is mid-publish or mid-restart...
        cap.alive = false;                      // ...and none starts one from here on
    }
    for (int e = 0; e < 2; ++e) {
        if (inputs[e]) {
            if (e < started || callbacks[e]) {
                inputs[e]->StopStreams();
                inputs[e]->SetCallback(nullptr);
                inputs[e]->DisableVideoInput();
            }
        }
    }
    // The SDK does not promise that no callback is still running when StopStreams returns:
    // give one in flight time to see `alive` and return before our references go.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int e = 0; e < 2; ++e) {
        Release(callbacks[e]);
        Release(inputs[e]);
    }
    for (IDeckLink* d : devices) d->Release();
    if (SUCCEEDED(co)) CoUninitialize();
}

#else  // no DeckLink SDK in this build

bool DeckLinkSource::Available() { return false; }
std::vector<std::string> DeckLinkSource::Enumerate() { return {}; }
std::string DeckLinkSource::UnavailableReason() { return "this build has no DeckLink SDK"; }
DeckLinkSource::PairStats DeckLinkSource::GetPairStats() const { return {}; }
void DeckLinkSource::Worker() {}

#endif

}  // namespace mp
