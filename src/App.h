// SPDX-License-Identifier: Apache-2.0
//
// App — wires the SDL3 window, OpenXR session, and Vulkan renderer, and owns the
// frame loop. For M0 the loop clears the two eyes to distinct colors; M1+ adds
// decode + UI (PRD §4).
#pragma once

#include "media/AudioMeter.h"
#include "media/AudioPlayer.h"
#include "media/RecentList.h"
#include "media/Settings.h"
#include "media/AutoConvergence.h"
#include "media/CameraSource.h"
#include "media/DxrStereo.h"
#include "media/InputProviderClient.h"
#include "media/LiftClient.h"
#include "media/MediaSource.h"
#include "media/VideoDecoder.h"
#include "media/VrView.h"
#include "platform/Window.h"
#include "rhi/VulkanRenderer.h"
#include "ui/ImGuiLayer.h"
#include "ui/SettingsPanel.h"
#include "ui/TransportUI.h"
#include "xr/XrSession.h"
#if defined(_WIN32)
#include "browser/CloudFetch.h"
#endif
#if MEDIAPLAYER_PRO
#include "pro/PerfSwitches.h"
#include "pro/ProController.h"
#endif

// The in-app media browser (#252) on Windows. The basic player uses it for Open by default;
// Pro builds it in but uses it only when Settings turns it on (#267), as a 2D Open dialog.
// macOS / Linux keep the system dialog until they get a provider.
#if defined(_WIN32)
#define MEDIAPLAYER_BROWSER 1
#include "browser/BackgroundGate.h"
#include "browser/MediaBrowser.h"
#include "browser/WindowsMediaProvider.h"
#include "rhi/StereoGridRenderer.h"
#include <memory>
#else
#define MEDIAPLAYER_BROWSER 0
#endif

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mp {

class App {
public:
    // Command-line launch options (main.cpp), applied by Initialize(). The live-camera
    // fields mirror MEDIAPLAYER_CAMERA / MEDIAPLAYER_CAMERA_FPS, which fill them when the
    // command line did not.
    struct LaunchOptions {
        bool camera = false;         // --camera[=<selector>]: start on the live camera
        std::string cameraSelector;  // "" / auto | index | name substring (CameraSelect)
        int cameraFps = 0;           // --camera-fps=<N>; 0 = highest <= 60
        int proAtlas10Bit = -1;      // Pro --10bit (1) / --8bit (0); -1 = MEDIAPLAYER_PRO_10BIT, else 8-bit
        // --auto-conv[=nearest|sharp|centre] (#92); MEDIAPLAYER_AUTO_CONV fills it when the
        // command line did not. Empty policy = nearest.
        bool autoConv = false;
        std::string autoConvPolicy;
        std::string url;             // --url <u> (#93); MEDIAPLAYER_URL fills it otherwise
        // Media Player Pro looks: --lut <file.cube> / --cdl <file.cc|.ccc|.cdl>
        // (MEDIAPLAYER_LUT / MEDIAPLAYER_CDL fill them otherwise). Ignored by the base player.
        std::string lutPath;
        std::string cdlPath;
        // Media Player Pro station (src/pro/Station.h); ignored by the base player.
        std::vector<std::string> proInputs;  // --input="Name=spec", --decklink..., --test-signal
        float proBudget[5] = {-2.0f, 1.0f, 1.0f, 2.0f, -0.3f};  // near %, far %, vertical px, dE, edge %
        unsigned proBudgetFromCli = 0;  // bit i: proBudget[i] came from the command line (wins over settings)
        bool proTimecode = true;             // --no-timecode
        double proSeqFps = 24.0;             // --seq-fps: EXR / DPX / TIFF sequence rate
        std::string proCompare;              // --compare=<file>: the B clip (B swaps A/B)
        // --ocio=<config> --ocio-src= --ocio-display= --ocio-view= --ocio-dst= --ocio-looks=
        std::string ocioConfig, ocioSrc, ocioDisplay, ocioView, ocioDst, ocioLooks;
        bool ocio = false;
    };
    void SetLaunchOptions(const LaunchOptions& o) { launch_ = o; }  // before Initialize()

    // `mediaPath` may be null/empty — then the app falls back to the RED|BLUE
    // L/R test pattern instead of a loaded stereo image/video.
    bool Initialize(const char* mediaPath);
    int Run();          // returns process exit code
    void Shutdown();

    // One frame of work (video pull + locate + draw + submit + FPS). Public so the
    // window's live-resize event watch can drive it during the modal resize loop.
    void RenderOneFrame();

private:
    void UpdateFps();
    void BuildTransportUI();   // ImGui transport bar (no-op without ImGui)

    // Stereo-layout env vars (MEDIAPLAYER_STEREO_DETECT / MEDIAPLAYER_LAYOUT). Called
    // from Initialize() BEFORE the command-line file is loaded — they used to live with
    // the other env reads in Run(), which is too late to affect that first load.
    void ReadStereoEnv();

    // Open-file. RequestOpenFile tries the workspace picker, else a native dialog.
    // LoadMedia opens a path fresh; ReloadMedia tears down current media first.
    void RequestOpenFile();
    // The system file dialog (Open's fallback, and the browser's "Other files...").
    void ShowNativeOpenDialog();
    // The media browser (#252): shown by Open where there is no workspace picker.
    void ShowBrowser();
    void CloseBrowser();
    void BuildBrowser();
    bool BrowserNext(int delta, std::string& path) const;
    void ApplyFolderRequests();
    void ApplyItemRequests();
    // A cloud-only file is downloaded in the background before it opens (#252): the
    // download's end, and its progress panel (Cancel).
    void PollCloudFetch();
    void BuildCloudFetchPanel();
    bool CloudFetchBusy() const {
#if defined(_WIN32)
        return cloudFetch_.Busy();
#else
        return false;
#endif
    }
    // The browser's rendering mode (#252 phase 2): flat, or 3D under its "3D only" filter,
    // borrowed while it shows. End: hand it back, or (`keep`) leave the panel as it is.
    void RequestBrowserMode();
    void EndBrowserMode(bool keep);
    // The 3D grid draws this frame: the "3D only" filter, and the panel already in 3D.
    bool GridStereo(uint32_t viewCount) const;  // the browser's Add folder / pin / unpin / hide
    // Open folder (#252 phase 0): a native folder dialog; the folder becomes the arrow-key list.
    void RequestOpenFolder();
    // A path the user chose (a picker, a Recent row): a file opens as is, a folder opens
    // its first supported file. Either is recorded in Recent when it loads.
    // `play`: a folder also starts the slideshow (a folder handed over on a command line).
    void OpenChosenPath(const std::string& path, bool play = false);
    // The media browser's slideshow (#252): play `paths` from `start` as a fixed playlist.
    void StartBrowserSlideshow(std::vector<std::string> paths, std::vector<int64_t> ids, size_t start,
                               int skippedCloud);
    // Esc in a slideshow from the browser: stop it and show the browser again.
    void BackToBrowser();
    void OpenRecent(int row);
    void NoteRecent(const std::string& path);
    void RefreshRecentLabels();
    bool LoadMedia(const std::string& path);  // LoadMediaFile + the VR projection (#155)
    bool LoadMediaFile(const std::string& path);
    int loadSphKind_ = -1;           // the video probe's spherical metadata for this load (#155)
    float loadSphCoverage_ = 0.0f;
    int loadSphPadding_ = 0;         // cubemap face padding (px) from the same metadata
    void ReloadMedia(const std::string& path);

    // Drag and drop (#44). Filters the batch through MediaSource::IsSupported, toasts
    // what was rejected, then loads: one file behaves exactly like Ctrl+O, while a
    // multi-file drop installs the dropped set as a playlist (see playlistFixed_).
    void HandleDroppedPaths(std::vector<std::string> paths);

    // Folder navigation: scan the current file's directory for supported assets, and
    // step prev/next (wrapping). delta is +1 (next) or -1 (prev). `path` is taken BY
    // VALUE on purpose: callers pass a reference into folderFiles_, which this clears.
    void RebuildFolderList(std::string path);
    void NavigateMedia(int delta);
    // A cloud placeholder (Windows): a slideshow steps over it rather than download it (#252).
    static bool IsCloudOnly(const std::string& path);
    // The slideshow ran out of files to play: stop it and say so.
    void EndOfSlideshow();
    // Start a dip-to-black transition that navigates by `delta` at full black (manual
    // ←/→). Shares the fade machine with the slideshow.
    void RequestNavTransition(int delta);

    // Transient toast (e.g. convergence %). Shows `msg` for a short time, then fades.
    void ShowToast(const std::string& msg);

    // Stereo layout override (#45, #128): cycle auto -> mono -> SBS-full -> SBS-half -> TB-full -> TB-half -> auto.
    // Re-derives layout_ and contentAspect_ in place; never touches the decoder.
    void CycleLayoutOverride();
    // "SBS-full — detected" / "mono — from filename" / "SBS-full (R|L) — metadata" ...
    std::string LayoutLabel() const;
    // The packing name, or the container format when that is what the user knows it by
    // ("MV-HEVC", "stereo HEIC", #154). LayoutLabel() = this + eye order + signal.
    std::string LayoutShortName() const;
    // Commit a resolved layout: layout_, the provenance fields, and the manual pin
    // (cleared here rather than at load entry, so a FAILED load leaves the pin on the
    // media still being displayed).
    void ApplyLayout(const MediaInfo& info);

    // Per-frame UI state: advance the auto-hide fade, toast fade, and slideshow machine.
    void TickUi();
    // Depth-aware cursor (XR_DXR_cursor_depth spec v7, ADR-046 Amendment 3): while the
    // player shows STEREO media, ask the runtime to measure the disparity under the
    // pointer and draw the cursor lifted over popped-out content. Called once per
    // frame before BeginFrame. Off (and costing nothing) for mono / the idle logo.
    void UpdateDepthCursor();
    void ToggleSlideshow();
    // Idempotent form of the above — a dropped folder starts the slideshow, and must not
    // stop it when one is already running. Call AFTER the first asset is loaded: it reads
    // isVideo_ to decide whether the clip needs play-once.
    void SetSlideshow(bool on);
    // Idle screen: composite the DisplayXR idle art onto a dark-grey backdrop and upload
    // it as a mono texture, so launching with no file/folder shows the brand lockup
    // instead of the RED|BLUE test pattern. Prefers the composed "mark + Media Player"
    // idle.png; falls back to the bare logo.png, then the test pattern if both are absent.
    void LoadIdleLogo();
    // Alpha-composite a transparent idle image centered on the dark-grey backdrop and
    // upload it. `occupy` is the fraction of the square canvas the art fills (the rest is
    // grey margin). Returns false if the GPU upload fails. Helper for LoadIdleLogo.
    bool CompositeIdleArt(const struct DecodedImage& art, float occupy);
    void ClearIdleLogo();  // leave the idle screen: restore the black letterbox background
    // The idle screen is a 2D lockup, so it asks the runtime for a FLAT rendering
    // mode while it is up and hands the previous one back when media arrives (#64).
    // Both edges only QUEUE the request — it is issued from RenderOneFrame, because
    // LoadIdleLogo() runs during Init, before the session is running.
    void RequestFlatModeForIdle();
    // Mode chip: V / the Mode button cycle the display's rendering mode, and every mode
    // change shows a toast naming the new mode (the plug-in's mode name).
    void CycleRenderingMode();
    // The 2D/3D switch (basic player, #252): a 2D file converts to 3D or back; a 3D file
    // shows flat (the left eye) or back in 3D. V and the switch on the transport.
    void Toggle3D();
    // The panel is in, or about to be in, its flat mode.
    bool PanelHeadingFlat() const;
    void ToastRenderingMode();
    void OnRenderingModeChanged();  // the runtime reported a rendering-mode change
    void TogglePlayback();   // play/pause (video+audio); restarts if the clip already ended
    void ToggleMute();       // silence audio (keeps playing); persists across clips
    void StepFrame(int n);   // pause + step n frames (']' +1 / '[' -1)

    // Live camera source (#90). The camera is a SOURCE like a file: it replaces the
    // current media and publishes into a FrameRing the render path already consumes.
    // LoadLive resolves a selector (CameraSelect grammar) against a fresh device list;
    // LoadLiveDevice opens one device (asynchronously — PollLive reports the outcome).
    bool LoadLive(const std::string& selector);
    bool LoadLiveDevice(const CameraDevice& dev);
    // Make `p` (already opened) the live source: stop the camera if `p` is something
    // else, reset the per-source state and wait for its first frame to settle the
    // layout. The camera goes through here; so can another LiveProducer (Pro: DeckLink).
    void EnterLive(LiveProducer* p, const std::string& name);
    bool LiveIsCamera() const { return live_ == &camera_; }
    void StopLive(bool showIdleIfEmpty);  // tear down; idle logo if nothing replaces it
    void ToggleLive();                    // 'C': off -> LoadLive(last or auto); on -> StopLive
    void PollLive();                      // per-frame: camera state edges -> toasts / log
    void OnLiveFrame(const FrameRing::Frame& f);  // first frame / size change -> layout
    void RefreshCameraList(bool rescan);  // fills cameraDevs_ + uiState_.cameraNames
    std::string LiveStatusText() const;   // the top-bar pill's status word
    // Settle layout_ from a live frame's aspect (camera AND stream): first frame or a
    // mid-stream size change. `tag` prefixes the log line.
    void SettleLiveLayout(const FrameRing::Frame& f, const char* tag);

    // Stream URL source (#93) — the network sibling of the live camera. It rides
    // video_ in live mode (VideoDecoder::OpenLive), so a file, the camera and a stream
    // are mutually exclusive by construction. LoadUrl classifies (media/StreamUrl.h),
    // refuses SRT & co. with a toast, and connects asynchronously; PollStream turns
    // state edges into toasts / log lines.
    bool LoadUrl(const std::string& url);
    void StopStream(bool showIdleIfEmpty);
    void PollStream();
    void OpenUrlPrompt();                 // Ctrl+U / the combo row: the URL popup
    std::string StreamStatusText() const; // STREAM | FROZEN | CONNECTING | RECONNECTING | FAILED
    // HLS / DASH rendition for the HUD / logs (#205): "720p auto", "1080p" (manual), "" (none).
    std::string StreamQualityLabel(const VideoDecoder::StreamStats& ss) const;

    // Auto-convergence (#92). DEFAULT OFF: raw footage stays raw unless the operator turns
    // it on. SetAutoConv toggles; the policy cycles nearest -> sharp -> centre (Focus is
    // entered by Ctrl+click / the MCP tool). SampleAutoConv hands the frame the renderer
    // just acquired to the worker (~5 Hz, downsampled in place, no full-frame copy);
    // TickAutoConv advances the smoothed shift every rendered frame.
    // Files too (Part of #128): a PLAYING clip samples its frames like a live source; a
    // still or a paused / ended clip has one picture, which SampleAutoConvStatic measures
    // once and again only when what the measurement depends on changes. A new file, a seek
    // and a frame step are a different picture: the shift snaps (AutoConvNewPicture). A LIF
    // carrying its own convergence keeps it: auto-conv stands aside (AutoConvApplies).
    void SetAutoConv(bool on, const char* why);
    void SetAutoConvPolicy(autoconv::SubjectPolicy p);
    void SetAutoConvFocus(float u, float v);  // left-eye normalised; switches to Focus
    void SampleAutoConv(const FrameRing::Frame& f);
    void SampleAutoConvStatic();
    void SubmitAutoConv(const uint8_t* luma, int w, int h, double nowMs);
    const uint8_t* AutoConvLuma8(const FrameRing::Frame& f);  // P016 -> 8-bit, else plane[0]
    void AutoConvNewPicture(bool newMedia);  // a seek (false) or a new file (true): snap
    void KeepStillForAutoConv(const uint8_t* rgba, int w, int h);
    // Packed stereo on screen (SBS or TB) from a live source or a file, not a LIF's own
    // convergence. Mono, Dual (this leg decodes its first track only), the idle logo and a
    // bound "Convert to 3D" picture (layout_ stays Mono) never measure.
    bool AutoConvApplies() const;
    // The estimate's shift as applied now: only on the composed pair it measured (signed for
    // the eye order on screen), and 0 while A measures the same picture itself.
    float MediaAutoShift() const;
    void TickAutoConv();
    std::string AutoConvHudText() const;

    // "Convert to 3D" (XR_DXR_lift spec v4): the runtime's 2D->3D conversion turns MONO
    // content into a side-by-side pair that replaces the picture. Off on every load; the
    // player never converts on its own. The toggle is hidden when the runtime does not
    // offer the extension or the content is not mono (ConvertEligible), and disabled
    // (with the reason) when the conversion task cannot serve this session.
    //   SetConvert     — the toggle (Shift+L, the top-bar button, MCP set_convert_3d).
    //   StopConvert    — off; `restorePicture` re-uploads the source picture over a bound
    //                    converted one (a still from its kept pixels, video / live from
    //                    the last frame handed to the converter until the next decode).
    //   ResetConvertStream — discontinuity (seek, live size change): recreate the stream.
    //   PumpConvert    — per frame: still submits, Poll, upload a landed result.
    bool ConvertEligible() const;
    bool SetConvert(bool on, const char* why);
    void StopConvert(bool restorePicture);
    void ResetConvertStream();
    void PumpConvert(double nowSec);
    bool SubmitConvertFrame(const FrameRing::Frame& f, double nowSec);
    void KeepStillForConvert(const uint8_t* rgba, int w, int h);
    // Provider selector (XR_DXR_lift spec 5): pick a candidate by id ("" = the default) as
    // the hint for this session's creates. "" on success, else why not. Never persisted.
    std::string SetConvertProvider(const std::string& id, const char* why);
    // Input providers (XR_DXR_input_providers): the families the Input popup shows, the
    // provider index behind each of a family's rows (row 0 = Default = -1), and a pick.
    std::vector<XrInputRoleFamilyDXR> InputFamiliesShown() const;
    std::vector<int> InputRowProviders(XrInputRoleFamilyDXR family) const;
    std::string SetInputProvider(XrInputRoleFamilyDXR family, const std::string& id, bool persist,
                                 const char* why);

    // Agent tools (XR_DXR_mcp_tools). SetupAgentTools registers the player's controls as
    // MCP tools on the runtime's per-process server (no-op when the MCP gate is off);
    // DispatchAgentTool runs a tool invocation against live player state — it executes on
    // the main loop (inside xr_.PollEvents), so it touches state without locking.
    void SetupAgentTools();
    std::string DispatchAgentTool(const std::string& tool, const std::string& argsJson,
                                  bool& success);
    // SDL native-dialog callback (Tier-0 fallback). May fire on another thread, so it
    // just hands the path to the main loop through the guarded slot below.
    static void NativeFileCallback(void* userdata, const char* const* filelist, int filter);

    Window window_;
    XrSession xr_;
    VulkanRenderer renderer_;
    VideoDecoder video_;
    AudioPlayer audio_;
    ImGuiLayer imgui_;
    bool muted_ = false;   // audio mute (M / speaker button); persists across clips
    float volume_ = 1.0f;  // 0..1, Pro's volume slider (LOXR-872); file and live audio
    void SetVolume(float v);
    // A/V sync, ms (LOXR-873): + = audio heard later than the picture. Read by the decode
    // thread through the master clock; 0 (the default, and always in the base player) = none.
    std::atomic<int> avOffsetMs_{0};
    void SetAvOffsetMs(int ms);
    int soloChannel_ = -1;  // Pro's channel solo (LOXR-874); -1 = none, reset per clip
    void SetSoloChannel(int ch);
#if MEDIAPLAYER_PRO
    AudioMeter audioMeter_;  // Pro's meters (LOXR-871): fed by the file and DeckLink audio
#endif

    bool hasMedia_ = false;       // an image or video was loaded (vs the idle logo)
    bool isLogo_ = false;         // the idle DisplayXR logo screen is showing (no media)
    bool isVideo_ = false;
    StereoLayout layout_ = StereoLayout::Mono;
    float contentAspect_ = 1.0f;  // per-eye display aspect (width/height), for letterboxing
    // The aspect the picture is fitted with: contentAspect_, stretched by Pro's anamorphic
    // desqueeze (1 in the base player).
    float proDesqueeze_ = 1.0f;
    // In the VR viewer (#155) the picture is a window into the scene that FILLS the window:
    // the fit aspect is the canvas's own, so nothing is letterboxed.
    float FitAspect() const;

    // --- VR180 / VR360 viewer (#155) -------------------------------------------------
    // A VR source is a PROJECTION on top of the packing (layout_ still says SBS / TB /
    // mono). Detected per file: container spherical metadata, then file-name hints; `P`
    // and MCP set_layout {projection} pin it; MEDIAPLAYER_VR seeds a pin for headless runs.
    vr::Projection vrAuto_ = vr::Projection::None;  // detected for the current media
    const char* vrSignal_ = "";                     // "metadata" / "filename" / "manual" / ""
    bool vrPinned_ = false;
    vr::Projection vrPinnedValue_ = vr::Projection::None;
    bool vrForced_ = false;                          // MEDIAPLAYER_VR
    vr::Projection vrForcedValue_ = vr::Projection::None;
    vr::Projection vrProj_ = vr::Projection::None;   // effective
    float vrFisheyeFovDeg_ = 180.0f;                 // dual-fisheye lens FOV (set_layout)
    int vrCubePadPx_ = 0;                            // cubemap face padding (container metadata)
    vr::ViewState vrView_;                           // yaw / pitch / fov the user steers
    vr::Limits vrLimits_;
    uint32_t vrViewGen_ = 0;                         // bumps on every view change (auto-conv)
    std::vector<uint8_t> vrAutoConvBuf_;             // the rendered eye windows, SBS luma
    bool VrActive() const;
    bool VrStereo() const { return packing::IsPacked(layout_); }
    // Life-size horizontal FOV for the current window, and the effective FOV (zoomed).
    float VrDefaultFovDeg() const;
    float VrFovDeg() const { return vrView_.fovDeg > 0.0f ? vrView_.fovDeg : VrDefaultFovDeg(); }
    float VrWindowAspect() const;
    // Resolve the projection for freshly loaded media (after ApplyLayout): spherical
    // metadata (kind / coverage from VideoStereoProbe, kind < 0 = none), then the name.
    // A VR source whose packing no strong layer decided takes the packing its aspect implies.
    void ResolveVr(const std::string& path, int sphericalKind, float sphericalCoverageDeg, int frameW, int frameH,
                   int sphericalPadding = 0);
    void SetVrProjectionPin(bool pin, vr::Projection p, const char* why);
    void CycleVrProjection();  // `P`: auto -> flat -> VR180 -> VR360 -> fisheye -> cubemap -> EAC -> auto
    void VrSteer(float dYawDeg, float dPitchDeg, float fovScale);  // clamps, bumps vrViewGen_
    void VrRecenter(const char* why);
    void VrHandleInput(bool& activity);
    // A VR projection just came on screen: `toast`, then (once, kept in hintsFile_) the
    // "drag to look" hint (#251).
    void VrViewerCameOn(const std::string& toast);
    // The projection picker's rows (#251): [0] auto, then kVrPickerOrder.
    void FillProjectionPicker();
    void SelectProjectionRow(int row);
    std::string VrViewJson() const;  // MCP get_view / set_view result

    // #133: the display the window is on, from the runtime's enumeration: on Windows the
    // monitor Windows itself says holds the window (matched by GDI name, so no DPI maths),
    // elsewhere the display whose desktop rect holds the window's centre; else the system
    // default display. Falls back to the default display's device name on a runtime that
    // does not enumerate.
    bool PanelIdentity(std::string& key, std::string& deviceName) const;
    // #135 Pro target-screen preview: the target's picture width and viewing distance (0 =
    // off) and the scale last applied. TargetScaled() scales the fitted picture about its
    // centre so it subtends the angle it does on the target (pro/TargetScreen.h).
    float proTargetW_ = 0.0f, proTargetD_ = 0.0f;
    float proPictureScale_ = 1.0f;
    bool proPreviewLimited_ = false;     // the target's scale does not fit a viewport: not previewed
    float proParallaxOffsetPct_ = 0.0f;  // what convergence adds to the shown parallax, % of the picture
    XrSession::ViewRect TargetScaled(XrSession::ViewRect fit);
    // The viewer's distance from the panel as the runtime renders it this frame (follows
    // eye tracking; the nominal distance when nobody is tracked), smoothed over ~0.5 s.
    // From each eye's off-axis frustum: the canvas's physical width over
    // tan(right) - tan(left), so it does not depend on where the reference space sits.
    float proViewerDistM_ = 0.0f;
    void UpdateViewerDistance(const XrView* views, uint32_t n, uint32_t canvasW);

    // Layered stereo-layout detection (#45). layoutSignal_ records WHICH layer decided
    // layout_, so the HUD can name it. autoInfo_ caches the automatic verdict, which is
    // what lets `L` toggle back out of a manual pin without re-decoding the media.
    StereoSignal layoutSignal_ = StereoSignal::Default;
    float layoutConfidence_ = 1.0f;
    bool mediaEyeSwap_ = false;   // container says the packing is R|L; XORs with swapEyes_
    MediaInfo autoInfo_{};
    bool layoutPinned_ = false;   // the user pinned a layout with L / the HUD button
    StereoLayout layoutPinnedValue_ = StereoLayout::Mono;
    // MEDIAPLAYER_LAYOUT seeds a pin for headless runs. Loads clear layoutPinned_, so the
    // forced value has to be re-applied at each load; these two remember it.
    bool layoutForced_ = false;
    StereoLayout layoutForcedValue_ = StereoLayout::Mono;
    // MEDIAPLAYER_STEREO_DETECT: off | meta | full.
    //   full (DEFAULT) — filename, container metadata, the pixel detector, then 2D.
    //                    The detector is what finds an untagged stereo file; anything it
    //                    cannot decide on opens as 2D (mono).
    //   meta           — skip the pixel detector. Everything unidentified is 2D.
    //   off            — no probe at all, so container metadata is ignored too. A perf
    //                    escape hatch, not a normal setting.
    enum class DetectMode { Off, Meta, Full };
    DetectMode detectMode_ = DetectMode::Full;

    // M4 stereo controls. convergence_ is horizontal image translation as a fraction
    // of a view tile (each eye shifted oppositely → moves the zero-disparity plane);
    // swapEyes_ flips which SBS half feeds each eye.
    float convergence_ = 0.0f;
    bool swapEyes_ = false;
    // Per-media baked convergence from a LIF's metadata (0 for non-LIF). Applied on
    // top of the user's convergence_ so the image lands at the author's intended
    // zero-disparity plane; the user slider still trims from there. The 0.5 factor maps
    // the LIF's normalized convergence to this app's per-tile shift (matches the
    // reference ViewShift: per-eye shift = 0.5 * convergence of an eye width).
    float mediaConvergence_ = 0.0f;
    // The loader's auto-convergence estimate for a stereo still with NO convergence of its
    // own (LIF without the field, MPO, stereo HEIC): a per-eye shift, fraction of the eye's
    // width. Applied by default (#180): 0 turns it off, Backspace back on, a new file resets.
    // A DisplayXR Camera file (src/media/DxrStereo.h) puts its own suggested convergence
    // here instead of an estimate: a still's dxr:Convergence, a clip's static one. A clip's
    // per-frame track, when it has one, replaces it frame by frame (MediaAutoShift).
    bool mediaAutoConvAvailable_ = false;
    float mediaAutoConvergence_ = 0.0f;
    bool mediaAutoConvOn_ = true;
    bool mediaConvFromFile_ = false;          // mediaAutoConvergence_ is the file's own suggestion
    dxrfile::ConvergenceCurve dxrClipCurve_;      // a camera clip's per-frame convergence (may be empty)

    // The shared, platform-neutral UI state (scrubber drag machine, auto-hide fade,
    // toast). Lives in ui/TransportUI.h so the Android leg drives the exact same
    // widget code — see that header for the ownership rules.
    ui::TransportState uiState_{};

    // Open-file flow. openFilePending_ gates the Open button while a picker is up.
    // The native-dialog callback may run off-thread; it parks the result here.
    bool openFilePending_ = false;
    bool nativeDialogUp_ = false;     // a desktop file/folder dialog asked for, no answer yet (#276)
    bool browserFirstFrame_ = false;  // the browser's first frame after ShowBrowser (#276)
    std::mutex nativePathMutex_;
    std::string nativePath_;
    bool hasNativePath_ = false;
    // Recent menu (#252 phase 0): what the user opened on purpose, kept in recentFile_
    // (empty = not kept). Arrow-key walks and the slideshow do not add to it.
    RecentList recent_;
    std::string recentFile_;
    // One-time hints already shown (#251), one id per line, beside recentFile_ (empty = not
    // kept: then a hint shows once per run).
    std::string hintsFile_;
    bool vrHintShown_ = false;
    bool vrHintPending_ = false;  // shown, but waiting for the toast slot
    // Settings (#265): settings.txt beside recent.txt, and the window that edits it. Saved
    // ~0.5 s after the last change (never mid-drag) and at shutdown.
    Settings settings_;
    ui::SettingsPanel settingsPanel_;
    // The transport UI had a menu, popup or focused text field open when it last drew (#276).
    // Kept across frames that draw no UI (shouldRender false, a mode switch, no HUD image), so
    // an Esc on such a frame still goes to the UI instead of the window's quit.
    bool uiHoldsKeys_ = false;
    uint64_t settingsGen_ = 0;
    std::chrono::steady_clock::time_point settingsChangedAt_{};
    void RegisterSettingsRows();
    void TickSettings(bool flush);
    void OpenSettings(const char* via);  // the gear / menu ("ui"), Ctrl+, ("key"), Pro's Station ("station")
    // The media browser may serve Open (it exists, or Pro can make it, and the setting allows it).
    bool BrowserAllowed() const;
    bool BrowserAvailable() const;  // ... whatever the setting says
    bool openAtDone_ = false;       // MEDIAPLAYER_OPEN_AT (test)

#if MEDIAPLAYER_BROWSER
    std::unique_ptr<browser::WindowsMediaProvider> browserProvider_;
    std::unique_ptr<browser::MediaBrowser> browser_;
    // The 3D grid (#252 phase 2, docs/BROWSER.md "3D grid"): draws the tiles into the
    // projection atlas, each eye's crop into its views, under the "3D only" filter.
    StereoGridRenderer grid_;
    bool gridReady_ = false;
    browser::ModeBorrow browserMode_;
    // Pro (#267): the browser is made on the first Open that wants it, never before, so a
    // station that leaves it off runs none of it. False once a make failed.
    bool browserDeferred_ = false;
    bool CreateBrowser();
    bool EnsureBrowser();
    std::string BrowserThisFolder() const;  // Pro's "This folder": the open file's, else the last opened
    browser::BackgroundGate browserGate_;   // Pro: capture first, thumbnails after
    void BuildLiveChip();                   // Pro: "LIVE: <input> <timecode>" over the browser
    float liveChipW_ = 0.0f;                // ... its width last frame, kept free in the status bar
#endif
#if defined(_WIN32)
    browser::win::CloudFetch cloudFetch_;
#endif
    // What asked for the open a cloud download is for: a user open adds the file to Recent,
    // the browser also makes up/down walk its grid. Set around those ReloadMedia calls.
    enum class OpenIntent { Other, User, Browser };
    OpenIntent openIntent_ = OpenIntent::Other;
    OpenIntent cloudIntent_ = OpenIntent::Other;
    bool browsing_ = false;          // the browser covers the player
    bool browserNav_ = false;        // up/down walk the browser's grid, not folderFiles_
    bool resumeAfterBrowse_ = false;  // a video was playing when the browser opened
    // Pinned folders and folders hidden from Media folders, kept beside the Recent list.
    RecentList pinned_{64}, hiddenFolders_{256};
    std::string pinnedFile_, hiddenFile_;
    bool pinPickPending_ = false;  // the folder dialog is choosing a folder to pin
#if MEDIAPLAYER_BROWSER
    // The browser's view (grid / details, card size, sort), kept between launches beside
    // pinned.txt; written when it changes and no drag is under way. Empty: not kept.
    std::string browserPrefsFile_;
    browser::DeskViewPrefs browserPrefsSaved_;
#endif

    // Folder navigation / slideshow. folderFiles_ holds the supported assets in the
    // current media's directory (sorted); folderIndex_ points at the loaded one.
    std::string currentMediaPath_;
    std::vector<std::string> folderFiles_;
    size_t folderIndex_ = 0;
    // Set when folderFiles_ is a fixed playlist rather than a directory scan: a multi-file
    // drop (#44), or a slideshow from the media browser (#252). RebuildFolderList() honours
    // it and re-locates the index inside the existing list instead of rescanning the parent
    // directory, which would otherwise destroy the playlist on the very next load. Cleared
    // by any later single-target open.
    bool playlistFixed_ = false;
    // The playlist is a slideshow the media browser asked for (#252): Esc goes back to the
    // browser instead of quitting, as long as this list is the one walked. playlistIds_ are
    // the browser's ids, in step with folderFiles_ (the ring goes to the file it ended on).
    bool fromBrowserList_ = false;
    bool startingBrowserList_ = false;  // its first file is loading: ReloadMedia keeps the flag
    std::vector<int64_t> playlistIds_;

    // Live camera (#90). isLive_ is mutually exclusive with file media: every file load
    // goes through ReloadMedia, which stops the camera first.
    CameraSource camera_;
    LiveProducer* live_ = &camera_;     // the producer live mode reads (isLive_)
    std::vector<uint8_t> autoConvLuma8_;
    int dumpFrame_ = 100;               // test: MEDIAPLAYER_DUMP_FRAME (which rendered frame the dumps take)  // P016 frames: 8-bit luma for auto-convergence
    LaunchOptions launch_{};
    bool isLive_ = false;               // the live camera is the current source
    bool livePaused_ = false;           // Space = freeze (the camera keeps streaming)
    std::vector<CameraDevice> cameraDevs_;  // NON-denied only, same order as uiState_.cameraNames
    std::string liveSelector_;          // last opened device name, for the 'C' re-open
    CameraSource::State liveStateSeen_ = CameraSource::State::Closed;  // PollLive edge detector
    double liveAgeMs_ = 0.0;            // publish -> upload age of the last uploaded frame
    double liveSlowSince_ = -1.0;       // start of a delivered-fps shortfall (rung-4 hint)
    bool liveSlowWarned_ = false;

    // Auto-convergence (#92). autoConvergence_ is the smoothed per-eye shift as a fraction
    // of the EYE's on-screen width (not the tile's), applied on top of convergence_ and
    // mediaConvergence_. It is exactly 0 while the feature has never been enabled.
    bool autoConvEnabled_ = false;
    autoconv::SubjectPolicy autoConvPolicy_ = autoconv::SubjectPolicy::Nearest;
    bool autoConvHasFocus_ = false;
    float autoConvFocus_[2] = {0.5f, 0.5f};
    float autoConvergence_ = 0.0f;
    autoconv::AutoConvergenceWorker autoConvWorker_;
    autoconv::AutoConvergenceController autoConvCtl_;
    autoconv::Result autoConvLast_{};
    bool autoConvHaveResult_ = false;
    double autoConvLastSubmitMs_ = -1e9;  // steady ms of the last worker submission
    double autoConvDownsampleMs_ = 0.0;   // render-thread cost of the last submission
    double autoConvLastTickSec_ = -1.0;
    double autoConvLastLogSec_ = 0.0;
    // Applied-value stats since the last enable (get_status / the 1 s log line).
    float autoConvMin_ = 0.0f, autoConvMax_ = 0.0f;
    double autoConvSum_ = 0.0, autoConvMsSum_ = 0.0, autoConvMsMax_ = 0.0;
    uint64_t autoConvTicks_ = 0, autoConvMeasures_ = 0;
    // Files (Part of #128). Every submission carries a tag; a measurement of a frame from
    // before a seek / load (tag < autoConvChangeFrom_) is dropped while a change is pending.
    uint64_t autoConvTag_ = 0;
    int autoConvChangePending_ = 0;    // 0 none, 1 glide (same picture), 2 snap (new picture)
    uint64_t autoConvChangeFrom_ = 0;
    double autoConvSnapUntilMs_ = -1e9;  // a seek's settle window: submissions in it snap
    uint64_t autoConvSnapLastTag_ = 0;   // the last submission made inside that window
    uint64_t autoConvMedia_ = 0;       // bumped per file load
    bool mediaHasBakedConv_ = false;   // a LIF with its own convergence: auto-conv stands aside
    const FrameRing::Frame* autoConvHeldFrame_ = nullptr;  // video_'s frame on screen (consumer slot)
    std::vector<uint8_t> autoConvStill_;  // a still's luma (<= 1920 wide), kept at load
    int autoConvStillW_ = 0, autoConvStillH_ = 0;
    // What the last file submission measured; the static path re-measures when it differs.
    struct AutoConvKey {
        bool valid = false;
        uint64_t media = 0;
        int64_t serial = 0;  // the clip frame's ring serial (0 for a still)
        StereoLayout layout = StereoLayout::Mono;
        bool swap = false;
        autoconv::SubjectPolicy policy = autoconv::SubjectPolicy::Nearest;
        float fx = -1.f, fy = -1.f;  // the focus pin (-1 = none)
        uint32_t vrGen = 0;          // the VR view (#155): a pan / zoom is a new picture to measure
        bool Same(const AutoConvKey& o) const {
            return valid && o.valid && media == o.media && serial == o.serial && layout == o.layout &&
                   swap == o.swap && policy == o.policy && fx == o.fx && fy == o.fy && vrGen == o.vrGen;
        }
    };
    AutoConvKey autoConvKey_;
    AutoConvKey AutoConvKeyNow(int64_t serial) const;

    // Stream URL (#93). isStream_ excludes isLive_ and isVideo_ (a stream is not a
    // seekable video: no transport bar, no scrub, no slideshow).
    bool isStream_ = false;
    // Live sources (camera #90 or stream #93) share freeze, the pill and auto-convergence.
    bool LiveSource() const { return isLive_ || isStream_; }
    bool LivePaused() const { return isLive_ ? livePaused_ : streamPaused_; }
    bool streamPaused_ = false;         // Space = freeze (the stream keeps decoding)
    std::string streamUrl_;             // what is open (raw, may carry credentials)
    VideoDecoder::StreamState streamStateSeen_ = VideoDecoder::StreamState::Idle;
    double streamLogAt_ = 0.0;          // next periodic stats log line (steady seconds)

    int mediaW_ = 0;              // full frame dims, for the HUD label
    int mediaH_ = 0;
    std::vector<uint8_t> hudPixels_;  // CPU-rasterized HUD buffer
    double lastHudResize_ = -1.0;     // SteadySeconds() of the last HUD swapchain resize

    // Frame / FPS tracking.
    uint64_t frames_ = 0;
    uint64_t rendered_ = 0;
    const char* prevMode_ = "";
    std::chrono::steady_clock::time_point fpsWindowStart_{};
    uint32_t fpsWindowFrames_ = 0;
    float fps_ = 0.0f;
    bool depthCursorOn_ = false;        // measured-disparity request is live (stereo media)
    bool depthCursorAllowed_ = true;    // MEDIAPLAYER_DEPTH_CURSOR=0 turns it off
    bool depthCursorPinned_ = false;    // MEDIAPLAYER_DEPTH_CURSOR_UV="u,v": test pointer
    float depthCursorPinU_ = 0.0f, depthCursorPinV_ = 0.0f;
    uint32_t depthCursorLogTick_ = 0;
    bool showHud_ = true;         // master UI enable (SHIFT+TAB); auto-hide governs the rest
    bool inRenderFrame_ = false;  // reentrancy guard (live-resize watch vs main loop)

    // Auto-hide: the UI fades in on activity and out after kIdleHideSeconds. The alpha
    // itself lives in uiState_; lastActivity_ is reset on mouse-move / control input.
    std::chrono::steady_clock::time_point lastActivity_{};
    std::chrono::steady_clock::time_point lastFrameTime_{};
    // Resting cursor position: motion is "real" only when it moves more than a few px
    // from here, so sensor jitter near rest can't keep the UI awake. <0 = uninitialized.
    float restMouseX_ = -1.0f;
    float restMouseY_ = -1.0f;

    // Slideshow ("diaporama"): auto-advance through folderFiles_. Stills hold for
    // kStillSeconds; videos play to the end. Transitions dip to black (transitionAlpha_).
    double slideshowImageElapsed_ = 0.0;
    enum class Transition { Playing, FadeOut, FadeIn };
    Transition transition_ = Transition::Playing;
    int pendingNavDelta_ = 0;       // navigation to apply at full black (slideshow or ←/→)

    // Idle-screen mode borrow (#64). pendingModeRequest_ is drained once per frame;
    // modeBeforeIdle_ is what to hand back; idleModeRequested_ is what we asked for,
    // kept so a mode the USER picked while idle is not undone on the way out.
    int32_t pendingModeRequest_ = -1;
    bool userModeChoice_ = false;  // V / the Mode button was used: Pro's auto-3D stays out
    // The 2D/3D switch's choice, kept while walking the list (Up / Down, the slideshow) and
    // dropped by any other open (#252): 3D files shown flat, 2D files converted.
    bool walking_ = false;          // NavigateMedia's load in progress
    bool stickFlat_ = false;
    bool stickConvert_ = false;
    std::string stickConvertTried_;  // the file the kept conversion was last started on
    int32_t modeBeforeIdle_ = -1;
    int32_t idleModeRequested_ = -1;
    // Mode chip: the idle borrow's target, whose change event is not toasted (consumed by
    // that event; cleared by a user mode change); and the once-at-start-up toast.
    int32_t silentModeTarget_ = -1;
    bool initialModeToastDone_ = false;

    // "Convert to 3D" (XR_DXR_lift). convertBound_: the renderer's texture is a converted
    // side-by-side result, so the per-view UVs treat the content as SBS-full while layout_
    // stays Mono (contentAspect_ = the mono frame's aspect: the result is two full eyes).
    lift::LiftClient lift_;
    // Input providers. inputRemember_ is the popup's "Remember for this app" checkbox: off
    // at every launch (never saved by the player); when on, a pick persists the per-app pin.
    input::InputProviderClient input_;
    bool inputRemember_ = false;
    bool convertBound_ = false;
    bool convertStillPending_ = false;  // still: submit until its result lands
    double convertStillSentAt_ = 0.0;   // look-around: when the still last went for conversion
    // Full-resolution pixels of the loaded still, kept ONLY when the runtime offers the
    // extension (so the base player on a runtime without it holds no extra copy): the
    // source to convert, and what toggling off puts back.
    std::vector<uint8_t> convertStill_;
    int convertStillW_ = 0, convertStillH_ = 0;
    std::vector<uint8_t> convertIn_;  // the last frame handed to the converter (RGBA8)
    uint32_t convertInW_ = 0, convertInH_ = 0;
    // The last video frame taken from the ring (the ring keeps it until the next take, so it
    // stays valid while the clip is paused) and the serial of the frame last handed to the
    // converter: a paused clip decodes nothing new, so PumpConvert hands this one over.
    const FrameRing::Frame* convertLastFrame_ = nullptr;
    int64_t convertSentSerial_ = -1;

#if MEDIAPLAYER_PRO
    // Media Player Pro (src/pro/, docs/PRO.md). Everything Pro adds hangs off pro_;
    // the trunk only calls into it from the `#if MEDIAPLAYER_PRO` hooks in App.cpp.
    pro::ProController pro_;
    void InitPro();
    pro::Status ProStatus() const;
    // Last frame's on-screen picture rect in window POINTS (pre-convergence), and the
    // per-eye content width in tile pixels: Pro's pointer->picture mapping and 1:1 zoom.
    float proPicX_ = 0.f, proPicY_ = 0.f, proPicW_ = 0.f, proPicH_ = 0.f;
    float proTileContentW_ = 0.f;
    // Inspection UI (docs/PRO.md, "UI"). Tab / the strip's Panels button shows or hides
    // the full UI, which then never times out. With the panels hidden, moving the mouse
    // shows a transient transport strip (and the pointer) that fades after
    // kStripHideSeconds, or at once on a click on the picture. Keys, the wheel and
    // Ctrl+click never show or hide UI. Shift+Tab is the clean feed: nothing is drawn,
    // not even the pointer. Decided in TickUi.
    bool proStrip_ = true;          // the strip is up (starts up, then fades)
    bool proHideStrip_ = false;     // main loop: a click on the picture / the pointer left
    std::chrono::steady_clock::time_point proStripSince_{};
    float proRestX_ = -1.0f, proRestY_ = -1.0f;  // pointer rest point for the 8 px wake
    bool proPanelsBeforeClean_ = false;
    bool proPanelsSeen_ = false;    // edge detector: Tab pressed while in the clean feed
    // A/B switches for the render thread's optional per-frame work (pro/PerfSwitches.h),
    // read once in InitPro: App skips auto-convergence sampling (autoConv) and the UI while
    // live (ui); the station gets the rest.
    pro::PerfSwitches proPerf_;
#endif
    // The OS pointer is hidden while the runtime draws the depth cursor, and while the UI
    // is idle; ApplyCursorHidden() combines the two.
    bool depthCursorShown_ = false;
    bool cursorHiddenApplied_ = false;
    // The pointer is hidden with the UI: Pro's strip, the basic player's chrome (#251).
    bool cursorIdle_ = false;
    bool KeyboardHeldByUi() const;  // #276: the window keeps no hotkeys while this is true
    void ApplyCursorHidden();
#if MEDIAPLAYER_PRO
    float proHud_[4] = {0.f, 0.f, 1.f, 1.f};  // HUD rect in window fractions (x, y, w, h)
#endif

    // Test scaffolding (env-gated).
    int startMode_ = -1;
    bool startModeRequested_ = false;
    const char* dumpPath_ = nullptr;
    bool dumped_ = false;
    bool convertAtStart_ = false;          // MEDIAPLAYER_CONVERT: toggle Convert to 3D on
    std::string convertProviderAtStart_;   // MEDIAPLAYER_CONVERT_PROVIDER: pick this id first
    std::string inputRequestAtStart_;      // MEDIAPLAYER_INPUT_REQUEST: "<family>=<id>"
    const char* openAfterPath_ = nullptr;  // ReloadMedia() this path after a few frames
    bool openedAfter_ = false;
    const char* dumpHudPath_ = nullptr;   // dump the rendered ImGui HUD image once
    bool dumpedHud_ = false;
};

} // namespace mp
