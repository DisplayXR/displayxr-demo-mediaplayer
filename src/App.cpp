// SPDX-License-Identifier: BSL-1.0
#include "App.h"

#include "Log.h"
#include "media/ImageDecoder.h"
#include "media/LifLoader.h"
#include "media/MediaSource.h"
#include "ui/Hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <SDL3/SDL.h>   // SDL_ShowOpenFileDialog (Tier-0 native file dialog)

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX        // keep std::min/std::max usable (windows.h defines min/max macros)
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shlobj.h>     // SHGetKnownFolderPath — resolve the user's Pictures folder
#endif

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include "imgui.h"
#endif

namespace mp {

namespace {
// RED|BLUE left/right test pattern (the no-media fallback when the idle logo is absent).
constexpr ClearColor kLeftImage{0.85f, 0.10f, 0.10f, 1.0f};   // RED  = left
constexpr ClearColor kRightImage{0.10f, 0.20f, 0.85f, 1.0f};  // BLUE = right

// Idle screen: the DisplayXR mark on a calm, slightly-cool dark grey (matches the shell
// home backdrop). Used both as the DrawViews background and the logo-composite fill.
constexpr float kIdleBgR = 0.12f, kIdleBgG = 0.12f, kIdleBgB = 0.13f;

// Convergence (horizontal image translation) budget: each '[' / ']' nudges by one
// step, clamped to ±max — fractions of a view tile, kept small for "subtle" depth.
constexpr float kConvergenceStep = 0.0025f;
constexpr float kConvergenceMax = 0.05f;

// Where the 'I'-key atlas captures land: <Pictures>/DisplayXR on Windows, else the
// working directory. xrCaptureAtlasEXT appends "_atlas.png" to the prefix we return;
// we number against existing "<stem>-<N>_<cols>x<rows>_atlas.png" so repeats accumulate
// instead of overwriting. Mirrors the modelviewer/gaussiansplat demos' convention.
std::string MakeCaptureAtlasPrefix(const std::string& stem, uint32_t cols, uint32_t rows) {
    namespace fs = std::filesystem;
    std::string dir;
#if defined(_WIN32)
    PWSTR picsW = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &picsW)) &&
        picsW) {
        char buf[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, picsW, -1, buf, MAX_PATH, nullptr, nullptr);
        CoTaskMemFree(picsW);
        dir = std::string(buf) + "\\DisplayXR";
    }
    const char* sep = "\\";
#else
    if (const char* home = std::getenv("HOME")) dir = std::string(home) + "/Pictures/DisplayXR";
    const char* sep = "/";
#endif
    if (dir.empty()) dir = ".";
    std::error_code ec;
    fs::create_directories(dir, ec);

    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "_%ux%u_atlas.png", cols, rows);
    const std::string head = stem + "-";
    const std::string suf = suffix;
    int maxN = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string fn = it->path().filename().string();
        if (fn.size() <= head.size() + suf.size()) continue;
        if (fn.compare(0, head.size(), head) != 0) continue;
        if (fn.compare(fn.size() - suf.size(), suf.size(), suf) != 0) continue;
        const std::string num = fn.substr(head.size(), fn.size() - head.size() - suf.size());
        bool digits = !num.empty();
        for (char c : num) digits = digits && (c >= '0' && c <= '9');
        if (digits) maxN = std::max(maxN, std::atoi(num.c_str()));
    }
    char tail[256];
    std::snprintf(tail, sizeof(tail), "%s-%d_%ux%u", stem.c_str(), maxN + 1, cols, rows);
    return dir + sep + tail;
}

// HUD layer disparity. 0 = zero-disparity plane (screen depth) so the UI sits exactly
// where the cursor is and clicks align across both eyes. (A small negative value would
// float it toward the viewer per PRD §9, but that offsets the perceived hit target.)
constexpr float kHudDisparity = 0.0f;

// Auto-hide / slideshow timing.
constexpr double kIdleHideSeconds = 5.0;    // fade the UI out after this much inactivity
constexpr double kFadeSeconds = 0.20;       // UI fade in/out duration
constexpr double kToastFadeSeconds = 0.30;  // toast fade in/out duration
constexpr double kStillSeconds = 5.0;       // slideshow: seconds to hold a still image
constexpr double kTransitionSeconds = 0.40; // slideshow: dip-to-black half-duration
// Scrub speed (video-seconds moved per UI frame) above which we show keyframes instead
// of exact frames — a fast sweep; below it, fine adjustment gets exact frames.
constexpr float kFastScrubSeconds = 1.0f;

// Per-eye display aspect (width/height): full SBS packs two eyes across the width
// (each eye half-width); half-SBS and mono use the full frame width.
float PerEyeAspect(StereoLayout layout, int frameW, int frameH) {
    if (frameH <= 0) return 1.0f;
    const float eyeW = (layout == StereoLayout::SbsFull) ? (float)frameW * 0.5f : (float)frameW;
    return eyeW / (float)frameH;
}

// "Match min-to-min": scale `contentAspect` so the content's SHORTER screen side equals
// the tile's shorter side, then center. The longer side then overflows (cropped by the
// scissor) when the content is more elongated than the tile, or sits inside it otherwise.
// Adapts dynamically to the window aspect (landscape vs portrait). No stretch.
XrSession::ViewRect MatchMinRect(const XrSession::ViewRect& tile, float contentAspect) {
    if (tile.w == 0 || tile.h == 0 || contentAspect <= 0.0f) return tile;
    const uint32_t tileMin = std::min(tile.w, tile.h);
    uint32_t cw, ch;
    if (contentAspect >= 1.0f) {           // landscape content: height is its min side
        ch = tileMin;
        cw = (uint32_t)((float)tileMin * contentAspect + 0.5f);
    } else {                               // portrait content: width is its min side
        cw = tileMin;
        ch = (uint32_t)((float)tileMin / contentAspect + 0.5f);
    }
    XrSession::ViewRect r;
    r.w = cw;
    r.h = ch;
    r.x = tile.x + (int32_t)(((int64_t)tile.w - (int64_t)cw) / 2);
    r.y = tile.y + (int32_t)(((int64_t)tile.h - (int64_t)ch) / 2);
    return r;
}
} // namespace

bool App::Initialize(const char* mediaPath) {
    // Default 1280x720; MEDIAPLAYER_WINDOW="WxH" overrides (e.g. "720x1280" to test
    // portrait aspect handling without a rebuild).
    int winW = 1280, winH = 720;
    if (const char* s = std::getenv("MEDIAPLAYER_WINDOW")) {
        int a = 0, b = 0;
        if (std::sscanf(s, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) { winW = a; winH = b; }
    }
    if (!window_.Create("DisplayXR Stereo Media Player", winW, winH)) return false;

    if (!xr_.Initialize(window_.NativeHandle())) {
        LOG_ERROR("OpenXR initialization failed");
        return false;
    }

    if (!renderer_.Initialize(xr_.PhysicalDevice(), xr_.Device(), xr_.GraphicsQueue(),
                              xr_.GraphicsQueueFamily(), (VkFormat)xr_.SwapchainFormat(),
                              xr_.SwapchainWidth(), xr_.SwapchainHeight(),
                              xr_.SwapchainImages())) {
        LOG_ERROR("Vulkan renderer initialization failed");
        return false;
    }
    // Transparent-background experiment: when the session was created with it, clear
    // the letterbox to alpha 0 so the runtime composes those pixels through.
    renderer_.SetTransparentLetterbox(xr_.TransparentBackground());

    // Audio is the A/V master: the video decoder paces frames to the audio clock when a
    // clip has sound (else it falls back to its own wall clock).
    video_.SetMasterClock([this]() { return audio_.ClockSeconds(); });

    // Load a stereo image or video if one was given; otherwise the loop falls back
    // to the RED|BLUE L/R test pattern (and the user can Open one at runtime). The
    // argument may be a single file OR a folder — for a folder we load the first
    // supported asset (sorted), and LoadMedia builds the prev/next list from it.
    if (mediaPath && *mediaPath) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::is_directory(fs::path(mediaPath), ec) && !ec) {
            std::vector<std::string> files;
            for (fs::directory_iterator it(mediaPath, ec), end; !ec && it != end;
                 it.increment(ec)) {
                std::error_code fe;
                if (!it->is_regular_file(fe) || fe) continue;
                const std::string s = it->path().string();
                if (MediaSource::IsSupported(s)) files.push_back(s);
            }
            std::sort(files.begin(), files.end());
            if (!files.empty()) {
                LOG_INFO("Folder '%s': %zu asset(s), opening first", mediaPath, files.size());
                LoadMedia(files.front());
            } else {
                LOG_WARN("No supported media in folder '%s'", mediaPath);
            }
        } else {
            LoadMedia(mediaPath);
        }
    }
    if (!hasMedia_) {
        LoadIdleLogo();  // DisplayXR mark on dark grey; falls back to RED|BLUE if absent
    }

    // Dear ImGui transport bar, rendered into the window-space HUD layer (M4). If it
    // can't init, RenderOneFrame falls back to the CPU-rasterized text HUD.
    if (xr_.HasHud()) {
        if (imgui_.Init(window_.SdlWindow(), xr_.VkInstanceHandle(), xr_.PhysicalDevice(),
                        xr_.Device(), xr_.GraphicsQueue(), xr_.GraphicsQueueFamily(),
                        (VkFormat)xr_.HudFormat(), xr_.HudWidth(), xr_.HudHeight(),
                        xr_.HudImages())) {
            window_.SetEventHook([this](void* ev) { imgui_.ProcessEvent(ev); });
        } else {
            LOG_WARN("ImGui unavailable — using the text HUD");
        }
    }
    return true;
}

int App::Run() {
    startMode_ = []() {
        const char* e = std::getenv("MEDIAPLAYER_START_MODE");
        return e ? std::atoi(e) : -1;
    }();
    dumpPath_ = std::getenv("MEDIAPLAYER_DUMP_ATLAS");
    dumpHudPath_ = std::getenv("MEDIAPLAYER_DUMP_HUD");
    openAfterPath_ = std::getenv("MEDIAPLAYER_OPEN_AFTER");  // test: swap media mid-run
    if (const char* h = std::getenv("MEDIAPLAYER_HUD")) showHud_ = (*h && *h != '0');
    // Env-gated initial stereo state (test scaffolding: lets a headless atlas dump
    // prove convergence/swap without keystrokes). Interactive keys still apply on top.
    if (const char* c = std::getenv("MEDIAPLAYER_CONV")) convergence_ = (float)std::atof(c);
    if (const char* s = std::getenv("MEDIAPLAYER_SWAP")) swapEyes_ = (*s && *s != '0');
    const auto bootNow = std::chrono::steady_clock::now();
    fpsWindowStart_ = bootNow;
    lastFrameTime_ = bootNow;
    lastActivity_ = bootNow;   // UI starts visible, then fades after the idle timeout
    if (const char* fs = std::getenv("MEDIAPLAYER_FULLSCREEN"); fs && *fs && *fs != '0')
        window_.ToggleFullscreen();

    // Render during the macOS modal resize loop too, for continuous live resize.
    window_.SetLiveResizeCallback([this]() { RenderOneFrame(); });

    bool keepRunning = true;
    while (keepRunning && !xr_.ExitRequested()) {
        keepRunning = window_.PumpEvents();
        const auto nowT = std::chrono::steady_clock::now();
        bool activity = window_.TakeMouseActivity();   // any input revives the UI
        if (window_.TakeCycleModeRequest()) { xr_.RequestNextMode(); activity = true; }  // 'V'
        if (window_.TakeToggleHudRequest()) {                        // SHIFT+TAB (master)
            showHud_ = !showHud_;
            LOG_INFO("HUD %s", showHud_ ? "on" : "off");
            activity = true;
        }
        // M4 transport / stereo controls.
        if (const int steps = window_.TakeConvergenceSteps(); steps != 0) {  // '-' / '='
            convergence_ += steps * kConvergenceStep;
            if (convergence_ > kConvergenceMax) convergence_ = kConvergenceMax;
            if (convergence_ < -kConvergenceMax) convergence_ = -kConvergenceMax;
            LOG_INFO("convergence=%+.3f", convergence_);
            char t[48];
            std::snprintf(t, sizeof(t), "Convergence %+.1f%%", convergence_ * 100.0f);
            ShowToast(t);
            activity = true;
        }
        if (window_.TakeResetConvergence()) {                         // '0'
            convergence_ = 0.0f;
            LOG_INFO("convergence reset");
            ShowToast("Convergence reset");
            activity = true;
        }
        if (window_.TakeAutoConvergeRequest()) {                      // Backspace
            if (mediaAutoConvAvailable_) {
                convergence_ = mediaAutoConvergence_;  // undo with '0'
                LOG_INFO("auto-convergence applied: %+.4f", convergence_);
                char t[48];
                std::snprintf(t, sizeof(t), "Auto-converge %+.1f%%", convergence_ * 100.0f);
                ShowToast(t);
            } else {
                ShowToast("Auto-converge: file already has convergence");
            }
            activity = true;
        }
        if (const int fs = window_.TakeFrameStep(); fs != 0) {        // '[' / ']'
            StepFrame(fs);
            activity = true;
        }
        if (window_.TakeSwapEyesRequest()) {                          // 'X'
            swapEyes_ = !swapEyes_;
            LOG_INFO("swap eyes %s", swapEyes_ ? "on" : "off");
            activity = true;
        }
        if (window_.TakeCaptureRequest()) {                           // 'I' — atlas snapshot
            if (xr_.HasAtlasCapture()) {
                const std::string stem = currentMediaPath_.empty()
                    ? std::string("capture")
                    : std::filesystem::path(currentMediaPath_).stem().string();
                const std::string prefix = MakeCaptureAtlasPrefix(
                    stem, xr_.ActiveTileColumns(), xr_.ActiveTileRows());
                ShowToast(xr_.CaptureAtlas(prefix) ? "Captured atlas" : "Capture failed");
            } else {
                ShowToast("Capture unsupported");
            }
            activity = true;
        }
        if (window_.TakeTogglePauseRequest()) { TogglePlayback(); activity = true; }  // Space
        if (window_.TakePrevMediaRequest()) { RequestNavTransition(-1); activity = true; }  // Left
        if (window_.TakeNextMediaRequest()) { RequestNavTransition(+1); activity = true; }  // Right
        if (window_.TakeToggleSlideshowRequest()) { ToggleSlideshow(); activity = true; } // 'S'
        if (window_.TakeToggleMuteRequest()) { ToggleMute(); activity = true; }            // 'M'
        if (window_.TakeMouseLeft()) {
            // Cursor left the window — drop the UI now (push idle past the hide threshold).
            lastActivity_ =
                nowT - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double>(kIdleHideSeconds + 1.0));
        }
        if (activity) lastActivity_ = nowT;
        xr_.PollEvents();

        // Open-file results: the workspace picker completion (drained by PollEvents)
        // and the native dialog (parked off-thread). Either may yield a new path.
        std::string picked;
        if (xr_.TakePickedFile(picked)) {
            openFilePending_ = false;
            if (!picked.empty()) ReloadMedia(picked);
        }
        bool gotNative = false;
        {
            std::lock_guard<std::mutex> lk(nativePathMutex_);
            if (hasNativePath_) { hasNativePath_ = false; picked = nativePath_; gotNative = true; }
        }
        if (gotNative) {
            openFilePending_ = false;
            if (!picked.empty()) ReloadMedia(picked);  // outside the lock
        }
        // Test hook: exercise the media-swap path without a dialog.
        if (openAfterPath_ && !openedAfter_ && frames_ > 150) {
            openedAfter_ = true;
            ReloadMedia(openAfterPath_);
        }

        if (xr_.IsRunning()) RenderOneFrame();
    }

    LOG_INFO("Exiting render loop after %llu frames (%llu rendered, userQuit=%d xrExit=%d)",
             (unsigned long long)frames_, (unsigned long long)rendered_,
             !keepRunning, xr_.ExitRequested());
    return 0;
}

void App::RenderOneFrame() {
    // The live-resize event watch can call us during the modal loop (and SDL may
    // fire an EXPOSED before the session is running). Skip when not running, and
    // never re-enter — OpenXR's wait/begin/end frame ordering must stay strict.
    if (!xr_.IsRunning() || inRenderFrame_) return;
    inRenderFrame_ = true;
    struct Guard { bool& f; ~Guard() { f = false; } } guard{inRenderFrame_};

    // Advance UI fades + the slideshow machine (may swap media at a fade's mid-black).
    TickUi();

    if (startMode_ >= 0 && !startModeRequested_) {
        xr_.RequestMode((uint32_t)startMode_);
        startModeRequested_ = true;
    }

    // Pull the latest decoded video frame (if any) and upload its YUV planes — the GPU
    // does the colour convert + downscale, so no swscale ran on the decode thread.
    if (isVideo_) {
        if (const FrameRing::Frame* vf = video_.Ring().AcquireLatest()) {
            const bool i420 = (vf->format == PixFormat::I420);
            renderer_.UploadYUV(vf->plane[0].data(), vf->plane[1].data(),
                                i420 ? vf->plane[2].data() : nullptr, (uint32_t)vf->width,
                                (uint32_t)vf->height, i420 ? 0 : 1, vf->fullRange);
        }
    }

    XrSession::Frame frame;
    if (!xr_.BeginFrame(frame)) return;

    XrSession::ViewRect rects[XrSession::kMaxViews];          // full per-view tiles (submitted)
    XrSession::ViewRect contentRects[XrSession::kMaxViews];   // aspect-fit content within each tile

    if (frame.shouldRender) {
        // Each active view's tile = canvas_px * mode.viewScale, so the rects we clear
        // and submit match what the runtime samples.
        uint32_t canvasW = 0, canvasH = 0;
        window_.PixelSize(canvasW, canvasH);
        xr_.ComputeViewRects(canvasW, canvasH, rects);

        // Map the 2-view source onto the N display views by eye-X vs the views' center.
        float minX = frame.views[0].pose.position.x;
        float maxX = minX;
        for (uint32_t v = 1; v < frame.viewCount; ++v) {
            const float x = frame.views[v].pose.position.x;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
        }
        const float centerX = 0.5f * (minX + maxX);

        ClearColor colors[XrSession::kMaxViews];
        ViewUV uvs[XrSession::kMaxViews];
        bool isLeftView[XrSession::kMaxViews];
        for (uint32_t v = 0; v < frame.viewCount; ++v) {
            // 'X' swaps which SBS half each eye samples (XOR the geometric L/R).
            const bool isLeft = (frame.views[v].pose.position.x <= centerX) != swapEyes_;
            isLeftView[v] = isLeft;
            colors[v] = isLeft ? kLeftImage : kRightImage;  // RED|BLUE fallback
            if (layout_ == StereoLayout::Mono) {
                uvs[v] = {0.0f, 0.0f, 1.0f, 1.0f};
            } else {
                uvs[v] = isLeft ? ViewUV{0.0f, 0.0f, 0.5f, 1.0f}
                                : ViewUV{0.5f, 0.0f, 0.5f, 1.0f};
            }
        }

        // Diagnostic: dump the per-view geometry once per mode change.
        const char* mode = xr_.ActiveModeName();
        if (mode != prevMode_) {
            prevMode_ = mode;
            LOG_INFO("Mode '%s': %u views, centerX=%.4f, canvas=%ux%u, tile=%ux%u",
                     mode, frame.viewCount, centerX, canvasW, canvasH, rects[0].w, rects[0].h);
        }

        if (renderer_.HasTexture()) {
            // Match the content's shorter side to the tile's shorter side (crop the
            // longer axis), then scale into each tile (handles non-uniform modes).
            const float sx = xr_.ActiveViewScaleX();
            const float sy = xr_.ActiveViewScaleY();
            const XrSession::ViewRect viewFit = MatchMinRect({0, 0, canvasW, canvasH}, contentAspect_);
            const int32_t lbx = (int32_t)((float)viewFit.x * sx);  // horizontal inset of the content
            // Convergence = horizontal image translation: shift the two eyes' content
            // oppositely within their tiles, moving the zero-disparity plane. A LIF's
            // baked convergence (mediaConvergence_, scaled 0.5 to match the reference
            // per-eye shift) lands it at the author's intended plane; convergence_ trims.
            const float conv = convergence_ + 0.5f * mediaConvergence_;
            int32_t cdx[XrSession::kMaxViews];
            for (uint32_t v = 0; v < frame.viewCount; ++v) {
                cdx[v] = (int32_t)(conv * (float)rects[v].w * (isLeftView[v] ? 1.0f : -1.0f));
                contentRects[v].x = rects[v].x + cdx[v] + lbx;
                contentRects[v].y = rects[v].y + (int32_t)((float)viewFit.y * sy + 0.5f);
                contentRects[v].w = (uint32_t)((float)viewFit.w * sx + 0.5f);
                contentRects[v].h = (uint32_t)((float)viewFit.h * sy + 0.5f);
            }

            // Build the draw list: each eye's content quad (scissor = its tile), then
            // mirror-fill ONLY the reconvergence de-occlusion strip (offset/2 wide) by
            // reflecting this eye's own content across the exposed edge (reflect-101). Any
            // match-min letterbox bar is left as-is (black). No second-view dependency.
            XrSession::ViewRect drawVp[XrSession::kMaxViews];
            XrSession::ViewRect drawClip[XrSession::kMaxViews];
            ViewUV drawUv[XrSession::kMaxViews];
            uint32_t n = 0;
            for (uint32_t v = 0; v < frame.viewCount; ++v) {
                // Clip the content to a FIXED window (the unshifted match-min rect, clamped
                // to the tile) — the same tile-relative region for both eyes. Convergence
                // shifts the content *within* this window, so the letterbox stays symmetric
                // (no region is content in one eye but black letterbox in the other); the
                // exposed window edge is the offset/2 de-occlusion, mirror-filled below.
                const int32_t wx0 = std::max(rects[v].x + lbx, rects[v].x);
                const int32_t wy0 = std::max(contentRects[v].y, rects[v].y);
                const int32_t wx1 = std::min(rects[v].x + lbx + (int32_t)contentRects[v].w,
                                             rects[v].x + (int32_t)rects[v].w);
                const int32_t wy1 = std::min(contentRects[v].y + (int32_t)contentRects[v].h,
                                             rects[v].y + (int32_t)rects[v].h);
                drawVp[n] = contentRects[v];
                drawClip[n] = {wx0, wy0, (uint32_t)std::max(0, wx1 - wx0),
                               (uint32_t)std::max(0, wy1 - wy0)};
                drawUv[n] = uvs[v];
                ++n;
            }
            for (uint32_t v = 0; v < frame.viewCount && n < XrSession::kMaxViews; ++v) {
                if (cdx[v] == 0) continue;          // no convergence shift → no de-occlusion
                const XrSession::ViewRect& cr = contentRects[v];
                const XrSession::ViewRect& t = rects[v];
                const int32_t cw = (int32_t)cr.w;
                int32_t gx, gw, vpx;
                if (cdx[v] > 0) {                   // content moved right → strip on its left edge
                    gx = cr.x - cdx[v];             // [cr.x - offset/2, cr.x]
                    gw = cdx[v];
                    vpx = cr.x - cw;                // reflected quad sits left of cr.x
                } else {                            // content moved left → strip on its right edge
                    gx = cr.x + cw;                 // [cr.x+cw, cr.x+cw + offset/2]
                    gw = -cdx[v];
                    vpx = cr.x + cw;                // reflected quad sits right of the edge
                }
                // Clamp the strip to the tile so it never spills into the letterbox or the
                // adjacent eye; when the content overflows the tile there's no strip to fill.
                const int32_t gl = std::max(gx, t.x);
                const int32_t gr = std::min(gx + gw, t.x + (int32_t)t.w);
                if (gr <= gl) continue;
                XrSession::ViewRect gap{gl, t.y, (uint32_t)(gr - gl), t.h};
                XrSession::ViewRect fillVp{vpx, cr.y, cr.w, cr.h};
                ViewUV mu{uvs[v].offX + uvs[v].scaleX, uvs[v].offY,
                          -uvs[v].scaleX, uvs[v].scaleY};  // mirror across the exposed edge
                drawVp[n] = fillVp;
                drawClip[n] = gap;
                drawUv[n] = mu;
                ++n;
            }
            renderer_.DrawViews(frame.imageIndex, drawVp, drawClip, drawUv, n);
        } else if (hasMedia_) {
            const ClearColor black[XrSession::kMaxViews] = {};  // video warming up
            renderer_.ClearViews(frame.imageIndex, black, rects, frame.viewCount);
        } else {
            renderer_.ClearViews(frame.imageIndex, colors, rects, frame.viewCount);
        }
        ++rendered_;

        if (dumpPath_ && !dumped_ && rendered_ >= 100) {
            // Prefer the runtime-owned atlas capture (compositor-owned swapchain makes the
            // app-side readback unreliable); fall back to the local readback if absent.
            if (xr_.HasAtlasCapture()) {
                std::string prefix = dumpPath_;
                if (prefix.size() > 4 && prefix.compare(prefix.size() - 4, 4, ".png") == 0)
                    prefix.resize(prefix.size() - 4);
                xr_.CaptureAtlas(prefix);
            } else {
                renderer_.DumpImage(frame.imageIndex, dumpPath_);
            }
            dumped_ = true;
        }
    }

    // Window-space HUD overlay: the ImGui UI (top bar + transport + toast + slideshow
    // dip) covering the whole window, or the CPU text HUD if ImGui is unavailable. The
    // ImGui path renders whenever anything is visible (fade / toast / transition); the
    // text fallback follows the SHIFT+TAB master toggle.
    XrSession::HudSubmit hud;
    const bool imguiReady = imgui_.Ready();
    // While the master toggle is on, keep submitting the (full-window, mostly-transparent)
    // HUD layer every frame so auto-hide is driven purely by alpha — no layer on/off that
    // could flicker or be held stale by the compositor. Stop only when master-off and
    // nothing transient (toast / slideshow dip) needs to show.
    const bool wantHud =
        frame.shouldRender && xr_.HasHud() &&
        (imguiReady ? (showHud_ || fadeAlpha_ > 0.001f || toastAlpha_ > 0.001f ||
                       transitionAlpha_ > 0.001f)
                    : showHud_);
    if (wantHud) {
        uint32_t cw = 0, ch = 0;
        window_.PixelSize(cw, ch);
        const float hudAR = (float)xr_.HudWidth() / (float)xr_.HudHeight();
        const float winAR = (ch > 0) ? (float)cw / (float)ch : 1.0f;

        uint32_t hudIdx = 0;
        if (xr_.AcquireHudImage(hudIdx)) {
            if (imguiReady) {
                // Full-window layer: ImGui positions the bars within the canvas; the
                // runtime composites it 1:1 over the content at screen depth.
                hud.enabled = true;
                hud.x = 0.0f;
                hud.y = 0.0f;
                hud.width = 1.0f;
                hud.height = 1.0f;
                hud.disparity = kHudDisparity;

                uint32_t pw = 0, phh = 0;
                window_.PointSize(pw, phh);
                imgui_.BeginFrame((float)pw, (float)phh, hud.x, hud.y, hud.width, hud.height);
                BuildTransportUI();
                imgui_.RenderToHud(hudIdx);
                if (dumpHudPath_ && !dumpedHud_ && rendered_ >= 100) {
                    renderer_.DumpExternalImage(xr_.HudImages()[hudIdx], xr_.HudWidth(),
                                                xr_.HudHeight(), dumpHudPath_);
                    dumpedHud_ = true;
                }
            } else {
                // CPU text-HUD fallback: top-left stats panel (pre-M4 behavior).
                const uint32_t tileW = rects[0].w, tileH = rects[0].h;
                char stereo[64];
                std::snprintf(stereo, sizeof(stereo), "conv %+.3f  eyes %s%s", convergence_,
                              swapEyes_ ? "swapped" : "normal",
                              (isVideo_ && video_.Paused()) ? "  [PAUSED]" : "");
                char text[320];
                if (isVideo_) {
                    std::snprintf(text, sizeof(text),
                                  "%.0f FPS   %s\nsrc %dx%d  %s  %s/%s\nwin %ux%u  tile %ux%u\n%s",
                                  fps_, xr_.ActiveModeName(), mediaW_, mediaH_,
                                  MediaSource::LayoutName(layout_), video_.CodecName(),
                                  video_.BackendName(), cw, ch, tileW, tileH, stereo);
                } else if (hasMedia_) {
                    std::snprintf(text, sizeof(text),
                                  "%.0f FPS   %s\nsrc %dx%d  %s\nwin %ux%u  tile %ux%u\n%s", fps_,
                                  xr_.ActiveModeName(), mediaW_, mediaH_,
                                  MediaSource::LayoutName(layout_), cw, ch, tileW, tileH, stereo);
                } else {
                    std::snprintf(text, sizeof(text),
                                  "%.0f FPS   %s\nsrc %s\nwin %ux%u  tile %ux%u\n%s",
                                  fps_, xr_.ActiveModeName(),
                                  isLogo_ ? "DisplayXR idle logo" : "RED|BLUE test",
                                  cw, ch, tileW, tileH, stereo);
                }
                hud::RenderText(hudPixels_, (int)xr_.HudWidth(), (int)xr_.HudHeight(), text);
                renderer_.UploadToSwapchainImage(xr_.HudImages()[hudIdx], hudPixels_.data(),
                                                 xr_.HudWidth(), xr_.HudHeight());
                hud.enabled = true;
                hud.x = 0.012f;
                hud.y = 0.018f;
                hud.height = 0.125f;
                hud.width = hud.height * hudAR / winAR;
                hud.disparity = 0.0f;
            }
            xr_.ReleaseHudImage();
        }
    }

    xr_.EndFrame(frame, rects, &hud);
    ++frames_;
    UpdateFps();
}

#if defined(MEDIAPLAYER_WITH_IMGUI)
namespace {
constexpr float kPi = 3.14159265358979f;  // IM_PI lives in imgui_internal.h; keep our own
enum class Icon { Play, Pause, Loop, Slideshow, Speaker, SpeakerMuted };

// A borderless icon button: an invisible hit-target with a hand-drawn glyph centered
// in it (no icon font needed). `active` tints it with the accent color (toggle-on);
// hover brightens. Returns true on click. Glyph colors go through GetColorU32 so the
// surrounding ImGui Alpha (the auto-hide fade) applies for free.
bool IconButton(const char* id, Icon kind, float size, bool active = false) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();   // mouse held down — immediate press feedback
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Toggle state (accent) must read THROUGH hover — hover/press only brighten the base,
    // they don't replace the accent, so an active toggle stays clearly cyan when hovered.
    ImVec4 col = active ? ImVec4(0.20f, 0.65f, 1.00f, 1.0f)       // toggled on  -> accent
                        : ImVec4(0.88f, 0.91f, 0.95f, 1.0f);      // off         -> light grey
    if (held) col = active ? ImVec4(0.55f, 0.85f, 1.00f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    else if (hovered) col = active ? ImVec4(0.42f, 0.78f, 1.00f, 1.0f)
                                   : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    const ImU32 u = ImGui::GetColorU32(col);
    const float r = size * 0.30f;
    const float th = size * 0.11f;

    switch (kind) {
        case Icon::Play: {
            const float h = size * 0.30f;
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.7f, c.y - h),
                                  ImVec2(c.x - h * 0.7f, c.y + h),
                                  ImVec2(c.x + h, c.y), u);
            break;
        }
        case Icon::Pause: {
            const float bw = size * 0.13f, bh = size * 0.30f, gap = size * 0.10f;
            dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh), ImVec2(c.x - gap, c.y + bh), u,
                              th * 0.4f);
            dl->AddRectFilled(ImVec2(c.x + gap, c.y - bh), ImVec2(c.x + gap + bw, c.y + bh), u,
                              th * 0.4f);
            break;
        }
        case Icon::Loop: {
            // Two arcs forming a near-circle, with a small arrowhead at each open end.
            dl->PathArcTo(c, r, kPi * 0.30f, kPi * 0.95f);
            dl->PathStroke(u, 0, th);
            dl->PathArcTo(c, r, kPi * 1.30f, kPi * 1.95f);
            dl->PathStroke(u, 0, th);
            const float ah = size * 0.13f;
            const ImVec2 e1(c.x + r * std::cos(kPi * 0.95f), c.y + r * std::sin(kPi * 0.95f));
            dl->AddTriangleFilled(ImVec2(e1.x - ah, e1.y), ImVec2(e1.x + ah * 0.4f, e1.y - ah),
                                  ImVec2(e1.x + ah * 0.4f, e1.y + ah), u);
            const ImVec2 e2(c.x + r * std::cos(kPi * 1.95f), c.y + r * std::sin(kPi * 1.95f));
            dl->AddTriangleFilled(ImVec2(e2.x + ah, e2.y), ImVec2(e2.x - ah * 0.4f, e2.y - ah),
                                  ImVec2(e2.x - ah * 0.4f, e2.y + ah), u);
            break;
        }
        case Icon::Slideshow: {
            // A photo frame with a small play triangle inside (auto-advancing stills).
            dl->AddRect(ImVec2(c.x - r, c.y - r * 0.78f), ImVec2(c.x + r, c.y + r * 0.78f), u,
                        size * 0.10f, 0, th * 0.8f);
            const float h = size * 0.16f;
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.5f, c.y - h), ImVec2(c.x - h * 0.5f, c.y + h),
                                  ImVec2(c.x + h, c.y), u);
            break;
        }
        case Icon::Speaker:
        case Icon::SpeakerMuted: {
            // Body box + cone (triangle pointing right).
            const float bx = -r * 0.35f;
            dl->AddRectFilled(ImVec2(c.x - r * 0.95f, c.y - r * 0.30f),
                              ImVec2(c.x + bx, c.y + r * 0.30f), u);
            dl->AddTriangleFilled(ImVec2(c.x + bx, c.y - r * 0.62f),
                                  ImVec2(c.x + bx, c.y + r * 0.62f), ImVec2(c.x + r * 0.25f, c.y), u);
            if (kind == Icon::Speaker) {  // two sound-wave arcs
                dl->PathArcTo(ImVec2(c.x + r * 0.1f, c.y), r * 0.58f, -kPi * 0.28f, kPi * 0.28f);
                dl->PathStroke(u, 0, th * 0.7f);
                dl->PathArcTo(ImVec2(c.x + r * 0.1f, c.y), r * 0.92f, -kPi * 0.28f, kPi * 0.28f);
                dl->PathStroke(u, 0, th * 0.7f);
            } else {                      // muted: a small X
                const float xc = c.x + r * 0.72f, d = r * 0.26f;
                dl->AddLine(ImVec2(xc - d, c.y - d), ImVec2(xc + d, c.y + d), u, th * 0.9f);
                dl->AddLine(ImVec2(xc - d, c.y + d), ImVec2(xc + d, c.y - d), u, th * 0.9f);
            }
            break;
        }
    }
    return ImGui::IsItemClicked();
}
}  // namespace
#endif

void App::BuildTransportUI() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const ImGuiWindowFlags pill = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_AlwaysAutoResize;

    // The bars fade in/out together with the idle timer.
    if (fadeAlpha_ > 0.001f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fadeAlpha_);
        const float iconSz = ImGui::GetFrameHeight();

        // --- Top window-space bar: full width, flush to the top of the window.
        //     Open / Mode on the left, Slideshow / Close on the right. ---
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(W, 0.0f), ImGuiCond_Always);
        const ImGuiWindowFlags topbar = pill & ~ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);  // flat bar, flush to edges
        if (ImGui::Begin("##wsui_top", nullptr, topbar)) {
            ImGui::BeginDisabled(openFilePending_);
            if (ImGui::Button("Open")) RequestOpenFile();
            ImGui::EndDisabled();
            ImGui::SameLine();
            char modeLabel[96];
            std::snprintf(modeLabel, sizeof(modeLabel), "Mode: %s", xr_.ActiveModeName());
            if (ImGui::Button(modeLabel)) xr_.RequestNextMode();
            // Current filename, centered in the bar.
            if (!currentMediaPath_.empty()) {
                const std::string name =
                    std::filesystem::path(currentMediaPath_).filename().string();
                const float tw = ImGui::CalcTextSize(name.c_str()).x;
                ImGui::SameLine();
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - tw) * 0.5f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(name.c_str());
            }
            // Right-align the slideshow toggle. (Close the app via ESC or the window's
            // own close button — no in-UI X needed.)
            ImGui::SameLine(ImGui::GetWindowWidth() - iconSz -
                            ImGui::GetStyle().WindowPadding.x);
            if (IconButton("##slideshow", Icon::Slideshow, iconSz, slideshowActive_))
                ToggleSlideshow();
        }
        ImGui::End();
        ImGui::PopStyleVar();

        // --- Bottom transport: play/pause + scrubber + loop (video only) ---
        if (isVideo_ && video_.DurationSeconds() > 0.0) {
            ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.955f), ImGuiCond_Always,
                                    ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(W * 0.8f, 0.0f), ImGuiCond_Always);
            const ImGuiWindowFlags bar = pill & ~ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::Begin("##transport", nullptr, bar)) {
                const float dur = (float)video_.DurationSeconds();
                const float pos = (float)video_.PositionSeconds();
                // Track playback into the knob, but not while dragging, and not while a
                // seek we issued is still landing — else it snaps back on release.
                if (!scrubActive_) {
                    if (scrubTarget_ >= 0.0f) {
                        if (std::fabs(pos - scrubTarget_) < 0.3f) scrubTarget_ = -1.0f;
                    } else {
                        scrubValue_ = pos;
                    }
                }
                auto mmss = [](double s, char* out, size_t n) {
                    if (s < 0.0) s = 0.0;
                    const int t = (int)s;
                    std::snprintf(out, n, "%d:%02d", t / 60, t % 60);
                };
                char cur[16], tot[16];
                mmss(scrubValue_, cur, sizeof(cur));
                mmss(dur, tot, sizeof(tot));

                const bool paused = video_.Paused() || video_.Ended();
                if (IconButton("##playpause", paused ? Icon::Play : Icon::Pause, iconSz))
                    TogglePlayback();
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", cur);
                ImGui::SameLine();
                // Leave room on the right for the total-time label + mute + loop icons.
                const float rightW = ImGui::CalcTextSize(tot).x + 2.0f * iconSz + 36.0f;
                ImGui::SetNextItemWidth(-rightW);
                const bool changed = ImGui::SliderFloat("##scrub", &scrubValue_, 0.0f, dur, "");
                const bool active = ImGui::IsItemActive();
                const float scrubDelta = std::fabs(scrubValue_ - lastScrubValue_);
                if (changed) {
                    // Fast sweep -> keyframe preview (responsive); slow/fine drag -> exact
                    // frame (precise). Audio goes silent while scrubbing.
                    const bool fast = active && scrubDelta > kFastScrubSeconds;
                    video_.Seek(scrubValue_, /*preview=*/fast);
                    scrubWasPreview_ = fast;
                    scrubTarget_ = scrubValue_;  // hold the knob until the decode lands
                    if (active) audio_.SetPaused(true);
                } else if (active && scrubWasPreview_) {
                    // Settled after a fast sweep -> resolve to the exact frame.
                    video_.Seek(scrubValue_, /*preview=*/false);
                    scrubWasPreview_ = false;
                }
                if (scrubActive_ && !active) {
                    // Released: settle on the exact frame and realign + resume audio.
                    video_.Seek(scrubValue_, /*preview=*/false);
                    audio_.Seek(scrubValue_);
                    if (!video_.Paused()) audio_.SetPaused(false);
                    scrubWasPreview_ = false;
                }
                scrubActive_ = active;
                lastScrubValue_ = scrubValue_;
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", tot);
                ImGui::SameLine();
                if (IconButton("##mute", muted_ ? Icon::SpeakerMuted : Icon::Speaker, iconSz, muted_))
                    ToggleMute();
                ImGui::SameLine();
                if (IconButton("##loop", Icon::Loop, iconSz, video_.Loop())) {
                    video_.ToggleLoop();
                    audio_.SetLoop(video_.Loop());
                }
            }
            ImGui::End();
        }
        ImGui::PopStyleVar();
    }

    // --- Toast (convergence readout, nav filename): own alpha, shows even when hidden ---
    if (toastAlpha_ > 0.001f && !toastText_.empty()) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, toastAlpha_);
        ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.14f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("##toast", nullptr, pill)) {
            ImGui::TextUnformatted(toastText_.c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // --- Slideshow dip-to-black: a full-canvas overlay above everything. Because the
    //     HUD layer composites on top of the content layer, this dims the whole view. ---
    if (transitionAlpha_ > 0.001f) {
        const int a = (int)(std::min(1.0f, transitionAlpha_) * 255.0f + 0.5f);
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H),
                                                      IM_COL32(0, 0, 0, a));
    }
#endif
}

void App::UpdateFps() {
    using namespace std::chrono;
    ++fpsWindowFrames_;
    const auto now = steady_clock::now();
    const double elapsed = duration<double>(now - fpsWindowStart_).count();
    if (elapsed >= 0.5) {
        fps_ = (float)(fpsWindowFrames_ / elapsed);
        fpsWindowFrames_ = 0;
        fpsWindowStart_ = now;

        char title[160];
        std::snprintf(title, sizeof(title),
                      "DisplayXR Stereo Media Player  —  %.0f fps  —  %s", fps_,
                      xr_.ActiveModeName());
        window_.SetTitle(title);
        LOG_DEBUG("fps=%.1f mode=%s", fps_, xr_.ActiveModeName());
    }
}

void App::Shutdown() {
    video_.Stop();        // join the decode thread (reads the audio clock) before audio,
    audio_.Stop();        // and before tearing down the GPU
    imgui_.Shutdown();    // before xr_ destroys the Vulkan device ImGui borrows
    renderer_.Shutdown();
    xr_.Shutdown();
    window_.Destroy();
}

// --- Open-file ------------------------------------------------------------------

bool App::LoadMedia(const std::string& path) {
    const MediaInfo info = MediaSource::Identify(path);
    if (info.kind == MediaKind::Video) {
        if (!video_.Open(path)) {
            LOG_ERROR("Open: cannot decode video '%s'", path.c_str());
            return false;
        }
        isVideo_ = true;
        hasMedia_ = true;
        ClearIdleLogo();
        layout_ = info.layout;
        mediaConvergence_ = 0.0f;  // no baked convergence for video
        mediaAutoConvAvailable_ = false;
        mediaW_ = video_.Width();
        mediaH_ = video_.Height();
        contentAspect_ = PerEyeAspect(info.layout, video_.Width(), video_.Height());
        // In slideshow, force play-once so the clip can end and advance, regardless of
        // the user's manual loop preference.
        if (slideshowActive_) video_.SetLoop(false);
        // Audio (optional — silent if the clip has none). Match mute + loop state.
        audio_.Open(path);
        audio_.SetMuted(muted_);
        audio_.SetLoop(video_.Loop());
        LOG_INFO("Playing %s video (%s), per-eye aspect %.3f",
                 MediaSource::LayoutName(info.layout), path.c_str(), contentAspect_);
        currentMediaPath_ = path;
        RebuildFolderList(path);
        return true;
    }
    // LIF container (JPEG + appended views): compose stereo to SBS; a non-stereo or
    // malformed LIF falls back to flat 2D inside the loader. Detect by content — real
    // LIFs commonly ship as .jpg, so we sniff the trailer magic rather than the name
    // (a plain SBS .jpg has no trailer and keeps its filename-based layout below).
    auto hasLifExt = [](const std::string& p) {
        const size_t n = p.size();
        if (n < 4) return false;
        auto lc = [](char c) { return (char)(c | 0x20); };  // ASCII lower
        return p[n - 4] == '.' && lc(p[n - 3]) == 'l' && lc(p[n - 2]) == 'i' &&
               lc(p[n - 1]) == 'f';
    };
    if (hasLifExt(path) || LifLoader::IsLif(path)) {
        LifResult lif = LifLoader::Load(path);
        if (!lif.ok) {
            LOG_ERROR("Open: cannot load LIF '%s'", path.c_str());
            return false;
        }
        if (!renderer_.UploadTexture(lif.image.pixels.data(), (uint32_t)lif.image.width,
                                     (uint32_t)lif.image.height)) {
            return false;
        }
        isVideo_ = false;
        hasMedia_ = true;
        ClearIdleLogo();
        layout_ = lif.layout;
        mediaConvergence_ = lif.convergence;  // baked reconvergence from the LIF
        mediaAutoConvAvailable_ = lif.stereo && !lif.hasConvergence;  // estimate available
        mediaAutoConvergence_ = lif.autoConvergence;
        mediaW_ = lif.image.width;
        mediaH_ = lif.image.height;
        contentAspect_ = PerEyeAspect(lif.layout, lif.image.width, lif.image.height);
        LOG_INFO("Displaying %s LIF (%s), per-eye aspect %.3f, baked convergence %+.4f",
                 lif.stereo ? "stereo" : "mono", path.c_str(), contentAspect_, mediaConvergence_);
        currentMediaPath_ = path;
        RebuildFolderList(path);
        return true;
    }
    DecodedImage img = ImageDecoder::Load(path);
    if (!img.Valid()) {
        LOG_ERROR("Open: cannot load image '%s'", path.c_str());
        return false;
    }
    const MediaInfo imgInfo = MediaSource::Identify(path, img.width, img.height);
    if (!renderer_.UploadTexture(img.pixels.data(), (uint32_t)img.width, (uint32_t)img.height)) {
        return false;
    }
    isVideo_ = false;
    hasMedia_ = true;
    ClearIdleLogo();
    layout_ = imgInfo.layout;
    mediaConvergence_ = 0.0f;  // plain SBS image carries no baked convergence
    mediaAutoConvAvailable_ = false;
    mediaW_ = img.width;
    mediaH_ = img.height;
    contentAspect_ = PerEyeAspect(imgInfo.layout, img.width, img.height);
    LOG_INFO("Displaying %s image (%s), per-eye aspect %.3f",
             MediaSource::LayoutName(imgInfo.layout), path.c_str(), contentAspect_);
    currentMediaPath_ = path;
    RebuildFolderList(path);
    return true;
}

void App::ReloadMedia(const std::string& path) {
    video_.Stop();   // joins the decode thread (which reads the audio clock) FIRST,
    audio_.Stop();   // then tear down audio so the clock callback can't outlive it.
    isVideo_ = false;
    scrubValue_ = 0.0f;
    scrubActive_ = false;
    scrubTarget_ = -1.0f;
    slideshowImageElapsed_ = 0.0;
    if (!LoadMedia(path)) {
        LOG_WARN("Open: keeping previous view (failed to open '%s')", path.c_str());
    }
}

void App::RebuildFolderList(std::string path) {
    namespace fs = std::filesystem;
    folderFiles_.clear();   // frees any string `path` might have aliased — it's a copy now
    folderIndex_ = 0;
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(path), ec);
    if (ec) p = fs::path(path);
    for (fs::directory_iterator it(p.parent_path(), ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const std::string s = it->path().string();
        if (MediaSource::IsSupported(s)) folderFiles_.push_back(s);
    }
    std::sort(folderFiles_.begin(), folderFiles_.end());
    for (size_t i = 0; i < folderFiles_.size(); ++i) {
        std::error_code e2;
        if (fs::equivalent(fs::path(folderFiles_[i]), p, e2) && !e2) {
            folderIndex_ = i;
            break;
        }
    }
    LOG_INFO("Folder: %zu asset(s), current index %zu", folderFiles_.size(), folderIndex_);
}

void App::NavigateMedia(int delta) {
    const size_t n = folderFiles_.size();
    if (n < 2) return;  // nothing to step to (LoadMedia rebuilt folderIndex_)
    const int wrapped = ((delta % (int)n) + (int)n) % (int)n;
    folderIndex_ = (folderIndex_ + (size_t)wrapped) % n;
    ReloadMedia(folderFiles_[folderIndex_]);  // sets currentMediaPath_ + rebuilds list
    // (filename is shown persistently in the top bar — no toast needed here.)
}

void App::RequestNavTransition(int delta) {
    if (folderFiles_.size() < 2) return;
    if (transition_ != Transition::Playing) return;  // already mid-transition — ignore
    pendingNavDelta_ = delta;
    transition_ = Transition::FadeOut;
}

void App::ShowToast(const std::string& msg) {
    toastText_ = msg;
    toastExpiry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
}

void App::ToggleSlideshow() {
    slideshowActive_ = !slideshowActive_;
    slideshowImageElapsed_ = 0.0;
    transition_ = Transition::Playing;
    if (slideshowActive_ && isVideo_) video_.SetLoop(false);  // play once, then advance
    LOG_INFO("slideshow %s", slideshowActive_ ? "on" : "off");
    ShowToast(slideshowActive_ ? "Slideshow on" : "Slideshow off");
}

void App::LoadIdleLogo() {
    // Resolve <exe-dir>/displayxr/ — the sidecar staged next to the binary (same folder
    // as the workspace manifest). SDL_GetBasePath gives the executable's directory; the
    // returned string is owned by SDL (do not free). Prefer the composed idle lockup
    // (DisplayXR mark + bold "Media Player" label, pre-padded as a square) and fall back
    // to the bare brand mark, then to the RED|BLUE test pattern if neither is present.
    namespace fs = std::filesystem;
    std::string base;
    if (const char* b = SDL_GetBasePath()) base = b;
    const fs::path dir = fs::path(base) / "displayxr";

    // {file, occupy}: idle.png carries its own margin (fills the square ~1:1); the bare
    // logo.png is the lone mark, centered at half scale so it doesn't dominate the view.
    const struct { const char* file; float occupy; } cands[] = {
        {"idle.png", 1.0f}, {"logo.png", 0.5f}};
    for (const auto& c : cands) {
        const std::string path = (dir / c.file).string();
        DecodedImage art = ImageDecoder::Load(path);
        if (!art.Valid()) continue;
        if (CompositeIdleArt(art, c.occupy)) {
            LOG_INFO("No media — showing the DisplayXR idle screen ('%s')", c.file);
            return;
        }
        LOG_WARN("Idle art upload failed for '%s'", c.file);
    }
    LOG_INFO("No media, and no idle art in '%s' — showing RED|BLUE L/R test pattern",
             dir.string().c_str());
}

bool App::CompositeIdleArt(const DecodedImage& art, float occupy) {
    // Alpha-composite the (transparent-background) art centered onto an opaque dark-grey
    // square sized so the art fills `occupy` of it. DrawViews fills the surrounding view
    // with the same grey, so the lockup floats on a seamless backdrop with no black
    // letterbox bars. Mono (one image fed to both eyes) so it sits flat at screen depth.
    const int C = (int)std::lround(std::max(art.width, art.height) / occupy);
    const uint8_t bg[3] = {(uint8_t)(kIdleBgR * 255.0f + 0.5f),
                           (uint8_t)(kIdleBgG * 255.0f + 0.5f),
                           (uint8_t)(kIdleBgB * 255.0f + 0.5f)};
    std::vector<uint8_t> canvas((size_t)C * C * 4);
    for (size_t i = 0; i < canvas.size(); i += 4) {
        canvas[i] = bg[0]; canvas[i + 1] = bg[1]; canvas[i + 2] = bg[2]; canvas[i + 3] = 255;
    }
    const int ox = (C - art.width) / 2, oy = (C - art.height) / 2;
    for (int y = 0; y < art.height; ++y) {
        for (int x = 0; x < art.width; ++x) {
            const uint8_t* s = &art.pixels[((size_t)y * art.width + x) * 4];
            const float a = s[3] / 255.0f;  // alpha-over the grey backdrop
            uint8_t* d = &canvas[((size_t)(oy + y) * C + (ox + x)) * 4];
            for (int c = 0; c < 3; ++c) d[c] = (uint8_t)(s[c] * a + d[c] * (1.0f - a) + 0.5f);
        }
    }
    if (!renderer_.UploadTexture(canvas.data(), (uint32_t)C, (uint32_t)C)) return false;
    renderer_.SetBackground(kIdleBgR, kIdleBgG, kIdleBgB);
    isLogo_ = true;
    layout_ = StereoLayout::Mono;
    contentAspect_ = 1.0f;
    return true;
}

void App::ClearIdleLogo() {
    if (!isLogo_) return;
    isLogo_ = false;
    renderer_.SetBackground(0.0f, 0.0f, 0.0f);  // back to the black media letterbox
}

void App::TogglePlayback() {
    if (!isVideo_) return;
    if (video_.Ended()) {
        video_.Seek(0.0);          // restart a clip that already ran to the end
        video_.SetPaused(false);
        audio_.Seek(0.0);
        audio_.SetPaused(false);
    } else {
        video_.TogglePaused();
        audio_.SetPaused(video_.Paused());
    }
    LOG_INFO("playback %s", video_.Paused() ? "paused" : "playing");
}

void App::ToggleMute() {
    muted_ = !muted_;
    audio_.SetMuted(muted_);
    LOG_INFO("audio %s", muted_ ? "muted" : "unmuted");
    ShowToast(muted_ ? "Muted" : "Unmuted");
}

void App::StepFrame(int n) {
    if (!isVideo_ || n == 0) return;
    const double fr = video_.FrameRate();
    const double fd = (fr > 1.0) ? 1.0 / fr : 1.0 / 30.0;  // frame duration (fallback 30fps)
    // Frame stepping is a paused operation; freeze both streams.
    if (!video_.Paused()) { video_.SetPaused(true); audio_.SetPaused(true); }
    // Seek so the target frame is the first one at/after sk: sk = pos + (n - 0.5)*fd.
    double t = video_.PositionSeconds() + ((double)n - 0.5) * fd;
    if (t < 0.0) t = 0.0;
    video_.Seek(t);             // exact
    audio_.Seek(t);             // keep audio aligned for the eventual resume
    scrubTarget_ = (float)t;    // hold the scrubber knob until the frame lands
}

void App::TickUi() {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    double dt = duration<double>(now - lastFrameTime_).count();
    lastFrameTime_ = now;
    if (dt < 0.0 || dt > 0.25) dt = 0.0;  // ignore the first frame and big hitches

    // Real mouse movement (only while the cursor is in the window): compare against the
    // resting position so a jittery sensor near rest doesn't count. A move > a few px
    // wakes the UI and re-bases the resting point.
    if (window_.MouseInWindow()) {
        float mx = 0.0f, my = 0.0f;
        SDL_GetMouseState(&mx, &my);
        if (restMouseX_ < 0.0f ||
            std::fabs(mx - restMouseX_) + std::fabs(my - restMouseY_) > 3.0f) {
            restMouseX_ = mx;
            restMouseY_ = my;
            lastActivity_ = now;
        }
    }
    // Dragging the scrubber counts as activity even with the cursor held still.
    if (scrubActive_) lastActivity_ = now;
    // Keep the UI pinned visible until a pending HUD dump lands (diagnostic/screenshot).
    if (dumpHudPath_ && !dumpedHud_) lastActivity_ = now;

    // Auto-hide fade: visible while the master toggle is on and we're not idle.
    const double idle = duration<double>(now - lastActivity_).count();
    const bool wantVisible = showHud_ && idle < kIdleHideSeconds;
    const float fadeTarget = wantVisible ? 1.0f : 0.0f;
    const float fadeStep = (float)(dt / kFadeSeconds);
    if (fadeAlpha_ < fadeTarget) fadeAlpha_ = std::min(fadeTarget, fadeAlpha_ + fadeStep);
    else if (fadeAlpha_ > fadeTarget) fadeAlpha_ = std::max(fadeTarget, fadeAlpha_ - fadeStep);

    // Toast fade: ease toward 1 while live, toward 0 once expired; clear when gone.
    const bool toastLive = !toastText_.empty() && now < toastExpiry_;
    const float toastTarget = toastLive ? 1.0f : 0.0f;
    const float toastStep = (float)(dt / kToastFadeSeconds);
    if (toastAlpha_ < toastTarget) toastAlpha_ = std::min(toastTarget, toastAlpha_ + toastStep);
    else if (toastAlpha_ > toastTarget) toastAlpha_ = std::max(toastTarget, toastAlpha_ - toastStep);
    if (!toastLive && toastAlpha_ <= 0.001f) toastText_.clear();

    // Dip-to-black transition machine, shared by the slideshow and manual ←/→ nav.
    // Slideshow auto-starts a transition when a still has shown long enough / a video
    // ended; manual nav starts one via RequestNavTransition(). The swap happens at full
    // black. pendingNavDelta_ carries the step (+1 next, -1 prev).
    const float transStep = (float)(dt / kTransitionSeconds);
    if (slideshowActive_ && transition_ == Transition::Playing && pendingNavDelta_ == 0) {
        bool advance = false;
        if (isVideo_) {
            advance = video_.Ended();
        } else if (hasMedia_) {
            slideshowImageElapsed_ += dt;
            advance = slideshowImageElapsed_ >= kStillSeconds;
        }
        if (advance && folderFiles_.size() > 1) {
            pendingNavDelta_ = +1;
            transition_ = Transition::FadeOut;
        }
    }
    switch (transition_) {
        case Transition::Playing:
            break;
        case Transition::FadeOut:
            transitionAlpha_ = std::min(1.0f, transitionAlpha_ + transStep);
            if (transitionAlpha_ >= 1.0f) {
                if (pendingNavDelta_ != 0) {
                    NavigateMedia(pendingNavDelta_);  // swap while fully black
                    pendingNavDelta_ = 0;
                }
                transition_ = Transition::FadeIn;
            }
            break;
        case Transition::FadeIn:
            transitionAlpha_ = std::max(0.0f, transitionAlpha_ - transStep);
            if (transitionAlpha_ <= 0.0f) transition_ = Transition::Playing;
            break;
    }
}

void App::RequestOpenFile() {
    if (openFilePending_) return;
    openFilePending_ = true;

    // Prefer the workspace spatial picker (XR_EXT_workspace_file_dialog). In a
    // workspace it spawns displayxr-file-picker.exe and the result arrives async via
    // xr_.TakePickedFile(); anywhere else we fall back to a native OS dialog.
    const XrSession::PickerStatus st = xr_.RequestFilePicker();
    if (st == XrSession::PickerStatus::Pending) {
        LOG_INFO("Open: workspace file picker requested");
        return;
    }
    LOG_INFO("Open: workspace picker unavailable (status=%d) — native dialog", (int)st);

    static const SDL_DialogFileFilter kFilters[] = {
        {"Stereo media", "mp4;mkv;mov;jpg;jpeg;png;lif"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog((SDL_DialogFileCallback)&App::NativeFileCallback, this,
                           window_.SdlWindow(), kFilters, 2, nullptr, false);
}

void App::NativeFileCallback(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* self = static_cast<App*>(userdata);
    std::string picked;
    // filelist == NULL → error; filelist[0] == NULL → user cancelled; else first path.
    if (filelist && filelist[0]) picked = filelist[0];
    std::lock_guard<std::mutex> lk(self->nativePathMutex_);
    self->nativePath_ = picked;   // empty = cancel/error → main loop just clears pending
    self->hasNativePath_ = true;
}

} // namespace mp
