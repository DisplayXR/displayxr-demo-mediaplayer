// SPDX-License-Identifier: BSL-1.0
#include "App.h"

#include "Log.h"
#include "media/ImageDecoder.h"
#include "media/MediaSource.h"
#include "ui/Hud.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>   // SDL_ShowOpenFileDialog (Tier-0 native file dialog)

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include "imgui.h"
#endif

namespace mp {

namespace {
// RED|BLUE left/right test pattern (shown when no media is loaded).
constexpr ClearColor kLeftImage{0.85f, 0.10f, 0.10f, 1.0f};   // RED  = left
constexpr ClearColor kRightImage{0.10f, 0.20f, 0.85f, 1.0f};  // BLUE = right

// Convergence (horizontal image translation) budget: each '[' / ']' nudges by one
// step, clamped to ±max — fractions of a view tile, kept small for "subtle" depth.
constexpr float kConvergenceStep = 0.0025f;
constexpr float kConvergenceMax = 0.05f;

// HUD layer disparity. 0 = zero-disparity plane (screen depth) so the UI sits exactly
// where the cursor is and clicks align across both eyes. (A small negative value would
// float it toward the viewer per PRD §9, but that offsets the perceived hit target.)
constexpr float kHudDisparity = 0.0f;

// Per-eye display aspect (width/height): full SBS packs two eyes across the width
// (each eye half-width); half-SBS and mono use the full frame width.
float PerEyeAspect(StereoLayout layout, int frameW, int frameH) {
    if (frameH <= 0) return 1.0f;
    const float eyeW = (layout == StereoLayout::SbsFull) ? (float)frameW * 0.5f : (float)frameW;
    return eyeW / (float)frameH;
}

// Aspect-fit `contentAspect` (width/height) inside `tile`, centered, with black bars
// (letterbox if the content is wider than the tile, pillarbox if narrower). No stretch.
XrSession::ViewRect FitRect(const XrSession::ViewRect& tile, float contentAspect) {
    if (tile.w == 0 || tile.h == 0 || contentAspect <= 0.0f) return tile;
    const float tileAspect = (float)tile.w / (float)tile.h;
    XrSession::ViewRect r = tile;
    if (contentAspect > tileAspect) {
        // Content is wider: fit to tile width, bars top/bottom.
        const uint32_t h = (uint32_t)((float)tile.w / contentAspect + 0.5f);
        r.w = tile.w;
        r.h = h;
        r.x = tile.x;
        r.y = tile.y + (int32_t)((tile.h - h) / 2);
    } else {
        // Content is narrower: fit to tile height, bars left/right.
        const uint32_t w = (uint32_t)((float)tile.h * contentAspect + 0.5f);
        r.w = w;
        r.h = tile.h;
        r.x = tile.x + (int32_t)((tile.w - w) / 2);
        r.y = tile.y;
    }
    return r;
}
} // namespace

bool App::Initialize(const char* mediaPath) {
    if (!window_.Create("DisplayXR Stereo Media Player", 1280, 720)) return false;

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

    // Load a stereo image or video if one was given; otherwise the loop falls back
    // to the RED|BLUE L/R test pattern (and the user can Open one at runtime).
    if (mediaPath && *mediaPath) {
        LoadMedia(mediaPath);
    }
    if (!hasMedia_) {
        LOG_INFO("No media loaded — showing RED|BLUE L/R test pattern");
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
    fpsWindowStart_ = std::chrono::steady_clock::now();

    // Render during the macOS modal resize loop too, for continuous live resize.
    window_.SetLiveResizeCallback([this]() { RenderOneFrame(); });

    bool keepRunning = true;
    while (keepRunning && !xr_.ExitRequested()) {
        keepRunning = window_.PumpEvents();
        if (window_.TakeCycleModeRequest()) xr_.RequestNextMode();   // 'V'
        if (window_.TakeToggleHudRequest()) {                        // SHIFT+TAB
            showHud_ = !showHud_;
            LOG_INFO("HUD %s", showHud_ ? "on" : "off");
        }
        // M4 transport / stereo controls.
        if (const int steps = window_.TakeConvergenceSteps(); steps != 0) {  // '[' / ']'
            convergence_ += steps * kConvergenceStep;
            if (convergence_ > kConvergenceMax) convergence_ = kConvergenceMax;
            if (convergence_ < -kConvergenceMax) convergence_ = -kConvergenceMax;
            LOG_INFO("convergence=%+.3f", convergence_);
        }
        if (window_.TakeResetConvergence()) {                         // '\'
            convergence_ = 0.0f;
            LOG_INFO("convergence reset");
        }
        if (window_.TakeSwapEyesRequest()) {                          // 'X'
            swapEyes_ = !swapEyes_;
            LOG_INFO("swap eyes %s", swapEyes_ ? "on" : "off");
        }
        if (window_.TakeTogglePauseRequest() && isVideo_) {           // Space
            video_.TogglePaused();
            LOG_INFO("playback %s", video_.Paused() ? "paused" : "playing");
        }
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

    if (startMode_ >= 0 && !startModeRequested_) {
        xr_.RequestMode((uint32_t)startMode_);
        startModeRequested_ = true;
    }

    // Pull the latest decoded video frame (if any) and upload it.
    if (isVideo_) {
        if (const FrameRing::Frame* vf = video_.Ring().AcquireLatest()) {
            renderer_.UploadTexture(vf->pixels.data(), (uint32_t)vf->width, (uint32_t)vf->height);
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
            // Letterbox in display-view space, then scale into each tile (handles
            // non-uniform modes the runtime stretches back to the full view).
            const float sx = xr_.ActiveViewScaleX();
            const float sy = xr_.ActiveViewScaleY();
            const XrSession::ViewRect viewFit = FitRect({0, 0, canvasW, canvasH}, contentAspect_);
            for (uint32_t v = 0; v < frame.viewCount; ++v) {
                // Convergence = horizontal image translation: shift the two eyes'
                // content oppositely within their tiles, moving the zero-disparity
                // plane. Exposed edges fall on the black letterbox the renderer clears.
                const int32_t cdx =
                    (int32_t)(convergence_ * (float)rects[v].w * (isLeftView[v] ? 1.0f : -1.0f));
                contentRects[v].x = rects[v].x + cdx + (int32_t)((float)viewFit.x * sx + 0.5f);
                contentRects[v].y = rects[v].y + (int32_t)((float)viewFit.y * sy + 0.5f);
                contentRects[v].w = (uint32_t)((float)viewFit.w * sx + 0.5f);
                contentRects[v].h = (uint32_t)((float)viewFit.h * sy + 0.5f);
            }
            // Pass the tile rects as scissor bounds so a convergence-shifted content
            // rect is truncated at its tile edge, never spilling into the other eye.
            renderer_.DrawViews(frame.imageIndex, contentRects, rects, uvs, frame.viewCount);
        } else if (hasMedia_) {
            const ClearColor black[XrSession::kMaxViews] = {};  // video warming up
            renderer_.ClearViews(frame.imageIndex, black, rects, frame.viewCount);
        } else {
            renderer_.ClearViews(frame.imageIndex, colors, rects, frame.viewCount);
        }
        ++rendered_;

        if (dumpPath_ && !dumped_ && rendered_ >= 100) {
            renderer_.DumpImage(frame.imageIndex, dumpPath_);
            dumped_ = true;
        }
    }

    // Window-space HUD overlay (SHIFT+TAB): the ImGui transport bar floating just in
    // front of the image, or the CPU text HUD if ImGui is unavailable.
    XrSession::HudSubmit hud;
    if (showHud_ && frame.shouldRender && xr_.HasHud()) {
        uint32_t cw = 0, ch = 0;
        window_.PixelSize(cw, ch);
        const float hudAR = (float)xr_.HudWidth() / (float)xr_.HudHeight();
        const float winAR = (ch > 0) ? (float)cw / (float)ch : 1.0f;

        uint32_t hudIdx = 0;
        if (xr_.AcquireHudImage(hudIdx)) {
            if (imgui_.Ready()) {
                // Transport bar: bottom-center, aspect-preserving, floats toward viewer.
                hud.enabled = true;
                hud.width = 0.70f;
                hud.height = hud.width * winAR / hudAR;
                hud.x = 0.5f * (1.0f - hud.width);
                hud.y = 1.0f - hud.height - 0.04f;
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
                                  "%.0f FPS   %s\nsrc RED|BLUE test\nwin %ux%u  tile %ux%u\n%s",
                                  fps_, xr_.ActiveModeName(), cw, ch, tileW, tileH, stereo);
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

void App::BuildTransportUI() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.08f, 0.70f));
    if (ImGui::Begin("##transport", nullptr, flags)) {
        // Row 1: open + transport + stereo toggles.
        ImGui::BeginDisabled(openFilePending_);
        if (ImGui::Button("Open", ImVec2(90, 0))) RequestOpenFile();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (isVideo_) {
            if (ImGui::Button(video_.Paused() ? "Play" : "Pause", ImVec2(120, 0)))
                video_.TogglePaused();
            ImGui::SameLine();
        }
        ImGui::Checkbox("Swap L/R", &swapEyes_);
        ImGui::SameLine();
        if (ImGui::Button("Fullscreen", ImVec2(140, 0))) window_.ToggleFullscreen();
        ImGui::SameLine();
        if (ImGui::Button("Reset conv", ImVec2(140, 0))) convergence_ = 0.0f;

        // Row 2: scrubber (video only). Position tracks playback except while dragging.
        if (isVideo_ && video_.DurationSeconds() > 0.0) {
            const float dur = (float)video_.DurationSeconds();
            const float pos = (float)video_.PositionSeconds();
            // Track playback into the knob, but not while dragging, and not while a seek
            // we issued is still landing — otherwise the knob snaps back to the stale
            // position on release. scrubTarget_ clears once the decode reaches it.
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
            ImGui::Text("%s", cur);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tot).x - 16.0f);
            const bool changed = ImGui::SliderFloat("##scrub", &scrubValue_, 0.0f, dur, "");
            scrubActive_ = ImGui::IsItemActive();
            if (changed) {
                video_.Seek(scrubValue_);
                scrubTarget_ = scrubValue_;  // hold the knob here until the decode lands
            }
            ImGui::SameLine();
            ImGui::Text("%s", tot);
        }

        // Row 3: convergence slider (full width).
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##conv", &convergence_, -kConvergenceMax, kConvergenceMax,
                           "convergence  %+.3f");

        // Row 4: format / stats.
        if (isVideo_) {
            ImGui::Text("%.0f FPS   %s   %dx%d %s   %s/%s", fps_, xr_.ActiveModeName(),
                        mediaW_, mediaH_, MediaSource::LayoutName(layout_),
                        video_.CodecName(), video_.BackendName());
        } else if (hasMedia_) {
            ImGui::Text("%.0f FPS   %s   %dx%d %s", fps_, xr_.ActiveModeName(), mediaW_,
                        mediaH_, MediaSource::LayoutName(layout_));
        } else {
            ImGui::Text("%.0f FPS   %s   RED|BLUE test", fps_, xr_.ActiveModeName());
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
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
    video_.Stop();        // join the decode thread before tearing down the GPU
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
        layout_ = info.layout;
        mediaW_ = video_.Width();
        mediaH_ = video_.Height();
        contentAspect_ = PerEyeAspect(info.layout, video_.Width(), video_.Height());
        LOG_INFO("Playing %s video (%s), per-eye aspect %.3f",
                 MediaSource::LayoutName(info.layout), path.c_str(), contentAspect_);
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
    layout_ = imgInfo.layout;
    mediaW_ = img.width;
    mediaH_ = img.height;
    contentAspect_ = PerEyeAspect(imgInfo.layout, img.width, img.height);
    LOG_INFO("Displaying %s image (%s), per-eye aspect %.3f",
             MediaSource::LayoutName(imgInfo.layout), path.c_str(), contentAspect_);
    return true;
}

void App::ReloadMedia(const std::string& path) {
    video_.Stop();   // joins the decode thread; no-op when current media is an image
    isVideo_ = false;
    scrubValue_ = 0.0f;
    scrubActive_ = false;
    if (!LoadMedia(path)) {
        LOG_WARN("Open: keeping previous view (failed to open '%s')", path.c_str());
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
        {"Stereo media", "mp4;mkv;mov;jpg;jpeg;png"},
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
