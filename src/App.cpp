// SPDX-License-Identifier: Apache-2.0
#include "App.h"
#include "media/ImageSequence.h"

#include "Log.h"
#include "media/ImageDecoder.h"
#include "media/LifLoader.h"
#include "media/MediaSource.h"
#include "media/MpoLoader.h"
#include "media/StereoDetect.h"
#include "media/StreamUrl.h"
#include "media/VideoStereoProbe.h"
#include "ui/Hud.h"

// The product name in the window title. Media Player Pro is the same trunk built with
// MEDIAPLAYER_PRO=1 (docs/PRO.md).
#if MEDIAPLAYER_PRO
#define MP_PRODUCT_NAME "DisplayXR Media Player Pro"
#else
#define MP_PRODUCT_NAME "DisplayXR Stereo Media Player"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include <nlohmann/json.hpp>  // parse agent-tool args / build results (XR_DXR_mcp_tools)
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
// working directory. xrCaptureAtlasDXR appends "_atlas.png" to the prefix we return;
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
constexpr double kStillSeconds = 5.0;       // slideshow: seconds to hold a still image
constexpr double kTransitionSeconds = 0.40; // slideshow: dip-to-black half-duration
// Scrub speed (video-seconds moved per UI frame) above which we show keyframes instead
// of exact frames — a fast sweep; below it, fine adjustment gets exact frames.
constexpr float kFastScrubSeconds = 1.0f;

// "Convert to 3D": the layout label while a converted result is on screen. ASCII arrow:
// the default ImGui font (and the CPU text HUD) have no U+2192.
constexpr const char* kConvertedLayoutName = "Mono -> 3D (converted)";

// Monotonic seconds for the converter's cadence (stale in-flight frames, re-polls).
double SteadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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
    // Default 1280x720; MEDIAPLAYER_WINDOW="WxH" or the uniform "WxH+X+Y" form
    // overrides (X,Y absolute virtual-desktop px — pins the window there and skips
    // the panel auto-placement; "WxH" alone still auto-places, e.g. "720x1280" to
    // test portrait aspect handling without a rebuild).
    int winW = 1280, winH = 720;
    int forceX = 0, forceY = 0;
    bool forcePos = false;
    if (const char* s = std::getenv("MEDIAPLAYER_WINDOW")) {
        int a = 0, b = 0, x = 0, y = 0;
        int n = std::sscanf(s, "%dx%d+%d+%d", &a, &b, &x, &y);
        if (n >= 2 && a > 0 && b > 0) { winW = a; winH = b; }
        if (n >= 4) { forceX = x; forceY = y; forcePos = true; }
    }
    if (!window_.Create(MP_PRODUCT_NAME, winW, winH)) return false;
#if MEDIAPLAYER_PRO
    // Pro is a monitoring tool: Escape leaves full screen and never quits a live session.
    window_.SetEscapeQuits(false);
#endif

    // Open the window ON the 3D panel (INV-1.3; runtime#715): on multi-monitor
    // boxes SDL centers new windows on the primary display, but the window-relative
    // weave is only correct on the 3D panel itself. The runtime reports the panel's
    // top-left via XrDisplayDesktopPositionDXR (display_info v16); XrSession fires
    // this one-shot placement after the properties query and before xrCreateSession,
    // so the position is settled when the session binding captures the HWND/XID and
    // the DP's phase tracking starts. SDL global desktop coordinates are top-down
    // virtual-desktop pixels on all platforms — pass (left, top) straight through.
    // (0, 0) = primary/unknown (old runtimes too): keep SDL's default placement.
    // With a known panel pixel size, center the window on the panel (parity with
    // the other demos); otherwise fall back to the panel's top-left corner.
    auto placeOnPanel = [this, winW, winH, forceX, forceY, forcePos](
                            int32_t left, int32_t top, uint32_t panelW, uint32_t panelH) {
        if (forcePos) {
            window_.SetPosition(forceX, forceY);
            LOG_INFO("MEDIAPLAYER_WINDOW override: placed window at absolute (%d, %d)",
                     forceX, forceY);
        } else if (left != 0 || top != 0) {
            int x = left, y = top;
            if (panelW > 0 && panelH > 0) {
                x = left + ((int)panelW - winW) / 2;
                y = top + ((int)panelH - winH) / 2;
            }
            window_.SetPosition(x, y);
            LOG_INFO("Placed window on 3D panel at (%d, %d)%s", x, y,
                     (panelW > 0) ? " (centered)" : "");
        }
        // Desktop Linux: the window is created HERE, now that the panel is known
        // (displayxr-common's window helper: after the system properties, before
        // xrCreateSession). A no-op elsewhere, where Create() already made it.
        window_.RealizeOnPanel(left, top, panelW, panelH);
    };
#if MEDIAPLAYER_PRO
    xr_.RequestLocal2D(true);  // pinned Pro panels show flat (HudSubmit::flat)
#endif
    if (!xr_.Initialize(window_.NativeHandle(), placeOnPanel)) {
        LOG_ERROR("OpenXR initialization failed");
        return false;
    }
    // "Convert to 3D": follow the runtime's 2D->3D conversion task. Without XR_DXR_lift the
    // client stays unattached and the toggle is never drawn.
    xr_.SetLiftEventHandler([this](const XrEventDataBaseHeader* e) { lift_.OnEvent(e); });
    if (xr_.HasLift()) lift_.Attach(xr_.LiftApi(), xr_.Handle());
    // Input providers: the Input popup appears only when the runtime offers the extension
    // and lists at least one provider.
    xr_.SetInputEventHandler([this](const XrEventDataBaseHeader* e) { input_.OnEvent(e); });
    if (xr_.HasInputProviders())
        input_.Attach(xr_.InputApi(), xr_.Instance(), xr_.SystemId(), xr_.Handle());

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
    ReadStereoEnv();   // must precede the first LoadMedia below
#if MEDIAPLAYER_PRO
    // Pro review formats (MXF, EXR / DPX / TIFF sequences, 10-bit kept): before any load.
    VideoDecoder::SetProFormats(true);
    MediaSource::SetProFormats(true);
    video_.SetSequenceFps(launch_.proSeqFps);
#endif

    // Live camera (#90). The command line (main.cpp) wins over the env vars. The picker
    // and 'C' key are desktop-only: Android's TransportCaps leave caps.camera false.
    uiState_.caps.camera = true;
    if (!launch_.camera) {
        if (const char* e = std::getenv("MEDIAPLAYER_CAMERA")) {
            launch_.camera = true;          // "auto" / "" = default pick; "0" = index 0
            launch_.cameraSelector = e;
        }
    }
    if (launch_.cameraFps <= 0) {
        if (const char* e = std::getenv("MEDIAPLAYER_CAMERA_FPS")) launch_.cameraFps = std::atoi(e);
    }
    // Auto-convergence (#92): DEFAULT OFF. --auto-conv wins over MEDIAPLAYER_AUTO_CONV,
    // whose value is the policy ("1"/"on" = the default policy, "0"/"off" = off).
    if (!launch_.autoConv) {
        if (const char* e = std::getenv("MEDIAPLAYER_AUTO_CONV")) {
            const std::string v = e;
            if (!v.empty() && v != "0" && v != "off") {
                launch_.autoConv = true;
                if (v != "1" && v != "on") launch_.autoConvPolicy = v;
            }
        }
    }
    if (!launch_.autoConvPolicy.empty()) {
        autoconv::SubjectPolicy p = autoconv::SubjectPolicy::Nearest;
        if (autoconv::ParsePolicy(launch_.autoConvPolicy, p) && p != autoconv::SubjectPolicy::Focus)
            autoConvPolicy_ = p;
        else
            LOG_WARN("auto-conv: unknown policy '%s' (nearest|sharp|centre) - using nearest",
                     launch_.autoConvPolicy.c_str());
    }
    // Comfort-clamp tunables (percent of eye width; CLAMP=1 turns the clamp on — it is
    // off by default, see AutoConvergenceController::Config).
    {
        autoconv::AutoConvergenceController::Config& cc = autoConvCtl_.MutableConfig();
        if (const char* e = std::getenv("MEDIAPLAYER_AUTO_CONV_FRONT")) cc.frontBudget = (float)std::atof(e) / 100.f;
        if (const char* e = std::getenv("MEDIAPLAYER_AUTO_CONV_REAR")) cc.rearBudget = (float)std::atof(e) / 100.f;
        if (const char* e = std::getenv("MEDIAPLAYER_AUTO_CONV_CLAMP")) cc.comfortClamp = (*e && *e != '0');
        LOG_INFO("auto-conv comfort clamp %s (front %.2f%%, rear %.2f%% of eye width)",
                 cc.comfortClamp ? "on" : "off", cc.frontBudget * 100.f, cc.rearBudget * 100.f);
    }
    if (launch_.autoConv) SetAutoConv(true, "launch");
    LOG_INFO("auto-conv: %s at start (policy %s)", autoConvEnabled_ ? "ON" : "OFF",
             autoconv::PolicyName(autoConvPolicy_));
    if (launch_.camera) {
        if (mediaPath && *mediaPath)
            LOG_WARN("--camera given: ignoring the media path '%s'", mediaPath);
        mediaPath = nullptr;
        if (!LoadLive(launch_.cameraSelector)) LOG_WARN("Live camera did not start");
    }

    // Stream URL (#93): --url, a URL given as the media path, or MEDIAPLAYER_URL (which
    // only auto-opens when the command line named nothing else, and always seeds the
    // Stream URL popup). Desktop-only UI: Android's TransportCaps leave caps.url false.
    uiState_.caps.url = true;
    const char* envUrl = std::getenv("MEDIAPLAYER_URL");
    uiState_.urlSeed = !launch_.url.empty() ? launch_.url : (envUrl ? envUrl : "");
    if (!launch_.camera) {
        std::string url = launch_.url;
        if (url.empty() && mediaPath && stream::IsNetworkUrl(mediaPath)) url = mediaPath;
        if (url.empty() && !(mediaPath && *mediaPath) && envUrl && *envUrl) url = envUrl;
        if (!url.empty()) {
            if (mediaPath && *mediaPath && url != mediaPath)
                LOG_WARN("--url given: ignoring the media path '%s'", mediaPath);
            mediaPath = nullptr;
            if (!LoadUrl(url)) LOG_WARN("Stream did not start");
        }
    }

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
        if (imgui_.Init((void*)window_.SdlWindow(), xr_.VkInstanceHandle(), xr_.PhysicalDevice(),
                        xr_.Device(), xr_.GraphicsQueue(), xr_.GraphicsQueueFamily(),
                        (VkFormat)xr_.HudFormat(), xr_.HudWidth(), xr_.HudHeight(),
                        xr_.HudImages(), renderer_.FramesInFlight())) {
            window_.SetEventHook([this](void* ev) { imgui_.ProcessEvent(ev); });
        } else {
            LOG_WARN("ImGui unavailable — using the text HUD");
        }
    }
#if MEDIAPLAYER_PRO
    InitPro();
#endif

    // Expose the player's transport controls to agents (XR_DXR_mcp_tools). Inert when the
    // runtime lacks the extension or the MCP capability gate is off.
    SetupAgentTools();

    // Now that the XR session exists (the runtime has bound/hidden the HWND in workspace
    // mode), reveal the window. Standalone: the real window appears. Workspace: the runtime
    // keeps it hidden and composites via the shell — same as the Gauss/model demos.
    window_.Show();
    return true;
}

int App::Run() {
    startMode_ = []() {
        const char* e = std::getenv("MEDIAPLAYER_START_MODE");
        return e ? std::atoi(e) : -1;
    }();
    dumpPath_ = std::getenv("MEDIAPLAYER_DUMP_ATLAS");
    dumpHudPath_ = std::getenv("MEDIAPLAYER_DUMP_HUD");
    if (const char* f = std::getenv("MEDIAPLAYER_DUMP_FRAME")) dumpFrame_ = std::max(1, std::atoi(f));
    openAfterPath_ = std::getenv("MEDIAPLAYER_OPEN_AFTER");  // test: swap media mid-run
    // Test: switch "Convert to 3D" on without a keystroke (headless runs). Retried until it
    // takes, because an in-process session may only learn the conversion task a moment
    // after start. Never set by the player itself.
    if (const char* c = std::getenv("MEDIAPLAYER_CONVERT")) convertAtStart_ = (*c && *c != '0');
    // Test: with MEDIAPLAYER_CONVERT, first pick this provider id (spec 5 hint).
    if (const char* p = std::getenv("MEDIAPLAYER_CONVERT_PROVIDER")) convertProviderAtStart_ = p;
    // Test: one non-persisted input-provider request, "<family>=<id>" (id from the runtime's
    // list; "" after '=' = back to the default), sent once the session runs.
    if (const char* r = std::getenv("MEDIAPLAYER_INPUT_REQUEST")) inputRequestAtStart_ = r;
    if (const char* h = std::getenv("MEDIAPLAYER_HUD")) showHud_ = (*h && *h != '0');
    // Env-gated initial stereo state (test scaffolding: lets a headless atlas dump
    // prove convergence/swap without keystrokes). Interactive keys still apply on top.
    if (const char* c = std::getenv("MEDIAPLAYER_CONV")) convergence_ = (float)std::atof(c);
    if (const char* s = std::getenv("MEDIAPLAYER_SWAP")) swapEyes_ = (*s && *s != '0');
    // Depth cursor kill switch, and a pinned pointer (canvas UV) for headless tests.
    if (const char* d = std::getenv("MEDIAPLAYER_DEPTH_CURSOR")) depthCursorAllowed_ = (*d && *d != '0');
    if (const char* p = std::getenv("MEDIAPLAYER_DEPTH_CURSOR_UV"))
        depthCursorPinned_ = std::sscanf(p, "%f,%f", &depthCursorPinU_, &depthCursorPinV_) == 2;
#if defined(__APPLE__)
    // macOS hides the pointer through displayxr-common's Cocoa adapter (+[NSCursor
    // hide]), NOT SDL_HideCursor: SDL's hide installs an invisible NSCursor, and the
    // runtime's default cursor look mirrors the CURRENT NSCursor — it would draw
    // nothing. The pointer UV still comes from us (setPointerUV overrides the adapter).
    xr_.Cursor().setWindowCocoa(window_.NativeHandle());
#endif
    xr_.Cursor().onDepthCursorShownChanged([this](bool shown) {
        // The runtime draws the cursor now: hide the OS pointer (and keep ImGui's SDL
        // backend from re-showing it every frame), or give it back.
        imgui_.SetCursorLocked(shown);
#if !defined(__APPLE__)
        window_.SetOsCursorHidden(shown);
#endif
        LOG_DEBUG("depth cursor: %s", shown ? "runtime draws (lifted over popped-out content; OS cursor hidden)"
                                           : "on the glass (OS cursor)");
    });
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
        if (window_.TakeCycleModeRequest()) { xr_.RequestNextMode(); userModeChoice_ = true; activity = true; }  // 'V'
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
        // Left / Right (and '[' / ']') step frames on video. On a still there is no frame
        // to step, so the one-shot arrow press walks the folder like Up / Down do - the
        // photo-viewer habit kept without a mode.
        {
            const int fs = window_.TakeFrameStep();
            const int press = window_.TakeArrowPress();
            if (isVideo_) {
                if (fs != 0) { StepFrame(fs); activity = true; }
            } else if (press != 0) {
                RequestNavTransition(press);
                activity = true;
            }
        }
        if (window_.TakeSwapEyesRequest()) {                          // 'X'
            swapEyes_ = !swapEyes_;
            LOG_INFO("swap eyes %s", swapEyes_ ? "on" : "off");
            activity = true;
        }
        if (window_.TakeToggleAutoConvRequest()) {                    // 'A' (#92)
            SetAutoConv(!autoConvEnabled_, "key");
            activity = true;
        }
        if (window_.TakeCycleAutoConvPolicyRequest()) {               // Shift+A (#92)
            using P = autoconv::SubjectPolicy;
            const P next = autoConvPolicy_ == P::Nearest     ? P::Sharpness
                           : autoConvPolicy_ == P::Sharpness ? P::Centre
                                                             : P::Nearest;
            SetAutoConvPolicy(next);
            activity = true;
        }
        if (float cx = 0.f, cy = 0.f; window_.TakeCtrlClick(cx, cy)) {  // Ctrl+click (#92)
            // Window points -> canvas pixels -> the eye image under the cursor (the content
            // rect before the convergence shift, which is at most a few percent).
            uint32_t pw = 0, ph = 0, cw = 0, ch = 0;
            window_.PointSize(pw, ph);
            window_.PixelSize(cw, ch);
            if (LiveSource() && hasMedia_ && pw && ph && cw && ch) {
                const XrSession::ViewRect fit = MatchMinRect({0, 0, cw, ch}, FitAspect());
                const float px = cx * (float)cw / (float)pw, py = cy * (float)ch / (float)ph;
                const float u = (px - (float)fit.x) / (float)std::max(1u, fit.w);
                const float v = (py - (float)fit.y) / (float)std::max(1u, fit.h);
                if (u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f) SetAutoConvFocus(u, v);
            }
            activity = true;
        }
        if (window_.TakeCaptureRequest()) {                           // 'I' — atlas snapshot
            if (xr_.HasAtlasCapture()) {
                const std::string stem = isLive_ ? std::string("live")
                    : isStream_ ? std::string("stream")
                    : currentMediaPath_.empty()
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
        if (window_.TakeOpenFileRequest()) { RequestOpenFile(); activity = true; }     // Ctrl+O
        // Dropped files (#44). Loading happens HERE, on the app thread — not in the SDL
        // event handler — matching the discipline the native dialog already follows.
        if (std::vector<std::string> dropped; window_.TakeDroppedPaths(dropped)) {
            HandleDroppedPaths(std::move(dropped));
            activity = true;
        }
        if (window_.TakeTogglePauseRequest()) { TogglePlayback(); activity = true; }  // Space
        if (window_.TakePrevMediaRequest()) { RequestNavTransition(-1); activity = true; }  // Up
        if (window_.TakeNextMediaRequest()) { RequestNavTransition(+1); activity = true; }  // Down
        if (window_.TakeToggleSlideshowRequest()) { ToggleSlideshow(); activity = true; } // 'S'
        if (window_.TakeToggleMuteRequest()) { ToggleMute(); activity = true; }            // 'M'
        if (window_.TakeCycleLayoutRequest()) { CycleLayoutOverride(); activity = true; }  // 'L'
        if (window_.TakeConvertRequest()) { SetConvert(!lift_.On(), "key"); activity = true; }  // Shift+L
        if (window_.TakeToggleCameraRequest()) { ToggleLive(); activity = true; }          // 'C'
        if (window_.TakeOpenUrlRequest()) { OpenUrlPrompt(); activity = true; }            // Ctrl+U
        // Camera hot-plug events (only arrive once the camera subsystem is up).
        if (std::vector<std::pair<uint32_t, uint32_t>> camEv; window_.TakeCameraEvents(camEv)) {
            for (const auto& ev : camEv) {
                if (ev.first == SDL_EVENT_CAMERA_DEVICE_REMOVED && isLive_ && LiveIsCamera() &&
                    ev.second == camera_.DeviceId()) {
                    camera_.NotifyRemoved();
                } else if (ev.first == SDL_EVENT_CAMERA_DEVICE_ADDED &&
                           uiState_.cameraComboWasOpen) {
                    RefreshCameraList(false);
                }
            }
            activity = true;
        }
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
        // Either picker is an explicit "load this one thing" gesture, so it retires any
        // playlist a multi-file drop installed and the directory scan resumes (#44).
        std::string picked;
        if (xr_.TakePickedFile(picked)) {
            openFilePending_ = false;
            if (!picked.empty()) { playlistFromDrop_ = false; ReloadMedia(picked); }
        }
        bool gotNative = false;
        {
            std::lock_guard<std::mutex> lk(nativePathMutex_);
            if (hasNativePath_) { hasNativePath_ = false; picked = nativePath_; gotNative = true; }
        }
        if (gotNative) {
            openFilePending_ = false;
            if (!picked.empty()) { playlistFromDrop_ = false; ReloadMedia(picked); }  // outside the lock
        }
        // Test hook: exercise the media-swap path without a dialog.
        if (openAfterPath_ && !openedAfter_ && frames_ > 150) {
            openedAfter_ = true;
            playlistFromDrop_ = false;
            ReloadMedia(openAfterPath_);
        }

        PollLive();
        PollStream();
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
    // INV-4.10: the video upload, the atlas pass and the HUD pass all record into ONE
    // frame command buffer that is submitted once. The normal path calls SubmitFrame()
    // explicitly before releasing the HUD / atlas images; this backstop covers every
    // early return (e.g. xrBeginFrame failing after an upload was recorded) so a
    // recorded frame is never left open. No-op when nothing is open.
    struct SubmitGuard { VulkanRenderer& r; ~SubmitGuard() { r.SubmitFrame(); } } submitGuard{renderer_};

    // Advance UI fades + the slideshow machine (may swap media at a fade's mid-black).
    TickUi();

    if (startMode_ >= 0 && !startModeRequested_) {
        xr_.RequestMode((uint32_t)startMode_);
        startModeRequested_ = true;
    }

    // Idle-screen mode borrow (#64), issued here rather than at the edge that queued
    // it: LoadIdleLogo() runs during Init, when the session exists but is not yet
    // running, and a mode request then is dropped.
    if (pendingModeRequest_ >= 0) {
        xr_.RequestMode((uint32_t)pendingModeRequest_);
        pendingModeRequest_ = -1;
    }

    // Pull the latest decoded video frame (if any) and upload its YUV planes — the GPU
    // does the colour convert + downscale, so no swscale ran on the decode thread.
    // The live camera (#90) publishes into a FrameRing of its own, so it rides the same
    // upload path. Freeze = stop acquiring: the last uploaded texture keeps drawing.
    // A stream (#93) is video_ in live mode: same ring, same freeze-by-not-acquiring.
    window_.SetKeyboardCaptured(false);  // BuildTransportUI re-asserts it while a field is focused
    FrameRing* ring = isLive_     ? (livePaused_ ? nullptr : &live_->Ring())
                      : isStream_ ? (streamPaused_ ? nullptr : &video_.Ring())
                                  : (isVideo_ ? &video_.Ring() : nullptr);
    if (ring) {
        if (const FrameRing::Frame* vf = ring->AcquireLatest()) {
#if MEDIAPLAYER_PRO
            // Pro: timecode, measured delay and the rig checks see every uploaded frame.
            pro_.GetStation().OnFrame(*vf, layout_, isLive_, isLive_ ? live_->GetStats().eyeOffsetFrames : -1,
                                      swapEyes_ != mediaEyeSwap_);  // rig checks measure the eyes as shown
            pro_.OnFrame(*vf, layout_);  // scopes, pixel probe, colour-space burn-in
#endif
            if (isLive_) {
                OnLiveFrame(*vf);
                SampleAutoConv(*vf);  // ~5 Hz; downsample only, measured on the worker
            } else if (isStream_) {
                SettleLiveLayout(*vf, "Stream");
                SampleAutoConv(*vf);  // streams converge like the camera (#93 follow-up)
            }
            // "Convert to 3D": hand the frame to the converter (latest-wins, one in flight).
            // While a converted picture is bound it REPLACES the source, so the source frame
            // is not uploaded; a stalled converter freezes on its last good picture.
            if (lift_.On()) SubmitConvertFrame(*vf, SteadySeconds());
            bool bound = convertBound_;
#if defined(_WIN32)
            // Zero-copy (#28): a GPU frame carries a shared RGBA texture handle (the producer
            // converted NV12->RGBA on the D3D side) — bind it directly. Falls back to the CPU
            // upload if the import fails.
            if (!bound && vf->gpu && vf->gpuSharedHandle)
                bound = renderer_.BindSharedRGBA(vf->gpuSharedHandle, (uint32_t)vf->width,
                                                 (uint32_t)vf->height);
#endif
            if (!bound) {
                const bool i420 = (vf->format == PixFormat::I420);
                renderer_.UploadYUV(vf->plane[0].data(), vf->plane[1].data(),
                                    i420 ? vf->plane[2].data() : nullptr, (uint32_t)vf->width,
                                    (uint32_t)vf->height,
                                    i420 ? 0 : vf->format == PixFormat::P016 ? 2 : 1, vf->fullRange,
                                    (int)vf->matrix);  // the frame's colour tag (BT.709 default)
            }
        }
    }

    if (!inputRequestAtStart_.empty() && rendered_ == 60) {  // MEDIAPLAYER_INPUT_REQUEST
        const size_t eq = inputRequestAtStart_.find('=');
        XrInputRoleFamilyDXR fam;
        if (eq != std::string::npos && input::ParseFamily(inputRequestAtStart_.substr(0, eq), fam))
            SetInputProvider(fam, inputRequestAtStart_.substr(eq + 1), /*persist=*/false, "env");
        inputRequestAtStart_.clear();
    }
    if (convertAtStart_ && rendered_ >= 30 && rendered_ % 60 == 30) {  // MEDIAPLAYER_CONVERT
        // No XR_DXR_lift at all: nothing will ever come, and no toast either.
        if (!convertProviderAtStart_.empty() && lift_.HasCandidates() &&
            SetConvertProvider(convertProviderAtStart_, "env").empty())
            convertProviderAtStart_.clear();
        if (lift_.GetAvailability() == lift::Availability::Hidden || SetConvert(true, "env") ||
            rendered_ > 1200)
            convertAtStart_ = false;
    }
    PumpConvert(SteadySeconds());  // "Convert to 3D": poll + upload a landed result (no-op off)
    TickAutoConv();  // smoothed auto-convergence shift (#92); exactly 0 while never enabled
    UpdateDepthCursor();

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
            // mediaEyeSwap_ is the container's word that the packing is R|L; XOR it in so
            // it composes with the user's X toggle instead of overriding it.
            const bool isLeft =
                (frame.views[v].pose.position.x <= centerX) != (swapEyes_ != mediaEyeSwap_);
            isLeftView[v] = isLeft;
            colors[v] = isLeft ? kLeftImage : kRightImage;  // RED|BLUE fallback
            // Mono AND Dual both sample the WHOLE source per eye: mono because there
            // is one picture, Dual because each eye's picture lives in its own
            // container track and this leg only ever decoded the first of them (the
            // `abl` left view). Slicing a Dual frame in half would show half a
            // left-eye picture to each eye. The Android leg is where Dual actually
            // renders stereo (two decoders); here it degrades to flat-left.
            // A bound "Convert to 3D" result is two full-width eyes side by side: sample it
            // as SBS-full while layout_ stays Mono (contentAspect_ is the mono frame's).
            if ((layout_ == StereoLayout::Mono || layout_ == StereoLayout::Dual) &&
                !convertBound_) {
                uvs[v] = {0.0f, 0.0f, 1.0f, 1.0f};
            } else {
                uvs[v] = isLeft ? ViewUV{0.0f, 0.0f, 0.5f, 1.0f}
                                : ViewUV{0.5f, 0.0f, 0.5f, 1.0f};
            }
        }

#if MEDIAPLAYER_PRO
        // Pro: QC view mode + pan/zoom rewrite the source rects; the grade (exposure, CDL,
        // LUT) and the QC shader mode are pushed to the renderer here, before the draw.
        {
            const bool stereoSrc =
                (layout_ != StereoLayout::Mono && layout_ != StereoLayout::Dual) || convertBound_;
            pro_.ApplyViewTransform(uvs, frame.viewCount, stereoSrc && hasMedia_ && !isLogo_,
                                    swapEyes_ != mediaEyeSwap_);
            pro_.Tick(ProStatus());
        }
#endif
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
            const XrSession::ViewRect viewFit = MatchMinRect({0, 0, canvasW, canvasH}, FitAspect());
            const int32_t lbx = (int32_t)((float)viewFit.x * sx);  // horizontal inset of the content
            // Convergence = horizontal image translation: shift the two eyes' content
            // oppositely within their tiles, moving the zero-disparity plane. A LIF's
            // baked convergence (mediaConvergence_, scaled 0.5 to match the reference
            // per-eye shift) lands it at the author's intended plane; convergence_ trims.
            // Auto-convergence (#92) adds its own shift, which is a fraction of the EYE's
            // on-screen width (contentW), converted here to the tile fraction the others use.
            const float convBase = convergence_ + 0.5f * mediaConvergence_;
            const float contentW = (float)viewFit.w * sx;
            int32_t cdx[XrSession::kMaxViews];
            for (uint32_t v = 0; v < frame.viewCount; ++v) {
                const float conv =
                    convBase + autoConvergence_ * contentW / (float)std::max(1u, rects[v].w);
                cdx[v] = autoconv::ConvergenceShiftPx(conv, rects[v].w, isLeftView[v]);
                contentRects[v].x = rects[v].x + cdx[v] + lbx;
                contentRects[v].y = rects[v].y + (int32_t)((float)viewFit.y * sy + 0.5f);
                contentRects[v].w = (uint32_t)((float)viewFit.w * sx + 0.5f);
                contentRects[v].h = (uint32_t)((float)viewFit.h * sy + 0.5f);
            }

#if MEDIAPLAYER_PRO
            {
                // The picture in window points (pointer mapping) and per-eye tile pixels (1:1).
                uint32_t pw = 0, ph = 0;
                window_.PointSize(pw, ph);
                const float k = canvasW ? (float)pw / (float)canvasW : 1.0f;
                proPicX_ = (float)viewFit.x * k;
                proPicY_ = (float)viewFit.y * (canvasH ? (float)ph / (float)canvasH : 1.0f);
                proPicW_ = (float)viewFit.w * k;
                proPicH_ = (float)viewFit.h * (canvasH ? (float)ph / (float)canvasH : 1.0f);
                proTileContentW_ = (float)contentRects[0].w;
            }
#endif
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
                mu.otherDX = uvs[v].otherDX;  // Pro QC views: same other-eye lookup
                mu.otherDY = uvs[v].otherDY;
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

        if (dumpPath_ && !dumped_ && rendered_ >= dumpFrame_) {
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
        (imguiReady ? (showHud_ || uiState_.fadeAlpha > 0.001f || uiState_.toastAlpha > 0.001f ||
                       uiState_.transitionAlpha > 0.001f
#if MEDIAPLAYER_PRO
                       || pro_.KeepHudAwake()
#endif
                       )
                    : showHud_);
    if (wantHud) {
        uint32_t cw = 0, ch = 0;
        window_.PixelSize(cw, ch);
        const float hudAR = (float)xr_.HudWidth() / (float)xr_.HudHeight();
        const float winAR = (ch > 0) ? (float)cw / (float)ch : 1.0f;

        uint32_t hudIdx = 0;
        if (xr_.AcquireHudImage(hudIdx)) {
            if (imguiReady) {
                // Aspect-preserving HUD footprint (matches the Gauss demo's demo-gs#8 fix).
                // The HUD swapchain is a fixed 16:9 image; the runtime stretches it per-axis to
                // fill the layer rect. A full-window rect on a non-16:9 tile stretches the two
                // axes unequally and distorts the buttons/glyphs. Pick a centered rect whose
                // fractions match the HUD aspect so both axes scale by the same factor (uniform,
                // no distortion). The SAME rect drives the cursor remap below, so hit-testing
                // stays aligned with what's drawn.
                float fracW = 1.0f, fracH = 1.0f;
                if (winAR > hudAR) fracW = hudAR / winAR;   // wide tile: full height, inset width
                else if (winAR > 0.0f) fracH = winAR / hudAR;  // tall tile: full width, inset height
                hud.enabled = true;
                hud.x = (1.0f - fracW) * 0.5f;  // centered horizontally
                hud.y = 0.0f;                   // anchored to the top so the bar stays flush
                hud.width = fracW;
                hud.height = fracH;
                hud.disparity = kHudDisparity;
#if MEDIAPLAYER_PRO
                proHud_[0] = hud.x;
                proHud_[1] = hud.y;
                proHud_[2] = hud.width;
                proHud_[3] = hud.height;
#endif

                uint32_t pw = 0, phh = 0;
                window_.PointSize(pw, phh);
                imgui_.BeginFrame((float)pw, (float)phh, hud.x, hud.y, hud.width, hud.height);
                BuildTransportUI();
#if MEDIAPLAYER_PRO
                // Pinned panels go out flat as well (Local2D layers over the same HUD image).
                for (const pro::ProController::FlatRect& r : pro_.FlatRects()) {
                    if (hud.flatCount >= XrSession::HudSubmit::kMaxFlat) break;
                    XrSession::HudSubmit::Rect& o = hud.flat[hud.flatCount++];
                    o.x = (int32_t)std::floor(r.x);
                    o.y = (int32_t)std::floor(r.y);
                    o.w = (int32_t)std::ceil(r.x + r.w) - o.x;
                    o.h = (int32_t)std::ceil(r.y + r.h) - o.y;
                }
                hud.windowPxW = cw;
                hud.windowPxH = ch;
                pro_.ClearFlatRects();
#endif
                imgui_.RenderToHud(hudIdx, renderer_.FrameCmd());  // joins the frame's command buffer
                // Desktop Linux: the pointer shape ImGui wants (I-beam over the URL
                // field, ...). The SDL backend does this itself elsewhere.
                window_.SetImGuiCursor(imgui_.MouseCursor());
                if (dumpHudPath_ && !dumpedHud_ && rendered_ >= dumpFrame_) {
                    renderer_.DumpExternalImage(xr_.HudImages()[hudIdx], xr_.HudWidth(),
                                                xr_.HudHeight(), dumpHudPath_);
                    dumpedHud_ = true;
                }
            } else {
                // CPU text-HUD fallback: top-left stats panel (pre-M4 behavior).
                const uint32_t tileW = rects[0].w, tileH = rects[0].h;
                char stereo[200];
                std::snprintf(stereo, sizeof(stereo), "conv %+.3f  eyes %s%s%s%s", convergence_,
                              swapEyes_ ? "swapped" : "normal",
                              ((isLive_ && livePaused_) || (isStream_ && streamPaused_)) ? "  [FROZEN]"
                              : (isVideo_ && video_.Paused()) ? "  [PAUSED]" : "",
                              LiveSource() ? "\n" : "",
                              LiveSource() ? AutoConvHudText().c_str() : "");  // #92
                // The layout label carries its provenance ("— detected" / "— from
                // filename" / ...), so the buffer needs headroom over the old 320.
                const std::string layoutLabel = LayoutLabel();
                char text[512];
                if (isLive_) {
                    const LiveStats cs = live_->GetStats();
                    std::snprintf(text, sizeof(text),
                                  "%.0f FPS   %s\nLIVE %s %dx%d  %s  cam %.0f fps  age %.0f ms\n"
                                  "win %ux%u  tile %ux%u\n%s",
                                  fps_, xr_.ActiveModeName(), live_->DeviceName().c_str(),
                                  mediaW_, mediaH_, layoutLabel.c_str(), cs.deliveredFps,
                                  liveAgeMs_, cw, ch, tileW, tileH, stereo);
                } else if (isStream_) {
                    const VideoDecoder::StreamStats ss = video_.GetStreamStats();
                    std::snprintf(text, sizeof(text),
                                  "%.0f FPS   %s\nSTREAM %s %dx%d  %.0f fps  %.0f kbit/s  %s\n"
                                  "win %ux%u  tile %ux%u\n%s",
                                  fps_, xr_.ActiveModeName(), stream::UrlHost(streamUrl_).c_str(),
                                  mediaW_, mediaH_, ss.fpsIn, ss.kbps, LayoutLabel().c_str(), cw,
                                  ch, tileW, tileH, stereo);
                } else if (isVideo_) {
                    std::snprintf(text, sizeof(text),
                                  "%.0f FPS   %s\nsrc %dx%d  %s  %s/%s\nwin %ux%u  tile %ux%u\n%s",
                                  fps_, xr_.ActiveModeName(), mediaW_, mediaH_,
                                  layoutLabel.c_str(), video_.CodecName(),
                                  video_.BackendName(), cw, ch, tileW, tileH, stereo);
                } else if (hasMedia_) {
                    std::snprintf(text, sizeof(text),
                                  "%.0f FPS   %s\nsrc %dx%d  %s\nwin %ux%u  tile %ux%u\n%s", fps_,
                                  xr_.ActiveModeName(), mediaW_, mediaH_,
                                  layoutLabel.c_str(), cw, ch, tileW, tileH, stereo);
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
            // The frame's ONE submit (upload + atlas + HUD), no wait — BEFORE the HUD
            // image is released (the runtime composes on this same queue).
            renderer_.SubmitFrame();
            xr_.ReleaseHudImage();
        }
    }

    // No HUD this frame: the same single submit, before EndFrame releases the atlas image.
    renderer_.SubmitFrame();
    xr_.EndFrame(frame, rects, &hud);
    ++frames_;
    UpdateFps();
}

// Fill the shared UI state from live player state and hand it the callbacks. The
// widgets themselves live in ui/TransportUI.cpp — shared verbatim with Android, so
// the two legs cannot drift apart.
void App::BuildTransportUI() {
    uiState_.hasMedia = hasMedia_;
    uiState_.isVideo = isVideo_;
    uiState_.mediaFilename =
        currentMediaPath_.empty() ? std::string()
                                  : std::filesystem::path(currentMediaPath_).filename().string();
    uiState_.modeName = xr_.ActiveModeName() ? xr_.ActiveModeName() : "";
    uiState_.layoutName = convertBound_ ? kConvertedLayoutName : MediaSource::LayoutName(layout_);
    uiState_.layoutTooltip = LayoutLabel();
    uiState_.layoutPinned = layoutPinned_;
    uiState_.positionSeconds = video_.PositionSeconds();
    uiState_.durationSeconds = video_.DurationSeconds();
    // "Paused" for the icon includes "ended" — a finished clip shows Play, not Pause.
    uiState_.paused = video_.Paused() || video_.Ended();
    uiState_.muted = muted_;
    uiState_.loop = video_.Loop();
    uiState_.openFilePending = openFilePending_;

    // Live camera (#90). Slideshow has nothing to advance through while live, so its
    // icon is hidden; the bottom transport hides itself because isVideo is false.
    uiState_.caps.slideshow = !isLive_ && !isStream_;
    uiState_.isLive = isLive_;
    uiState_.livePaused = livePaused_;
    uiState_.liveDevice = isLive_ ? live_->DeviceName() : std::string();
    uiState_.liveStatus = isLive_ ? LiveStatusText() : std::string();
    uiState_.cameraFps = isLive_ ? live_->GetStats().deliveredFps : 0.0f;
    uiState_.panelFps = fps_;
    uiState_.frameAgeMs = (float)liveAgeMs_;
    uiState_.cameraCurrent = -1;
    for (size_t i = 0; isLive_ && LiveIsCamera() && i < cameraDevs_.size(); ++i)
        if (cameraDevs_[i].id == camera_.DeviceId()) uiState_.cameraCurrent = (int)i;
    if (isLive_) uiState_.mediaFilename.clear();
    uiState_.autoConvOn = autoConvEnabled_;
    {
        char t[64];
        if (autoConvEnabled_)
            std::snprintf(t, sizeof(t), "Auto-conv: %s %+.1f%%",
                          autoconv::PolicyName(autoConvPolicy_), autoConvergence_ * 100.0f);
        else
            std::snprintf(t, sizeof(t), "Auto-conv: off");
        uiState_.autoConvLabel = t;
    }
    // "Convert to 3D" (XR_DXR_lift): drawn only for mono content on a runtime that offers
    // the extension; greyed with the reason while the conversion task cannot serve.
    {
        const lift::Availability av = lift_.GetAvailability();
        const lift::State st = lift_.GetState();
        uiState_.caps.convert = av != lift::Availability::Hidden && ConvertEligible();
        uiState_.convertOn = lift_.On();
        uiState_.convertEnabled = av == lift::Availability::Enabled || lift_.On();
        uiState_.convertWarming = st == lift::State::Warming;
        uiState_.convertUnavailable = st == lift::State::Unavailable;
        const std::string why = lift_.Reason();
        uiState_.convertTooltip =
            !why.empty() ? why
            : lift_.On() ? std::string("Shift+L: show the original 2D picture")
                         : std::string("Shift+L: convert this 2D picture to 3D");
        // Provider selector (spec 5): the runtime's candidates, names verbatim.
        uiState_.convertProviders.clear();
        for (const lift::Candidate& c : lift_.Candidates()) {
            const bool remote = c.hosting == XR_LIFT_PROVIDER_HOSTING_REMOTE_DXR;
            ui::ConvertProviderRow row;
            row.label = c.name + "  (" + lift::TaskStateName(c.state) +
                        (remote ? ", remote" : "") + ")";
            row.preview = remote ? c.name + " (remote)" : c.name;
            row.enabled = !c.Terminal();
            uiState_.convertProviders.push_back(std::move(row));
        }
        uiState_.convertProviderCurrent = lift_.EffectiveCandidate();
    }
    // Input popup (XR_DXR_input_providers): one combo per family the runtime can supply.
    uiState_.caps.input = input_.Attached();  // hidden only without the extension
    uiState_.inputRemember = inputRemember_;
    uiState_.inputFamilies.clear();
    for (XrInputRoleFamilyDXR fam : InputFamiliesShown()) {
        ui::InputFamilyRow row;
        row.label = input::FamilyLabel(fam);
        row.rows.push_back("Default");
        row.enabled = input_.FamilyOffered(fam);
        const std::vector<int> map = InputRowProviders(fam);
        // Pre-select the current holder; this app's own request (if it names another
        // provider) wins the display, so a pick is visible before the holder moves.
        const int shown = input_.Requested(fam) >= 0 ? input_.Requested(fam) : input_.Current(fam);
        for (size_t r = 1; r < map.size(); ++r) {
            const input::Provider& pv = input_.Providers()[map[r]];
            std::string label = pv.name;
            if (pv.current & input::FamilyBit(fam)) label += "  (in use)";
            if (!pv.present) label += "  (not connected)";
            if (pv.appPinned) label += "  (remembered)";
            row.rows.push_back(label);
            if (map[r] == shown) row.current = (int)r;
        }
        uiState_.inputFamilies.push_back(std::move(row));
    }
    // Stream URL (#93).
    uiState_.isStream = isStream_;
    uiState_.streamStatus = isStream_ ? StreamStatusText() : std::string();
    uiState_.streamHost = isStream_ ? stream::UrlHost(streamUrl_) : std::string();
    if (isStream_) {
        const VideoDecoder::StreamStats ss = video_.GetStreamStats();
        uiState_.streamFps = ss.fpsIn;
        uiState_.streamKbps = ss.kbps;
        uiState_.mediaFilename.clear();
    }

    ui::TransportActions actions;
    actions.Open = [this] { RequestOpenFile(); };
    actions.NextMode = [this] { xr_.RequestNextMode(); userModeChoice_ = true; };
    actions.CycleLayout = [this] { CycleLayoutOverride(); };
    actions.ToggleSlideshow = [this] { ToggleSlideshow(); };
    actions.TogglePlayback = [this] { TogglePlayback(); };
    actions.ToggleMute = [this] { ToggleMute(); };
    actions.ToggleLoop = [this] {
        video_.ToggleLoop();
        audio_.SetLoop(video_.Loop());
    };
    actions.Seek = [this](float sec, bool preview) {
        video_.Seek(sec, preview);
        if (!preview) ResetConvertStream();  // a jump: no pre-seek result may land after it
    };
    actions.ScrubHeld = [this] { audio_.SetPaused(true); };
    actions.ScrubReleased = [this](float sec) {
        video_.Seek(sec, /*preview=*/false);
        ResetConvertStream();
        audio_.Seek(sec);
        if (!video_.Paused()) audio_.SetPaused(false);
    };

    actions.RefreshCameras = [this](bool rescan) { RefreshCameraList(rescan); };
    actions.OpenCamera = [this](int row) {
        if (row >= 0 && row < (int)cameraDevs_.size()) {
            const CameraDevice dev = cameraDevs_[row];  // copy: a refresh may reallocate
            LoadLiveDevice(dev);
        }
    };
    actions.StopCamera = [this] {
        if (isStream_) StopStream(/*showIdleIfEmpty=*/true);
        else StopLive(/*showIdleIfEmpty=*/true);
    };
    actions.OpenUrl = [this](const std::string& url) { LoadUrl(url); };
    actions.ToggleAutoConv = [this] { SetAutoConv(!autoConvEnabled_, "ui"); };
    actions.ToggleConvert = [this] { SetConvert(!lift_.On(), "ui"); };
    actions.SelectInputProvider = [this](int family, int row) {
        const std::vector<XrInputRoleFamilyDXR> fams = InputFamiliesShown();
        if (family < 0 || family >= (int)fams.size()) return;
        const std::vector<int> map = InputRowProviders(fams[family]);
        if (row < 0 || row >= (int)map.size()) return;
        const std::string id = map[row] < 0 ? std::string() : input_.Providers()[map[row]].id;
        SetInputProvider(fams[family], id, inputRemember_, "ui");
    };
    actions.SetInputRemember = [this](bool remember) { inputRemember_ = remember; };
    actions.SelectConvertProvider = [this](int row) {
        const auto& cands = lift_.Candidates();
        if (row >= 0 && row < (int)cands.size()) SetConvertProvider(cands[row].id, "ui");
    };

    ui::BuildTransportUI(uiState_, actions);
#if MEDIAPLAYER_PRO
    if (showHud_) pro_.BuildUI(ProStatus(), uiState_.fadeAlpha);
#endif
#if defined(MEDIAPLAYER_WITH_IMGUI)
    // A focused text field (the Stream URL popup) owns the keyboard: no hotkeys.
    window_.SetKeyboardCaptured(ImGui::GetIO().WantTextInput);
#endif
}

void App::UpdateDepthCursor() {
    dxr::CursorDepth& cursor = xr_.Cursor();
    // Measured disparity is spec v7. Only stereo media qualifies: mono content (and the
    // idle logo) is on the glass by construction, so asking would be pure cost. There
    // is no depth map or lifted-content path in this player to prefer over it.
    // A "Convert to 3D" picture is stereo too.
    const bool want = depthCursorAllowed_ && xr_.CursorDepthSpec() >= 7 && hasMedia_ && !isLogo_ &&
                      (layout_ != StereoLayout::Mono || convertBound_);
    if (want != depthCursorOn_) {
        depthCursorOn_ = want;
        // enable(0) makes the helper inert: nothing is chained on xrLocateViews, so the
        // runtime does no copy, readback or match, and the OS cursor is restored.
        cursor.enable(want ? xr_.CursorDepthSpec() : 0);
        cursor.useMeasuredDisparity(want);
        LOG_INFO("depth cursor: measured-disparity %s", want ? "ON (stereo media)" : "off");
    }
    if (!want) return;

    // The pointer as a canvas UV (the canvas is the window's client area). Off the
    // canvas, or over the transport UI, nothing is requested this frame and the OS
    // cursor stays, so the buttons keep their normal pointer.
    if (depthCursorPinned_) {
        // Test mode: also read back the placement and log it ~1/s (last frame's result).
        cursor.setWantPlacement(true);
        cursor.setPointerUV(depthCursorPinU_, depthCursorPinV_);
        if (++depthCursorLogTick_ % 60 == 0) {
            const XrCursorDepthPlacementDXR& pl = cursor.placement();
            LOG_INFO("depth cursor @uv(%.3f,%.3f): willDraw=%d active=%d disparity=%.4f target=%.4f z=%.4f",
                     depthCursorPinU_, depthCursorPinV_, (int)cursor.drawState().willDraw, (int)pl.isActive,
                     pl.disparity, pl.targetDisparity, pl.pose.position.z);
        }
        return;
    }
    uint32_t pw = 0, ph = 0;
    window_.PointSize(pw, ph);
    float mx = 0.0f, my = 0.0f;
    window_.MousePosition(mx, my);
    XrVector2f uv{};
    if (window_.MouseInWindow() && !imgui_.WantCaptureMouse() &&
        dxr::cursorCanvasUV(mx, my, pw, ph, /*yUp=*/false, uv))
        cursor.setPointerUV(uv.x, uv.y);
    else
        cursor.setPointerUV(-1.0f, -1.0f);
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
                      MP_PRODUCT_NAME "  —  %.0f fps  —  %s", fps_,
                      xr_.ActiveModeName());
        window_.SetTitle(title);
        LOG_DEBUG("fps=%.1f mode=%s", fps_, xr_.ActiveModeName());
    }
}

void App::Shutdown() {
    autoConvWorker_.Stop();  // #92
#if MEDIAPLAYER_PRO
    pro_.GetStation().Shutdown();  // Pro live inputs (DeckLink / test rig) join their threads
#endif
    camera_.Stop();       // join the camera worker + SDL_CloseCamera before SDL_Quit
    video_.Stop();        // join the decode thread (reads the audio clock) before audio,
    audio_.Stop();        // and before tearing down the GPU
    imgui_.Shutdown();    // before xr_ destroys the Vulkan device ImGui borrows
    renderer_.Shutdown();
    lift_.Detach();       // destroy the conversion stream before its session
    input_.Detach();
    xr_.Shutdown();
    window_.Destroy();
}

// --- Open-file ------------------------------------------------------------------

void App::ReadStereoEnv() {
    // Read in Initialize(), BEFORE the command-line file is loaded. These two used to sit
    // with the other env reads in Run(), which runs after that first LoadMedia -- so they
    // silently had no effect on the file the app was launched with, only on files opened
    // later.
    //
    // MEDIAPLAYER_STEREO_DETECT: default `full` (App.h detectMode_) — filename, metadata,
    // the pixel detector, then the aspect assumption; `meta` skips the pixel detector;
    // `off` skips the probe entirely (container metadata included). An unrecognised value
    // reads as `meta`. Only the pixel detector can conclude a file is MONO, so under `meta`
    // / `off` (or when it cannot decide) mono content shows as stereo until the user pins
    // Mono with L — which is also what brings up the "Convert to 3D" toggle for it.
    if (const char* d = std::getenv("MEDIAPLAYER_STEREO_DETECT")) {
        if (std::strcmp(d, "off") == 0) detectMode_ = DetectMode::Off;
        else if (std::strcmp(d, "full") == 0) detectMode_ = DetectMode::Full;
        else detectMode_ = DetectMode::Meta;
        LOG_INFO("Stereo detection: %s", d);
    }
    // MEDIAPLAYER_LAYOUT forces a layout for headless/atlas-dump runs. Loads clear the
    // pin, so layoutForced_ re-applies it at every load.
    if (const char* l = std::getenv("MEDIAPLAYER_LAYOUT")) {
        layoutForced_ = true;
        if (std::strcmp(l, "mono") == 0) layoutForcedValue_ = StereoLayout::Mono;
        else if (std::strcmp(l, "sbshalf") == 0) layoutForcedValue_ = StereoLayout::SbsHalf;
        else if (std::strcmp(l, "sbsfull") == 0 || std::strcmp(l, "sbs") == 0)
            layoutForcedValue_ = StereoLayout::SbsFull;
        else layoutForced_ = false;   // "auto" or anything unrecognised
        LOG_INFO("Stereo layout forced: %s", layoutForced_ ? l : "auto");
    }
}

bool App::LoadMedia(const std::string& path) {
    const MediaInfo probeInfo = MediaSource::Identify(path);

    // Layered layout resolution (#45). Work out up front which layers we even need:
    // a manual pin short-circuits everything, and a decisive filename means the content
    // detector has nothing to add (we still want a metadata pass for the eye-swap bit).
    if (layoutForced_) { layoutPinned_ = true; layoutPinnedValue_ = layoutForcedValue_; }
    const StereoLayout* manual = layoutPinned_ ? &layoutPinnedValue_ : nullptr;
    StereoLayout fnLayout = StereoLayout::Mono;
    const bool fnDecided = MediaSource::LayoutFromFilename(path, fnLayout);
    const bool wantContent = !manual && !fnDecided && detectMode_ == DetectMode::Full;

    const MediaInfo info = probeInfo;
    if (info.kind == MediaKind::Video) {
#if defined(_WIN32)
        // Zero-copy decode (#28): pin the D3D11VA decode device to the Vulkan adapter so
        // decoded surfaces can be shared into Vulkan without a CPU round trip. OPT-IN during
        // bring-up (MEDIAPLAYER_ZEROCOPY=1) — the CPU-upload path remains the default.
        const char* zc = std::getenv("MEDIAPLAYER_ZEROCOPY");
        if (zc && *zc && *zc != '0' && xr_.ZeroCopyCapable() && xr_.DeviceLUIDValid())
            video_.SetInteropAdapterLUID(xr_.DeviceLUID());
#endif
        // Probe BEFORE opening for playback: a bounded, software-only pre-roll on its own
        // context. See VideoStereoProbe.h for why this is synchronous rather than
        // analysing presented frames.
        VideoStereoProbe::Result pr;
        if (detectMode_ != DetectMode::Off && !manual)
            pr = VideoStereoProbe::Run(path, wantContent);
        MediaInfo meta;
        if (pr.haveMeta) {
            meta.layout = pr.metaLayout;
            meta.eyeSwap = pr.metaInvert;
            // AVStereo3D says "side by side" but is silent on full vs half — settle that
            // from the frame aspect, the same way the content path does.
            if (meta.layout != StereoLayout::Mono && pr.width > 0 && pr.height > 0) {
                bool ambiguous = false;
                meta.layout = StereoDetect::ChooseFullOrHalf(
                    (float)pr.width / (float)pr.height, ambiguous);
            }
        }
        const MediaInfo vinfo = MediaSource::Resolve(
            path, MediaKind::Video, pr.ok ? pr.width : 0, pr.ok ? pr.height : 0,
            manual, pr.haveMeta ? &meta : nullptr,
            pr.content.decided ? &pr.content : nullptr);

        if (!video_.Open(path)) {
            LOG_ERROR("Open: cannot decode video '%s'", path.c_str());
            return false;
        }
        isVideo_ = true;
        hasMedia_ = true;
        ClearIdleLogo();
        ApplyLayout(vinfo);
        mediaConvergence_ = 0.0f;  // no baked convergence for video
        mediaAutoConvAvailable_ = false;
        KeepStillForConvert(nullptr, 0, 0);  // a video converts from its decoded frames
#if MEDIAPLAYER_PRO
        pro_.OnStill(nullptr, 0, 0, layout_);  // scopes follow the frames again
#endif
        mediaW_ = video_.Width();
        mediaH_ = video_.Height();
        contentAspect_ = PerEyeAspect(layout_, video_.Width(), video_.Height());
        // In slideshow, force play-once so the clip can end and advance, regardless of
        // the user's manual loop preference.
        if (uiState_.slideshowActive) video_.SetLoop(false);
        // Audio (optional — silent if the clip has none). Match mute + loop state.
        audio_.Open(path);
        audio_.SetMuted(muted_);
        audio_.SetLoop(video_.Loop());
        LOG_INFO("Playing %s video (%s), per-eye aspect %.3f",
                 LayoutLabel().c_str(), path.c_str(), contentAspect_);
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
        // The container is authoritative — the detector never runs on this path. A manual
        // pin still applies on top: forcing mono is a legitimate "show me the raw composed
        // frame" view.
        MediaInfo linfo;
        linfo.kind = MediaKind::Image;
        linfo.layout = manual ? *manual : lif.layout;
        linfo.signal = manual ? StereoSignal::Manual : StereoSignal::Metadata;
        linfo.confidence = 1.0f;
        autoInfo_ = MediaInfo{MediaKind::Image, lif.layout, StereoSignal::Metadata, false, 1.0f};
        ApplyLayout(linfo);
        mediaConvergence_ = lif.convergence;  // baked reconvergence from the LIF
        mediaAutoConvAvailable_ = lif.stereo && !lif.hasConvergence;  // estimate available
        mediaAutoConvergence_ = lif.autoConvergence;
        KeepStillForConvert(lif.image.pixels.data(), lif.image.width, lif.image.height);
#if MEDIAPLAYER_PRO
        pro_.OnStill(lif.image.pixels.data(), lif.image.width, lif.image.height, layout_);
#endif
        mediaW_ = lif.image.width;
        mediaH_ = lif.image.height;
        contentAspect_ = PerEyeAspect(layout_, lif.image.width, lif.image.height);
        LOG_INFO("Displaying %s LIF (%s), per-eye aspect %.3f, baked convergence %+.4f",
                 lif.stereo ? "stereo" : "mono", path.c_str(), contentAspect_, mediaConvergence_);
        currentMediaPath_ = path;
        RebuildFolderList(path);
        return true;
    }
    // MPO container (concatenated JPEGs indexed by an APP2 "MPF" segment): like LIF, the
    // loader composes the pair to SBS and the layout is settled. Must come AFTER the LIF
    // sniff (a LIF is also a JPEG) and BEFORE the plain-image path. A file that isn't a
    // well-formed two-view MPO returns ok=false and falls through to stb below, which
    // decodes its primary image as an ordinary JPEG.
    auto hasMpoExt = [](const std::string& p) {
        const size_t n = p.size();
        if (n < 4) return false;
        auto lc = [](char c) { return (char)(c | 0x20); };  // ASCII lower
        return p[n - 4] == '.' && lc(p[n - 3]) == 'm' && lc(p[n - 2]) == 'p' &&
               lc(p[n - 1]) == 'o';
    };
    if (hasMpoExt(path) || MpoLoader::IsMpo(path)) {
        MpoResult mpo = MpoLoader::Load(path);
        if (mpo.ok) {
            if (!renderer_.UploadTexture(mpo.image.pixels.data(), (uint32_t)mpo.image.width,
                                         (uint32_t)mpo.image.height)) {
                return false;
            }
            isVideo_ = false;
            hasMedia_ = true;
            ClearIdleLogo();
            MediaInfo minfo;
            minfo.kind = MediaKind::Image;
            minfo.layout = manual ? *manual : mpo.layout;
            minfo.signal = manual ? StereoSignal::Manual : StereoSignal::Metadata;
            minfo.confidence = 1.0f;
            autoInfo_ =
                MediaInfo{MediaKind::Image, mpo.layout, StereoSignal::Metadata, false, 1.0f};
            ApplyLayout(minfo);
            mediaConvergence_ = 0.0f;   // MPO carries no baked convergence...
            mediaAutoConvAvailable_ = true;   // ...so the estimate is always on offer
            mediaAutoConvergence_ = mpo.autoConvergence;
            KeepStillForConvert(mpo.image.pixels.data(), mpo.image.width, mpo.image.height);
#if MEDIAPLAYER_PRO
            pro_.OnStill(mpo.image.pixels.data(), mpo.image.width, mpo.image.height, layout_);
#endif
            mediaW_ = mpo.image.width;
            mediaH_ = mpo.image.height;
            contentAspect_ = PerEyeAspect(layout_, mpo.image.width, mpo.image.height);
            LOG_INFO("Displaying stereo MPO (%s), per-eye aspect %.3f", path.c_str(),
                     contentAspect_);
            currentMediaPath_ = path;
            RebuildFolderList(path);
            return true;
        }
    }
    DecodedImage img = ImageDecoder::Load(path);
    if (!img.Valid()) {
        LOG_ERROR("Open: cannot load image '%s'", path.c_str());
        return false;
    }
    // The layer that solves unsuffixed stills: a half-SBS frame is dimensionally identical
    // to a mono one, so only the pixels can tell them apart.
    StereoDetectResult content;
    if (wantContent) {
        content = StereoDetect::AnalyzeRGBA(img.pixels.data(), img.width, img.height,
                                            (ptrdiff_t)img.width * 4);
    }
    const MediaInfo imgInfo =
        MediaSource::Resolve(path, MediaKind::Image, img.width, img.height, manual,
                             /*meta=*/nullptr, content.decided ? &content : nullptr);
    if (!renderer_.UploadTexture(img.pixels.data(), (uint32_t)img.width, (uint32_t)img.height)) {
        return false;
    }
    isVideo_ = false;
    hasMedia_ = true;
    ClearIdleLogo();
    ApplyLayout(imgInfo);
    mediaConvergence_ = 0.0f;  // plain SBS image carries no baked convergence
    mediaAutoConvAvailable_ = false;
    KeepStillForConvert(img.pixels.data(), img.width, img.height);
#if MEDIAPLAYER_PRO
    pro_.OnStill(img.pixels.data(), img.width, img.height, layout_);
#endif
    mediaW_ = img.width;
    mediaH_ = img.height;
    contentAspect_ = PerEyeAspect(layout_, img.width, img.height);
    LOG_INFO("Displaying %s image (%s), per-eye aspect %.3f",
             LayoutLabel().c_str(), path.c_str(), contentAspect_);
    currentMediaPath_ = path;
    RebuildFolderList(path);
    return true;
}

void App::ApplyLayout(const MediaInfo& info) {
    layout_ = info.layout;
    layoutSignal_ = info.signal;
    layoutConfidence_ = info.confidence;
    mediaEyeSwap_ = info.eyeSwap;
    if (info.signal != StereoSignal::Manual) autoInfo_ = info;
    // The pin is per-file: carrying "SBS-full" onto the next mono photo in a folder would
    // make the slideshow look broken. Cleared HERE — at the point a load commits — rather
    // than at LoadMedia entry, so a failed open leaves the still-displayed media's pin on.
    layoutPinned_ = false;
#if MEDIAPLAYER_PRO
    // A monitoring station shows stereo content in 3D: when the panel is in a flat mode
    // that nobody chose (a runtime that starts in 2D, e.g. the sim display), ask for the
    // first 3D mode. A V press or the Mode button is a choice and is left alone; so is
    // MEDIAPLAYER_START_MODE. The base player keeps the panel's mode as it finds it.
    if (layout_ != StereoLayout::Mono && !isLogo_ && !userModeChoice_ && startMode_ < 0 &&
        pendingModeRequest_ < 0 && xr_.CurrentModeIsFlat()) {
        const int32_t m = xr_.FindStereoMode();
        if (m >= 0) {
            pendingModeRequest_ = m;
            LOG_INFO("Pro: stereo content on a flat panel - requesting the first 3D mode [%d]", m);
        }
    }
#endif
    // Toast only for a genuine content verdict. The aspect rung is now the routine
    // default rather than an exception, so toasting it would fire on nearly every load
    // and on every slideshow step; the HUD label carries that state persistently instead.
    if (info.signal == StereoSignal::Content) ShowToast(LayoutLabel());
}

std::string App::LayoutLabel() const {
    if (convertBound_) return kConvertedLayoutName;  // "Convert to 3D" result on screen
    std::string s = MediaSource::LayoutName(layout_);
    if (mediaEyeSwap_) s += " (R|L)";
    s += " — ";
    s += MediaSource::SignalName(layoutSignal_);
    return s;
}

void App::CycleLayoutOverride() {
    // A Dual (two-track) file is PINNED: its layout is a container fact, not a guess,
    // and none of the cycle's stops can render it. Cycling one into SBS would slice a
    // single full view in half; into Mono it is already flat here. So refuse.
    if (autoInfo_.layout == StereoLayout::Dual) {
        LOG_INFO("Layout override ignored: this file carries one view per track (Dual)");
        return;
    }
    // auto -> mono -> SBS-full -> SBS-half -> auto
    if (!layoutPinned_) {
        layoutPinned_ = true;
        layoutPinnedValue_ = StereoLayout::Mono;
    } else if (layoutPinnedValue_ == StereoLayout::Mono) {
        layoutPinnedValue_ = StereoLayout::SbsFull;
    } else if (layoutPinnedValue_ == StereoLayout::SbsFull) {
        layoutPinnedValue_ = StereoLayout::SbsHalf;
    } else {
        layoutPinned_ = false;
    }
    layoutForced_ = false;   // an explicit keypress retires the env-var seed

    // Re-derive in place. The cached autoInfo_ is exactly why returning to auto costs no
    // reload and no re-decode.
    if (layoutPinned_) {
        layout_ = layoutPinnedValue_;
        layoutSignal_ = StereoSignal::Manual;
        layoutConfidence_ = 1.0f;
    } else {
        layout_ = autoInfo_.layout;
        layoutSignal_ = autoInfo_.signal;
        layoutConfidence_ = autoInfo_.confidence;
        mediaEyeSwap_ = autoInfo_.eyeSwap;
    }
    contentAspect_ = PerEyeAspect(layout_, mediaW_, mediaH_);
    LOG_INFO("Layout override: %s", LayoutLabel().c_str());
    ShowToast(LayoutLabel());
}

void App::ReloadMedia(const std::string& path) {
    // Every file path (Open, drop, open_file, navigation) lands here, so this is the one
    // place a file replaces the live camera.
    const bool wasLive = isLive_ || isStream_;
    // "Convert to 3D" is off on every load. Put the source picture back first, so a load
    // that fails keeps showing the previous media as it was.
    StopConvert(/*restorePicture=*/true);
    if (isLive_) StopLive(/*showIdleIfEmpty=*/false);
    if (isStream_) StopStream(/*showIdleIfEmpty=*/false);  // #93: a file replaces the stream
    video_.Stop();   // joins the decode thread (which reads the audio clock) FIRST,
    audio_.Stop();   // then tear down audio so the clock callback can't outlive it.
#if defined(_WIN32)
    // Release zero-copy imports tied to the just-stopped decoder's shared textures (#28).
    renderer_.ClearSharedImports();
#endif
    isVideo_ = false;
    uiState_.scrubValue = 0.0f;
    uiState_.scrubActive = false;
    uiState_.scrubTarget = -1.0f;
    slideshowImageElapsed_ = 0.0;
    if (!LoadMedia(path)) {
        LOG_WARN("Open: keeping previous view (failed to open '%s')", path.c_str());
        if (wasLive) {
            // The "previous view" was a camera that is now closed: a frozen frame with no
            // source behind it would read as a hung feed. Fall back to the idle screen.
            hasMedia_ = false;
            mediaW_ = mediaH_ = 0;
            LoadIdleLogo();
        }
    }
}

// --- Live camera (#90) -----------------------------------------------------------------

void App::RefreshCameraList(bool rescan) {
    // A rescan re-inits SDL's camera subsystem, which CameraSource refuses while a device
    // is open. After an unplug the dead handle is still held, so the user's "Rescan" to
    // find the replugged camera would be refused forever: release it first. The last
    // frame stays on screen (the renderer keeps its last upload) and isLive_ stays set.
    // When the same device is back in the fresh list it is re-opened at once: a Rescan
    // after an unplug means "reconnect", not "show me a list" (David, first hardware run).
    bool reconnect = false;
    if (rescan && isLive_ && LiveIsCamera()) {
        const CameraSource::State st = camera_.GetState();
        if (st == CameraSource::State::Lost || st == CameraSource::State::Failed ||
            st == CameraSource::State::Denied) {
            LOG_INFO("Live: releasing '%s' (%s) before rescan", camera_.DeviceName().c_str(),
                     CameraSource::StateName(st));
            camera_.Stop();
            liveStateSeen_ = CameraSource::State::Closed;
        }
        reconnect = LiveIsCamera() && (camera_.GetState() == CameraSource::State::Closed) &&
                    !liveSelector_.empty();
    }
    cameraDevs_.clear();
    uiState_.cameraNames.clear();
    for (CameraDevice& d : CameraSource::Enumerate(rescan)) {
        if (d.denied) continue;  // the picker never even shows a tracking camera
        uiState_.cameraNames.push_back(d.name);
        cameraDevs_.push_back(std::move(d));
    }
    if (reconnect) {
        for (const CameraDevice& d : cameraDevs_) {
            if (d.name == liveSelector_) {
                LOG_INFO("Live: '%s' is back — reconnecting", d.name.c_str());
                const CameraDevice dev = d;  // copy: LoadLiveDevice may not touch the list
                LoadLiveDevice(dev);
                return;
            }
        }
        LOG_WARN("Live: '%s' not found after rescan", liveSelector_.c_str());
        ShowToast("Camera not found - plug it in (webcam mode) and Rescan");
    }
}

bool App::LoadLive(const std::string& selector) {
    RefreshCameraList(false);
    const int idx = SelectCamera(cameraDevs_, selector);
    if (idx < 0) {
        LOG_WARN("Live: no camera matches '%s' (%zu selectable)",
                 selector.empty() ? "auto" : selector.c_str(), cameraDevs_.size());
        ShowToast("No camera found");
        return false;
    }
    const CameraDevice dev = cameraDevs_[(size_t)idx];
    return LoadLiveDevice(dev);
}

bool App::LoadLiveDevice(const CameraDevice& dev) {
    // Open FIRST (it validates synchronously and returns at once — the device itself
    // opens on the worker): a refused device leaves the current media playing.
    if (!camera_.Open(dev, 0, 0, launch_.cameraFps)) {
        ShowToast("Camera: " + camera_.LastError());
        if (isLive_ && LiveIsCamera()) StopLive(/*showIdleIfEmpty=*/true);  // closed by Open
        return false;
    }
    liveSelector_ = dev.name;
    EnterLive(&camera_, dev.name);
    return true;
}

void App::EnterLive(LiveProducer* p, const std::string& name) {
    StopConvert(/*restorePicture=*/true);  // off on every source change
    KeepStillForConvert(nullptr, 0, 0);
#if MEDIAPLAYER_PRO
    pro_.OnStill(nullptr, 0, 0, layout_);  // scopes follow the frames again
#endif
    // One live producer at a time: a previous one (the camera, or a Pro input) stops.
    if (live_ != p && isLive_) live_->Stop();
    if (p != &camera_) camera_.Stop();
    live_ = p;
    if (isStream_) StopStream(/*showIdleIfEmpty=*/false);  // #93: the camera replaces it
    video_.Stop();   // joins the decode thread before audio (see ReloadMedia)
    audio_.Stop();
#if defined(_WIN32)
    renderer_.ClearSharedImports();
#endif
    isLive_ = true;
    livePaused_ = false;
    isVideo_ = false;
    hasMedia_ = true;
    // No folder behind a camera: ↑/↓ and the slideshow have nothing to step through.
    currentMediaPath_.clear();
    folderFiles_.clear();
    folderIndex_ = 0;
    playlistFromDrop_ = false;
    if (uiState_.slideshowActive) SetSlideshow(false);
    uiState_.scrubValue = 0.0f;
    uiState_.scrubActive = false;
    uiState_.scrubTarget = -1.0f;
    slideshowImageElapsed_ = 0.0;
    // Raw footage: no baked reconvergence; the user's -/= convergence still applies.
    mediaConvergence_ = 0.0f;
    mediaAutoConvAvailable_ = false;
    mediaEyeSwap_ = false;
    // Unknown until the first frame — OnLiveFrame settles the layout (and leaves the idle
    // screen) then, so the previous picture stays up for the ~0.5 s the device takes.
    mediaW_ = mediaH_ = 0;
    liveAgeMs_ = 0.0;
    liveSlowSince_ = -1.0;
    liveSlowWarned_ = false;
    liveStateSeen_ = CameraSource::State::Opening;
    autoConvWorker_.RequestReset();  // #92: a new camera is a new scene
    LOG_INFO("Live: opening %s '%s'", p->Kind(), name.c_str());
    ShowToast("Live: " + name);
}

void App::StopLive(bool showIdleIfEmpty) {
    live_->Stop();
    autoConvWorker_.RequestReset();  // #92: the next source is a new scene
    isLive_ = false;
    livePaused_ = false;
    liveStateSeen_ = CameraSource::State::Closed;
    liveAgeMs_ = 0.0;
    if (showIdleIfEmpty) {
        hasMedia_ = false;
        isVideo_ = false;
        mediaW_ = mediaH_ = 0;
        LoadIdleLogo();
    }
}

void App::ToggleLive() {
    if (isLive_) {
        // 'C' on a lost/blocked camera = reconnect (rescan + re-open the same device),
        // not "live off": the user is trying to get the picture back.
        const CameraSource::State st = live_->GetState();
        if (LiveIsCamera() && (st == CameraSource::State::Lost || st == CameraSource::State::Failed ||
                               st == CameraSource::State::Denied || st == CameraSource::State::Closed)) {
            RefreshCameraList(/*rescan=*/true);
            return;
        }
        StopLive(/*showIdleIfEmpty=*/true);
        ShowToast("Live off");
        return;
    }
    LoadLive(liveSelector_.empty() ? launch_.cameraSelector : liveSelector_);
}

std::string App::LiveStatusText() const {
    if (!isLive_) return std::string();
    switch (live_->GetState()) {
        case CameraSource::State::Opening: return "OPENING";
        case CameraSource::State::Streaming: return livePaused_ ? "FROZEN" : "LIVE";
        case CameraSource::State::Stalled: return "NO SIGNAL";
        case CameraSource::State::Lost: return LiveIsCamera() ? "NO CAMERA" : "NO INPUT";
        case CameraSource::State::Denied: return "BLOCKED";
        case CameraSource::State::Failed: return "ERROR";
        case CameraSource::State::Closed: break;  // released after a loss (see RefreshCameraList)
    }
    return "NO CAMERA";
}

void App::OnLiveFrame(const FrameRing::Frame& f) {
    liveAgeMs_ = (CameraSource::NowSeconds() - live_->GetStats().lastPublishSec) * 1000.0;
    SettleLiveLayout(f, "Live");
}

void App::SettleLiveLayout(const FrameRing::Frame& f, const char* tag) {
    if (f.width == mediaW_ && f.height == mediaH_) return;
    // First frame, or a mid-stream size change (a capture box switching inputs): settle
    // the layout from the frame aspect. No content detector and no metadata for a live
    // feed — 3840x2160 (the Eyes) is 16:9, which ChooseFullOrHalf reads as SBS-half.
    const bool firstFrame = (mediaW_ == 0);
    if (firstFrame) {
        ClearIdleLogo();
    } else {
        ResetConvertStream();  // a size change is a discontinuity for the converter
    }
    mediaW_ = f.width;
    mediaH_ = f.height;
    bool ambiguous = false;
    MediaInfo li;
    li.kind = MediaKind::Video;
    li.signal = StereoSignal::Aspect;
    li.confidence = 0.5f;
    li.layout = StereoDetect::ChooseFullOrHalf((float)mediaW_ / (float)mediaH_, ambiguous);
    // A live source that knows its packing (two inputs side by side, a mono input) says
    // so; the aspect guess is only for sources that cannot tell (the camera: -1).
    if (isLive_ && live_->LayoutHint() >= 0) {
        li.layout = (StereoLayout)live_->LayoutHint();
        li.signal = StereoSignal::Metadata;
        li.confidence = 1.0f;
    }
    autoInfo_ = li;  // ApplyLayout skips autoInfo_ for Manual; L -> auto must land here
    if (layoutForced_) {  // MEDIAPLAYER_LAYOUT
        li.layout = layoutForcedValue_;
        li.signal = StereoSignal::Manual;
        li.confidence = 1.0f;
    }
    const bool keepPin = layoutPinned_ && !firstFrame;  // a size change keeps a user pin
    const StereoLayout pinned = layoutPinnedValue_;
    ApplyLayout(li);  // clears layoutPinned_
    if (layoutForced_) {
        layoutPinned_ = true;
        layoutPinnedValue_ = layoutForcedValue_;
    }
    if (keepPin) {
        layoutPinned_ = true;
        layoutPinnedValue_ = pinned;
        layout_ = pinned;
        layoutSignal_ = StereoSignal::Manual;
    }
    contentAspect_ = PerEyeAspect(layout_, mediaW_, mediaH_);
    LOG_INFO("%s: %dx%d %s frames, %s, per-eye aspect %.3f", tag, mediaW_, mediaH_,
             f.format == PixFormat::NV12 ? "NV12" : "I420", LayoutLabel().c_str(),
             contentAspect_);
}

// --- Stream URL (#93) ------------------------------------------------------------------

bool App::LoadUrl(const std::string& url) {
    const stream::UrlVerdict v = stream::ClassifyUrl(url);
    const std::string shown = stream::RedactUrl(url);
    if (v.cls == stream::UrlClass::Unsupported) {
        LOG_WARN("Stream: refusing '%s': %s", shown.c_str(), v.reason.c_str());
        ShowToast(v.reason);
        return false;
    }
    if (v.cls == stream::UrlClass::File) {
        LOG_WARN("Stream: '%s' is not a stream URL", shown.c_str());
        ShowToast("Not a stream URL: " + shown);
        return false;
    }
    uiState_.urlSeed = url;  // remembered for the popup, even if the connect fails
    StopConvert(/*restorePicture=*/true);  // off on every source change
    KeepStillForConvert(nullptr, 0, 0);
#if MEDIAPLAYER_PRO
    pro_.OnStill(nullptr, 0, 0, layout_);  // scopes follow the frames again
#endif
    if (isLive_) StopLive(/*showIdleIfEmpty=*/false);
    video_.Stop();   // joins the decode thread (reads the audio clock) before audio
    audio_.Stop();
#if defined(_WIN32)
    renderer_.ClearSharedImports();
#endif
    if (!video_.OpenLive(url)) {  // only without FFmpeg
        ShowToast("Streams unavailable in this build");
        isStream_ = false;
        hasMedia_ = false;
        mediaW_ = mediaH_ = 0;
        LoadIdleLogo();
        return false;
    }
    isStream_ = true;
    autoConvWorker_.RequestReset();  // a new stream is a new scene
    streamPaused_ = false;
    streamUrl_ = url;
    isVideo_ = false;
    hasMedia_ = true;
    // No folder behind a stream: arrows and the slideshow have nothing to step through.
    currentMediaPath_.clear();
    folderFiles_.clear();
    folderIndex_ = 0;
    playlistFromDrop_ = false;
    if (uiState_.slideshowActive) SetSlideshow(false);
    uiState_.scrubValue = 0.0f;
    uiState_.scrubActive = false;
    uiState_.scrubTarget = -1.0f;
    slideshowImageElapsed_ = 0.0;
    mediaConvergence_ = 0.0f;
    mediaAutoConvAvailable_ = false;
    mediaEyeSwap_ = false;
    // Layout is settled from the first frame (SettleLiveLayout); until then the previous
    // picture stays up, exactly like the camera.
    mediaW_ = mediaH_ = 0;
    streamStateSeen_ = VideoDecoder::StreamState::Connecting;
    streamLogAt_ = 0.0;
    LOG_INFO("Stream: connecting to '%s'", shown.c_str());
    ShowToast("Connecting: " + stream::UrlHost(url));
    return true;
}

void App::StopStream(bool showIdleIfEmpty) {
    video_.Stop();
    isStream_ = false;
    streamPaused_ = false;
    streamStateSeen_ = VideoDecoder::StreamState::Idle;
    if (showIdleIfEmpty) {
        hasMedia_ = false;
        isVideo_ = false;
        mediaW_ = mediaH_ = 0;
        LoadIdleLogo();
    }
}

void App::OpenUrlPrompt() {
    showHud_ = true;  // the popup lives on the HUD layer
    uiState_.urlPopupRequest = true;
}

std::string App::StreamStatusText() const {
    if (!isStream_) return std::string();
    using S = VideoDecoder::StreamState;
    switch (video_.GetStreamState()) {
        case S::Streaming: return streamPaused_ ? "FROZEN" : "STREAM";
        case S::Connecting: return "CONNECTING";
        case S::Reconnecting: return "RECONNECTING";
        case S::Failed: return "FAILED";
        case S::Idle: break;
    }
    return "FAILED";
}

void App::PollStream() {
    if (!isStream_) return;
    using S = VideoDecoder::StreamState;
    const S st = video_.GetStreamState();
    const std::string host = stream::UrlHost(streamUrl_);
    if (st != streamStateSeen_) {
        const S was = streamStateSeen_;
        streamStateSeen_ = st;
        const VideoDecoder::StreamStats ss = video_.GetStreamStats();
        switch (st) {
            case S::Streaming:
                LOG_INFO("Stream: '%s' streaming %dx%d %s/%s, first frame %.0f ms after open, "
                         "reconnects=%d",
                         host.c_str(), ss.width, ss.height, ss.codec.c_str(), ss.backend.c_str(),
                         ss.connectMs, ss.reconnects);
                if (was == S::Reconnecting) ShowToast("Stream back: " + host);
                break;
            case S::Reconnecting:
                LOG_WARN("Stream: '%s' lost (%s) - reconnecting", host.c_str(),
                         ss.lastError.c_str());
                ShowToast("Stream lost - reconnecting");
                break;
            case S::Failed:
                LOG_WARN("Stream: '%s' failed: %s", host.c_str(), ss.lastError.c_str());
                ShowToast("Stream: " + (ss.lastError.empty() ? std::string("failed") : ss.lastError));
                // Never got a frame: nothing to freeze on, back to the idle screen.
                if (mediaW_ == 0) StopStream(/*showIdleIfEmpty=*/true);
                break;
            default:
                break;
        }
    }
    // Throttled stats line (every 5 s while streaming) — the log is the headless hook.
    if (isStream_ && st == S::Streaming) {
        const double now = CameraSource::NowSeconds();
        if (now >= streamLogAt_) {
            if (streamLogAt_ > 0.0) {
                const VideoDecoder::StreamStats ss = video_.GetStreamStats();
                LOG_INFO("Stream: %s %dx%d in %.1f fps %.0f kbit/s, panel %.0f fps, frames=%llu "
                         "dropped=%llu reconnects=%d, %s",
                         host.c_str(), ss.width, ss.height, ss.fpsIn, ss.kbps, fps_,
                         (unsigned long long)ss.frames, (unsigned long long)ss.dropped,
                         ss.reconnects, LayoutLabel().c_str());
            }
            streamLogAt_ = now + 5.0;
        }
    }
}

void App::PollLive() {
    if (!isLive_) return;
    const CameraSource::State st = live_->GetState();
    const double now = CameraSource::NowSeconds();
    const bool cam = LiveIsCamera();
    if (st != liveStateSeen_) {
        const CameraSource::State was = liveStateSeen_;
        liveStateSeen_ = st;
        using S = CameraSource::State;
        switch (st) {
            case S::Streaming: {
                const CameraSource::Stats cs = live_->GetStats();
                LOG_INFO("Live: '%s' streaming %dx%d NV12 @%d requested (%s range)%s",
                         live_->DeviceName().c_str(), cs.w, cs.h, cs.fpsReq,
                         cs.fullRange ? "full" : "limited",
                         cs.mjpgPath ? " via MJPG decoders" : "");
                if (was == S::Stalled) ShowToast("Live");
                break;
            }
            case S::Stalled:
                ShowToast(cam ? "No camera signal" : "No input signal");
                break;
            case S::Lost:
                ShowToast(cam ? "Camera disconnected" : "Input lost");
                break;
            case S::Denied:
            case S::Failed: {
                const std::string err = live_->LastError();
                const bool denied = (st == S::Denied);
                LOG_WARN("Live: '%s' %s: %s", live_->DeviceName().c_str(),
                         denied ? "blocked" : "failed", err.c_str());
                ShowToast(denied ? "Camera blocked by Windows privacy settings - "
                                   "Settings > Privacy & security > Camera"
                                 : std::string(cam ? "Camera: " : "Input: ") + err);
                // Never got a frame: there is nothing to freeze on, so go back to idle
                // rather than leave a dead source up. After frames, keep the last one.
                if (mediaW_ == 0) StopLive(/*showIdleIfEmpty=*/true);
                break;
            }
            default:
                break;
        }
    }
    // Ladder rung 4: the device delivers well under what was asked (CPU-bound decode on
    // a weak box). Hint once; no automatic re-open (that re-negotiates the device).
    if (isLive_ && cam && st == CameraSource::State::Streaming && !liveSlowWarned_) {
        const CameraSource::Stats cs = camera_.GetStats();
        const bool warm = cs.fpsReq > 0 && cs.published > (uint64_t)cs.fpsReq * 2;
        if (warm && cs.deliveredFps < 0.8f * (float)cs.fpsReq) {
            if (liveSlowSince_ < 0.0) liveSlowSince_ = now;
            if (now - liveSlowSince_ >= 3.0) {
                liveSlowWarned_ = true;
                LOG_WARN("Live: camera delivers %.0f of %d fps requested", cs.deliveredFps,
                         cs.fpsReq);
                char t[96];
                std::snprintf(t, sizeof(t), "Camera: CPU-bound at %.0f fps; try --camera-fps=30",
                              cs.deliveredFps);
                ShowToast(t);
            }
        } else {
            liveSlowSince_ = -1.0;
        }
    }
}

// --- Auto-convergence (#92) ----------------------------------------------------------

void App::SetAutoConv(bool on, const char* why) {
    if (on == autoConvEnabled_) return;
    autoConvEnabled_ = on;
    if (on) {
        autoConvWorker_.Start();  // idles on a condition variable while off
        autoConvWorker_.RequestReset();
        autoConvHaveResult_ = false;
        autoConvLastSubmitMs_ = -1e9;
        autoConvMin_ = autoConvMax_ = autoConvergence_;
        autoConvSum_ = autoConvMsSum_ = autoConvMsMax_ = 0.0;
        autoConvTicks_ = autoConvMeasures_ = 0;
    }
    LOG_INFO("auto-conv %s (%s, policy %s)", on ? "ON" : "OFF", why,
             autoconv::PolicyName(autoConvPolicy_));
    ShowToast(on ? std::string("Auto-convergence: ") + autoconv::PolicyName(autoConvPolicy_)
                 : std::string("Auto-convergence off"));
}

void App::SetAutoConvPolicy(autoconv::SubjectPolicy p) {
    autoConvPolicy_ = p;
    if (p != autoconv::SubjectPolicy::Focus) autoConvHasFocus_ = false;
    LOG_INFO("auto-conv policy %s", autoconv::PolicyName(p));
    ShowToast(std::string("Auto-conv policy: ") + autoconv::PolicyName(p));
}

void App::SetAutoConvFocus(float u, float v) {
    autoConvFocus_[0] = std::min(1.0f, std::max(0.0f, u));
    autoConvFocus_[1] = std::min(1.0f, std::max(0.0f, v));
    autoConvHasFocus_ = true;
    autoConvPolicy_ = autoconv::SubjectPolicy::Focus;
    LOG_INFO("auto-conv focus pinned at (%.3f, %.3f)", autoConvFocus_[0], autoConvFocus_[1]);
    if (!autoConvEnabled_) SetAutoConv(true, "focus");
    else ShowToast("Auto-conv: subject pinned");
}

void App::SampleAutoConv(const FrameRing::Frame& f) {
    if (!autoConvEnabled_ || !LiveSource() || LivePaused()) return;
    if (layout_ != StereoLayout::SbsFull && layout_ != StereoLayout::SbsHalf) return;
    if (f.gpu || f.plane[0].empty() || f.width < 64 || f.height < 32) return;
    const double nowMs = CameraSource::NowSeconds() * 1000.0;
    if (nowMs - autoConvLastSubmitMs_ < 200.0) return;  // ~5 Hz
    autoConvLastSubmitMs_ = nowMs;
    // Measure on the halves the viewer's eyes actually see (X / container swap).
    const bool swap = (swapEyes_ != mediaEyeSwap_);
    const bool focus = autoConvPolicy_ == autoconv::SubjectPolicy::Focus && autoConvHasFocus_;
    const uint8_t* luma = f.plane[0].data();
    if (f.format == PixFormat::P016) {  // 16-bit (Pro's 10-bit path): the measurement is 8-bit
        const size_t n = (size_t)f.width * (size_t)f.height;
        autoConvLuma8_.resize(n);
        const uint16_t* s16 = reinterpret_cast<const uint16_t*>(f.plane[0].data());
        for (size_t i = 0; i < n; ++i) autoConvLuma8_[i] = (uint8_t)(s16[i] >> 8);
        luma = autoConvLuma8_.data();
    }
    autoConvDownsampleMs_ =
        autoConvWorker_.Submit(luma, f.width, f.width, f.height, swap,
                               autoConvPolicy_, focus ? autoConvFocus_ : nullptr, nowMs);
}

void App::TickAutoConv() {
    const double now = CameraSource::NowSeconds();
    double dt = autoConvLastTickSec_ < 0.0 ? 0.0 : now - autoConvLastTickSec_;
    autoConvLastTickSec_ = now;
    if (dt > 0.25) dt = 0.25;  // a stall is not a reason to jump
    autoconv::Result r;
    if (autoConvWorker_.Running() && autoConvWorker_.TakeResult(r)) {
        autoConvLast_ = r;
        autoConvHaveResult_ = true;
        ++autoConvMeasures_;
        autoConvMsSum_ += r.ms;
        autoConvMsMax_ = std::max(autoConvMsMax_, r.ms);
        if (autoConvEnabled_ && LiveSource()) autoConvCtl_.Update(r);
    }
    const bool stereo = layout_ == StereoLayout::SbsFull || layout_ == StereoLayout::SbsHalf;
    autoConvCtl_.Tick(dt, autoConvEnabled_ && LiveSource() && stereo);
    autoConvergence_ = autoConvCtl_.Value();
    if (!autoConvEnabled_) return;
    autoConvMin_ = std::min(autoConvMin_, autoConvergence_);
    autoConvMax_ = std::max(autoConvMax_, autoConvergence_);
    autoConvSum_ += autoConvergence_;
    ++autoConvTicks_;
    if (now - autoConvLastLogSec_ >= 1.0) {
        autoConvLastLogSec_ = now;
        const autoconv::Result& l = autoConvLast_;
        LOG_INFO("autoconv: %s d=%+.2f%% conv=%+.3f%% target=%+.3f%% ncc %.2f blocks %d "
                 "rule %s%s%s ok=%d meas %.2f ms (max %.2f) ds %.2f ms near %+.2f%% far %+.2f%% "
                 "mad %.1f eye %dx%d",
                 autoconv::PolicyName(autoConvPolicy_), l.disparityFrac * 100.0f,
                 autoConvergence_ * 100.0f, autoConvCtl_.Target() * 100.0f, l.ncc,
                 l.blocksMatched, l.rule, *autoConvCtl_.Clamp() ? " clamp-" : "",
                 autoConvCtl_.Clamp(), l.ok ? 1 : 0, l.ms, autoConvMsMax_, autoConvDownsampleMs_,
                 l.nearFrac * 100.0f, l.farFrac * 100.0f, l.sceneMad, l.eyeW, l.eyeH);
    }
}

std::string App::AutoConvHudText() const {
    char t[128];
    if (!autoConvEnabled_) {
        std::snprintf(t, sizeof(t), "autoconv OFF");
    } else {
        std::snprintf(t, sizeof(t), "autoconv ON %s  d=%+.1f%%  conv=%+.1f%%  ncc %.2f  %.1f ms",
                      autoconv::PolicyName(autoConvPolicy_), autoConvLast_.disparityFrac * 100.0f,
                      autoConvergence_ * 100.0f, autoConvLast_.ncc, autoConvLast_.ms);
    }
    return t;
}

void App::HandleDroppedPaths(std::vector<std::string> paths) {
    namespace fs = std::filesystem;

    const size_t dropped = paths.size();
    // A dropped FOLDER means the same thing as a folder on the command line, so it is
    // expanded the same way (non-recursive, sorted, supported files only) — see the argv
    // branch in Init(). Without this a folder fails the extension test in IsSupported()
    // and is rejected as an unsupported file, which is the one drop gesture a media
    // player with folder navigation should obviously accept.
    const bool droppedOneFolder =
        dropped == 1 && [&] { std::error_code ec; return fs::is_directory(paths[0], ec) && !ec; }();

    std::vector<std::string> good;
    good.reserve(dropped);
    size_t rejected = 0;
    for (auto& p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec) && !ec) {
            std::vector<std::string> inFolder;
            for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code fe;
                if (!it->is_regular_file(fe) || fe) continue;
                const std::string s = it->path().string();
                if (MediaSource::IsSupported(s)) inFolder.push_back(s);
            }
            std::sort(inFolder.begin(), inFolder.end());   // folders contribute in name order
            if (inFolder.empty()) ++rejected;              // an empty folder is a rejected item
            good.insert(good.end(), std::make_move_iterator(inFolder.begin()),
                        std::make_move_iterator(inFolder.end()));
            continue;
        }
#if MEDIAPLAYER_PRO
        if (pro_.HandleDroppedPath(p)) continue;  // a .cube / CDL loads as a look, not media
#endif
        if (MediaSource::IsSupported(p)) good.push_back(std::move(p));  // keep drop order
        else ++rejected;
    }
    if (good.empty()) {
        // Never reject silently — the user aimed at the window and nothing happened.
        LOG_INFO("Drop: %zu item(s), none supported", dropped);
        ShowToast(droppedOneFolder      ? "No supported media in folder"
                  : dropped == 1        ? "Unsupported file type"
                                        : "No supported files in drop");
        return;
    }

    if (droppedOneFolder) {
        // Exactly the argv-folder gesture: open the first asset and let the normal
        // directory scan govern ↑/↓, so the folder itself becomes the nav list.
        playlistFromDrop_ = false;
        LOG_INFO("Drop: folder '%s', %zu asset(s), opening first", paths[0].c_str(), good.size());
        ReloadMedia(good.front());
        // Dropping a folder means "play this folder" — so it starts the slideshow, unlike
        // dropping a single file. SetSlideshow (not ToggleSlideshow) so a second folder
        // drop during a running slideshow keeps it running instead of stopping it, and it
        // runs AFTER the load because it reads isVideo_ to set play-once on a clip.
        SetSlideshow(true);
        char t[64];
        std::snprintf(t, sizeof(t), "Slideshow: %zu items", good.size());
        ShowToast(t);   // replaces SetSlideshow's generic "Slideshow on"
        return;
    }

    if (good.size() == 1) {
        // A single drop is the same gesture as Ctrl+O: load it and let the normal
        // directory scan govern ↑/↓, so the user can walk the containing folder.
        playlistFromDrop_ = false;
        LOG_INFO("Drop: loading '%s'", good[0].c_str());
        if (rejected > 0) {
            char t[64];
            std::snprintf(t, sizeof(t), "Loaded 1 of %zu (%zu unsupported)", dropped, rejected);
            ShowToast(t);
        }
        ReloadMedia(good[0]);
        return;
    }

    // Multi-file drop: the dropped set IS the nav list. Install it BEFORE loading —
    // RebuildFolderList() sees playlistFromDrop_ and re-locates the index inside it
    // rather than rescanning the parent directory.
    folderFiles_ = std::move(good);
    folderIndex_ = 0;
    playlistFromDrop_ = true;
    LOG_INFO("Drop: playlist of %zu asset(s), %zu unsupported", folderFiles_.size(), rejected);
    char t[80];
    if (rejected > 0) {
        std::snprintf(t, sizeof(t), "Playlist: %zu items (%zu unsupported)",
                      folderFiles_.size(), rejected);
    } else {
        std::snprintf(t, sizeof(t), "Playlist: %zu items", folderFiles_.size());
    }
    ShowToast(t);
    ReloadMedia(folderFiles_[0]);
}

void App::RebuildFolderList(std::string path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(path), ec);
    if (ec) p = fs::path(path);
    if (playlistFromDrop_) {
        // A dropped playlist replaces the directory scan (#44) — only re-locate the
        // current entry. NOTE: folderFiles_ must NOT be cleared here, and `path` may
        // alias into it, which is exactly why this runs before any mutation.
        for (size_t i = 0; i < folderFiles_.size(); ++i) {
            std::error_code e2;
            if (fs::equivalent(fs::path(folderFiles_[i]), p, e2) && !e2) {
                folderIndex_ = i;
                break;
            }
        }
        LOG_INFO("Playlist: %zu asset(s), current index %zu", folderFiles_.size(), folderIndex_);
        return;
    }
    folderFiles_.clear();   // frees any string `path` might have aliased — it's a copy now
    folderIndex_ = 0;
    for (fs::directory_iterator it(p.parent_path(), ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const std::string s = it->path().string();
        if (MediaSource::IsSupported(s)) folderFiles_.push_back(s);
    }
    std::sort(folderFiles_.begin(), folderFiles_.end());
    {
        // An EXR / DPX / TIFF sequence (Pro) is one clip: keep its first frame only. The
        // current file stays listed so the index below still finds it.
        std::vector<std::string> kept;
        std::vector<std::string> seen;  // sequence keys already listed (a folder may interleave two)
        for (const std::string& f : folderFiles_) {
            const std::string k = imageseq::Key(f);
            std::error_code e3;
            const bool dup = !k.empty() && std::find(seen.begin(), seen.end(), k) != seen.end();
            if (dup && !fs::equivalent(fs::path(f), p, e3)) continue;
            if (!k.empty() && !dup) seen.push_back(k);
            kept.push_back(f);
        }
        folderFiles_.swap(kept);
    }
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

void App::ShowToast(const std::string& msg) { ui::ShowTransportToast(uiState_, msg); }

void App::ToggleSlideshow() { SetSlideshow(!uiState_.slideshowActive); }

void App::SetSlideshow(bool on) {
    if (on && (isLive_ || isStream_)) {
        ShowToast("Slideshow unavailable while live");
        return;
    }
    uiState_.slideshowActive = on;
    slideshowImageElapsed_ = 0.0;
    transition_ = Transition::Playing;
    if (uiState_.slideshowActive && isVideo_) video_.SetLoop(false);  // play once, then advance
    LOG_INFO("slideshow %s", uiState_.slideshowActive ? "on" : "off");
    ShowToast(uiState_.slideshowActive ? "Slideshow on" : "Slideshow off");
}

void App::LoadIdleLogo() {
    // Resolve <exe-dir>/displayxr/ — the sidecar staged next to the binary (same folder
    // as the workspace manifest). SDL_GetBasePath gives the executable's directory; the
    // returned string is owned by SDL (do not free). Prefer the composed idle lockup
    // (DisplayXR mark + bold "Media Player" label, pre-padded as a square) and fall back
    // to the bare brand mark, then to the RED|BLUE test pattern if neither is present.
    StopConvert(/*restorePicture=*/false);  // nothing to convert on the idle screen
    KeepStillForConvert(nullptr, 0, 0);
#if MEDIAPLAYER_PRO
    pro_.OnStill(nullptr, 0, 0, layout_);  // scopes follow the frames again
#endif
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
            RequestFlatModeForIdle();
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
    // Hand back the mode the idle screen borrowed — but only if it is still the one
    // we asked for. If the user pressed V (or the Mode button) while the logo was up,
    // that is their choice and media inherits it.
    if (idleModeRequested_ >= 0 && pendingModeRequest_ == idleModeRequested_) {
        // The borrow was never sent (media replaced the logo before the first frame,
        // e.g. a live input opened during Init): cancel it rather than go flat.
        pendingModeRequest_ = -1;
    } else if (modeBeforeIdle_ >= 0 && idleModeRequested_ >= 0 &&
               (xr_.CurrentModeIndex() == (uint32_t)idleModeRequested_ ||
                xr_.CurrentModeIndex() == (uint32_t)modeBeforeIdle_)) {  // sent, change not yet reported
        pendingModeRequest_ = modeBeforeIdle_;
    }
    modeBeforeIdle_ = -1;
    idleModeRequested_ = -1;
}

void App::RequestFlatModeForIdle() {
    // MEDIAPLAYER_START_MODE is an explicit test pin: it owns the mode outright.
    if (startMode_ >= 0) return;
    const int32_t flat = xr_.FindFlatMode();
    if (flat < 0) return;  // no flat mode on offer (or workspace-locked) — leave it
    if ((uint32_t)flat == xr_.CurrentModeIndex()) return;  // already flat
    modeBeforeIdle_ = (int32_t)xr_.CurrentModeIndex();
    idleModeRequested_ = flat;
    pendingModeRequest_ = flat;
}

void App::TogglePlayback() {
    if (isLive_) {
        // Freeze, not pause: the camera keeps streaming (no re-negotiation on resume);
        // RenderOneFrame just stops acquiring, so the last frame stays up.
        livePaused_ = !livePaused_;
        LOG_INFO("live %s", livePaused_ ? "frozen" : "resumed");
        ShowToast(livePaused_ ? "Live - frozen" : "Live");
        return;
    }
    if (isStream_) {
        // Same freeze as the camera: the stream keeps decoding (no reconnect on resume).
        streamPaused_ = !streamPaused_;
        LOG_INFO("stream %s", streamPaused_ ? "frozen" : "resumed");
        ShowToast(streamPaused_ ? "Stream - frozen" : "Stream");
        return;
    }
    if (!isVideo_) return;
    if (video_.Ended()) {
        video_.Seek(0.0);          // restart a clip that already ran to the end
        ResetConvertStream();
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
    ResetConvertStream();
    audio_.Seek(t);             // keep audio aligned for the eventual resume
    uiState_.scrubTarget = (float)t;    // hold the scrubber knob until the frame lands
}

// --- Convert to 3D (XR_DXR_lift) ---------------------------------------------------------

bool App::ConvertEligible() const {
    // Mono content only (a plain still, a mono LIF, a mono video, a live source settled
    // mono — or anything the user pinned Mono with L). The idle screen never converts.
    return hasMedia_ && !isLogo_ && layout_ == StereoLayout::Mono && mediaW_ > 0 && mediaH_ > 0;
}

void App::KeepStillForConvert(const uint8_t* rgba, int w, int h) {
    // Only when the runtime offers the extension: a runtime without it costs no copy.
    convertStill_.clear();
    convertStill_.shrink_to_fit();
    convertStillW_ = convertStillH_ = 0;
    if (!rgba || w <= 0 || h <= 0 || lift_.GetAvailability() == lift::Availability::Hidden)
        return;
    convertStill_.assign(rgba, rgba + (size_t)w * h * 4);
    convertStillW_ = w;
    convertStillH_ = h;
}

bool App::SetConvert(bool on, const char* why) {
    if (!on) {
        if (!lift_.On() && !convertBound_) return true;
        // A paused clip decodes nothing new: re-decode the frame on screen so the full-
        // resolution source replaces the converted picture (StopConvert's interim is the
        // downscaled frame last handed to the converter).
        const bool pausedVideo = isVideo_ && convertBound_ && (video_.Paused() || video_.Ended());
        StopConvert(/*restorePicture=*/true);
        if (pausedVideo) {
            const double fr = video_.FrameRate();
            const double fd = (fr > 1.0) ? 1.0 / fr : 1.0 / 30.0;
            video_.Seek(std::max(0.0, video_.PositionSeconds() - 0.5 * fd));
        }
        LOG_INFO("Convert to 3D off (%s)", why);
        ShowToast("Convert to 3D: off");
        return true;
    }
    if (lift_.GetAvailability() == lift::Availability::Hidden) {
        ShowToast("2D->3D conversion is not available");
        return false;
    }
    if (!ConvertEligible()) {
        ShowToast("Convert to 3D needs 2D (mono) content");
        return false;
    }
    const bool video = isVideo_ || LiveSource();
    if (!video && convertStill_.empty()) {
        ShowToast("Convert to 3D: reopen the picture");
        return false;
    }
    if (!lift_.Start(video)) {
        const std::string reason = lift_.Reason();
        lift_.Stop();  // the toggle stays off; the reason is the answer
        LOG_INFO("Convert to 3D refused (%s): %s", why, reason.c_str());
        ShowToast(reason.empty() ? std::string("Convert to 3D is unavailable") : reason);
        return false;
    }
    // A converted picture wants the panel in 3D: on a flat panel ask for the stereo mode,
    // exactly as V would (and count it as the user's mode choice). Turning the toggle off
    // leaves the mode alone; otherwise the player keeps the mode as it finds it.
    if (xr_.CurrentModeIsFlat()) {
        const int32_t m = xr_.FindStereoMode();
        if (m >= 0) {
            pendingModeRequest_ = m;
            LOG_INFO("Convert to 3D: flat panel - requesting 3D mode [%d]", m);
        }
    }
    userModeChoice_ = true;
    if (!video) {
        // A still converts once: downscale its kept pixels now, PumpConvert submits them
        // (and again only if no result came back within the stale period).
        lift::FitExtent((uint32_t)convertStillW_, (uint32_t)convertStillH_,
                        lift::LiftClient::kMaxInputLongEdge, convertInW_, convertInH_);
        lift::ScaleRgba(convertStill_.data(), (uint32_t)convertStillW_, (uint32_t)convertStillH_,
                        (uint32_t)convertStillW_ * 4, convertInW_, convertInH_, convertIn_);
        convertStillPending_ = true;
    }
    LOG_INFO("Convert to 3D on (%s): %s, task %s", why, video ? "video" : "still",
             lift::TaskStateName(lift_.TaskState()));
    ShowToast(lift_.GetState() == lift::State::Warming ? "Convert to 3D (warming)"
                                                       : "Convert to 3D");
    return true;
}

std::string App::SetConvertProvider(const std::string& id, const char* why) {
    const std::string err = lift_.SetPreferred(id);
    if (!err.empty()) {
        LOG_INFO("Convert to 3D: provider pick refused (%s): %s", why, err.c_str());
        ShowToast(err);
        return err;
    }
    // A live stream was re-created with the hint; a still has to be handed over again.
    if (lift_.On() && !isVideo_ && !LiveSource()) convertStillPending_ = true;
    const int i = lift_.EffectiveCandidate();
    const std::string name = i >= 0 ? lift_.Candidates()[i].name : std::string("default");
    LOG_INFO("Convert to 3D: provider %s (%s)", lift_.Preferred().empty() ? "default" : "picked",
             why);
    ShowToast("2D->3D: " + name);
    return std::string();
}

std::vector<XrInputRoleFamilyDXR> App::InputFamiliesShown() const {
    std::vector<XrInputRoleFamilyDXR> out;
    for (XrInputRoleFamilyDXR f : {XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR,
                                   XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR,
                                   XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR})
        out.push_back(f);  // every family; one nobody supplies draws greyed
    return out;
}

std::vector<int> App::InputRowProviders(XrInputRoleFamilyDXR family) const {
    std::vector<int> map{-1};  // row 0 = Default
    const XrInputRoleFamilyFlagsDXR bit = input::FamilyBit(family);
    for (size_t i = 0; i < input_.Providers().size(); ++i)
        if (input_.Providers()[i].supported & bit) map.push_back((int)i);
    return map;
}

std::string App::SetInputProvider(XrInputRoleFamilyDXR family, const std::string& id, bool persist,
                                  const char* why) {
    const std::string err = input_.SetPreference(family, id, persist);
    if (!err.empty()) {
        ShowToast(err);
        return err;
    }
    std::string name = "default";
    for (const input::Provider& p : input_.Providers())
        if (!id.empty() && p.id == id) name = p.name;
    LOG_INFO("Input: %s -> %s (%s%s)", input::FamilyKey(family), id.empty() ? "default" : "picked",
             why, persist ? ", remembered for this app" : "");
    ShowToast(std::string(input::FamilyLabel(family)) + ": " + name +
              (persist ? " (remembered)" : ""));
    return std::string();
}

void App::StopConvert(bool restorePicture) {
    if (!lift_.On() && !convertBound_) return;
    const bool wasBound = convertBound_;
    lift_.Stop();
    convertBound_ = false;
    convertStillPending_ = false;
    if (restorePicture && wasBound && hasMedia_ && !isLogo_) {
        if (!isVideo_ && !LiveSource() && !convertStill_.empty()) {
            renderer_.UploadTexture(convertStill_.data(), (uint32_t)convertStillW_,
                                    (uint32_t)convertStillH_);
        } else if (!convertIn_.empty()) {
            // Video / live: the last frame handed to the converter, until the next decode
            // (the render loop uploads every new frame again now that nothing is bound).
            renderer_.UploadTexture(convertIn_.data(), convertInW_, convertInH_);
        }
    }
    convertIn_.clear();
    convertInW_ = convertInH_ = 0;
}

void App::ResetConvertStream() {
    if (!lift_.On()) return;
    lift_.Reset();
    if (!isVideo_ && !LiveSource()) convertStillPending_ = true;
}

void App::SubmitConvertFrame(const FrameRing::Frame& f, double nowSec) {
    // Latest-wins: only when nothing is in flight, never every decoded frame. A zero-copy
    // GPU frame (#28, Windows opt-in) carries no CPU planes, so it cannot be submitted here.
    if (!lift_.WantsFrame(nowSec) || f.gpu || f.width <= 0 || f.height <= 0) return;
    const int fmt = f.format == PixFormat::I420   ? 0
                    : f.format == PixFormat::NV12 ? 1
                    : f.format == PixFormat::P016 ? 2
                                                  : 3;
    lift::FitExtent((uint32_t)f.width, (uint32_t)f.height, lift::LiftClient::kMaxInputLongEdge,
                    convertInW_, convertInH_);
    if (!lift::ConvertYuvToRgba(f.plane[0].data(), f.plane[1].data(),
                                fmt == 0 ? f.plane[2].data() : nullptr, (uint32_t)f.width,
                                (uint32_t)f.height, fmt, f.fullRange, convertInW_, convertInH_,
                                convertIn_))
        return;
    lift_.Submit(convertIn_.data(), convertInW_, convertInH_, convertInW_ * 4, f.serial, nowSec);
}

void App::PumpConvert(double nowSec) {
    if (!lift_.Attached()) return;
    lift_.Tick(nowSec);
    if (!lift_.On()) return;
    if (!ConvertEligible()) {  // the layout left Mono (L / set_layout / a live re-settle)
        StopConvert(/*restorePicture=*/true);
        return;
    }
    if (convertStillPending_ && !isVideo_ && !LiveSource() && !convertIn_.empty() &&
        lift_.WantsFrame(nowSec)) {
        lift_.Submit(convertIn_.data(), convertInW_, convertInH_, convertInW_ * 4, 0, nowSec);
    }
    lift::Result r;
    if (!lift_.Poll(r)) return;
    // Non-blocking: the result is a CPU image owned by the runtime until the next acquire;
    // UploadTexture stages a copy into the open frame's command buffer.
    if (renderer_.UploadTexture(r.rgba, r.width, r.height)) {
        if (!convertBound_ || r.sizeChanged)
            LOG_INFO("Convert to 3D: showing %ux%u converted (%u views, frame %llu)", r.width,
                     r.height, r.viewCount, (unsigned long long)r.frameId);
        convertBound_ = true;
        if (!isVideo_ && !LiveSource()) convertStillPending_ = false;
    }
}

void App::SetupAgentTools() {
    if (!xr_.HasMcpTools()) {
        LOG_INFO("XR_DXR_mcp_tools unavailable — agent tools not registered");
        return;
    }
    // The app id MUST equal the manifest `id` (linter INV-10.1). It is the agent-visible
    // tool prefix (e.g. mediaplayer__play_pause through the workspace aggregator).
    if (!xr_.SetMcpAppId("mediaplayer")) {
        LOG_WARN("xrSetMCPAppInfoDXR failed — skipping agent-tool registration");
        return;
    }

    // Descriptions are agent-facing API docs: state what the tool does, the units, and the
    // preconditions. Schemas are JSON Schema objects; mark required args.
    xr_.RegisterMcpTool(
        "play_pause",
        "Toggle play/pause of the current video. If the clip already played to the end, "
        "restarts it from the beginning. No-op for still images. Returns the new playing "
        "state and the current position in seconds.",
        "{\"type\":\"object\"}");

    xr_.RegisterMcpTool(
        "open_file",
        "Open a media file by path, replacing whatever is loaded. Supports SBS images "
        "(jpg/png), LIF containers, and SBS video (mp4/mkv/mov). Returns the loaded file "
        "and its kind on success; an error result if the path cannot be opened.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Path to the media file to open (absolute recommended).\"}},"
        "\"required\":[\"path\"]}");

    xr_.RegisterMcpTool(
        "seek",
        "Seek the current video to an absolute position in seconds, clamped to "
        "[0, duration]. Requires a video to be open (errors for still images). Returns the "
        "applied position in seconds.",
        "{\"type\":\"object\",\"properties\":{\"position_s\":{\"type\":\"number\","
        "\"minimum\":0,\"description\":\"Target position in seconds from the start.\"}},"
        "\"required\":[\"position_s\"]}");

    xr_.RegisterMcpTool(
        "navigate",
        "Step to the previous or next supported asset in the current file's folder "
        "(wrapping). Requires at least two assets in the folder. Returns the newly-loaded "
        "file plus its folder index and the folder asset count.",
        "{\"type\":\"object\",\"properties\":{\"delta\":{\"type\":\"integer\","
        "\"description\":\"Steps to move: +1 for next, -1 for previous.\"}},"
        "\"required\":[\"delta\"]}");

    xr_.RegisterMcpTool(
        "get_status",
        "Read the player's live state: whether a video is playing, the current and total "
        "position in seconds, the open file path, the media kind, mute state, the folder "
        "index/count used by navigate, the resolved stereo layout with the signal that "
        "decided it, and the Convert-to-3D state (convert3d_available / _state / _active). "
        "Requires no arguments.",
        "{\"type\":\"object\"}");

    xr_.RegisterMcpTool(
        "set_layout",
        "Override how the current media is unpacked into left/right eyes, or return to "
        "automatic detection. Mirrors the L key. The override applies to the currently "
        "loaded file only and is dropped when other media is loaded.",
        "{\"type\":\"object\",\"properties\":{\"layout\":{\"type\":\"string\","
        "\"enum\":[\"auto\",\"mono\",\"sbs_full\",\"sbs_half\"],"
        "\"description\":\"Layout to pin, or 'auto' to restore detection.\"}},"
        "\"required\":[\"layout\"]}");

    xr_.RegisterMcpTool(
        "list_cameras",
        "List the live cameras the player can open (index + name), in the order open_camera's "
        "numeric selector uses. Tracking/IR cameras (e.g. a 3D panel's eye-tracking camera) "
        "are never openable and are reported separately under 'blocked'. Enumerating wakes "
        "the camera subsystem; it does not open any device.",
        "{\"type\":\"object\"}");

    xr_.RegisterMcpTool(
        "open_camera",
        "Switch the player to a live camera, replacing the current media. The device opens "
        "asynchronously: poll get_status (camera_state becomes 'streaming'). The layout is "
        "guessed from the frame aspect (16:9 -> SBS-half); set_layout overrides it and "
        "play_pause freezes/resumes the picture.",
        "{\"type\":\"object\",\"properties\":{\"selector\":{\"type\":\"string\","
        "\"description\":\"'auto' (default: SpatialLabs Eyes first), an index from "
        "list_cameras, or a case-insensitive name substring.\"}}}");

    xr_.RegisterMcpTool(
        "set_auto_convergence",
        "Live auto-convergence (#92): measure the subject's stereo disparity and shift the "
        "eyes so it sits at the display plane. DEFAULT OFF. 'enabled' turns it on/off (the "
        "shift ramps back to 0 when off). 'policy' picks the subject: nearest (the nearest "
        "strong depth plane - a person at the camera), sharp (the sharpest blocks - shallow "
        "depth of field), centre (centre-weighted), focus (needs focus_x/focus_y). "
        "focus_x/focus_y (0..1, normalised over one eye's picture) pin the subject and imply "
        "policy focus. Returns the auto_conv_* fields of get_status.",
        "{\"type\":\"object\",\"properties\":{"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"policy\":{\"type\":\"string\",\"enum\":[\"nearest\",\"sharp\",\"centre\",\"focus\"]},"
        "\"focus_x\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},"
        "\"focus_y\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1}}}");

    xr_.RegisterMcpTool(
        "open_url",
        "Open a live network stream by URL, replacing the current media: rtsp://, rtmp(s)://, "
        "udp:// or rtp:// (MPEG-TS), http(s):// (progressive TS/FLV/fMP4) or an HLS .m3u8. "
        "Connects asynchronously: poll get_status (stream_state becomes 'streaming'; "
        "'reconnecting' after a loss, 'failed' once it gives up). srt:// is refused. The "
        "layout is guessed from the frame aspect; set_layout overrides it and play_pause "
        "freezes/resumes the picture. Stop it with close_camera, or by opening anything else.",
        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\","
        "\"description\":\"Stream URL, e.g. rtsp://192.168.1.10:554/stream1 or "
        "udp://127.0.0.1:5004.\"}},\"required\":[\"url\"]}");

    xr_.RegisterMcpTool(
        "set_convert_3d",
        "Convert to 3D: have the runtime's 2D->3D conversion turn the current MONO picture "
        "(a 2D still, a mono video or live source, or anything pinned mono with set_layout) "
        "into a stereo pair that replaces it on screen. Mirrors Shift+L and the top-bar "
        "toggle. Off on every load; turning it on asks for the panel's 3D mode if it is flat. "
        "Errors when the runtime offers no conversion, the content is not mono, or the "
        "conversion task cannot serve this session (the error says why). 'provider' (runtimes "
        "with provider choice) picks one of get_status's convert3d_candidates by id for this "
        "session only, or \"\" for the default; it never changes the user's default. "
        "Returns the convert3d_* fields of get_status.",
        "{\"type\":\"object\",\"properties\":{"
        "\"on\":{\"type\":\"boolean\",\"description\":\"true = convert, false = show the "
        "original 2D picture.\"},"
        "\"provider\":{\"type\":\"string\",\"description\":\"A convert3d_candidates id, or "
        "empty for the default.\"}},"
        "\"required\":[\"on\"]}");

    xr_.RegisterMcpTool(
        "set_input_provider",
        "Ask the runtime for a specific input provider for this app's controllers, hand "
        "tracking or gamepad (XR_DXR_input_providers). 'provider' is an id from get_status's "
        "input_providers list, or \"\" to go back to the default. 'persist' (default false) "
        "also saves it as the user's per-app input setting - the same record DisplayXR's "
        "settings edit; only set it when the user asked to remember the choice. Roles move on "
        "the app's next input sync. Errors when the runtime offers no input-provider choice.",
        "{\"type\":\"object\",\"properties\":{"
        "\"family\":{\"type\":\"string\",\"enum\":[\"controllers\",\"hand_tracking\",\"gamepad\"]},"
        "\"provider\":{\"type\":\"string\",\"description\":\"An input_providers id, or empty for "
        "the default.\"},"
        "\"persist\":{\"type\":\"boolean\"}},"
        "\"required\":[\"family\",\"provider\"]}");

    xr_.RegisterMcpTool(
        "close_camera",
        "Stop the live camera (or live stream) and return to the idle screen. Errors if "
        "neither is live.",
        "{\"type\":\"object\"}");

    xr_.SetMcpToolHandler([this](const std::string& tool, const std::string& args,
                                 bool& success) {
        return DispatchAgentTool(tool, args, success);
    });
    LOG_INFO("Agent tools registered (appId=mediaplayer)");
#if MEDIAPLAYER_PRO
    pro_.RegisterTools([this](const char* n, const char* d, const char* s) { xr_.RegisterMcpTool(n, d, s); });
#endif
}

std::string App::DispatchAgentTool(const std::string& tool, const std::string& argsJson,
                                   bool& success) {
    using json = nlohmann::json;
    success = true;
#if MEDIAPLAYER_PRO
    {
        std::string proResult;
        if (pro_.DispatchTool(tool, argsJson, proResult, success)) return proResult;  // Pro tools
    }
#endif

    // Parse args leniently: empty/whitespace means "no arguments".
    json args = json::object();
    if (argsJson.find_first_not_of(" \t\r\n") != std::string::npos) {
        json parsed = json::parse(argsJson, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            success = false;
            return "{\"error\":\"arguments are not valid JSON\"}";
        }
        if (parsed.is_object()) {
            args = std::move(parsed);
        } else if (!parsed.is_null()) {
            success = false;
            return "{\"error\":\"arguments must be a JSON object\"}";
        }
    }

    const bool playing = isLive_     ? !livePaused_
                         : isStream_ ? !streamPaused_
                                     : (isVideo_ && !video_.Paused());
    const double positionS = isVideo_ ? video_.PositionSeconds() : 0.0;
    const double durationS = isVideo_ ? video_.DurationSeconds() : 0.0;

    json out;
    auto fillAutoConv = [this](json& o) {
        const autoconv::Result& l = autoConvLast_;
        o["auto_conv_enabled"] = autoConvEnabled_;
        o["auto_conv_policy"] = autoconv::PolicyName(autoConvPolicy_);
        o["auto_conv_value"] = autoConvergence_;  // applied per-eye shift, fraction of eye width
        o["auto_conv_target"] = autoConvCtl_.Target();
        o["auto_conv_clamp"] = autoConvCtl_.Clamp();
        o["auto_conv_disparity_frac"] = l.disparityFrac;  // x_left - x_right, + = in front
        o["auto_conv_ok"] = l.ok;
        o["auto_conv_ncc"] = l.ncc;
        o["auto_conv_ms"] = l.ms;
        o["auto_conv_rule"] = l.rule;
        o["auto_conv_blocks"] = l.blocksMatched;
        o["auto_conv_near_frac"] = l.nearFrac;
        o["auto_conv_far_frac"] = l.farFrac;
        o["auto_conv_downsample_ms"] = autoConvDownsampleMs_;
        o["auto_conv_measurements"] = autoConvMeasures_;
        o["auto_conv_ms_mean"] = autoConvMeasures_ ? autoConvMsSum_ / (double)autoConvMeasures_ : 0.0;
        o["auto_conv_ms_max"] = autoConvMsMax_;
        o["auto_conv_value_min"] = autoConvMin_;
        o["auto_conv_value_max"] = autoConvMax_;
        o["auto_conv_value_mean"] = autoConvTicks_ ? autoConvSum_ / (double)autoConvTicks_ : 0.0;
        if (autoConvHasFocus_) {
            o["auto_conv_focus_x"] = autoConvFocus_[0];
            o["auto_conv_focus_y"] = autoConvFocus_[1];
        }
    };
    auto fillConvert = [this](json& o) {
        const lift::Availability av = lift_.GetAvailability();
        o["convert3d_available"] = av == lift::Availability::Enabled && ConvertEligible();
        o["convert3d_offered"] = lift::AvailabilityName(av);  // hidden | disabled | enabled
        o["convert3d_state"] = lift::StateName(lift_.GetState());
        o["convert3d_active"] = convertBound_;
        o["convert3d_reason"] = lift_.Reason();
        // Spec 5: the runtime's candidates (names verbatim). "selected" = what the player's
        // next create uses (its pick, else the user's default); "runtime_selected" = what a
        // hint-less create would bind.
        json cands = json::array();
        const int eff = lift_.EffectiveCandidate();
        for (size_t i = 0; i < lift_.Candidates().size(); ++i) {
            const lift::Candidate& c = lift_.Candidates()[i];
            cands.push_back({{"id", c.id},
                             {"name", c.name},
                             {"state", lift::TaskStateName(c.state)},
                             {"default", c.isUserDefault},
                             {"selected", (int)i == eff},
                             {"runtime_selected", c.isSelected},
                             {"remote", c.hosting == XR_LIFT_PROVIDER_HOSTING_REMOTE_DXR}});
        }
        o["convert3d_candidates"] = cands;
        o["convert3d_provider"] = lift_.Preferred();  // "" = no hint (the default)
        if (av != lift::Availability::Hidden) {
            o["convert3d_task_state"] = lift::TaskStateName(lift_.TaskState());
            o["convert3d_frames_submitted"] = lift_.FramesSubmitted();
            o["convert3d_frames_converted"] = lift_.FramesConverted();
        }
    };
    auto fillInput = [this](json& o) {
        // Current holder per family for this app (null when none / no extension), plus the
        // runtime's list (names verbatim).
        auto holder = [this](XrInputRoleFamilyDXR f) -> json {
            const int i = input_.Current(f);
            if (i < 0) return nullptr;
            return {{"id", input_.Providers()[i].id}, {"name", input_.Providers()[i].name}};
        };
        o["input_controllers"] = holder(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR);
        o["input_hand_tracking"] = holder(XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR);
        o["input_gamepad"] = holder(XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR);
        json list = json::array();
        for (const input::Provider& p : input_.Providers()) {
            json fams = json::array(), cur = json::array(), req = json::array();
            for (XrInputRoleFamilyDXR f :
                 {XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR, XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR,
                  XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR}) {
                if (p.supported & input::FamilyBit(f)) fams.push_back(input::FamilyKey(f));
                if (p.current & input::FamilyBit(f)) cur.push_back(input::FamilyKey(f));
                if (p.requested & input::FamilyBit(f)) req.push_back(input::FamilyKey(f));
            }
            list.push_back({{"id", p.id},
                            {"name", p.name},
                            {"families", fams},
                            {"current", cur},
                            {"requested", req},
                            {"present", p.present},
                            {"user_default", p.userDefault},
                            {"app_pinned", p.appPinned}});
        }
        o["input_providers"] = list;
    };
    if (tool == "set_input_provider") {
        XrInputRoleFamilyDXR fam;
        if (!args.contains("family") || !args["family"].is_string() ||
            !input::ParseFamily(args["family"].get<std::string>(), fam)) {
            success = false;
            return "{\"error\":\"family must be controllers|hand_tracking|gamepad\"}";
        }
        if (!args.contains("provider") || !args["provider"].is_string()) {
            success = false;
            return "{\"error\":\"missing required string arg 'provider' (\\\"\\\" = default)\"}";
        }
        if (!input_.Attached()) {
            success = false;
            return "{\"error\":\"this runtime offers no input-provider choice\"}";
        }
        const bool persist =
            args.contains("persist") && args["persist"].is_boolean() && args["persist"].get<bool>();
        const std::string err =
            SetInputProvider(fam, args["provider"].get<std::string>(), persist, "mcp");
        if (!err.empty()) {
            success = false;
            out["error"] = err;
        }
        fillInput(out);
        return out.dump();
    }
    if (tool == "set_convert_3d") {
        if (!args.contains("on") || !args["on"].is_boolean()) {
            success = false;
            return "{\"error\":\"missing required boolean arg 'on'\"}";
        }
        // provider (spec 5, runtime #2178): a candidate id from convert3d_candidates, or ""
        // for the default. A per-session hint; it never changes the user's default.
        if (args.contains("provider") && !args["provider"].is_null()) {
            if (!args["provider"].is_string()) {
                success = false;
                return "{\"error\":\"provider must be a string (a convert3d_candidates id)\"}";
            }
            const std::string err = SetConvertProvider(args["provider"].get<std::string>(), "mcp");
            if (!err.empty()) {
                success = false;
                out["error"] = err;
                fillConvert(out);
                return out.dump();
            }
        }
        const bool want = args["on"].get<bool>();
        if (!SetConvert(want, "mcp") && want) {
            success = false;
            out["error"] = uiState_.toastText.empty() ? std::string("convert to 3D unavailable")
                                                      : uiState_.toastText;
        }
        fillConvert(out);
        return out.dump();
    }
    if (tool == "set_auto_convergence") {
        const bool hasFx = args.contains("focus_x") && args["focus_x"].is_number();
        const bool hasFy = args.contains("focus_y") && args["focus_y"].is_number();
        if (hasFx != hasFy) {
            success = false;
            return "{\"error\":\"focus_x and focus_y go together\"}";
        }
        if (args.contains("policy")) {
            autoconv::SubjectPolicy p;
            if (!args["policy"].is_string() || !autoconv::ParsePolicy(args["policy"].get<std::string>(), p)) {
                success = false;
                return "{\"error\":\"policy must be nearest|sharp|centre|focus\"}";
            }
            if (p == autoconv::SubjectPolicy::Focus && !hasFx && !autoConvHasFocus_) {
                success = false;
                return "{\"error\":\"policy focus needs focus_x/focus_y\"}";
            }
            if (p != autoconv::SubjectPolicy::Focus) SetAutoConvPolicy(p);
            else autoConvPolicy_ = p;
        }
        if (hasFx) SetAutoConvFocus(args["focus_x"].get<float>(), args["focus_y"].get<float>());
        if (args.contains("enabled") && args["enabled"].is_boolean())
            SetAutoConv(args["enabled"].get<bool>(), "mcp");
        fillAutoConv(out);
        return out.dump();
    }
    if (tool == "play_pause") {
        TogglePlayback();
        out["playing"] = isLive_     ? !livePaused_
                         : isStream_ ? !streamPaused_
                                     : (isVideo_ && !video_.Paused());
        out["position_s"] = isVideo_ ? video_.PositionSeconds() : 0.0;
        return out.dump();
    }
    if (tool == "open_file") {
        if (!args.contains("path") || !args["path"].is_string() ||
            args["path"].get<std::string>().empty()) {
            success = false;
            return "{\"error\":\"missing required string arg 'path'\"}";
        }
        const std::string path = args["path"].get<std::string>();
        playlistFromDrop_ = false;   // explicit single-target open retires a drop playlist
        ReloadMedia(path);  // tears down current media, then loads; keeps prior on failure
        if (currentMediaPath_ != path) {
            success = false;
            out["error"] = "could not open '" + path + "'";
            return out.dump();
        }
        out["file"] = currentMediaPath_;
        out["is_video"] = isVideo_;
        out["duration_s"] = isVideo_ ? video_.DurationSeconds() : 0.0;
        return out.dump();
    }
    if (tool == "seek") {
        if (!isVideo_) {
            success = false;
            return "{\"error\":\"no video is open (seek requires a video)\"}";
        }
        if (!args.contains("position_s") || !args["position_s"].is_number()) {
            success = false;
            return "{\"error\":\"missing required number arg 'position_s'\"}";
        }
        double t = args["position_s"].get<double>();
        if (t < 0.0) t = 0.0;
        if (durationS > 0.0 && t > durationS) t = durationS;
        video_.Seek(t);             // exact
        ResetConvertStream();
        audio_.Seek(t);             // keep audio aligned
        uiState_.scrubTarget = (float)t;    // hold the scrubber knob until the decode lands
        out["position_s"] = t;
        return out.dump();
    }
    if (tool == "navigate") {
        if (!args.contains("delta") || !args["delta"].is_number()) {
            success = false;
            return "{\"error\":\"missing required integer arg 'delta'\"}";
        }
        if (folderFiles_.size() < 2) {
            success = false;
            return "{\"error\":\"no other media in the current folder\"}";
        }
        NavigateMedia(args["delta"].get<int>());
        out["file"] = currentMediaPath_;
        out["index"] = static_cast<uint64_t>(folderIndex_);
        out["count"] = static_cast<uint64_t>(folderFiles_.size());
        return out.dump();
    }
    if (tool == "get_status") {
        out["playing"] = playing;
        out["position_s"] = positionS;
        out["duration_s"] = durationS;
        out["file"] = currentMediaPath_;
        out["has_media"] = hasMedia_;
        out["is_video"] = isVideo_;
        out["muted"] = muted_;
        out["folder_index"] = static_cast<uint64_t>(folderIndex_);
        out["folder_count"] = static_cast<uint64_t>(folderFiles_.size());
        out["layout"] = MediaSource::LayoutName(layout_);
        out["layout_signal"] = MediaSource::SignalName(layoutSignal_);
        out["layout_confidence"] = layoutConfidence_;
        out["layout_pinned"] = layoutPinned_;
        out["eye_swap"] = mediaEyeSwap_;
        out["swap_eyes"] = swapEyes_;
        out["live"] = isLive_;
        out["live_paused"] = livePaused_;
        fillAutoConv(out);
        fillConvert(out);
        fillInput(out);
        out["camera"] = isLive_ ? live_->DeviceName() : std::string();
        out["live_kind"] = isLive_ ? live_->Kind() : "";
        out["camera_state"] = isLive_ ? CameraSource::StateName(live_->GetState()) : "closed";
        if (isLive_) {
            const CameraSource::Stats cs = live_->GetStats();
            out["camera_fps"] = cs.deliveredFps;
            out["camera_fps_requested"] = cs.fpsReq;
            out["camera_width"] = cs.w;
            out["camera_height"] = cs.h;
            out["camera_full_range"] = cs.fullRange;
            out["camera_frames"] = cs.published;
            out["camera_dropped"] = cs.dropped;
            out["frame_age_ms"] = liveAgeMs_;
            out["panel_fps"] = fps_;
            out["camera_error"] = live_->LastError();
        }
        // Stream URL (#93).
        out["stream"] = isStream_;
        out["stream_paused"] = streamPaused_;
        out["stream_url"] = isStream_ ? stream::RedactUrl(streamUrl_) : std::string();
        out["stream_state"] =
            isStream_ ? VideoDecoder::StreamStateName(video_.GetStreamState()) : "closed";
        if (isStream_) {
            const VideoDecoder::StreamStats ss = video_.GetStreamStats();
            out["stream_fps"] = ss.fpsIn;
            out["stream_kbps"] = ss.kbps;
            out["reconnects"] = ss.reconnects;
            out["stream_width"] = ss.width;
            out["stream_height"] = ss.height;
            out["stream_frames"] = ss.frames;
            out["stream_dropped"] = ss.dropped;
            out["stream_connect_ms"] = ss.connectMs;
            out["stream_codec"] = ss.codec;
            out["stream_backend"] = ss.backend;
            out["stream_error"] = ss.lastError;
            out["panel_fps"] = fps_;
        }
        return out.dump();
    }
    if (tool == "set_layout") {
        if (!args.contains("layout") || !args["layout"].is_string()) {
            success = false;
            return "{\"error\":\"missing required string arg 'layout'\"}";
        }
        const std::string want = args["layout"].get<std::string>();
        bool pin = true;
        StereoLayout value = StereoLayout::Mono;
        if (want == "auto") pin = false;
        else if (want == "mono") value = StereoLayout::Mono;
        else if (want == "sbs_full") value = StereoLayout::SbsFull;
        else if (want == "sbs_half") value = StereoLayout::SbsHalf;
        else {
            success = false;
            return "{\"error\":\"layout must be auto|mono|sbs_full|sbs_half\"}";
        }
        // Reuse the cycle's re-derivation rather than duplicating it: step until the
        // state matches (the cycle has 4 stops, so this always terminates).
        for (int i = 0; i < 4; ++i) {
            if (layoutPinned_ == pin && (!pin || layoutPinnedValue_ == value)) break;
            CycleLayoutOverride();
        }
        out["layout"] = MediaSource::LayoutName(layout_);
        out["layout_signal"] = MediaSource::SignalName(layoutSignal_);
        out["layout_pinned"] = layoutPinned_;
        return out.dump();
    }

    if (tool == "list_cameras") {
        json cams = json::array(), blocked = json::array();
        cameraDevs_.clear();
        uiState_.cameraNames.clear();
        for (CameraDevice& d : CameraSource::Enumerate(false)) {
            if (d.denied) {
                blocked.push_back(d.name);
                continue;
            }
            cams.push_back({{"index", (uint64_t)cameraDevs_.size()}, {"name", d.name}});
            uiState_.cameraNames.push_back(d.name);
            cameraDevs_.push_back(std::move(d));
        }
        out["cameras"] = cams;
        out["blocked"] = blocked;
        return out.dump();
    }
    if (tool == "open_camera") {
        std::string sel;
        if (args.contains("selector")) {
            if (args["selector"].is_string()) sel = args["selector"].get<std::string>();
            else if (args["selector"].is_number_integer())
                sel = std::to_string(args["selector"].get<int>());
        }
        if (!LoadLive(sel)) {
            success = false;
            out["error"] = uiState_.toastText.empty() ? std::string("camera did not open")
                                                      : uiState_.toastText;
            return out.dump();
        }
        out["camera"] = camera_.DeviceName();
        out["camera_state"] = CameraSource::StateName(camera_.GetState());
        return out.dump();
    }
    if (tool == "open_url") {
        if (!args.contains("url") || !args["url"].is_string() ||
            args["url"].get<std::string>().empty()) {
            success = false;
            return "{\"error\":\"missing required string arg 'url'\"}";
        }
        if (!LoadUrl(args["url"].get<std::string>())) {
            success = false;
            out["error"] = uiState_.toastText.empty() ? std::string("stream did not open")
                                                      : uiState_.toastText;
            return out.dump();
        }
        out["stream_url"] = stream::RedactUrl(streamUrl_);
        out["stream_state"] = VideoDecoder::StreamStateName(video_.GetStreamState());
        return out.dump();
    }
    if (tool == "close_camera") {
        if (!isLive_ && !isStream_) {
            success = false;
            return "{\"error\":\"no camera or stream is live\"}";
        }
        if (isStream_) StopStream(/*showIdleIfEmpty=*/true);
        else StopLive(/*showIdleIfEmpty=*/true);
        out["live"] = false;
        return out.dump();
    }

    success = false;
    return "{\"error\":\"unknown tool '" + tool + "'\"}";
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
        window_.MousePosition(mx, my);
        if (restMouseX_ < 0.0f ||
            std::fabs(mx - restMouseX_) + std::fabs(my - restMouseY_) > 3.0f) {
            restMouseX_ = mx;
            restMouseY_ = my;
            lastActivity_ = now;
        }
    }
    // Dragging the scrubber counts as activity even with the cursor held still.
    if (uiState_.scrubActive) lastActivity_ = now;
    // Keep the UI pinned visible until a pending HUD dump lands (diagnostic/screenshot).
    if (dumpHudPath_ && !dumpedHud_) lastActivity_ = now;

    // Auto-hide + toast fades are shared with Android — see ui/TransportUI.cpp. What
    // counts as "activity" is platform-specific and stays here (mouse motion above).
    const double idle = duration<double>(now - lastActivity_).count();
    ui::TickTransportUI(uiState_, dt, /*uiAwake=*/showHud_ && idle < kIdleHideSeconds);

    // Dip-to-black transition machine, shared by the slideshow and manual ↑/↓ nav.
    // Slideshow auto-starts a transition when a still has shown long enough / a video
    // ended; manual nav starts one via RequestNavTransition(). The swap happens at full
    // black. pendingNavDelta_ carries the step (+1 next, -1 prev).
    const float transStep = (float)(dt / kTransitionSeconds);
    if (uiState_.slideshowActive && transition_ == Transition::Playing && pendingNavDelta_ == 0) {
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
            uiState_.transitionAlpha = std::min(1.0f, uiState_.transitionAlpha + transStep);
            if (uiState_.transitionAlpha >= 1.0f) {
                if (pendingNavDelta_ != 0) {
                    NavigateMedia(pendingNavDelta_);  // swap while fully black
                    pendingNavDelta_ = 0;
                }
                transition_ = Transition::FadeIn;
            }
            break;
        case Transition::FadeIn:
            uiState_.transitionAlpha = std::max(0.0f, uiState_.transitionAlpha - transStep);
            if (uiState_.transitionAlpha <= 0.0f) transition_ = Transition::Playing;
            break;
    }
}

void App::RequestOpenFile() {
    if (openFilePending_) return;
    openFilePending_ = true;

    // Prefer the workspace spatial picker (XR_DXR_workspace_file_dialog). In a
    // workspace it spawns displayxr-file-picker.exe and the result arrives async via
    // xr_.TakePickedFile(); anywhere else we fall back to a native OS dialog.
    const XrSession::PickerStatus st = xr_.RequestFilePicker();
    if (st == XrSession::PickerStatus::Pending) {
        LOG_INFO("Open: workspace file picker requested");
        return;
    }
    LOG_INFO("Open: workspace picker unavailable (status=%d) — native dialog", (int)st);
    // The native dialog is a desktop window — useful for standalone/dev runs, but inside a
    // fullscreen workspace it opens behind the compositor and looks like a dead button. Toast
    // so the fallback is never a silent no-op.
    ShowToast("File picker unavailable — opening desktop dialog");

    static const SDL_DialogFileFilter kFilters[] = {
        // Keep in sync with MediaSource::IsSupported — SDL wants its own filter string.
        {"Stereo media", "mp4;mkv;mov;jpg;jpeg;png;lif;mpo"},
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

#if MEDIAPLAYER_PRO
// ---------------------------------------------------------------------------
// Media Player Pro glue: the only place App hands trunk state to src/pro/.
// ---------------------------------------------------------------------------

void App::InitPro() {
    pro::Host h;
    h.getConvergence = [this] { return convergence_; };
    h.setConvergence = [this](float c) {
        convergence_ = std::clamp(c, -kConvergenceMax, kConvergenceMax);
    };
    h.getSwapEyes = [this] { return swapEyes_; };
    h.setSwapEyes = [this](bool s) { swapEyes_ = s; };
    h.toast = [this](const std::string& t) { ShowToast(t); };
    h.pointToContent = [this](float x, float y, float& u, float& v) {
        if (proPicW_ < 1.f || proPicH_ < 1.f) return false;
        u = (x - proPicX_) / proPicW_;
        v = (y - proPicY_) / proPicH_;
        return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
    };
    h.contentSizePoints = [this](float& w, float& hh) {
        w = proPicW_;
        hh = proPicH_;
        return w > 1.f && hh > 1.f;
    };
    h.oneToOneZoom = [this] {
        // One source pixel per tile pixel: the eye's source width over its on-tile width.
        const bool sbs = layout_ != StereoLayout::Mono && layout_ != StereoLayout::Dual;
        const float eyeW = (float)mediaW_ * (sbs ? 0.5f : 1.0f);
        return proTileContentW_ > 1.f ? std::max(1.0f, eyeW / proTileContentW_) : 1.0f;
    };
    h.pictureInHud = [this](float& x, float& y, float& w, float& hh) {
        // The (unzoomed) picture as fractions of the HUD layer, where ImGui draws.
        uint32_t pw = 0, ph = 0;
        window_.PointSize(pw, ph);
        if (pw == 0 || ph == 0 || proPicW_ < 1.f || proHud_[2] <= 0.f || proHud_[3] <= 0.f) return false;
        x = (proPicX_ / (float)pw - proHud_[0]) / proHud_[2];
        y = (proPicY_ / (float)ph - proHud_[1]) / proHud_[3];
        w = (proPicW_ / (float)pw) / proHud_[2];
        hh = (proPicH_ / (float)ph) / proHud_[3];
        return true;
    };
    // A/B compare (Pro, B): reload the other clip at the same position.
    h.currentMedia = [this] { return (isLive_ || isStream_) ? std::string() : currentMediaPath_; };
    h.reloadMedia = [this](const std::string& path) {
        ReloadMedia(path);
        return currentMediaPath_ == path;
    };
    h.mediaPosition = [this] { return isVideo_ ? video_.PositionSeconds() : -1.0; };
    h.mediaPaused = [this] { return isVideo_ && video_.Paused(); };
    h.seekMedia = [this](double s, bool paused) {
        if (!isVideo_) return;
        video_.Seek(s);
        if (paused && !video_.Paused()) TogglePlayback();
    };
    h.mediaStart = [this] { return isVideo_ ? video_.StartSeconds() : 0.0; };
    h.setMediaPaused = [this](bool paused) {
        if (isVideo_ && video_.Paused() != paused) TogglePlayback();
    };
    h.isFullscreen = [this] { return window_.IsFullscreen(); };
    h.toggleFullscreen = [this] { window_.ToggleFullscreen(); };
    h.setDesqueeze = [this](float f) { proDesqueeze_ = f > 0.1f ? f : 1.0f; };
    h.mediaFps = [this] { return isVideo_ ? video_.FrameRate() : 0.0; };
    // Scope pictures: one ImGui texture per slot, (re)made when the size changes.
    auto scopeIds = std::make_shared<std::vector<std::array<int, 3>>>(4, std::array<int, 3>{-1, 0, 0});
    h.userTexture = [this, scopeIds](int slot, int w, int hh, const uint8_t* rgba) -> void* {
        if (slot < 0 || slot >= (int)scopeIds->size() || !imgui_.Ready() || w < 1 || hh < 1) return nullptr;
        auto& e = (*scopeIds)[slot];  // {id, w, h}; id -1 not made, -2 failed (never retried)
        if (e[0] >= 0 && (e[1] != w || e[2] != hh)) e[0] = -1;  // size changed: a new texture
        if (e[0] == -1 && rgba) {
            e[0] = imgui_.CreateTexture((uint32_t)w, (uint32_t)hh);
            if (e[0] < 0) e[0] = -2;
            e[1] = w;
            e[2] = hh;
        }
        if (e[0] < 0) return nullptr;
        if (rgba) imgui_.UpdateTexture(e[0], rgba);
        return imgui_.TextureId(e[0]);
    };
    pro_.Init(&renderer_, window_.SdlWindow(), std::move(h));
    pro_.SetFlatPanelsAvailable(xr_.HasLocal2D());

    // Pro input rides the same SDL event hook as ImGui, after ImGui has seen the event.
    const bool imguiOk = imgui_.Ready();
    window_.SetEventHook([this, imguiOk](void* ev) {
        bool wantMouse = false, wantKeys = false;
        if (imguiOk) {
            imgui_.ProcessEvent(ev);
            wantMouse = imgui_.WantCaptureMouse();
#if defined(MEDIAPLAYER_WITH_IMGUI)
            wantKeys = ImGui::GetIO().WantTextInput;
#endif
        }
        pro_.HandleEvent(ev, wantMouse, wantKeys);
    });

    if (launch_.lutPath.empty())
        if (const char* e = std::getenv("MEDIAPLAYER_LUT")) launch_.lutPath = e;
    if (launch_.cdlPath.empty())
        if (const char* e = std::getenv("MEDIAPLAYER_CDL")) launch_.cdlPath = e;
    pro_.ApplyTestEnv();
    if (!launch_.lutPath.empty()) pro_.LoadLut(launch_.lutPath);
    if (!launch_.cdlPath.empty()) pro_.LoadCdl(launch_.cdlPath);
    if (launch_.ocio) {
        ocio::Request req;
        req.config = launch_.ocioConfig;
        req.src = launch_.ocioSrc;
        req.display = launch_.ocioDisplay;
        req.view = launch_.ocioView;
        req.dst = launch_.ocioDst;
        req.looks = launch_.ocioLooks;
        pro_.LoadOcio(req);
    }
    if (!launch_.proCompare.empty()) pro_.SetCompareB(launch_.proCompare);

    // The monitoring station (inputs, timecode / delay, rig checks, snapshots).
    pro::StationHost sh;
    sh.enterLive = [this](LiveProducer* p, const std::string& name) {
        if (isStream_) StopStream(/*showIdleIfEmpty=*/false);
        EnterLive(p, name);
    };
    sh.stopLive = [this] { if (isLive_) StopLive(/*showIdleIfEmpty=*/true); };
    sh.openCamera = [this](const std::string& sel) { return LoadLive(sel); };
    sh.openUrl = [this](const std::string& url) { return LoadUrl(url); };
    sh.capturePrefix = [this](const std::string& stem) {
        return MakeCaptureAtlasPrefix(stem, xr_.ActiveTileColumns(), xr_.ActiveTileRows());
    };
    sh.captureAtlas = [this](const std::string& prefix) {
        return xr_.HasAtlasCapture() && xr_.CaptureAtlas(prefix);
    };
    sh.toast = [this](const std::string& t) { ShowToast(t); };
    sh.displayPeriodMs = [this] { return fps_ > 1.0f ? 1000.0 / fps_ : 1000.0 / 60.0; };
    // Input-list thumbnails: one small ImGui texture per input slot, created on first use.
    auto thumbIds = std::make_shared<std::vector<int>>(pro::Station::kMaxThumbs, -1);
    sh.thumbTexture = [this, thumbIds](int slot, const uint8_t* rgba) -> void* {
        if (slot < 0 || slot >= (int)thumbIds->size() || !imgui_.Ready()) return nullptr;
        int& id = (*thumbIds)[slot];  // -1 not made yet, -2 creation failed (never retried)
        if (id == -1 && rgba) {
            id = imgui_.CreateTexture(pro::Station::kThumbW, pro::Station::kThumbH);
            if (id < 0) id = -2;
        }
        if (id < 0) return nullptr;
        if (rgba) imgui_.UpdateTexture(id, rgba);
        return imgui_.TextureId(id);
    };
    pro::StationOptions so;
    so.inputs = launch_.proInputs;
    so.budget.nearPct = launch_.proBudget[0];
    so.budget.farPct = launch_.proBudget[1];
    so.budget.maxVerticalPx = launch_.proBudget[2];
    so.budget.maxDeltaE = launch_.proBudget[3];
    so.timecode = launch_.proTimecode;
    so.startFirstInput = !hasMedia_ || isLogo_;  // a file / camera / URL from the command line wins
    if (const char* e = std::getenv("MEDIAPLAYER_TEST_VSHIFT")) so.testCfg.vShiftPx = (float)std::atof(e);
    if (const char* e = std::getenv("MEDIAPLAYER_TEST_EYE_DELAY")) so.testCfg.eyeDelay = std::atoi(e);
    if (const char* e = std::getenv("MEDIAPLAYER_TEST_GAIN_R")) so.testCfg.gainR = (float)std::atof(e);
    if (const char* e = std::getenv("MEDIAPLAYER_TEST_DEPTH")) so.testCfg.depthScale = (float)std::atof(e);
    pro_.GetStation().Init(std::move(sh), so);
    LOG_INFO("Media Player Pro: ready (lut=%s cdl=%s)", launch_.lutPath.c_str(), launch_.cdlPath.c_str());
}

pro::Status App::ProStatus() const {
    pro::Status st;
    st.isLogo = isLogo_ || !hasMedia_;
    st.stereo = hasMedia_ && !isLogo_ && layout_ != StereoLayout::Mono && layout_ != StereoLayout::Dual;
    st.layoutName = MediaSource::LayoutName(layout_);
    st.width = mediaW_;
    st.height = mediaH_;
    st.panelFps = fps_;
    st.modeName = xr_.ActiveModeName() ? xr_.ActiveModeName() : "";
    st.live = isLive_;
    st.eyesSwapped = swapEyes_ != mediaEyeSwap_;
    st.hasPicture = hasMedia_ && !isLogo_ && (isLive_ || isStream_ || isVideo_);
    if (isLive_) {
        st.sourceKind = LiveIsCamera() ? "Camera" : live_->Kind();
        st.sourceLabel = live_->DeviceName();
        st.sourceFps = live_->GetStats().deliveredFps;
        st.frameAgeMs = (float)liveAgeMs_;
    } else if (isStream_) {
        st.sourceKind = "Stream";
        st.sourceLabel = stream::UrlHost(streamUrl_);
        st.sourceFps = video_.GetStreamStats().fpsIn;
    } else if (!currentMediaPath_.empty()) {
        st.sourceKind = isVideo_ ? "Video" : "Image";
        st.sourceLabel = std::filesystem::path(currentMediaPath_).filename().string();
    }
    return st;
}
#endif  // MEDIAPLAYER_PRO

} // namespace mp
