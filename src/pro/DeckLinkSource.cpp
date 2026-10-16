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
// The Desktop Video SDK is one C++ API on three platforms with three different COM
// skins: Windows is real COM (CoCreateInstance, BSTR, GUID equality), macOS is
// CFPlugInCOM (CreateDeckLinkIteratorInstance, CFStringRef, CFUUIDBytes), Linux is the
// SDK's LinuxCOM.h (plain C strings you free(), 16-byte REFIID structs). Everything
// below the `dl` shim is shared.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX  // std::min / std::max below, not the windows.h macros
#endif
#include <windows.h>
#include <DeckLinkAPI.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <DeckLinkAPI.h>
#else
#include <DeckLinkAPI.h>
#endif
#if __has_include(<DeckLinkAPIVersion.h>)
#include <DeckLinkAPIVersion.h>
#endif
#ifndef BLACKMAGIC_DECKLINK_API_VERSION
#error "DeckLinkAPIVersion.h not found: the SDK's include folder must be on the include path (CMake MP_DECKLINK_SDK_DIR)"
#endif
// 14.3 moved pixel access from IDeckLinkVideoFrame::GetBytes to IDeckLinkVideoBuffer
// (frames are IOSurface / allocator-backed since then). Both are supported; which one is
// compiled in is decided by the SDK version, and at run time the installed Desktop Video
// has to be at least that version (Worker() checks and says so).
#define MP_DL_SDK_HAS_VIDEO_BUFFER (BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000)
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
    using S = DeckLinkConfig::Stereo;
    if (cfg_.stereo == S::Pair || cfg_.stereo == S::Dual || dualActive_)
        return 1;  // StereoLayout::SbsFull: the two eyes composed side by side
    if (cfg_.stereo == S::Mono) return 0;  // StereoLayout::Mono
    if (cfg_.stereo == S::Sbs) {
        // One cable carrying both eyes: a 16:9 frame is the squeezed half-width kind
        // (Resolve's "side by side" stereo out), anything wider than 3:1 is full width.
        const LiveStats st = GetStats();
        return (st.w > 0 && st.h > 0 && st.w > 3 * st.h) ? 1 : 2;  // SbsFull : SbsHalf
    }
    return -1;  // Auto: guessed from the aspect (and from the card's 3D flag, above)
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
        std::vector<std::string> names;
        for (const std::string& d : devs) names.push_back(d.substr(d.find(": ") + 2));
        auto nameOf = [&](const std::string& sel) {
            const int i = Pick(names, sel);
            return i >= 0 ? names[(size_t)i] : "DeckLink " + sel;
        };
        name_ = nameOf(cfg.device);
        if (cfg.stereo == DeckLinkConfig::Stereo::Pair) {
            name_ += " + " + nameOf(cfg.rightDevice);
            const int l = Pick(names, cfg.device), r = Pick(names, cfg.rightDevice);
            if (l >= 0 && l == r) {
                Fail(LiveState::Failed, "two-input stereo needs two different inputs; \"" + cfg.device +
                                            "\" and \"" + cfg.rightDevice + "\" are the same device (" +
                                            names[(size_t)l] + ")");
                return false;
            }
        }
    }
#endif
    SetState(LiveState::Opening);
    stop_ = false;
    worker_ = std::thread([this] { Worker(); });
    return true;
}

void DeckLinkSource::Stop() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(stopMutex_);
            stop_ = true;
        }
        stopCv_.notify_all();
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
        std::fprintf(out, "No Blackmagic inputs: %s.\n", UnavailableReason().c_str());
        return;
    }
    std::fprintf(out, "Blackmagic devices (use the number or part of the name):\n");
    for (const std::string& d : devs) std::fprintf(out, "  %s\n", d.c_str());
}

#if defined(MEDIAPLAYER_WITH_DECKLINK)

bool DeckLinkSource::Available() { return true; }

// ------------------------------------------------------------------- platform shim
namespace dl {

#if defined(_WIN32)
using String = BSTR;
using Bool = BOOL;
// A string the SDK handed us: convert to UTF-8 and release it.
std::string Take(String b) {
    if (!b) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, b, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, b, -1, s.data(), n, nullptr, nullptr);
    SysFreeString(b);
    return s;
}
// COM on this thread for the scope's lifetime (no-op where COM is already up).
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};
// Null when Blackmagic Desktop Video is not installed (the iterator is registered by it).
IDeckLinkIterator* CreateIterator() {
    IDeckLinkIterator* it = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)&it)))
        return nullptr;
    return it;
}
IDeckLinkAPIInformation* CreateApiInformation() {
    IDeckLinkAPIInformation* info = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkAPIInformation, nullptr, CLSCTX_ALL, IID_IDeckLinkAPIInformation,
                                (void**)&info)))
        return nullptr;
    return info;
}
bool IidIs(REFIID a, REFIID b) { return a == b; }
bool IsIUnknown(REFIID a) { return a == IID_IUnknown; }
#else
#if defined(__APPLE__)
using String = CFStringRef;
std::string Take(String s) {
    if (!s) return {};
    std::string out;
    char small[512];
    if (CFStringGetCString(s, small, sizeof(small), kCFStringEncodingUTF8)) {
        out = small;
    } else {
        const CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
        std::vector<char> big((size_t)n, '\0');
        if (CFStringGetCString(s, big.data(), n, kCFStringEncodingUTF8)) out = big.data();
    }
    CFRelease(s);
    return out;
}
bool IsIUnknown(REFIID a) {
    const CFUUIDBytes u = CFUUIDGetUUIDBytes(IUnknownUUID);
    return std::memcmp(&a, &u, sizeof(u)) == 0;
}
#else  // Linux: LinuxCOM.h
using String = const char*;
std::string Take(String s) {
    std::string out = s ? s : "";
    std::free((void*)s);  // the SDK strdup()s what it returns
    return out;
}
bool IsIUnknown(REFIID a) { return std::memcmp(&a, &IID_IUnknown, sizeof(REFIID)) == 0; }
#endif
using Bool = bool;
struct ComScope {};
// Null when the DeckLinkAPI framework / libDeckLinkAPI.so is not installed: the
// dispatch file (DeckLinkAPIDispatch.cpp) loads it at run time.
IDeckLinkIterator* CreateIterator() { return CreateDeckLinkIteratorInstance(); }
IDeckLinkAPIInformation* CreateApiInformation() { return CreateDeckLinkAPIInformationInstance(); }
bool IidIs(REFIID a, REFIID b) { return std::memcmp(&a, &b, sizeof(REFIID)) == 0; }
#endif

}  // namespace dl

namespace {

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

// "14.4.1" from the SDK's 0xMMmmrrbb layout (BMDDeckLinkAPIVersion uses the same).
std::string VersionText(int64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%d.%d.%d", (int)((v >> 24) & 0xff), (int)((v >> 16) & 0xff), (int)((v >> 8) & 0xff));
    return b;
}

// The installed Desktop Video's API version (0 = unknown), and whether this build's SDK
// is newer than it. An app built against 14.3+ reads frames through interfaces an older
// driver does not have, so it must refuse instead of showing nothing.
int64_t DriverVersion() {
    IDeckLinkAPIInformation* info = dl::CreateApiInformation();
    if (!info) return 0;
    int64_t v = 0;
    if (FAILED(info->GetInt(BMDDeckLinkAPIVersion, &v))) v = 0;
    info->Release();
    return v;
}
std::string DriverTooOld(int64_t driver) {
    if (driver <= 0) return {};
    const int64_t need = (int64_t)BLACKMAGIC_DECKLINK_API_VERSION & 0xffff0000;
    if ((driver & 0xffff0000) >= need) return {};
    return "Blackmagic Desktop Video " + VersionText(driver) + " is installed; this build needs " +
           VersionText(need) + " or newer (update Desktop Video)";
}

// Devices with a video input, in the driver's order, with their display names.
// `driver` says whether Desktop Video answered at all.
std::vector<IDeckLink*> AllInputDevices(std::vector<std::string>& names, bool* driver = nullptr,
                                        std::vector<int64_t>* groups = nullptr) {
    std::vector<IDeckLink*> out;
    IDeckLinkIterator* it = dl::CreateIterator();
    if (driver) *driver = it != nullptr;
    if (!it) return out;
    IDeckLink* d = nullptr;
    while (it->Next(&d) == S_OK) {
        bool capture = true;
        int64_t group = -1;
        IDeckLinkProfileAttributes* attrs = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attrs)) && attrs) {
            int64_t io = 0;
            if (SUCCEEDED(attrs->GetInt(BMDDeckLinkVideoIOSupport, &io))) capture = (io & bmdDeviceSupportsCapture) != 0;
            if (FAILED(attrs->GetInt(BMDDeckLinkDeviceGroupID, &group))) group = -1;
            attrs->Release();
        }
        if (!capture) {  // an output-only sub-device: not something to open as an input
            d->Release();
            continue;
        }
        dl::String s = nullptr;
        d->GetDisplayName(&s);
        names.push_back(dl::Take(s));
        if (groups) groups->push_back(group);
        out.push_back(d);
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
// False when the frame's pixels cannot be read.
bool ConvertEye(IDeckLinkVideoFrame* f, livepix::P016& dst, int x0) {
    void* bytes = nullptr;
#if MP_DL_SDK_HAS_VIDEO_BUFFER
    IDeckLinkVideoBuffer* buf = nullptr;
    if (FAILED(f->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&buf)) || !buf) return false;
    const bool ok = SUCCEEDED(buf->StartAccess(bmdBufferAccessRead)) && SUCCEEDED(buf->GetBytes(&bytes)) && bytes;
#else
    IUnknown* buf = nullptr;  // nothing to release on the pre-14.3 path
    const bool ok = SUCCEEDED(f->GetBytes(&bytes)) && bytes;
#endif
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
#if MP_DL_SDK_HAS_VIDEO_BUFFER
        buf->EndAccess(bmdBufferAccessRead);
#endif
    }
    Release(buf);
    return ok;
}

// The frame's timecode as text and as a pairing key (-1 when it has none). Above 30 fps
// RP188 counts frame PAIRS and tells the two apart with the field mark, which the key
// has to carry or every other pair of frames shares a key (TimecodeKey()).
int ReadTimecode(IDeckLinkVideoInputFrame* f, int fpsInt, char* text, size_t n) {
    text[0] = 0;
    IDeckLinkTimecode* tc = nullptr;
    if (FAILED(f->GetTimecode(bmdTimecodeRP188Any, &tc)) || !tc) {
        Release(tc);
        if (FAILED(f->GetTimecode(bmdTimecodeVITC, &tc)) || !tc) {
            Release(tc);
            return -1;
        }
    }
    unsigned char hh = 0, mm = 0, ss = 0, ff = 0;
    int key = -1;
    if (SUCCEEDED(tc->GetComponents(&hh, &mm, &ss, &ff))) {
        const BMDTimecodeFlags flags = tc->GetFlags();
        const bool field = (flags & bmdTimecodeFieldMark) != 0;
        std::snprintf(text, n, "%02u:%02u:%02u%c%02u", hh, mm, ss, (flags & bmdTimecodeIsDropFrame) ? ';' : ':', ff);
        key = TimecodeKey(hh, mm, ss, ff, field, fpsInt);
    }
    tc->Release();
    return key;
}

// The signal's colorimetry from the frame's metadata (VPID / HDMI InfoFrame as the card
// reports it): the Y'CbCr matrix the shader must decode with, and the transfer as a tag.
// Frames without metadata keep the converters' SD/HD rule.
void ReadColour(IDeckLinkVideoFrame* f, ColorMatrix& matrix, ColorTransfer& transfer, bool& warnedHdr) {
    IDeckLinkVideoFrameMetadataExtensions* md = nullptr;
    if (FAILED(f->QueryInterface(IID_IDeckLinkVideoFrameMetadataExtensions, (void**)&md)) || !md) return;
    int64_t v = 0;
    if (SUCCEEDED(md->GetInt(bmdDeckLinkFrameMetadataColorspace, &v))) {
        if (v == bmdColorspaceRec601) matrix = ColorMatrix::BT601;
        else if (v == bmdColorspaceRec709) matrix = ColorMatrix::BT709;
        else if (v == bmdColorspaceRec2020) matrix = ColorMatrix::BT2020;
    }
    if ((f->GetFlags() & bmdFrameContainsHDRMetadata) &&
        SUCCEEDED(md->GetInt(bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc, &v))) {
        transfer = v == 2 ? ColorTransfer::PQ : v == 3 ? ColorTransfer::HLG : ColorTransfer::SDR;  // CEA 861.3
        if (transfer != ColorTransfer::SDR && !warnedHdr) {
            LOG_WARN("decklink: the signal is %s; it is shown as SDR (no tone mapping) - monitor through a LUT",
                     transfer == ColorTransfer::PQ ? "PQ" : "HLG");
            warnedHdr = true;
        }
    }
    md->Release();
}

const char* FieldText(BMDFieldDominance fd) {
    switch (fd) {
        case bmdLowerFieldFirst: return "interlaced (lower field first)";
        case bmdUpperFieldFirst: return "interlaced (upper field first)";
        case bmdProgressiveSegmentedFrame: return "PsF";
        default: return "progressive";
    }
}

}  // namespace

// Everything the card's callback threads share for one capture session.
struct DeckLinkCapture {
    DeckLinkSource* src = nullptr;
    bool pair = false;     // two inputs, eye 0 and eye 1
    bool dual = false;     // one input in the card's dual-stream 3D mode
    bool sameCard = false; // Pair: both inputs are sub-devices of one card (one clock)
    std::atomic<int> fpsInt{25};
    std::atomic<double> fpsExact{25.0};
    std::atomic<bool> noSignal[2]{};
    std::atomic<bool> dualActive{false};  // Auto switched to the card's dual-stream mode

    std::mutex m;  // the frames below + the ring's single-producer side
    std::atomic<bool> alive{true};  // cleared before the inputs stop: late callbacks do nothing
    livepix::P016 out;

    // Pair: the last few frames of each eye; TimecodePairer decides which two to show.
    TimecodePairer pairer;
    struct EyeFrame {
        livepix::P016 px;
        double arrived = 0.0;
        int64_t hwNs = -1;  // the card's hardware reference clock, nanoseconds (-1 unknown)
        char tc[16] = {0};
    };
    // No timecode, one card: its hardware clock stands in (#108's pairing). Frame index =
    // round((t - t0) / duration) with t0 from the first frame and the duration the card
    // reports, both in nanoseconds (not a rounded fps, and not microseconds: at 59.94 a
    // duration rounded to 1 us drifts half a frame in seven minutes), so two genlocked
    // inputs under half a frame apart share an index for days. Two separate cards have
    // two clocks, so there it is not used at all.
    std::atomic<int64_t> hwT0{INT64_MIN};
    uint64_t framesIn[2] = {0, 0};
    EyeFrame hist[2][TimecodePairer::kHistory];
    bool warnedUnsynced = false;
    bool warnedSize = false;
    bool warnedConvert = false;
    bool warnedHdr = false;

    void SetFps(double fps) {
        fpsExact = fps;
        fpsInt = (int)std::lround(fps);
        src->SetFps(fps);
    }

    // Format changed on an input: the pairing history is from the old signal.
    void ResetPairing() {  // cap.m held
        pairer.Reset();
        hwT0 = INT64_MIN;
        framesIn[0] = framesIn[1] = 0;
    }

    // The card's dual-stream state, mirrored into the source for LayoutHint(). m held.
    void SetDualActive(bool on) {
        dualActive = on;
        if (alive) src->dualActive_ = on;
    }

    void NoSignal(int eye, bool on) {
        if (noSignal[eye] == on) return;
        noSignal[eye] = on;
        if (on) LOG_WARN("decklink: input %d has no signal", eye);
        else LOG_INFO("decklink: input %d signal back", eye);
        src->SetNoSignal(eye, on);
    }

    void OnFrame(int eye, IDeckLinkVideoInputFrame* f) {
        if (!alive) return;
        const int w = (int)f->GetWidth() & ~1, h = (int)f->GetHeight() & ~1;
        const double now = LiveProducer::NowSeconds();
        char tc[16];
        int key = ReadTimecode(f, fpsInt, tc, sizeof(tc));
        BMDTimeValue hwTime = 0, hwDur = 0;
        int64_t hwNs = -1, durNs = 0;
        if (SUCCEEDED(f->GetHardwareReferenceTimestamp(1000000000, &hwTime, &hwDur))) {
            hwNs = (int64_t)hwTime;
            durNs = (int64_t)hwDur;
        }
        if (!pair) {
            // one input: only this card thread touches `out`; the lock keeps Stop() simple
            std::lock_guard<std::mutex> lk(m);
            out.Resize(dual ? 2 * w : w, h);
            if (!ConvertEye(f, out, 0)) {
                if (!warnedConvert) LOG_WARN("decklink: cannot read the frame pixels (driver older than this build's SDK?)");
                warnedConvert = true;
                return;
            }
            ReadColour(f, out.matrix, out.transfer, warnedHdr);
            if (dual) {
                IDeckLinkVideoFrame3DExtensions* ext = nullptr;
                IDeckLinkVideoFrame* right = nullptr;
                const bool haveRight = SUCCEEDED(f->QueryInterface(IID_IDeckLinkVideoFrame3DExtensions, (void**)&ext)) &&
                                       SUCCEEDED(ext->GetFrameForRightEye(&right)) && right && ConvertEye(right, out, w);
                Release(right);
                Release(ext);
                // No right eye: showing the left in both halves would read as a perfect
                // rig to the checks and to the viewer, so this is a no-signal, not a frame.
                NoSignal(1, !haveRight);
                if (!haveRight) return;
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
        if (!ConvertEye(f, ef.px, 0)) {
            if (!warnedConvert) LOG_WARN("decklink: cannot read the frame pixels (driver older than this build's SDK?)");
            warnedConvert = true;
            return;
        }
        ReadColour(f, ef.px.matrix, ef.px.transfer, warnedHdr);
        ef.arrived = now;
        ef.hwNs = hwNs;
        bool hwKey = false;
        if (key < 0 && hwNs >= 0 && sameCard) {
            int64_t t0 = hwT0.load();
            if (t0 == INT64_MIN) {
                hwT0.compare_exchange_strong(t0, hwNs);
                t0 = hwT0.load();
            }
            const int64_t period = durNs > 0 ? durNs : (int64_t)std::llround(1e9 / std::max(1.0, fpsExact.load()));
            key = HwClockKey(hwNs, t0, period);
            hwKey = true;
        }
        std::memcpy(ef.tc, tc, sizeof(tc));
        std::lock_guard<std::mutex> lk(m);
        ++framesIn[eye];
        const TimecodePairer::Decision d = pairer.Add(eye, slot, key, now, 1.0 / std::max(1.0, fpsExact.load()));
        if (!d.publish || !alive) return;
        if (noSignal[1 - eye]) return;  // the other eye is dark: never fuse it with a stale frame
        {
            const EyeFrame& lf = hist[0][d.slot[0]];
            const EyeFrame& rf = hist[1][d.slot[1]];
            std::lock_guard<std::mutex> ps(src->pairMutex_);
            DeckLinkSource::PairStats& st = src->pairStats_;
            ++st.pairs;
            const uint64_t in = framesIn[0] + framesIn[1];
            st.unpaired = in > 2 * st.pairs ? in - 2 * st.pairs : 0;
            st.hwClockPairing = hwKey;
            st.separateCards = !sameCard;
            st.timecode = !hwKey && key >= 0;
            if (lf.hwNs >= 0 && rf.hwNs >= 0 && sameCard) {
                st.lastSkewMs = (double)(rf.hwNs - lf.hwNs) / 1e6;
                st.maxAbsSkewMs = std::max(st.maxAbsSkewMs, std::fabs(st.lastSkewMs));
            } else {
                st.lastSkewMs = (rf.arrived - lf.arrived) * 1000.0;  // arrival times: coarser
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
        out.matrix = l.px.matrix;  // the left eye's colorimetry stands for the pair
        out.transfer = l.px.transfer;
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
             bool autoDual, BMDPixelFormat yuvFormat)
        : cap_(std::move(cap)), eye_(eye), input_(input), flags_(flags), pixelFormat_(yuvFormat),
          yuvFormat_(yuvFormat), autoDual_(autoDual) {
        input_->AddRef();  // our own reference: a late callback never uses a released input
    }
    ~Callback() { input_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override {
        if (dl::IsIUnknown(iid) || dl::IidIs(iid, IID_IDeckLinkInputCallback)) {
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
        if ((detected & bmdDetectedVideoInputDualStream3D) && autoDual_ && !cap_->dualActive && !dualRefused_) {
            flags_ |= bmdVideoInputDualStream3D;  // support is per mode: try it, fall back below
            flagsChanged = true;
        } else if (!(detected & bmdDetectedVideoInputDualStream3D) && autoDual_ && cap_->dualActive) {
            // the signal went back to 2D: leave dual-stream capture
            flags_ &= ~(BMDVideoInputFlags)bmdVideoInputDualStream3D;
            flagsChanged = true;
            std::lock_guard<std::mutex> lk(cap_->m);
            if (!cap_->alive) return S_OK;
            cap_->dual = false;
            cap_->SetDualActive(false);
            cap_->NoSignal(1, false);
            LOG_INFO("decklink: the signal is 2D again; left dual-stream 3D capture");
        }
        if (!(events & (bmdVideoInputDisplayModeChanged | bmdVideoInputColorspaceChanged)) &&
            pf == pixelFormat_ && !flagsChanged)
            return S_OK;
        dl::String name = nullptr;
        mode->GetName(&name);
        BMDTimeValue dur = 0;
        BMDTimeScale scale = 0;
        if (SUCCEEDED(mode->GetFrameRate(&dur, &scale)) && dur) cap_->SetFps((double)scale / (double)dur);
        const BMDFieldDominance fd = mode->GetFieldDominance();
        LOG_INFO("decklink: input %d signal is now %s, %s, %s%s", eye_, dl::Take(name).c_str(),
                 (detected & bmdDetectedVideoInputRGB444) ? "RGB 4:4:4" : "YUV 4:2:2", FieldText(fd),
                 (detected & bmdDetectedVideoInputDualStream3D) ? ", dual-stream 3D" : "");
        if ((fd == bmdLowerFieldFirst || fd == bmdUpperFieldFirst) && !warnedInterlaced_) {
            LOG_WARN("decklink: input %d is interlaced: fields are shown combed and the rig checks are "
                     "unreliable; feed PsF or progressive", eye_);
            warnedInterlaced_ = true;
        }
        pixelFormat_ = pf;
        // Restart under the capture lock, and only while capture is still on: the worker
        // clears `alive` (taking the same lock) before it stops and disables the input.
        std::lock_guard<std::mutex> restartLock(cap_->m);
        if (!cap_->alive) return S_OK;
        cap_->ResetPairing();  // frames from the old format must not pair with the new
        input_->PauseStreams();
        HRESULT r = input_->EnableVideoInput(mode->GetDisplayMode(), pf, flags_);
        if (FAILED(r) && (flags_ & bmdVideoInputDualStream3D) && !cap_->dualActive) {
            LOG_WARN("decklink: the signal is dual-stream 3D but this card cannot capture it in this "
                     "mode; showing one eye");
            flags_ &= ~(BMDVideoInputFlags)bmdVideoInputDualStream3D;
            dualRefused_ = true;
            r = input_->EnableVideoInput(mode->GetDisplayMode(), pf, flags_);
        } else if (SUCCEEDED(r) && (flags_ & bmdVideoInputDualStream3D) && !cap_->dualActive) {
            LOG_INFO("decklink: switched to dual-stream 3D capture");
            cap_->dual = true;  // (cap_->m is held: restartLock)
            cap_->SetDualActive(true);
        }
        if (FAILED(r) && pf == bmdFormat10BitYUV) {  // refused 10-bit in this format: 8-bit
            pixelFormat_ = bmdFormat8BitYUV;  // (yuvFormat_ stays 10-bit: the next format may take it)
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
            cap_->NoSignal(eye_, true);
            return S_OK;  // keep showing the last picture; the stall watchdog flags it
        }
        cap_->NoSignal(eye_, false);
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
    BMDPixelFormat yuvFormat_;  // 10-bit; 8-bit when the card refused 10-bit at open
    bool autoDual_;
    bool dualRefused_ = false;
    bool warnedInterlaced_ = false;
};

}  // namespace

std::string DeckLinkSource::UnavailableReason() {
    dl::ComScope com;
    std::vector<std::string> names;
    bool driver = false;
    for (IDeckLink* d : AllInputDevices(names, &driver)) d->Release();
    if (!driver) return "Blackmagic Desktop Video driver not installed";
    const std::string old = DriverTooOld(DriverVersion());
    if (!old.empty()) return old;
    if (names.empty()) return "no DeckLink input found (Desktop Video is installed)";
    return "";
}

DeckLinkSource::PairStats DeckLinkSource::GetPairStats() const {
    std::lock_guard<std::mutex> lk(pairMutex_);
    return pairStats_;
}

std::vector<std::string> DeckLinkSource::Enumerate() {
    dl::ComScope com;
    std::vector<std::string> names;
    for (IDeckLink* d : AllInputDevices(names)) d->Release();
    std::vector<std::string> out;
    for (size_t i = 0; i < names.size(); ++i) out.push_back(std::to_string(i) + ": " + names[i]);
    return out;
}

void DeckLinkSource::Worker() {
    dl::ComScope com;
    std::vector<std::string> names;
    std::vector<int64_t> groups;
    bool driver = false;
    std::vector<IDeckLink*> devices = AllInputDevices(names, &driver, &groups);
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
    } else if (!(err = DriverTooOld(DriverVersion())).empty()) {
        // said above
    } else if (devices.empty()) {
        err = "no DeckLink input found (Desktop Video is installed; is the card seated, and its profile set to expose inputs?)";
    } else {
        const std::string sel[2] = {cfg_.device, cfg_.rightDevice};
        int picked[2] = {-1, -1};
        for (int e = 0; e < nInputs && err.empty(); ++e) {
            const int idx = Pick(names, sel[e]);
            if (idx < 0) {
                err = "no Blackmagic input matches \"" + sel[e] + "\" (found " + ListText(names) + ")";
                break;
            }
            if (e == 1 && idx == picked[0]) {
                err = "two-input stereo needs two different inputs (both are " + names[(size_t)idx] + ")";
                break;
            }
            picked[e] = idx;
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
                    const int64_t conn = in == "hdmi" ? bmdVideoConnectionHDMI : in == "sdi" ? bmdVideoConnectionSDI : 0;
                    if (conn && FAILED(config->SetInt(bmdDeckLinkConfigVideoInputConnection, conn)))
                        LOG_WARN("decklink: %s: cannot select input %s", names[(size_t)idx].c_str(),
                                 cfg_.input.c_str());
                }
                dl::Bool detection = false;
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
                        SUCCEEDED(dm->GetFrameRate(&dur, &scale)) && dur)
                        cap.SetFps((double)scale / (double)dur);
                    Release(dm);
                    const bool autoDual = cfg_.stereo == DeckLinkConfig::Stereo::Auto && !pair;
                    callbacks[e] = new Callback(capPtr, e, inputs[e], flags, autoDual, yuvFmt);
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
        if (err.empty() && pair) {
            const int64_t g0 = groups[(size_t)picked[0]], g1 = groups[(size_t)picked[1]];
            cap.sameCard = g0 >= 0 && g0 == g1;
            LOG_INFO("decklink: two inputs on %s", cap.sameCard ? "one card: paired by timecode, else by the card's clock"
                                                                : "separate cards: paired by timecode only (jam-sync both)");
        }
    }

    if (!err.empty()) {
        LOG_WARN("decklink: %s", err.c_str());
        Fail(LiveState::Failed, err);
    } else {
        std::unique_lock<std::mutex> lk(stopMutex_);
        while (!stop_) {
            stopCv_.wait_for(lk, std::chrono::milliseconds(50));
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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (int e = 0; e < 2; ++e) {
        Release(callbacks[e]);
        Release(inputs[e]);
    }
    for (IDeckLink* d : devices) d->Release();
}

#else  // no DeckLink SDK in this build

bool DeckLinkSource::Available() { return false; }
std::vector<std::string> DeckLinkSource::Enumerate() { return {}; }
std::string DeckLinkSource::UnavailableReason() { return "this build has no DeckLink SDK"; }
DeckLinkSource::PairStats DeckLinkSource::GetPairStats() const { return {}; }
void DeckLinkSource::Worker() {}

#endif

}  // namespace mp
