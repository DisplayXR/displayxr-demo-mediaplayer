// SPDX-License-Identifier: BSL-1.0
//
// App — wires the SDL3 window, OpenXR session, and Vulkan renderer, and owns the
// frame loop. For M0 the loop clears the two eyes to distinct colors; M1+ adds
// decode + UI (PRD §4).
#pragma once

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
    // SDL native-dialog callback (Tier-0 fallback). May fire on another thread, so it
    // just hands the path to the main loop through the guarded slot below.
    static void NativeFileCallback(void* userdata, const char* const* filelist, int filter);

    Window window_;
    XrSession xr_;
    VulkanRenderer renderer_;
    VideoDecoder video_;
    ImGuiLayer imgui_;

    bool hasMedia_ = false;       // an image or video was loaded (vs the test pattern)
    bool isVideo_ = false;
    StereoLayout layout_ = StereoLayout::Mono;
    float contentAspect_ = 1.0f;  // per-eye display aspect (width/height), for letterboxing

    // M4 stereo controls. convergence_ is horizontal image translation as a fraction
    // of a view tile (each eye shifted oppositely → moves the zero-disparity plane);
    // swapEyes_ flips which SBS half feeds each eye.
    float convergence_ = 0.0f;
    bool swapEyes_ = false;

    // Scrubber: displayed position tracks playback except while the user drags it.
    float scrubValue_ = 0.0f;
    bool scrubActive_ = false;

    // Open-file flow. openFilePending_ gates the Open button while a picker is up.
    // The native-dialog callback may run off-thread; it parks the result here.
    bool openFilePending_ = false;
    std::mutex nativePathMutex_;
    std::string nativePath_;
    bool hasNativePath_ = false;
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
    bool showHud_ = false;
    bool inRenderFrame_ = false;  // reentrancy guard (live-resize watch vs main loop)

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
