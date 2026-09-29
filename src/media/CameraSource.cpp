// SPDX-License-Identifier: Apache-2.0
#include "media/CameraSource.h"

#include "Log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace mp {

namespace {
// Open-count across instances: a rescan (SDL_QuitSubSystem) must never pull a device
// out from under a running worker.
std::atomic<int> g_openCameras{0};

constexpr double kStallSeconds = 2.0;      // Streaming -> Stalled after this long frameless
constexpr double kFirstFrameSeconds = 5.0; // Opening -> Stalled if the first frame never lands
constexpr double kPermissionSeconds = 5.0; // wait this long for a permission verdict
constexpr double kStatsLogSeconds = 5.0;   // INFO stats cadence (never per frame)

std::string DenyEnv() {
    const char* e = std::getenv("MEDIAPLAYER_CAMERA_DENY");
    return e ? std::string(e) : std::string();
}

// A Media Foundation activation blocked by the Windows privacy toggle fails with
// E_ACCESSDENIED (0x80070005); a declined macOS prompt reports "denied".
bool LooksDenied(const std::string& err) {
    std::string l(err);
    for (char& c : l) c = (char)std::tolower((unsigned char)c);
    return l.find("80070005") != std::string::npos || l.find("denied") != std::string::npos;
}

// Everything SDL can hand us as NV12: its own NV12, or any other 8-bit uncompressed
// format it converts in its camera thread. 10-bit (P010) is out of #90's scope.
bool UsableFormat(SDL_PixelFormat f) {
    if (f == SDL_PIXELFORMAT_UNKNOWN) return false;
    if (f == SDL_PIXELFORMAT_MJPG) return true;
    if (f == SDL_PIXELFORMAT_P010) return false;
    if (SDL_ISPIXELFORMAT_10BIT(f)) return false;
    return true;
}
}  // namespace

double CameraSource::NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

const char* CameraSource::StateName(State s) {
    switch (s) {
        case State::Closed: return "closed";
        case State::Opening: return "opening";
        case State::Streaming: return "streaming";
        case State::Stalled: return "stalled";
        case State::Lost: return "lost";
        case State::Denied: return "denied";
        case State::Failed: return "failed";
    }
    return "?";
}

CameraSource::~CameraSource() { Stop(); }

std::vector<CameraDevice> CameraSource::Enumerate(bool rescan) {
    std::vector<CameraDevice> out;
    if (SDL_WasInit(SDL_INIT_CAMERA) != 0 && rescan) {
        if (g_openCameras.load() == 0) {
            SDL_QuitSubSystem(SDL_INIT_CAMERA);
            LOG_INFO("camera: rescanning devices");
        } else {
            LOG_INFO("camera: rescan skipped while a camera is open");
        }
    }
    if (SDL_WasInit(SDL_INIT_CAMERA) == 0) {
        if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
            LOG_WARN("camera: SDL_InitSubSystem(CAMERA) failed: %s", SDL_GetError());
            return out;
        }
        const char* drv = SDL_GetCurrentCameraDriver();
        LOG_INFO("camera: subsystem up (SDL driver: %s)", drv ? drv : "none");
    }
    const std::string denyEnv = DenyEnv();
    int n = 0;
    SDL_CameraID* ids = SDL_GetCameras(&n);
    for (int i = 0; ids && i < n; ++i) {
        CameraDevice d;
        d.id = ids[i];
        const char* nm = SDL_GetCameraName(ids[i]);
        d.name = nm ? nm : "";
        d.denied = IsDeniedCameraName(d.name, denyEnv);
        int fc = 0;
        SDL_CameraSpec** specs = SDL_GetCameraSupportedFormats(ids[i], &fc);
        for (int f = 0; specs && f < fc; ++f) {
            const SDL_CameraSpec* s = specs[f];
            if (!UsableFormat(s->format)) continue;
            CameraFormat cf;
            cf.fourccOrSdlFormat = (uint32_t)s->format;
            cf.w = s->width;
            cf.h = s->height;
            cf.fpsNum = s->framerate_numerator;
            cf.fpsDen = s->framerate_denominator > 0 ? s->framerate_denominator : 1;
            cf.mjpg = (s->format == SDL_PIXELFORMAT_MJPG);
            cf.nv12 = (s->format == SDL_PIXELFORMAT_NV12);
            d.formats.push_back(cf);
        }
        SDL_free(specs);
        out.push_back(std::move(d));
    }
    SDL_free(ids);
    size_t denied = 0;
    for (const auto& d : out) denied += d.denied ? 1 : 0;
    LOG_INFO("camera: %zu device(s) (%zu blocked)", out.size(), denied);
    return out;
}

void CameraSource::PrintDeviceList(FILE* out) {
    const std::vector<CameraDevice> devs = Enumerate(false);
    const char* drv = SDL_GetCurrentCameraDriver();
    std::fprintf(out, "cameras (SDL driver: %s)\n", drv ? drv : "none");
    if (devs.empty()) std::fprintf(out, "  (none)\n");
    int index = 0;
    for (const CameraDevice& d : devs) {
        if (d.denied) {
            std::fprintf(out, "  [-] %s   [blocked: tracking/IR camera - never opened]\n",
                         d.name.c_str());
            continue;
        }
        std::fprintf(out, "  [%d] %s\n", index++, d.name.c_str());
        // One line per pixel format, each distinct size@fps once, in SDL's order.
        std::vector<uint32_t> seenFmt;
        for (const CameraFormat& f : d.formats) {
            if (std::find(seenFmt.begin(), seenFmt.end(), f.fourccOrSdlFormat) != seenFmt.end())
                continue;
            seenFmt.push_back(f.fourccOrSdlFormat);
            const char* fname = SDL_GetPixelFormatName((SDL_PixelFormat)f.fourccOrSdlFormat);
            if (fname && std::strncmp(fname, "SDL_PIXELFORMAT_", 16) == 0) fname += 16;
            std::string line;
            std::vector<std::string> seenMode;
            for (const CameraFormat& g : d.formats) {
                if (g.fourccOrSdlFormat != f.fourccOrSdlFormat) continue;
                char m[48];
                std::snprintf(m, sizeof(m), "%dx%d@%d", g.w, g.h, FormatFps(g));
                if (std::find(seenMode.begin(), seenMode.end(), m) != seenMode.end()) continue;
                seenMode.push_back(m);
                line += "  ";
                line += m;
            }
            std::fprintf(out, "        %-6s%s%s\n", fname ? fname : "?", line.c_str(),
                         f.mjpg ? "   (compressed: not used yet)" : "");
        }
        CameraFormat pick;
        if (ChooseFormat(d, 0, 0, 0, pick, /*allowMjpg=*/false))
            std::fprintf(out, "        default: %dx%d@%d\n", pick.w, pick.h, FormatFps(pick));
        else
            std::fprintf(out, "        default: none (MJPG only - not supported yet)\n");
    }
    std::fflush(out);
}

std::string CameraSource::LastError() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return lastError_;
}

CameraSource::Stats CameraSource::GetStats() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return stats_;
}

void CameraSource::Fail(State s, const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        lastError_ = why;
    }
    state_ = s;
}

bool CameraSource::Open(const CameraDevice& dev, int wantW, int wantH, int wantFps) {
    Stop();
    id_ = dev.id;
    name_ = dev.name;
    removed_ = false;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        stats_ = Stats{};
        lastError_.clear();
    }
    // Third denylist check (enumeration and SelectCamera are the other two). Re-derived
    // from the name so a caller-built CameraDevice with denied=false cannot slip through.
    if (dev.denied || IsDeniedCameraName(dev.name, DenyEnv())) {
        LOG_WARN("camera: refusing to open '%s' — it is a tracking/IR camera (denylist)",
                 dev.name.c_str());
        Fail(State::Failed, "'" + dev.name + "' is a tracking camera and is never opened");
        return false;
    }
    CameraFormat fmt;
    if (!ChooseFormat(dev, wantW, wantH, wantFps, fmt, /*allowMjpg=*/false)) {
        const bool anyMjpg = std::any_of(dev.formats.begin(), dev.formats.end(),
                                         [](const CameraFormat& f) { return f.mjpg; });
        const std::string why = anyMjpg
            ? "camera offers MJPG only — not supported yet (#90 follow-up)"
            : "camera offers no usable format";
        LOG_WARN("camera: '%s': %s", dev.name.c_str(), why.c_str());
        Fail(State::Failed, why);
        return false;
    }
    fpsReq_ = FormatFps(fmt);
    w_ = fmt.w;
    h_ = fmt.h;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        stats_.w = fmt.w;
        stats_.h = fmt.h;
        stats_.fpsReq = fpsReq_;
    }
    LOG_INFO("camera: opening '%s' (id %u) as NV12 %dx%d@%d", dev.name.c_str(), dev.id, fmt.w,
             fmt.h, fpsReq_);
    state_ = State::Opening;
    stop_ = false;
    ++g_openCameras;
    worker_ = std::thread([this, id = dev.id, fmt] { WorkerMain(id, fmt); });
    return true;
}

void CameraSource::Stop() {
    if (worker_.joinable()) {
        stop_ = true;
        worker_.join();
        --g_openCameras;
        LOG_INFO("camera: closed '%s'", name_.c_str());
    }
    stop_ = false;
    state_ = State::Closed;
}

void CameraSource::WorkerMain(uint32_t id, CameraFormat fmt) {
    // Always request NV12: the Windows Frame Server exposes native NV12 types for MJPG
    // UVC devices, and SDL converts any other uncompressed format in its own thread.
    SDL_CameraSpec spec{};
    spec.format = SDL_PIXELFORMAT_NV12;
    spec.colorspace = SDL_COLORSPACE_UNKNOWN;
    spec.width = fmt.w;
    spec.height = fmt.h;
    spec.framerate_numerator = fmt.fpsNum;
    spec.framerate_denominator = fmt.fpsDen;
    const double t0 = NowSeconds();
    SDL_Camera* cam = SDL_OpenCamera((SDL_CameraID)id, &spec);
    if (!cam) {
        const std::string err = SDL_GetError();
        LOG_WARN("camera: SDL_OpenCamera('%s') failed: %s", name_.c_str(), err.c_str());
        Fail(LooksDenied(err) ? State::Denied : State::Failed, err);
        return;
    }

    // Permission: Windows desktop apps get no prompt (approved at once, or blocked at
    // activation above); macOS shows the NSCameraUsageDescription prompt.
    int perm = SDL_GetCameraPermissionState(cam);
    while (perm == 0 && !stop_ && NowSeconds() - t0 < kPermissionSeconds) {
        SDL_Delay(5);
        perm = SDL_GetCameraPermissionState(cam);
    }
    if (perm < 0) {
        LOG_WARN("camera: permission denied for '%s'", name_.c_str());
        Fail(State::Denied, "camera permission denied");
        SDL_CloseCamera(cam);
        return;
    }

    SDL_CameraSpec got{};
    if (!SDL_GetCameraFormat(cam, &got)) got = spec;
    bool fullRange = SDL_COLORSPACERANGE(got.colorspace) == SDL_COLOR_RANGE_FULL;
    w_ = got.width;
    h_ = got.height;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        stats_.w = got.width;
        stats_.h = got.height;
        stats_.fullRange = fullRange;
    }
    const char* gotName = SDL_GetPixelFormatName(got.format);
    LOG_INFO("camera: '%s' open in %.0f ms: %s %dx%d @ %d/%d, colorspace 0x%08x (%s range)",
             name_.c_str(), (NowSeconds() - t0) * 1000.0, gotName ? gotName : "?", got.width,
             got.height, got.framerate_numerator, got.framerate_denominator,
             (unsigned)got.colorspace, fullRange ? "full" : "limited");

    double lastFrame = NowSeconds();
    double lastStatsLog = lastFrame;
    uint64_t logPublished = 0;
    double copyMsSum = 0.0;
    uint64_t copyCount = 0;
    bool loggedFirst = false, loggedBadFormat = false;
    fpsWindowStart_ = lastFrame;
    fpsWindowCount_ = 0;

    while (!stop_) {
        if (removed_ && state_ != State::Lost) {
            LOG_WARN("camera: '%s' disconnected", name_.c_str());
            Fail(State::Lost, "camera disconnected");
        }
        if (state_ == State::Lost) {
            SDL_Delay(10);
            continue;
        }
        // Drain SDL's queue down to the NEWEST surface: showing a stale frame adds a
        // frame of latency per queued surface, so the older ones are released unseen.
        Uint64 ts = 0;
        SDL_Surface* s = SDL_AcquireCameraFrame(cam, &ts);
        if (s) {
            for (;;) {
                Uint64 ts2 = 0;
                SDL_Surface* newer = SDL_AcquireCameraFrame(cam, &ts2);
                if (!newer) break;
                SDL_ReleaseCameraFrame(cam, s);
                s = newer;
                std::lock_guard<std::mutex> lk(statsMutex_);
                ++stats_.dropped;
            }
        }
        const double now = NowSeconds();
        if (!s) {
            const State st = state_.load();
            if (st == State::Streaming && now - lastFrame > kStallSeconds) {
                LOG_WARN("camera: '%s' stalled (no frame for %.0f s)", name_.c_str(),
                         kStallSeconds);
                state_ = State::Stalled;
            } else if (st == State::Opening && now - lastFrame > kFirstFrameSeconds) {
                LOG_WARN("camera: '%s' opened but delivered no frame in %.0f s", name_.c_str(),
                         kFirstFrameSeconds);
                state_ = State::Stalled;
            }
            SDL_DelayNS(1000000);  // 1 ms: SDL's queue has no wait primitive
        } else {
            if (s->format != SDL_PIXELFORMAT_NV12) {
                if (!loggedBadFormat) {
                    const char* fn = SDL_GetPixelFormatName(s->format);
                    LOG_WARN("camera: '%s' delivers %s, not NV12 — frames dropped",
                             name_.c_str(), fn ? fn : "?");
                    loggedBadFormat = true;
                    Fail(State::Failed, std::string("camera delivered ") + (fn ? fn : "?") +
                                            " instead of NV12");
                }
                SDL_ReleaseCameraFrame(cam, s);
                SDL_Delay(5);
                continue;
            }
            // The surface's own colorspace is authoritative once frames flow.
            const SDL_Colorspace cs = SDL_GetSurfaceColorspace(s);
            if (cs != SDL_COLORSPACE_UNKNOWN)
                fullRange = SDL_COLORSPACERANGE(cs) == SDL_COLOR_RANGE_FULL;
            if (!loggedFirst) {
                LOG_INFO("camera: first frame %dx%d pitch %d (%s range), %.0f ms after open",
                         s->w, s->h, s->pitch, fullRange ? "full" : "limited",
                         (now - t0) * 1000.0);
                loggedFirst = true;
            }
            const auto c0 = std::chrono::steady_clock::now();
            PublishNV12((const uint8_t*)s->pixels, s->pitch, s->w, s->h, fullRange);
            copyMsSum += std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - c0)
                             .count();
            ++copyCount;
            SDL_ReleaseCameraFrame(cam, s);
            lastFrame = now;
            const State st = state_.load();
            if (st == State::Opening || st == State::Stalled) {
                if (st == State::Stalled)
                    LOG_INFO("camera: '%s' frames resumed", name_.c_str());
                state_ = State::Streaming;
            }
        }
        if (now - lastStatsLog >= kStatsLogSeconds) {
            const Stats st = GetStats();
            LOG_INFO("camera: %s %dx%d  %.1f fps delivered (req %d), %llu published "
                     "(+%llu), %llu dropped, copy %.2f ms/frame",
                     StateName(state_.load()), st.w, st.h, st.deliveredFps, st.fpsReq,
                     (unsigned long long)st.published,
                     (unsigned long long)(st.published - logPublished),
                     (unsigned long long)st.dropped,
                     copyCount ? copyMsSum / (double)copyCount : 0.0);
            logPublished = st.published;
            copyMsSum = 0.0;
            copyCount = 0;
            lastStatsLog = now;
        }
    }
    SDL_CloseCamera(cam);
}

void CameraSource::NotePublished() {
    const double now = NowSeconds();
    ++fpsWindowCount_;
    std::lock_guard<std::mutex> lk(statsMutex_);
    ++stats_.published;
    stats_.lastPublishSec = now;
    const double el = now - fpsWindowStart_;
    if (el >= 1.0) {
        stats_.deliveredFps = (float)((double)fpsWindowCount_ / el);
        fpsWindowCount_ = 0;
        fpsWindowStart_ = now;
    }
}

void CameraSource::PublishNV12(const uint8_t* pixels, int pitch, int w, int h, bool fullRange) {
    if (!pixels || w <= 0 || h <= 0 || pitch < w) return;
    std::lock_guard<std::mutex> lk(publishMutex_);
    FrameRing::Frame& f = ring_.WriteBuffer();
    const size_t ySize = (size_t)w * (size_t)h;
    const size_t uvSize = (size_t)w * (size_t)(h / 2);  // (w/2 texels x 2 B) x h/2 rows
    // resize() to the same size is a no-op and vectors keep their capacity, so after the
    // first frame in each of the 3 ring slots there are no further allocations.
    if (f.plane[0].size() != ySize) f.plane[0].resize(ySize);
    if (f.plane[1].size() != uvSize) f.plane[1].resize(uvSize);
    // SDL's NV12 convention: the UV plane starts right after h rows of Y, same pitch.
    for (int y = 0; y < h; ++y)
        std::memcpy(f.plane[0].data() + (size_t)y * w, pixels + (size_t)y * pitch, (size_t)w);
    const uint8_t* uv = pixels + (size_t)pitch * (size_t)h;
    for (int y = 0; y < h / 2; ++y)
        std::memcpy(f.plane[1].data() + (size_t)y * w, uv + (size_t)y * pitch, (size_t)w);
    f.width = w;
    f.height = h;
    f.format = PixFormat::NV12;
    f.fullRange = fullRange;
    f.gpu = false;
    f.gpuSharedTexture = nullptr;
    f.gpuSharedHandle = nullptr;
    ring_.Publish();
    NotePublished();
}

void CameraSource::PublishI420(const uint8_t* const plane[3], const int linesize[3], int w,
                               int h, bool fullRange) {
    if (!plane[0] || !plane[1] || !plane[2] || w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(publishMutex_);
    FrameRing::Frame& f = ring_.WriteBuffer();
    const int cw = w / 2, ch = h / 2;
    const size_t sizes[3] = {(size_t)w * h, (size_t)cw * ch, (size_t)cw * ch};
    const int widths[3] = {w, cw, cw};
    const int rows[3] = {h, ch, ch};
    for (int p = 0; p < 3; ++p) {
        if (f.plane[p].size() != sizes[p]) f.plane[p].resize(sizes[p]);
        for (int y = 0; y < rows[p]; ++y)
            std::memcpy(f.plane[p].data() + (size_t)y * widths[p],
                        plane[p] + (size_t)y * linesize[p], (size_t)widths[p]);
    }
    f.width = w;
    f.height = h;
    f.format = PixFormat::I420;
    f.fullRange = fullRange;
    f.gpu = false;
    f.gpuSharedTexture = nullptr;
    f.gpuSharedHandle = nullptr;
    ring_.Publish();
    NotePublished();
}

}  // namespace mp
