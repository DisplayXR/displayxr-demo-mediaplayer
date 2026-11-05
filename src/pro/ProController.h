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
//   - QC view modes: 3D, left eye only, right eye only, eye difference, anaglyph, false colour
//   - framing overlays + burn-ins (Overlays.h), scopes + pixel probe + zebras (Scopes.h)
//   - pan / zoom (identical for both eyes, so depth is preserved while zoomed)
//   - the grade: exposure, ASC CDL (.cc/.ccc/.cdl), 3D LUT (.cube), with bypass
//   - the inspector panel (ImGui) and the Pro key map
#pragma once

#include "color/Cdl.h"
#include "color/CubeLut.h"
#include "pro/CompareClip.h"
#include "pro/LookLoader.h"
#include "pro/TargetScreen.h"
#include "pro/OcioBake.h"
#include "pro/Overlays.h"
#include "pro/ResolveLink.h"
#include "pro/Scopes.h"
#include "pro/Station.h"
#include "rhi/VulkanRenderer.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;

namespace mp::pro {

enum class ViewMode { Stereo3D = 0, LeftOnly, RightOnly, Difference, Anaglyph, FalseColour, Count };
const char* ViewModeName(ViewMode m);

// Channel isolation (9 cycles) and the wipe (8 cycles), both in the grade shader.
enum class Channel { RGB = 0, Red, Green, Blue, Alpha, Luma, Count };
const char* ChannelName(Channel c);
// before | after the grade; left eye | right eye; this clip | the compare clip B
enum class Wipe { Off = 0, Grade, Eyes, Clips, Count };

// HDR tone map (#134), GradeParams::toneMap: 0 auto, 1 on (roll off), 2 off (clip), 3 raw.
inline const char* ToneMapName(int m) { return m == 1 ? "on" : m == 2 ? "off" : m == 3 ? "raw" : "auto"; }
inline int ToneMapFromName(const std::string& s, int fallback) {
    return s == "auto" || s == "0"  ? 0
           : s == "on" || s == "1"  ? 1
           : s == "off" || s == "2" ? 2
           : s == "raw" || s == "3" ? 3
                                    : fallback;
}
// An untagged source (#169), GradeParams::forceTransfer: 0 as tagged (SDR), 1 HLG, 2 PQ.
inline const char* UntaggedName(int m) { return m == 1 ? "hlg" : m == 2 ? "pq" : "sdr"; }
inline int UntaggedFromName(const std::string& s, int fallback) {
    return s == "sdr" || s == "0" ? 0 : s == "hlg" || s == "1" ? 1 : s == "pq" || s == "2" ? 2 : fallback;
}
inline const char* HdrTransferName(int t) {
    return t == 1 ? "HLG" : t == 2 ? "PQ" : t == 3 ? "Linear" : "SDR";
}
// "PQ -> SDR 1000 nits" / "Linear, clip" for the status bar; "" for SDR.
std::string HdrSummary(const VulkanRenderer::HdrState& h);
const char* WipeName(Wipe w);

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
    std::function<void(bool paused)> setMediaPaused;
    std::function<double()> mediaStart;  // the file's first timestamp (Position/Seek are in it)
    std::function<bool()> isFullscreen;
    std::function<void()> toggleFullscreen;
    std::function<void(float)> setDesqueeze;  // anamorphic: stretch the picture's width
    std::function<double()> mediaFps;         // the open video's frame rate, 0 when not a video
    std::function<std::string()> displayDeviceName;  // the 3D panel's OS monitor name (#133)
    // #133: the display the player's window is on: the runtime's stable key ("" on a
    // runtime before display_info v24) and its OS device name. False when unknown.
    std::function<bool(std::string& key, std::string& deviceName)> panelIdentity;
    std::function<bool()> displaysChanged;  // true once after the runtime's displays-changed event
    // #135 target-screen preview: show the picture at the angular size it has on a screen
    // widthM wide seen from distanceM (0, 0 = off); the scale it is shown at; the panel's
    // width and nominal viewing distance (false when the runtime does not say).
    std::function<void(float widthM, float distanceM)> setTargetScreen;
    std::function<float()> pictureScale;
    std::function<bool()> previewLimited;     // the target's scale is too large to preview
    std::function<float()> parallaxOffsetPct;  // what convergence adds to the shown parallax, % of width
    std::function<float()> viewerDistance;  // m: the tracked viewer, else the nominal distance
    std::function<bool(float& widthM, float& viewerM)> panelGeometry;
    // An RGBA texture for ImGui::Image (scope pictures), per slot: upload `rgba` (nullptr =
    // no change) and return its ImTextureID, nullptr when there is none.
    std::function<void*(int slot, int w, int h, const uint8_t* rgba)> userTexture;
};

class ProController {
public:
    void Init(VulkanRenderer* renderer, SDL_Window* window, Host host);

    // Test scaffolding: MEDIAPLAYER_PRO_VIEW=0..4 (view mode) and MEDIAPLAYER_PRO_ZOOM=<x>
    // set the initial view, so a headless atlas dump can prove them without keystrokes.
    void ApplyTestEnv();

    // Launch-time looks (--lut / --cdl, or MEDIAPLAYER_LUT / MEDIAPLAYER_CDL). A LUT
    // (and an OCIO bake) loads asynchronously: it applies a few frames later, in Tick().
    void LoadLut(const std::string& path);
    void LoadCdl(const std::string& path);
    // Per-panel display calibration (#133). Load `cube` as this panel's calibration and,
    // with `install`, keep it for this panel (copied to its file in the calibration folder,
    // so it loads automatically from then on). ClearCalibration drops it and, with
    // `uninstall`, deletes the panel's kept file - unless what is showing is a run-only
    // calibration (MEDIAPLAYER_PRO_CAL, or one whose keep failed): then only that is dropped
    // and the kept file stays (#169). False (with calError_ and a toast) when the delete
    // failed; the calibration then stays applied, matching the file still on disk.
    void LoadCalibration(const std::string& cube, bool install);
    bool ClearCalibration(bool uninstall);
    // #135: judge (and preview) the depth for a target screen; an off Screen = this panel.
    void SetTarget(const target::Screen& t);
    // An OpenColorIO transform baked into the grade's 3D LUT (needs a build with MP_OCIO_ROOT).
    // True = queued; config / colour-space errors surface later as lutError_.
    bool LoadOcio(const ocio::Request& req, std::string* err = nullptr);
    // A/B compare: the B clip; B (key) swaps to the other at the same position.
    void SetCompareB(const std::string& path) { compare_[1] = path; }
    void ToggleCompare();

    // Every uploaded frame (video, stream, live) and every still: the scopes' and the pixel
    // probe's source, and the colour-space burn-in. OnStill(nullptr) = frames again.
    void OnFrame(const FrameRing::Frame& f, StereoLayout layout);
    void OnStill(const uint8_t* rgba, int w, int h, StereoLayout layout);

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
    void ApplyViewTransform(ViewUV* uvs, uint32_t viewCount, bool stereo, bool eyesSwapped,
                            bool topBottom = false);

    // The inspector (ImGui). `uiAlpha` is the trunk's auto-hide fade; a pinned
    // inspector ignores it and stays up.
    void BuildUI(const Status& st, float uiAlpha);

    // Pinned panels are shown FLAT (crisp 2D, not woven with the picture): their HUD rects
    // from the last BuildUI, in HUD pixels; App hands them to XrSession::HudSubmit::flat.
    // Empty while the panels are unpinned (fading panels stay woven, so the picture is
    // never cut by a half-transparent card) or MEDIAPLAYER_PRO_PANELS_3D=1.
    struct FlatRect { float x, y, w, h; };
    const std::vector<FlatRect>& FlatRects() const { return flatRects_; }
    // App takes the rects once per frame: a frame that skips BuildUI (Shift+Tab hid the
    // UI while pinned) must not resubmit last frame's cards over a HUD with no panels.
    void ClearFlatRects() { flatRects_.clear(); }
    void SetFlatPanelsAvailable(bool on) { flatAvailable_ = on; }

    // True while Pro wants the HUD kept alive (pinned inspector).
    bool KeepHudAwake() const { return inspectorPinned_ || depthOverlay_ || OverlaysOn() || scopesOn_; }

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
    void BuildWipeLine(const Status& st);
    void SetWipe(Wipe w, bool announce);
    void BuildOverlays(const Status& st, float uiAlpha);
    void BuildOverlaySection();
    void BuildHdrControls();  // Inspector > Color: the HDR tone map (#134)
    // Inspector > Color > OpenColorIO: pick a config (the built-in ACES ones or a show's
    // .ocio), the input colour space, display and view, then bake it as the look (LoadOcio).
    void BuildOcioControls();
    void OpenOcioConfigDialog();
    void DescribeOcioConfig();  // fills ocioInfo_ for ocioUiConfig_ and picks the defaults
    std::string ocioUiConfig_ = "ocio://studio-config-latest";
    std::string ocioInfoFor_;  // the config ocioInfo_ describes ("" = not described yet)
    std::string ocioInfoError_;
    ocio::ConfigInfo ocioInfo_;
    int ocioSrc_ = 0, ocioDisplay_ = 0, ocioView_ = 0;
    void BuildMonitoringSection(const Status& st);
    void BuildScopesPanel(float alpha, float W, float H, float panelW);
    void BuildPixelProbe(const Status& st);
    void UpdateScopes();
    scopes::Grade ScopeGrade();
    bool OverlaysOn() const {
        return overlays_.safeAreas || overlays_.frameLine > 0 || overlays_.centreCross || overlays_.burnFrame ||
               overlays_.burnClip || overlays_.burnColour;
    }
    void BuildStatusBar(const Status& st, float alpha);
    void ZoomAt(float factor, float u, float v);
    void SetZoom(float z);
    void ClampPan();
    void OpenLookDialog(bool cdl);
    // The live Resolve panel (ProResolve.cpp): status out, playhead + commands in, follow.
    void TickResolve(const Status& st);
    void BuildResolveLines();
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
    LookLoader looks_;             // parses / bakes LUTs off the render thread
    std::string lutLoading_;       // source of the look being built ('' = none)
    std::string lutError_;         // why the last look failed ('' = it did not)
    // #134: an OCIO look on scene-linear EXR is baked for PQ-coded linear input. The request
    // is kept so a change of source (EXR <-> other) re-bakes it the other way.
    ocio::Request ocioReq_;
    bool ocioBakePqInput_ = false;  // the bake in flight was for PQ-coded linear input
    bool lutIsOcio_ = false;        // the LUT on the GPU is an OCIO bake ...
    bool lutPqShaper_ = false;      // ... baked for PQ-coded linear input
    bool SourceIsSceneLinear() const;

    // Display calibration (#133): per panel, after the grade and QC views. Not part of the
    // show grade, so G (bypass all) leaves it on; Shift+G bypasses it.
    LookLoader calLoader_;         // its own loader: a look load must not supersede it
    bool calResolved_ = false;     // the panel key was looked up (first Tick)
    std::string calKey_;           // this panel's key (pro/DisplayCalibration.h)
    bool calKeyFromRuntime_ = false;  // the runtime's stable key (display_info v24), not the device name
    int calCheckTicks_ = 0;        // frames since the panel was last looked up
    std::string calPath_, calName_;
    std::string calLoading_, calError_;
    uint64_t calLoadId_ = 0;       // the load still wanted; another finishing is dropped
    bool calInstall_ = false;      // the pending load is kept for this panel once it applies
    bool calLoaded_ = false;       // a calibration LUT is on the GPU
    bool calKept_ = false;         // ...and it is the panel's kept file (not run-only, #169)
    bool calBypass_ = false;       // Shift+G / the Inspector / set_display_calibration
    void ResolveCalibration();     // find this panel's key; auto-load its installed file
    void TickCalibration();        // apply a calibration the loader finished
    std::string CalibrationLabel() const;

    // Target screen (#135): off by default. With one set, the rig check reports depth in
    // its terms and alerts on background divergence; with the preview on (the default once
    // a target is chosen), the picture is shown at the angular size it has there.
    target::Screen target_;
    bool targetPreview_ = true;
    bool targetDirty_ = false;     // push target_ / targetPreview_ to the app + station
    float customTargetW_ = 3.0f, customTargetD_ = 4.0f;
    void ApplyTarget();
    void PushTargetToStation();  // target_ and how the picture is shown, for its figures and alerts

    // File dialog result (SDL may call back on another thread).
    std::mutex dialogMutex_;
    std::string dialogPath_;
    bool dialogIsCdl_ = false;
    bool dialogIsCompare_ = false;  // the "Load B..." dialog (the clip wipe's compare clip)
    bool dialogIsCal_ = false;      // the display calibration dialog (#133)
    bool dialogIsOcio_ = false;     // an OpenColorIO config (.ocio) for the OCIO picker
    bool dialogReady_ = false;
    bool dialogOpen_ = false;
    bool pendingDialogIsCdl_ = false;
    bool pendingDialogIsCompare_ = false;
    bool pendingDialogIsCal_ = false;
    bool pendingDialogIsOcio_ = false;
    void OpenCompareDialog();
    void OpenCalibrationDialog();

    bool inspectorPinned_ = false;  // Tab / the Pin box: panels stay up; unpinned they follow the UI fade
    std::vector<FlatRect> flatRects_;
    bool flatAvailable_ = false;    // the runtime takes Local2D layers (XR_DXR_local_3d_zone)
    bool flatPanels_ = true;        // MEDIAPLAYER_PRO_PANELS_3D=1 keeps pinned panels woven
    bool FlatNow() const { return flatAvailable_ && flatPanels_ && inspectorPinned_; }
    void AddFlatRectOfCurrentWindow();
    Station station_;
    bool depthOverlay_ = false;    // D: the rig check's depth matches drawn on the picture

    // Channel isolation, display gamma trim and the wipe (position across the picture as
    // shown, dragged with the left button near the line).
    Channel channel_ = Channel::RGB;
    float gamma_ = 1.0f;
    Wipe wipe_ = Wipe::Off;
    float wipePos_ = 0.5f;
    bool wipeDragging_ = false;
    CompareClip compareClip_;                    // B for Wipe::Clips, following the open clip
    void TickCompareClip();
    void TickLooks();  // apply a look the loader finished (render thread)
    std::string CompareOther() const { return compare_[1 - compareSide_]; }  // the clip not showing
    bool NearWipeLine(float x, float y) const;  // window points

    // Framing overlays (Q cycles the frame line) and the picture's anamorphic desqueeze.
    OverlaySettings overlays_;
    int appliedDesqueeze_ = 0;
    // Monitoring: scopes (W), pixel probe (P), zebras (7; level 0..1), false colour (6 = a view).
    bool scopesOn_ = false, parade_ = false, probeOn_ = false, zebraOn_ = false;
    float zebraLevel_ = 0.95f;
    float testProbeU_ = -1.0f, testProbeV_ = -1.0f;  // MEDIAPLAYER_PRO_PROBE: a fixed probe point
    static constexpr int kScopeW = 480;          // analysis width (left eye)
    scopes::ScopeWorker scopeWorker_;            // box filter, grade, bins, pictures: off the render thread
    bool scopeIsStill_ = false;                  // a still holds the analysis until frames return
    bool haveScopeSource_ = false;               // the worker holds an analysis image
    double nextScopeSec_ = 0.0;
    std::string gradeKey_;                       // the grade + picture settings last submitted
    std::shared_ptr<const color::CubeLut> lutSnap_;  // the LUT as the worker sees it
    const float* lutSnapData_ = nullptr;         // lut_'s data when lutSnap_ was taken
    void SubmitScopeJob(scopes::ScopeWorker::Job&& job);
    std::string ScopeKey();
    scopes::Result scope_;                       // the last finished measurement
    std::vector<uint8_t> waveRgba_, vecRgba_;
    bool scopeTexDirty_ = false;
    std::string colourTag_;                      // "BT.709 SDR, 10-bit" (the colour burn-in)
    std::string compare_[2];       // A (what was open) and B
    int compareSide_ = 0;          // 0 = A showing
    bool pendingCompare_ = false;  // B pressed: swap in Tick()
    bool rightOnlyView_ = false;
    bool stereoSource_ = false;
    bool wasFullscreen_ = false;   // full screen hides everything but the picture
    bool pinnedBeforeFullscreen_ = true;
    int ticks_ = 0, testCompareFrame_ = -1;  // MEDIAPLAYER_PRO_COMPARE_FRAME (screenshots)

    // Live Resolve panel (ProResolve.cpp).
    resolve::Follower follower_;
    resolve::LinkSample link_;
    bool followResolve_ = true;     // the inspector's "Follow Resolve" box
    double nextStatusSec_ = 0.0, nextLinkPollSec_ = 0.0;
    long long statusSeq_ = 0;
    long long linkFileStamp_ = -1;  // last_write_time of resolve_link.json last read
    long long commandsOffset_ = -1; // bytes of resolve_commands.jsonl already handled (-1 = skip history)
    long long lastCommandId_ = 0;
    std::string stationDir_;        // resolved once (the lookup walks the folder)
    void TickResolveImpl(const Status& st);
};

}  // namespace mp::pro
