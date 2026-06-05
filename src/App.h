// SPDX-License-Identifier: BSL-1.0
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

    // Per-frame UI state: advance the auto-hide fade, toast fade, and slideshow machine.
    void TickUi();
    void ToggleSlideshow();
    void TogglePlayback();   // play/pause (video+audio); restarts if the clip already ended
    void ToggleMute();       // silence audio (keeps playing); persists across clips
    void StepFrame(int n);   // pause + step n frames (']' +1 / '[' -1)
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

    bool hasMedia_ = false;       // an image or video was loaded (vs the test pattern)
    bool isVideo_ = false;
    StereoLayout layout_ = StereoLayout::Mono;
    float contentAspect_ = 1.0f;  // per-eye display aspect (width/height), for letterboxing

    // M4 stereo controls. convergence_ is horizontal image translation as a fraction
    // of a view tile (each eye shifted oppositely → moves the zero-disparity plane);
    // swapEyes_ flips which SBS half feeds each eye.
    float convergence_ = 0.0f;
    bool swapEyes_ = false;

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
