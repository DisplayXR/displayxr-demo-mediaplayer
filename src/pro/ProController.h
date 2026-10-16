// SPDX-License-Identifier: Apache-2.0
//
// ProController — everything Media Player Pro adds on top of the shared player trunk.
//
// Media Player and Media Player Pro are ONE codebase (docs/PRO.md): the same App,
// renderer, decoders and transport UI, built twice. The Pro build defines
// MEDIAPLAYER_PRO=1 and links this module; App calls into it from a handful of
// clearly-marked `#if MEDIAPLAYER_PRO` hooks and otherwise behaves exactly like the
// base player. Keep Pro features HERE (or in other src/pro/ files) rather than as
// scattered #ifs in the trunk — the trunk must stay readable as the base player.
//
// What lives here today (Phase 1 of docs/PRO.md):
//   - QC view modes: 3D, left eye only, right eye only, eye difference, anaglyph
//   - pan / zoom (identical for both eyes, so depth is preserved while zoomed)
//   - the grade: exposure, ASC CDL (.cc/.ccc/.cdl), 3D LUT (.cube), with bypass
//   - the inspector panel (ImGui) and the Pro key map
#pragma once

#include "color/Cdl.h"
#include "color/CubeLut.h"
#include "pro/OcioBake.h"
#include "pro/Station.h"
#include "rhi/VulkanRenderer.h"

#include <functional>
#include <mutex>
#include <string>

struct SDL_Window;

namespace mp::pro {

enum class ViewMode { Stereo3D = 0, LeftOnly, RightOnly, Difference, Anaglyph, Count };
const char* ViewModeName(ViewMode m);

// Live player state the inspector shows, refreshed by App every frame.
struct Status {
    std::string sourceLabel;    // file name, camera name or stream host
    std::string sourceKind;     // "File" / "Camera" / "Stream" / ""
    std::string layoutName;     // "SBS-half", "Mono", ...
    bool stereo = false;        // the source carries two eyes
    bool isLogo = false;        // the idle screen is up (no grade, no QC)
    int width = 0, height = 0;  // full source frame
    float panelFps = 0.0f;
    float sourceFps = 0.0f;     // camera delivered / stream decoded fps (0 for files)
    float frameAgeMs = 0.0f;    // live: publish -> upload age
    bool live = false;          // a live source (camera, DeckLink, test rig) is showing
    bool hasPicture = false;    // live, video or stream: rig checks + history apply
    bool eyesSwapped = false;   // the user's swap XOR the container's (R|L packing)
    std::string modeName;       // display mode, e.g. "LeiaSR"
};

// Callbacks into App for state the trunk owns.
struct Host {
    std::function<float()> getConvergence;
    std::function<void(float)> setConvergence;
    std::function<bool()> getSwapEyes;
    std::function<void(bool)> setSwapEyes;
    std::function<void(const std::string&)> toast;
    // Window point (in points) -> normalised position inside the LEFT-EYE content
    // (0..1 across the picture before zoom). False when the point is off the picture.
    std::function<bool(float x, float y, float& u, float& v)> pointToContent;
    // Size of the (unzoomed) picture on screen in window points, for pan speed.
    std::function<bool(float& w, float& h)> contentSizePoints;
    // Zoom at which one source pixel maps to one panel pixel in the current mode.
    std::function<float()> oneToOneZoom;
    // The unzoomed picture as fractions (0..1) of the HUD layer ImGui draws into.
    std::function<bool(float& x, float& y, float& w, float& h)> pictureInHud;
    // A/B compare: the open file ("" when live / streaming), reload a file (true when it
    // opened), the playback position (-1 = not a video) / pause state, and seek.
    std::function<std::string()> currentMedia;
    std::function<bool(const std::string&)> reloadMedia;
    std::function<double()> mediaPosition;
    std::function<bool()> mediaPaused;
    std::function<void(double seconds, bool paused)> seekMedia;
    std::function<bool()> isFullscreen;
    std::function<void()> toggleFullscreen;
};

class ProController {
public:
    void Init(VulkanRenderer* renderer, SDL_Window* window, Host host);

    // Test scaffolding: MEDIAPLAYER_PRO_VIEW=0..4 (view mode) and MEDIAPLAYER_PRO_ZOOM=<x>
    // set the initial view, so a headless atlas dump can prove them without keystrokes.
    void ApplyTestEnv();

    // Launch-time looks (--lut / --cdl, or MEDIAPLAYER_LUT / MEDIAPLAYER_CDL).
    void LoadLut(const std::string& path);
    void LoadCdl(const std::string& path);
    // An OpenColorIO transform baked into the grade's 3D LUT (needs a build with MP_OCIO_ROOT).
    bool LoadOcio(const ocio::Request& req, std::string* err = nullptr);
    // A/B compare: the B clip; B (key) swaps to the other at the same position.
    void SetCompareB(const std::string& path) { compare_[1] = path; }
    void ToggleCompare();

    // Raw SDL event, after ImGui has seen it. Returns true when Pro consumed it.
    bool HandleEvent(const void* sdlEvent, bool imguiWantsMouse, bool imguiWantsKeyboard);

    // A dropped .cube / .cc / .ccc / .cdl loads as a look instead of media. Returns
    // true when the path was a look (consumed), false to let the trunk handle it.
    bool HandleDroppedPath(const std::string& path);

    // Per frame, before drawing: picks up file-dialog results and pushes the grade
    // (or the identity, on the idle screen) to the renderer.
    void Tick(const Status& st);

    // Rewrite the per-view source UVs for the view mode and pan / zoom. `stereo` means
    // the frame packs two eyes side by side; `eyesSwapped` means its packing is R|L
    // (the user's swap XOR the container's). The QC views address the two halves
    // directly, so they work even when every display view maps to the same eye (no
    // viewer tracked).
    void ApplyViewTransform(ViewUV* uvs, uint32_t viewCount, bool stereo, bool eyesSwapped);

    // The inspector (ImGui). `uiAlpha` is the trunk's auto-hide fade; a pinned
    // inspector ignores it and stays up.
    void BuildUI(const Status& st, float uiAlpha);

    // True while Pro wants the HUD kept alive (pinned inspector).
    bool KeepHudAwake() const { return inspectorPinned_ || depthOverlay_; }

    ViewMode Mode() const { return mode_; }
    // The monitoring station: inputs, timecode / delay, rig checks, snapshots (Station.h).
    Station& GetStation() { return station_; }
    // Agent tools (ProTools.cpp): registered and dispatched from the trunk's MCP code.
    using RegisterFn = std::function<void(const char* name, const char* description, const char* schema)>;
    void RegisterTools(const RegisterFn& reg);
    // True when `tool` is a Pro tool (result / success set).
    bool DispatchTool(const std::string& tool, const std::string& argsJson, std::string& result, bool& success);
    float Zoom() const { return zoom_; }

    // Pro launches with eye-swap and convergence controls on the inspector, so the
    // trunk's top-bar title is replaced.
    static const char* ProductName() { return "DisplayXR Media Player Pro"; }

private:
    void SetMode(ViewMode m, bool announce);
    void BuildDepthOverlay(const Status& st);
    void BuildStatusBar(const Status& st);
    void ZoomAt(float factor, float u, float v);
    void SetZoom(float z);
    void ClampPan();
    void OpenLookDialog(bool cdl);
    static void DialogCallback(void* userdata, const char* const* filelist, int filter);

    VulkanRenderer* renderer_ = nullptr;
    SDL_Window* window_ = nullptr;
    Host host_;

    ViewMode mode_ = ViewMode::Stereo3D;
    float zoom_ = 1.0f;            // 1 = fit; >1 magnifies
    float panX_ = 0.0f, panY_ = 0.0f;  // top-left of the visible window, in eye-UV (0..1-1/zoom)
    bool panning_ = false;
    float panLastX_ = 0.0f, panLastY_ = 0.0f;

    // Grade.
    GradeParams grade_{};
    bool bypass_ = false;
    color::CubeLut lut_;
    std::string lutPath_, lutName_;
    color::Cdl cdl_;
    std::string cdlPath_, cdlName_;
    bool lutPending_ = false;      // lut_ parsed, waiting for the render thread to upload

    // File dialog result (SDL may call back on another thread).
    std::mutex dialogMutex_;
    std::string dialogPath_;
    bool dialogIsCdl_ = false;
    bool dialogReady_ = false;
    bool dialogOpen_ = false;
    bool pendingDialogIsCdl_ = false;

    bool inspectorPinned_ = true;  // Tab toggles
    Station station_;
    bool depthOverlay_ = false;    // D: the rig check's depth matches drawn on the picture
    std::string compare_[2];       // A (what was open) and B
    int compareSide_ = 0;          // 0 = A showing
    bool pendingCompare_ = false;  // B pressed: swap in Tick()
    bool rightOnlyView_ = false;
    bool stereoSource_ = false;
    bool wasFullscreen_ = false;   // full screen hides everything but the picture
    bool pinnedBeforeFullscreen_ = true;
    int ticks_ = 0, testCompareFrame_ = -1;  // MEDIAPLAYER_PRO_COMPARE_FRAME (screenshots)
};

}  // namespace mp::pro
