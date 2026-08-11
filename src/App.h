// SPDX-License-Identifier: Apache-2.0
//
// App — wires the SDL3 window, OpenXR session, and Vulkan renderer, and owns the
// frame loop. For M0 the loop clears the two eyes to distinct colors; M1+ adds
// decode + UI (PRD §4).
#pragma once

#include "media/AudioPlayer.h"
#include "media/MediaSource.h"
#include "media/VideoDecoder.h"
#include "platform/Window.h"
#include "rhi/VulkanRenderer.h"
#include "ui/ImGuiLayer.h"
#include "xr/XrSession.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mp {

class App {
public:
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

    // Open-file. RequestOpenFile tries the workspace picker, else a native dialog.
    // LoadMedia opens a path fresh; ReloadMedia tears down current media first.
    void RequestOpenFile();
    bool LoadMedia(const std::string& path);
    void ReloadMedia(const std::string& path);

    // Drag and drop (#44). Filters the batch through MediaSource::IsSupported, toasts
    // what was rejected, then loads: one file behaves exactly like Ctrl+O, while a
    // multi-file drop installs the dropped set as a playlist (see playlistFromDrop_).
    void HandleDroppedPaths(std::vector<std::string> paths);

    // Folder navigation: scan the current file's directory for supported assets, and
    // step prev/next (wrapping). delta is +1 (next) or -1 (prev). `path` is taken BY
    // VALUE on purpose: callers pass a reference into folderFiles_, which this clears.
    void RebuildFolderList(std::string path);
    void NavigateMedia(int delta);
    // Start a dip-to-black transition that navigates by `delta` at full black (manual
    // ←/→). Shares the fade machine with the slideshow.
    void RequestNavTransition(int delta);

    // Transient toast (e.g. convergence %). Shows `msg` for a short time, then fades.
    void ShowToast(const std::string& msg);

    // Stereo layout override (#45): cycle auto -> mono -> SBS-full -> SBS-half -> auto.
    // Re-derives layout_ and contentAspect_ in place; never touches the decoder.
    void CycleLayoutOverride();
    // "SBS-full — detected" / "mono — from filename" / "SBS-full (R|L) — metadata" ...
    std::string LayoutLabel() const;
    // Commit a resolved layout: layout_, the provenance fields, and the manual pin
    // (cleared here rather than at load entry, so a FAILED load leaves the pin on the
    // media still being displayed).
    void ApplyLayout(const MediaInfo& info);

    // Per-frame UI state: advance the auto-hide fade, toast fade, and slideshow machine.
    void TickUi();
    void ToggleSlideshow();
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
    void TogglePlayback();   // play/pause (video+audio); restarts if the clip already ended
    void ToggleMute();       // silence audio (keeps playing); persists across clips
    void StepFrame(int n);   // pause + step n frames (']' +1 / '[' -1)

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

    bool hasMedia_ = false;       // an image or video was loaded (vs the idle logo)
    bool isLogo_ = false;         // the idle DisplayXR logo screen is showing (no media)
    bool isVideo_ = false;
    StereoLayout layout_ = StereoLayout::Mono;
    float contentAspect_ = 1.0f;  // per-eye display aspect (width/height), for letterboxing

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
    // MEDIAPLAYER_STEREO_DETECT: off | meta | full (default full).
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
    // Coarse auto-convergence estimate for LIFs whose metadata carries NO convergence
    // field. Not applied automatically — Backspace sets convergence_ to it (undo with 0).
    bool mediaAutoConvAvailable_ = false;
    float mediaAutoConvergence_ = 0.0f;

    // Scrubber: displayed position tracks playback except while the user drags it, or
    // while an issued seek hasn't landed yet (scrubTarget_ >= 0 holds the knob steady
    // so it doesn't snap back to the stale position on mouse-release).
    float scrubValue_ = 0.0f;
    bool scrubActive_ = false;
    float scrubTarget_ = -1.0f;
    // Velocity-aware scrubbing: a fast sweep shows keyframes (responsive on long-GOP 8K),
    // a slow/fine drag shows exact frames, and settling after a sweep resolves to exact.
    float lastScrubValue_ = 0.0f;
    bool scrubWasPreview_ = false;

    // Open-file flow. openFilePending_ gates the Open button while a picker is up.
    // The native-dialog callback may run off-thread; it parks the result here.
    bool openFilePending_ = false;
    std::mutex nativePathMutex_;
    std::string nativePath_;
    bool hasNativePath_ = false;

    // Folder navigation / slideshow. folderFiles_ holds the supported assets in the
    // current media's directory (sorted); folderIndex_ points at the loaded one.
    std::string currentMediaPath_;
    std::vector<std::string> folderFiles_;
    size_t folderIndex_ = 0;
    // Set when folderFiles_ came from a multi-file drop rather than a directory scan
    // (#44). RebuildFolderList() honours it and re-locates the index inside the existing
    // list instead of rescanning the parent directory, which would otherwise destroy the
    // dropped set on the very next load. Cleared by any later single-target open.
    bool playlistFromDrop_ = false;

    int mediaW_ = 0;              // full frame dims, for the HUD label
    int mediaH_ = 0;
    std::vector<uint8_t> hudPixels_;  // CPU-rasterized HUD buffer

    // Frame / FPS tracking.
    uint64_t frames_ = 0;
    uint64_t rendered_ = 0;
    const char* prevMode_ = "";
    std::chrono::steady_clock::time_point fpsWindowStart_{};
    uint32_t fpsWindowFrames_ = 0;
    float fps_ = 0.0f;
    bool showHud_ = true;         // master UI enable (SHIFT+TAB); auto-hide governs the rest
    bool inRenderFrame_ = false;  // reentrancy guard (live-resize watch vs main loop)

    // Auto-hide: the UI fades in on activity and out after kIdleHideSeconds. fadeAlpha_
    // (0..1) scales every widget; lastActivity_ is reset on mouse-move / control input.
    float fadeAlpha_ = 0.0f;
    std::chrono::steady_clock::time_point lastActivity_{};
    std::chrono::steady_clock::time_point lastFrameTime_{};
    // Resting cursor position: motion is "real" only when it moves more than a few px
    // from here, so sensor jitter near rest can't keep the UI awake. <0 = uninitialized.
    float restMouseX_ = -1.0f;
    float restMouseY_ = -1.0f;

    // Transient toast (convergence readout, nav filename). Independent alpha so it shows
    // even when the bars are hidden.
    std::string toastText_;
    std::chrono::steady_clock::time_point toastExpiry_{};
    float toastAlpha_ = 0.0f;

    // Slideshow ("diaporama"): auto-advance through folderFiles_. Stills hold for
    // kStillSeconds; videos play to the end. Transitions dip to black (transitionAlpha_).
    bool slideshowActive_ = false;
    double slideshowImageElapsed_ = 0.0;
    enum class Transition { Playing, FadeOut, FadeIn };
    Transition transition_ = Transition::Playing;
    float transitionAlpha_ = 0.0f;  // 0 = clear, 1 = full black
    int pendingNavDelta_ = 0;       // navigation to apply at full black (slideshow or ←/→)

    // Test scaffolding (env-gated).
    int startMode_ = -1;
    bool startModeRequested_ = false;
    const char* dumpPath_ = nullptr;
    bool dumped_ = false;
    const char* openAfterPath_ = nullptr;  // ReloadMedia() this path after a few frames
    bool openedAfter_ = false;
    const char* dumpHudPath_ = nullptr;   // dump the rendered ImGui HUD image once
    bool dumpedHud_ = false;
};

} // namespace mp
