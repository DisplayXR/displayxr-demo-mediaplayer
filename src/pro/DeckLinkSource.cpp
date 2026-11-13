// SPDX-License-Identifier: Apache-2.0
#include "pro/DeckLinkSource.h"

#include "Log.h"
#include "pro/ConvertPool.h"
#include "pro/DeckLinkFormat.h"
#include "pro/DirectPair.h"
#include "pro/PerfSwitches.h"
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

// DeckLinkConfig::ParseStereo / StereoName are in pro/DeckLinkSpec.cpp (no SDK, unit-tested).

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
    {
        std::lock_guard<std::mutex> lk(warnMutex_);
        formatWarning_[0].clear();
        formatWarning_[1].clear();
    }
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
    } else {
        std::fprintf(out, "Blackmagic devices (use the number or part of the name):\n");
        for (const std::string& d : devs) std::fprintf(out, "  %s\n", d.c_str());
    }
    // Also with no card: station acceptance E2 reads the driver version from this line.
    const std::string v = DriverVersionText();
    if (!v.empty()) std::fprintf(out, "Desktop Video %s\n", v.c_str());
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
bool IsIUnknown(REFIID a) {
    const REFIID u = IID_IUnknown;  // a compound literal in LinuxCOM.h: no address to take
    return std::memcmp(&a, &u, sizeof(u)) == 0;
}
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

// One eye's pixels as the card delivered them, copied out of the SDK's buffer so the
// callback can return before they are converted (#141).
struct RawPixels {
    std::vector<uint8_t> bytes;
    int w = 0, h = 0, rowBytes = 0;
    BMDPixelFormat pf = bmdFormat10BitYUV;
};

// False when the frame's pixels cannot be read.
bool CopyPixels(IDeckLinkVideoFrame* f, RawPixels& out) {
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
        out.w = (int)f->GetWidth();
        out.h = (int)f->GetHeight();
        out.rowBytes = (int)f->GetRowBytes();
        out.pf = f->GetPixelFormat();
        const uint8_t* b = (const uint8_t*)bytes;
        out.bytes.assign(b, b + (size_t)out.rowBytes * out.h);  // same size after warm-up: no allocation
#if MP_DL_SDK_HAS_VIDEO_BUFFER
        buf->EndAccess(bmdBufferAccessRead);
#endif
    }
    Release(buf);
    return ok;
}

std::atomic<int> g_lastBitDepth{0};

BMDPixelFormat ToBmd(dlfmt::Pix p) {
    switch (p) {
        case dlfmt::Pix::Yuv10: return bmdFormat10BitYUV;
        case dlfmt::Pix::Yuv8: return bmdFormat8BitYUV;
        case dlfmt::Pix::Rgb10: return bmdFormat10BitRGB;
        case dlfmt::Pix::Bgra8: return bmdFormat8BitBGRA;
    }
    return bmdFormat8BitYUV;
}

// The signal's bit depth as the card detected it; 0 when it does not say.
int SignalBits(BMDDetectedVideoInputFormatFlags d) {
    if (d & bmdDetectedVideoInput12BitDepth) return 12;
    if (d & bmdDetectedVideoInput10BitDepth) return 10;
    if (d & bmdDetectedVideoInput8BitDepth) return 8;
    return 0;
}

// One eye's copied pixels as a conversion to P016 at column x0 of dst (10-bit kept; 8-bit
// widened), for livepix::ToP016Banded. hasMatrix / matrix: the signal's colorimetry, if the
// card reported it. An r210 (RGB) frame is encoded with it, so the tag the caller sets
// afterwards is the one the pixels were encoded with.
livepix::EyeJob RawJob(const RawPixels& p, int x0, bool hasMatrix, ColorMatrix matrix) {
    const bool deep = p.pf == bmdFormat10BitYUV || p.pf == bmdFormat10BitRGB;
    g_lastBitDepth.store(deep ? 10 : 8, std::memory_order_relaxed);
    livepix::EyeJob j;
    j.fmt = p.pf == bmdFormat10BitYUV   ? livepix::RawFormat::V210
            : p.pf == bmdFormat10BitRGB ? livepix::RawFormat::R210
            : p.pf == bmdFormat8BitBGRA ? livepix::RawFormat::Bgra
                                        : livepix::RawFormat::Uyvy;
    j.src = p.bytes.data();
    j.srcRowBytes = p.rowBytes;
    j.w = p.w & ~1;
    j.h = p.h & ~1;
    j.x0 = x0;
    j.rgbMatrix = hasMatrix ? matrix : DefaultMatrixForHeight(j.h);
    return j;
}
livepix::EyeJob ConvertJob(const RawPixels& p, livepix::P016& dst, int x0, bool hasMatrix, ColorMatrix matrix) {
    livepix::EyeJob j = RawJob(p, x0, hasMatrix, matrix);
    j.dst = &dst;
    return j;
}
// The same into eye `eye`'s half of a reserved pair frame (direct pairing), the matrix tag
// into *matrixOut: what ConvertJob would leave on a P016 of the eye's own.
livepix::EyeJob HalfJob(const RawPixels& p, FrameRing::Frame& frame, int eye, int eyeW, int eyeH, bool hasMatrix,
                        ColorMatrix matrix, ColorMatrix* matrixOut) {
    livepix::EyeJob j = RawJob(p, eye * eyeW, hasMatrix, matrix);
    j.planes = livepix::PairPlanes(frame, eyeW, eyeH);
    j.matrixOut = matrixOut;
    return j;
}

// The frame's timecode as text and as a frame count at the full frame rate (-1 when it
// has none). A high-frame-rate timecode (HFRTC, counts every frame above 30 fps) is
// preferred when the card has one; otherwise RP188 (VITC1 / VITC2 / LTC) or VITC, whose
// frame pairs at 50/60p are unfolded with the field mark (TimecodeFrames).
// `fullCount` is this input's latch: set once its ff reaches half the rate (a source that
// counts every frame without an HFRTC); the caller resets it when the format changes.
int ReadTimecode(IDeckLinkVideoInputFrame* f, int fpsInt, char* text, size_t n, std::atomic<bool>& fullCount) {
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
        if (TimecodeCountsEveryFrame(t, std::max(1, fpsInt))) fullCount = true;
        t.fullCount = fullCount;
        FormatTimecode(t, std::max(1, fpsInt), text, n);
        key = TimecodeFrames(t, std::max(1, fpsInt));
    }
    tc->Release();
    return key;
}

// The signal's colorimetry from the frame's metadata (VPID / HDMI InfoFrame as the card
// reports it): the Y'CbCr matrix the shader must decode with, and the transfer as a tag.
// Frames without metadata keep the converters' SD/HD rule (`hasMatrix` stays false).
void ReadColour(IDeckLinkVideoFrame* f, bool& hasMatrix, ColorMatrix& matrix, ColorTransfer& transfer,
                bool& warnedHdr) {
    IDeckLinkVideoFrameMetadataExtensions* md = nullptr;
    if (FAILED(f->QueryInterface(IID_IDeckLinkVideoFrameMetadataExtensions, (void**)&md)) || !md) return;
    int64_t v = 0;
    if (SUCCEEDED(md->GetInt(bmdDeckLinkFrameMetadataColorspace, &v))) {
        hasMatrix = true;
        if (v == bmdColorspaceRec601) matrix = ColorMatrix::BT601;
        else if (v == bmdColorspaceRec709) matrix = ColorMatrix::BT709;
        else if (v == bmdColorspaceRec2020) matrix = ColorMatrix::BT2020;
        else hasMatrix = false;
    }
    if ((f->GetFlags() & bmdFrameContainsHDRMetadata) &&
        SUCCEEDED(md->GetInt(bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc, &v))) {
        transfer = v == 2 ? ColorTransfer::PQ : v == 3 ? ColorTransfer::HLG : ColorTransfer::SDR;  // CEA 861.3
        if (transfer != ColorTransfer::SDR && !warnedHdr) {
            LOG_WARN("decklink: the signal is %s; it is tone-mapped to the SDR panel",
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
    bool strictPairs = false;  // matched pairs only: one card, MEDIAPLAYER_DECKLINK_STRICT_PAIRS != 0
    bool warnedRefused = false;
    std::atomic<int> fpsInt{25};
    std::atomic<double> fpsExact{25.0};
    std::atomic<bool> tcFullCount[2] = {false, false};  // per input: timecode counts every frame
    std::atomic<bool> noSignal[2]{};
    std::atomic<bool> dualActive{false};  // Auto switched to the card's dual-stream mode

    std::mutex m;  // the frames below + the pairing state
    // The ring's single-producer side: one compose + publish at a time. Taken while m is
    // held (lock order m, then publishM), so publishes keep the order of the pairing
    // decisions; a pair is then composed with only publishM held, so the other eye's lane
    // can claim a slot and pair meanwhile (pins below). The worker takes both to clear
    // `alive`: no publish is in progress after that.
    std::mutex publishM;
    std::atomic<bool> alive{true};  // cleared before the inputs stop: late callbacks do nothing
    livepix::P016 out;

    // Pair: the last few frames of each eye; TimecodePairer decides which two to show.
    TimecodePairer pairer;
    struct EyeFrame {
        livepix::P016 px;   // the eye's pixels, unless direct pairing put them in a ring frame's half
        double arrived = 0.0;
        int64_t hwNs = -1;  // the card's hardware reference clock, nanoseconds (-1 unknown)
        char tc[16] = {0};
        // Direct pairing: the eye's tags and size wherever its pixels are (px's own otherwise)
        ColorMatrix matrix = ColorMatrix::BT709;
        ColorTransfer transfer = ColorTransfer::SDR;
        int w = 0, h = 0;
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
    // Pair: composes reading hist[eye][slot] outside m (m guards the counts). A lane never
    // converts into a pinned slot: it waits on pinCv until the compose is done (rare: the
    // slot it is due to fill is normally the oldest, and the pair shown the newest).
    int pins[2][TimecodePairer::kHistory] = {};
    std::condition_variable pinCv;
    // Direct pairing (pro/DirectPair.h, Pro 0.9.8): a frame whose pairing key is known before
    // it is converted goes straight into its half of a reserved ring frame, and a pair of two
    // such halves is published without the compose. MEDIAPLAYER_DECKLINK_DIRECT_PAIR=0 (or a
    // latest-wins ring, MEDIAPLAYER_LIVE_QUEUE=0) composes every pair as before. m guards the
    // table; a lane writes its claimed half without m (the table's file comment has the rules).
    bool directPair = false;
    struct RingFrames final : DirectPairTable::Frames {
        explicit RingFrames(DeckLinkCapture* c) : cap(c) {}
        DeckLinkCapture* cap;
        void* Reserve(int eyeW, int eyeH) override { return cap->ReserveFrame(eyeW, eyeH); }
        void Cancel(void* frame) override { cap->CancelFrame(frame); }
    } ringFrames{this};
    DirectPairTable table{&ringFrames};
    void* ReserveFrame(int eyeW, int eyeH) { return src->ReservePairFrame(eyeW, eyeH); }
    void CancelFrame(void* frame) { src->CancelPairFrame(static_cast<FrameRing::Frame*>(frame)); }
    bool warnedUnsynced = false;
    bool warnedTcByClock = false;  // said once: timecodes disagree, paired by the card's clock
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
        ++generation;  // frames already queued in the lanes are from the old signal
        pairer.Reset();
        hwT0 = INT64_MIN;
        framesIn[0] = framesIn[1] = 0;
        // Direct entries are for the old signal: retired (a lane still writing one finishes,
        // and its frame can still be composed); the rest go back to the ring.
        if (directPair) table.Sweep(pairer.LastKey(), pairer.Epoch(), generation.load());
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

    // A restart after a format change that cannot enable the input in any format (#169):
    // the input is left stopped and the source goes Failed, so the status bar reads ERROR
    // and the toast / get_rig_status give the reason, instead of a live cable reading as a
    // stall. A later restart that works brings it back. m held.
    void RestartFailed(const std::string& why) {
        if (alive) src->Fail(LiveState::Failed, why);
    }
    void RestartOk() {
        if (alive && src->GetState() == LiveState::Failed) src->SetState(LiveState::Opening);
    }
    void FormatWarning(int eye, const std::string& text) {
        if (alive) src->SetFormatWarning(eye, text);
    }

    // ---- the hand-off (#141). The card's callback must return within a frame or the
    // card drops frames; a 4K v210 eye takes ~7 ms to unpack and a pair another ~3 ms to
    // compose, so the callback only copies the pixels out (~1.3 ms at 4K) and reads the
    // frame's metadata, and each input's own lane thread converts, pairs and publishes.
    // A lane holds one pending frame: if the next arrives first, the pending one is
    // replaced (newest wins) and counted in LiveStats::lateFrames.
    // The convert is split into row bands over the lane's ConvertPool: on one thread a
    // station's two 2160p59.94 inputs reached ~40 fps, and a dual-stream input converts two
    // eyes per frame on its one lane (MEDIAPLAYER_DECKLINK_CONVERT_THREADS, pro/ConvertPool.h).
    struct RawFrame {
        RawPixels px, right;  // right: dual-stream 3D's right eye
        bool dual = false;
        bool hasMatrix = false;
        ColorMatrix matrix = ColorMatrix::BT709;
        ColorTransfer transfer = ColorTransfer::SDR;
        double now = 0.0;
        char tc[16] = {0};
        int key = -1;
        int64_t hwNs = -1, durNs = 0;
        uint32_t generation = 0;
    };
    struct Lane {
        std::mutex m;
        std::condition_variable cv;
        RawFrame slot[3];  // one being filled, one pending, one being converted
        int pending = -1, busy = -1;
        bool quit = false;
        std::thread th;
        std::unique_ptr<livepix::ConvertPool> pool;  // the convert's threads (th runs band 0)
    };
    Lane lanes[2];
    std::atomic<uint32_t> generation{0};  // bumped on a format change: queued frames are stale

    // n lanes, each carrying up to inputEyes eyes per frame (2: a dual-stream input, or
    // Auto, which switches to dual-stream when the card detects it).
    void StartLanes(int n, int inputEyes) {
        if (n <= 0) return;
        const int threads = livepix::ConvertThreads(n * inputEyes, inputEyes);
        for (int e = 0; e < n; ++e) {
            lanes[e].pool = std::make_unique<livepix::ConvertPool>(threads);
            lanes[e].th = std::thread([this, e] { LaneLoop(e); });
        }
        LOG_INFO("decklink: converting on %d thread(s) per input%s%s", lanes[0].pool->Threads(),
                 inputEyes > 1 ? " (shared by the two eyes in dual-stream 3D)" : "",
                 std::getenv("MEDIAPLAYER_DECKLINK_CONVERT_THREADS") ? " (MEDIAPLAYER_DECKLINK_CONVERT_THREADS)" : "");
    }
    void StopLanes() {
        for (Lane& l : lanes) {
            {
                std::lock_guard<std::mutex> lk(l.m);
                l.quit = true;
            }
            l.cv.notify_all();
            if (l.th.joinable()) l.th.join();
            l.pool.reset();  // after the lane: it is the pool's only caller
        }
    }

    // On the card's thread: copy and timestamp, nothing else.
    void Enqueue(int eye, IDeckLinkVideoInputFrame* f) {
        if (!alive) return;
        Lane& lane = lanes[eye];
        int s = 0;
        {
            std::lock_guard<std::mutex> lk(lane.m);
            while (s == lane.pending || s == lane.busy) ++s;
        }
        RawFrame& r = lane.slot[s];  // ours alone until it is made pending below
        r.now = LiveProducer::NowSeconds();
        r.generation = generation.load();
        r.key = ReadTimecode(f, fpsInt, r.tc, sizeof(r.tc), tcFullCount[eye]);
        BMDTimeValue hwTime = 0, hwDur = 0;
        r.hwNs = -1;
        r.durNs = 0;
        if (SUCCEEDED(f->GetHardwareReferenceTimestamp(1000000000, &hwTime, &hwDur))) {
            r.hwNs = (int64_t)hwTime;
            r.durNs = (int64_t)hwDur;
        }
        if (!CopyPixels(f, r.px)) {
            if (!warnedConvert) LOG_WARN("decklink: cannot read the frame pixels (driver older than this build's SDK?)");
            warnedConvert = true;
            return;
        }
        r.hasMatrix = false;
        r.transfer = ColorTransfer::SDR;
        ReadColour(f, r.hasMatrix, r.matrix, r.transfer, warnedHdr);
        r.dual = !pair && dual;
        if (r.dual) {
            IDeckLinkVideoFrame3DExtensions* ext = nullptr;
            IDeckLinkVideoFrame* right = nullptr;
            const bool haveRight = SUCCEEDED(f->QueryInterface(IID_IDeckLinkVideoFrame3DExtensions, (void**)&ext)) &&
                                   SUCCEEDED(ext->GetFrameForRightEye(&right)) && right && CopyPixels(right, r.right);
            Release(right);
            Release(ext);
            // No right eye: showing the left in both halves would read as a perfect
            // rig to the checks and to the viewer, so this is a no-signal, not a frame.
            NoSignal(1, !haveRight);
            if (!haveRight) return;
        }
        bool late = false;
        {
            std::lock_guard<std::mutex> lk(lane.m);
            late = lane.pending >= 0;
            lane.pending = s;
        }
        lane.cv.notify_one();
        if (late && alive) src->CountLate();
    }

    void LaneLoop(int eye) {
        Lane& lane = lanes[eye];
        for (;;) {
            int s;
            {
                std::unique_lock<std::mutex> lk(lane.m);
                lane.cv.wait(lk, [&] { return lane.quit || lane.pending >= 0; });
                if (lane.quit) return;
                s = lane.busy = lane.pending;
                lane.pending = -1;
            }
            const RawFrame& r = lane.slot[s];
            if (r.generation == generation.load()) Process(eye, r);
            std::lock_guard<std::mutex> lk(lane.m);
            lane.busy = -1;
        }
    }

    // On the input's lane thread: convert, pair, publish.
    void Process(int eye, const RawFrame& r) {
        if (!alive) return;
        const int w = r.px.w & ~1, h = r.px.h & ~1;
        livepix::ConvertPool* pool = lanes[eye].pool.get();
        if (!pair) {
            // one input: only this lane touches `out`; the lock keeps Stop() simple
            std::lock_guard<std::mutex> lk(m);
            out.Resize(r.dual ? 2 * w : w, h);
            // dual-stream: both eyes in one go, their bands side by side on the pool
            livepix::EyeJob jobs[2];
            int n = 0;
            jobs[n++] = ConvertJob(r.px, out, 0, r.hasMatrix, r.matrix);
            if (r.dual) jobs[n++] = ConvertJob(r.right, out, w, r.hasMatrix, r.matrix);
            livepix::ToP016Banded(pool, jobs, n);
            if (r.hasMatrix) out.matrix = r.matrix;
            out.transfer = r.transfer;
            // into the ring in row bands on the same pool (a dual-stream 4K pair is 50 MB)
            if (alive)
                src->Publish(out, /*fullRange=*/false, r.now, r.tc[0] ? r.tc : nullptr, r.dual ? 0 : -1, pool);
            return;
        }
        if (directPair) {
            ProcessPairDirect(eye, r, w, h, pool);
            return;
        }
        // Pair: claim this eye's oldest slot, convert into it WITHOUT the lock (the two
        // lanes convert in parallel; a busy slot is never chosen, nor one the other lane is
        // still composing into the ring), then decide.
        int slot;
        {
            std::unique_lock<std::mutex> lk(m);
            pinCv.wait(lk, [&] { return pins[eye][pairer.NextSlot(eye)] == 0; });
            slot = pairer.NextSlot(eye);
            pairer.MarkBusy(eye, slot);
        }
        EyeFrame& ef = hist[eye][slot];
        ef.px.Resize(w, h);
        const livepix::EyeJob job = ConvertJob(r.px, ef.px, 0, r.hasMatrix, r.matrix);
        livepix::ToP016Banded(pool, &job, 1);
        if (r.hasMatrix) ef.px.matrix = r.matrix;
        ef.px.transfer = r.transfer;
        ef.arrived = r.now;
        ef.hwNs = r.hwNs;
        const int clockKey = ClockKey(r);
        std::memcpy(ef.tc, r.tc, sizeof(r.tc));
        std::unique_lock<std::mutex> lk(m);
        ++framesIn[eye];
        const TimecodePairer::Decision d =
            pairer.Add(eye, slot, r.key, r.now, 1.0 / std::max(1.0, fpsExact.load()), clockKey);
        NoteDecision(d);
        if (!d.publish || !alive) return;
        if (noSignal[1 - eye]) return;  // the other eye is dark: never fuse it with a stale frame
        if (!CountPair(d, r.key, /*direct=*/false)) return;
        const EyeFrame& l = hist[0][d.slot[0]];
        const EyeFrame& rt = hist[1][d.slot[1]];
        WarnPair(d, l.px.width, l.px.height, rt.px.width, rt.px.height);
        // The pair is composed straight into the ring (no intermediate side-by-side frame),
        // in row bands on this lane's pool, and outside m: ~50 MB at 4K, which held the
        // other eye's lane off its next frame for the whole copy. The two slots are pinned
        // so neither lane converts into them meanwhile; publishM, taken before m is
        // released, keeps the ring single-producer and the publishes in decision order.
        const int s0 = d.slot[0], s1 = d.slot[1];
        const double captureSec = std::min(l.arrived, rt.arrived);
        char tc[sizeof(l.tc)];
        std::memcpy(tc, l.tc, sizeof(tc));
        const int offsetFrames = d.offsetFrames;
        std::unique_lock<std::mutex> pub(publishM);
        ++pins[0][s0];
        ++pins[1][s1];
        lk.unlock();
        if (alive)
            src->PublishPair(l.px, rt.px, /*fullRange=*/false, captureSec, tc[0] ? tc : nullptr, offsetFrames, pool);
        pub.unlock();  // before m: the next publisher may be holding m while it waits for publishM
        lk.lock();
        --pins[0][s0];
        --pins[1][s1];
        lk.unlock();
        pinCv.notify_all();
    }

    // Two keys: the frame's timecode, and (both inputs on one card) its frame on the card's
    // clock. The pairer uses the clock when the timecode is missing or the two inputs'
    // timecodes don't belong together (Resolve's dual SDI output). -1: not on one card.
    int ClockKey(const RawFrame& r) {
        int clockKey = -1;
        if (r.hwNs >= 0 && sameCard) {
            int64_t t0 = hwT0.load();
            if (t0 == INT64_MIN) {
                hwT0.compare_exchange_strong(t0, r.hwNs);
                t0 = hwT0.load();
            }
            const int64_t period = r.durNs > 0 ? r.durNs : (int64_t)std::llround(1e9 / std::max(1.0, fpsExact.load()));
            clockKey = HwClockKey(r.hwNs, t0, period);
        }
        return clockKey;
    }

    // The decision's warnings and the strict refusal count. m held.
    void NoteDecision(const TimecodePairer::Decision& d) {
        if (d.byClock && d.tcOffsetFrames > 0 && !warnedTcByClock) {
            LOG_WARN("decklink: the two inputs' timecodes are %d frames apart (not jam-synced; e.g. Resolve's dual "
                     "SDI output times only its first output) - pairing by the card's clock instead",
                     d.tcOffsetFrames);
            warnedTcByClock = true;
        }
        if (d.refused && alive) {
            std::lock_guard<std::mutex> ps(src->pairMutex_);
            ++src->pairStats_.mismatchRefused;
            src->pairStats_.strict = true;
            if (!warnedRefused) {
                LOG_WARN("decklink: matched pairs only: the eyes are %d frame(s) apart (an input fell behind) - "
                         "holding the last matched pair instead of showing them (counted in mismatch_refused)",
                         d.offsetFrames);
                warnedRefused = true;
            }
        }
    }

    // A pair about to be shown: the strict guard, then the counts. False (counted in
    // mismatch_refused) when the eyes are not one instant on the card's clock: never show it.
    // `direct`: published whole (direct pairing), else composed. m held.
    bool CountPair(const TimecodePairer::Decision& d, int key, bool direct) {
        const EyeFrame& lf = hist[0][d.slot[0]];
        const EyeFrame& rf = hist[1][d.slot[1]];
        std::lock_guard<std::mutex> ps(src->pairMutex_);
        DeckLinkSource::PairStats& st = src->pairStats_;
        st.strict = strictPairs;
        // Last line of defence: on one card both eyes of a true pair carry the same
        // capture instant on the card's clock. More than half a frame apart is not a
        // pair, whatever the pairing decided: never show it.
        if (strictPairs && lf.hwNs >= 0 && rf.hwNs >= 0) {
            const double halfFrameMs = 500.0 / std::max(1.0, fpsExact.load());
            if (std::fabs((double)(rf.hwNs - lf.hwNs) / 1e6) > halfFrameMs) {
                ++st.mismatchRefused;
                return false;
            }
        }
        ++st.pairs;
        if (direct) ++st.directPairs;
        else ++st.composedPairs;
        const uint64_t in = framesIn[0] + framesIn[1];
        st.unpaired = in > 2 * st.pairs ? in - 2 * st.pairs : 0;
        st.hwClockPairing = d.byClock;
        st.separateCards = !sameCard;
        st.timecode = !d.byClock && !d.unsynced && key >= 0;
        if (lf.hwNs >= 0 && rf.hwNs >= 0 && sameCard) {
            st.lastSkewMs = (double)(rf.hwNs - lf.hwNs) / 1e6;
            st.maxAbsSkewMs = std::max(st.maxAbsSkewMs, std::fabs(st.lastSkewMs));
        } else {
            st.lastSkewMs = (rf.arrived - lf.arrived) * 1000.0;  // arrival times: coarser
            st.maxAbsSkewMs = std::max(st.maxAbsSkewMs, std::fabs(st.lastSkewMs));
        }
        return true;
    }

    void WarnPair(const TimecodePairer::Decision& d, int lw, int lh, int rw, int rh) {  // m held
        if (d.unsynced && !warnedUnsynced) {
            LOG_WARN("decklink: the two inputs' timecodes are %d frames apart - not jam-synced; "
                     "pairing the newest frames instead", d.offsetFrames);
            warnedUnsynced = true;
        }
        if ((lw != rw || lh != rh) && !warnedSize) {
            LOG_WARN("decklink: the two inputs differ in size (%dx%d vs %dx%d)", lw, lh, rw, rh);
            warnedSize = true;
        }
    }

    // Direct pairing (pro/DirectPair.h): the same claim, convert, decide and publish as above,
    // but a frame whose pairing key is known before it is converted goes straight into its
    // half of a reserved ring frame, and a decision that pairs the two halves of one entry
    // publishes that frame whole: no compose. Every other decision is composed as above, from
    // wherever each eye's pixels are.
    void ProcessPairDirect(int eye, const RawFrame& r, int w, int h, livepix::ConvertPool* pool) {
        // Before the claim: the pairer may pair this frame by it (DirectKey).
        const int clockKey = ClockKey(r);
        // Only a frame the converters take whole (they write every sample of a fitting eye,
        // so the half is exactly what the eye's own P016 would hold).
        const bool fits = !r.px.bytes.empty() && w > 0 && h > 1;
        int slot, entry = -1;
        FrameRing::Frame* frame = nullptr;
        {
            std::unique_lock<std::mutex> lk(m);
            pinCv.wait(lk, [&] { return pins[eye][pairer.NextSlot(eye)] == 0; });
            slot = pairer.NextSlot(eye);
            pairer.MarkBusy(eye, slot);
            table.SlotReused(eye, slot);
            table.NoteSize(eye, w, h);
            const int key = fits ? pairer.DirectKey(r.key, clockKey) : -1;
            if (key >= 0) {
                DirectPairTable::Refused why = DirectPairTable::Refused::None;
                entry = table.Claim(eye, slot, key, pairer.Epoch(), generation.load(), w, h, &why);
                if (entry >= 0) {
                    frame = static_cast<FrameRing::Frame*>(table.Frame(entry));
                } else if (why == DirectPairTable::Refused::NoFrame && alive) {
                    std::lock_guard<std::mutex> ps(src->pairMutex_);
                    ++src->pairStats_.directNoFrame;  // no ring frame to spare: converted the old way
                }
            }
        }
        EyeFrame& ef = hist[eye][slot];
        if (frame) {
            // Our half alone, without m: the other lane writes only the other half, and the
            // frame is in no ring queue until both halves are Done (pro/DirectPair.h).
            const livepix::EyeJob job = HalfJob(r.px, *frame, eye, w, h, r.hasMatrix, r.matrix, &ef.matrix);
            livepix::ToP016Banded(pool, &job, 1);
            if (r.hasMatrix) ef.matrix = r.matrix;
            ef.transfer = r.transfer;
        } else {
            ef.px.Resize(w, h);
            const livepix::EyeJob job = ConvertJob(r.px, ef.px, 0, r.hasMatrix, r.matrix);
            livepix::ToP016Banded(pool, &job, 1);
            if (r.hasMatrix) ef.px.matrix = r.matrix;
            ef.px.transfer = r.transfer;
            ef.matrix = ef.px.matrix;
            ef.transfer = ef.px.transfer;
        }
        ef.w = w;
        ef.h = h;
        ef.arrived = r.now;
        ef.hwNs = r.hwNs;
        std::memcpy(ef.tc, r.tc, sizeof(r.tc));
        std::unique_lock<std::mutex> lk(m);
        if (entry >= 0) table.Done(entry, eye);
        ++framesIn[eye];
        const TimecodePairer::Decision d =
            pairer.Add(eye, slot, r.key, r.now, 1.0 / std::max(1.0, fpsExact.load()), clockKey);
        NoteDecision(d);
        PublishDirect(eye, d, r.key, pool, lk);
        // After every decision: retire what can no longer be published whole, release what
        // nothing needs (m held again; PublishDirect may have dropped it for a compose).
        table.Sweep(pairer.LastKey(), pairer.Epoch(), generation.load());
        lk.unlock();
        pinCv.notify_all();
    }

    // The decision, shown: whole when it pairs the two halves of one entry, else composed from
    // wherever each eye is. Returns with m held.
    void PublishDirect(int eye, const TimecodePairer::Decision& d, int key, livepix::ConvertPool* pool,
                       std::unique_lock<std::mutex>& lk) {
        if (!d.publish || !alive) return;
        if (noSignal[1 - eye]) return;  // the other eye is dark: never fuse it with a stale frame
        const int s0 = d.slot[0], s1 = d.slot[1];
        int ent[2] = {-1, -1};
        const DirectPairTable::Where where[2] = {table.Locate(0, s0, &ent[0]), table.Locate(1, s1, &ent[1])};
        if (where[0] == DirectPairTable::Where::Gone || where[1] == DirectPairTable::Where::Gone) {
            // An eye whose pixels already went to the screen in a whole pair (or were recycled):
            // the old path would have shown that eye again; nothing new is shown instead.
            std::lock_guard<std::mutex> ps(src->pairMutex_);
            ++src->pairStats_.directSkipped;
            return;
        }
        const int whole = table.Whole(s0, s1);
        if (!CountPair(d, key, whole >= 0)) return;
        const EyeFrame& l = hist[0][s0];
        const EyeFrame& rt = hist[1][s1];
        WarnPair(d, l.w, l.h, rt.w, rt.h);
        const double captureSec = std::min(l.arrived, rt.arrived);
        char tc[sizeof(l.tc)];
        std::memcpy(tc, l.tc, sizeof(tc));
        const ColorMatrix matrix = l.matrix;  // the left eye's tags stand for the pair (PublishPair)
        const ColorTransfer transfer = l.transfer;
        if (whole >= 0) {
            // Both halves of one instant, written: the frame goes to the ring as it is. publishM
            // (taken with m held, as for a compose) keeps the publishes in decision order behind
            // a compose still running.
            const int ew = table.EyeW(whole), eh = table.EyeH(whole);
            FrameRing::Frame* f = static_cast<FrameRing::Frame*>(table.TakeForPublish(whole));
            std::lock_guard<std::mutex> pub(publishM);
            src->PublishPairFrame(f, ew, eh, matrix, transfer, /*fullRange=*/false, captureSec, tc[0] ? tc : nullptr,
                                  d.offsetFrames);
            return;
        }
        // Composed, outside m, as the old path: a history slot's P016 is pinned (no lane
        // converts into it), an entry's half is pinned in the table (not published, released
        // or recycled; the other half may still be written meanwhile, which is other memory).
        livepix::EyeView view[2];
        const int slots[2] = {s0, s1};
        for (int e = 0; e < 2; ++e) {
            if (where[e] == DirectPairTable::Where::Half) {
                const FrameRing::Frame* f = static_cast<const FrameRing::Frame*>(table.Frame(ent[e]));
                view[e] = livepix::PairHalf(*f, e, table.EyeW(ent[e]), table.EyeH(ent[e]));
                table.Pin(ent[e]);
            } else {
                view[e] = livepix::EyeOf(hist[e][slots[e]].px);
                ++pins[e][slots[e]];
            }
        }
        std::unique_lock<std::mutex> pub(publishM);
        lk.unlock();
        if (alive)
            src->PublishPair(view[0], view[1], matrix, transfer, /*fullRange=*/false, captureSec, tc[0] ? tc : nullptr,
                             d.offsetFrames, pool);
        pub.unlock();  // before m: the next publisher may be holding m while it waits for publishM
        lk.lock();
        for (int e = 0; e < 2; ++e) {
            if (where[e] == DirectPairTable::Where::Half) table.Unpin(ent[e]);
            else --pins[e][slots[e]];
        }
    }
};

namespace {

// Receives one input's frames on the card's thread.
class Callback : public IDeckLinkInputCallback {
public:
    Callback(std::shared_ptr<DeckLinkCapture> cap, int eye, IDeckLinkInput* input, BMDVideoInputFlags flags,
             bool autoDual, BMDPixelFormat yuvFormat)
        : cap_(std::move(cap)), eye_(eye), input_(input), flags_(flags), requested_(yuvFormat),
          yuv10Ok_(yuvFormat == bmdFormat10BitYUV), autoDual_(autoDual),
          explicitDual_(!autoDual && (flags & bmdVideoInputDualStream3D) != 0) {
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
        // The formats to ask for, best first: RGB 4:4:4 at its own depth (r210 for 10 and
        // 12 bit), then 8-bit RGB, then YUV, which the card converts to (#169).
        const bool rgb = (detected & bmdDetectedVideoInputRGB444) != 0;
        const int signalBits = SignalBits(detected);
        dlfmt::Pix cand[4];
        const int nCand = dlfmt::Candidates(rgb, signalBits, yuv10Ok_, cand);
        const BMDPixelFormat pf = ToBmd(cand[0]);
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
            pf == requested_ && !flagsChanged)
            return S_OK;
        if (events & bmdVideoInputDisplayModeChanged) {
            dualRefused_ = false;  // support is per mode
            // --decklink-stereo=dual asked for dual-stream: a refusal in the old mode does
            // not carry over to this one, so ask again.
            if (explicitDual_) flags_ |= bmdVideoInputDualStream3D;
        }
        dl::String name = nullptr;
        mode->GetName(&name);
        const std::string modeName = dl::Take(name);
        BMDTimeValue dur = 0;
        BMDTimeScale scale = 0;
        if (SUCCEEDED(mode->GetFrameRate(&dur, &scale)) && dur) {
            cap_->tcFullCount[eye_] = false;  // a new format: learn its timecode count again
            cap_->SetFps((double)scale / (double)dur);
        }
        const BMDFieldDominance fd = mode->GetFieldDominance();
        LOG_INFO("decklink: input %d signal is now %s, %s, %d-bit, %s%s", eye_, modeName.c_str(),
                 rgb ? "RGB 4:4:4" : "YUV 4:2:2", signalBits, FieldText(fd),
                 (detected & bmdDetectedVideoInputDualStream3D) ? ", dual-stream 3D" : "");
        if ((fd == bmdLowerFieldFirst || fd == bmdUpperFieldFirst) && !warnedInterlaced_) {
            LOG_WARN("decklink: input %d is interlaced: fields are shown combed and the rig checks are "
                     "unreliable; feed PsF or progressive", eye_);
            warnedInterlaced_ = true;
        }
        requested_ = pf;
        // Restart under the capture lock, and only while capture is still on: the worker
        // clears `alive` (taking the same lock) before it stops and disables the input.
        std::lock_guard<std::mutex> restartLock(cap_->m);
        if (!cap_->alive) return S_OK;
        cap_->ResetPairing();  // frames from the old format must not pair with the new
        input_->PauseStreams();
        // Each format in turn until the card takes one (a refusal is per format and mode;
        // yuv10Ok_ stays as it is: the next mode may take 10-bit again).
        dlfmt::Pix got = cand[0];
        auto enable = [&]() {
            HRESULT hr = E_FAIL;
            for (int i = 0; i < nCand && FAILED(hr); ++i) {
                hr = input_->EnableVideoInput(mode->GetDisplayMode(), ToBmd(cand[i]), flags_);
                if (SUCCEEDED(hr)) got = cand[i];
            }
            return hr;
        };
        HRESULT r = enable();
        const BMDVideoInputFlags kDual = bmdVideoInputDualStream3D;
        if (FAILED(r) && (flags_ & kDual)) {
            // Dual-stream support is per mode: refused here whether capture was switching into
            // it or was already in it before this format change (#169). Fall back to one input.
            LOG_WARN("decklink: input %d: this card cannot capture dual-stream 3D in %s; showing one eye", eye_,
                     modeName.c_str());
            flags_ &= ~kDual;
            cap_->dual = false;  // (cap_->m is held) one eye is what will arrive: do not wait for a right one
            if (cap_->dualActive) cap_->SetDualActive(false);
            cap_->NoSignal(1, false);  // no right eye is expected now, so it is not "dark"
            dualRefused_ = true;
            r = enable();
        } else if (SUCCEEDED(r) && (flags_ & kDual)) {
            if (!cap_->dual) LOG_INFO("decklink: switched to dual-stream 3D capture");
            cap_->dual = true;  // (cap_->m is held: restartLock)
            if (autoDual_) cap_->SetDualActive(true);
        }
        if (FAILED(r)) {
            // Starting the streams now would deliver nothing: a live cable would read as a
            // stall (NO SIGNAL) with the reason only in the log. Leave the input stopped and
            // fail the source, so the reason reaches the status bar, a toast and get_rig_status.
            const std::string why = "DeckLink input " + std::to_string(eye_) + " cannot capture the new format (" +
                                    modeName + (rgb ? ", RGB 4:4:4" : ", YUV 4:2:2") +
                                    "); capture stopped - re-select the input or change the source format";
            LOG_WARN("decklink: %s", why.c_str());
            cap_->RestartFailed(why);
            input_->FlushStreams();
            return S_OK;
        }
        if (got != cand[0])
            LOG_INFO("decklink: input %d: %s refused in this format, using %s", eye_, dlfmt::Name(cand[0]),
                     dlfmt::Name(got));
        // Depth lost to the format the card took (12-bit RGB as r210, 10-bit as 8-bit): once
        // per format change, in the log and on the Station panel (cleared when nothing is lost).
        const std::string loss = dlfmt::DepthLossText(rgb, signalBits, got);
        if (!loss.empty()) LOG_WARN("decklink: input %d: %s", eye_, loss.c_str());
        cap_->FormatWarning(eye_, loss);
        cap_->RestartOk();
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
        cap_->Enqueue(eye_, frame);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<DeckLinkCapture> cap_;  // outlives any callback the SDK still delivers
    int eye_;
    IDeckLinkInput* input_;
    BMDVideoInputFlags flags_;
    BMDPixelFormat requested_;  // the best format for the current signal (what a change compares with)
    bool yuv10Ok_;              // false when the card refused 10-bit YUV at open
    bool autoDual_;
    bool explicitDual_;         // --decklink-stereo=dual: ask for dual-stream again in each new mode
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

int DeckLinkSource::LastBitDepth() { return g_lastBitDepth.load(std::memory_order_relaxed); }

#endif  // MEDIAPLAYER_WITH_DECKLINK

void DeckLinkSource::SetFormatWarning(int eye, const std::string& text) {
    std::lock_guard<std::mutex> lk(warnMutex_);
    formatWarning_[eye & 1] = text;
}

std::string DeckLinkSource::FormatWarning() const {
    std::lock_guard<std::mutex> lk(warnMutex_);
    if (formatWarning_[0].empty()) return formatWarning_[1];
    if (formatWarning_[1].empty() || formatWarning_[1] == formatWarning_[0]) return formatWarning_[0];
    return formatWarning_[0] + "; " + formatWarning_[1];
}

#if defined(MEDIAPLAYER_WITH_DECKLINK)

std::string DeckLinkSource::DriverVersionText() {
    dl::ComScope com;
    const int64_t v = DriverVersion();
    return v > 0 ? VersionText(v) : std::string();
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
    if (pair) {
        // Direct pairing (pro/DirectPair.h): each eye converted straight into its half of the
        // ring frame. Needs the ring's queue mode (reserved frames); set before the lanes start.
        const bool on = pro::PerfSwitchOn(std::getenv("MEDIAPLAYER_DECKLINK_DIRECT_PAIR"));
        cap.directPair = on && Ring().QueueMode();
        {
            std::lock_guard<std::mutex> lk(pairMutex_);
            pairStats_.directPair = cap.directPair;
        }
        LOG_INFO("decklink: two-input pairs: %s",
                 cap.directPair ? "each eye converted straight into its half of the ring frame (direct pairing; "
                                  "MEDIAPLAYER_DECKLINK_DIRECT_PAIR=0 composes them as before)"
                 : on ? "composed from the two eyes (direct pairing needs the live queue; MEDIAPLAYER_LIVE_QUEUE=0)"
                      : "composed from the two eyes (MEDIAPLAYER_DECKLINK_DIRECT_PAIR=0)");
    }
    // One input carries two eyes per frame in dual-stream 3D, which Auto switches to when
    // the card detects it: its lane's pool is sized for both.
    const bool twoEyeInput = !pair && (cap.dual || cfg_.stereo == DeckLinkConfig::Stereo::Auto);
    cap.StartLanes(nInputs, twoEyeInput ? 2 : 1);  // before any input streams: the callbacks hand frames to these

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
            // Matched pairs only, on one card: a left and a right from different instants
            // are never shown (the last matched pair holds instead). The card's clock always
            // matches eventually, so this cannot freeze; two cards without matching timecode
            // have nothing to match on and keep newest-of-each.
            const char* sp = std::getenv("MEDIAPLAYER_DECKLINK_STRICT_PAIRS");
            cap.strictPairs = cap.sameCard && !(sp && sp[0] == '0');
            {
                std::lock_guard<std::mutex> lk(cap.m);
                cap.pairer.SetStrict(cap.strictPairs);
            }
            LOG_INFO("decklink: matched pairs only: %s%s", cap.strictPairs ? "on" : "off",
                     cap.sameCard ? (cap.strictPairs ? "" : " (MEDIAPLAYER_DECKLINK_STRICT_PAIRS=0)")
                                  : " (two cards: no shared clock to verify pairs)");
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
        std::lock_guard<std::mutex> lk(cap.m);          // no callback is mid-restart, no lane mid-pairing...
        std::lock_guard<std::mutex> pk(cap.publishM);   // ...or mid-publish (a pair composes without m)...
        cap.alive = false;                              // ...and none starts one from here on
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
    cap.StopLanes();  // `alive` is clear: a lane mid-frame publishes nothing and returns
    {
        // Direct pairing: the ring frames still reserved go back to the ring (it outlives this
        // capture: the next Open reserves from it again). No lane runs now, no compose.
        std::lock_guard<std::mutex> lk(cap.m);
        cap.table.ReleaseAll();
    }
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
int DeckLinkSource::LastBitDepth() { return 0; }
std::string DeckLinkSource::DriverVersionText() { return {}; }
DeckLinkSource::PairStats DeckLinkSource::GetPairStats() const { return {}; }
void DeckLinkSource::Worker() {}

#endif

}  // namespace mp
