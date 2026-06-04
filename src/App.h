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
#include "xr/XrSession.h"

#include <chrono>
#include <cstdint>
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

    Window window_;
    XrSession xr_;
    VulkanRenderer renderer_;
    VideoDecoder video_;

    bool hasMedia_ = false;       // an image or video was loaded (vs the test pattern)
    bool isVideo_ = false;
    StereoLayout layout_ = StereoLayout::Mono;
    float contentAspect_ = 1.0f;  // per-eye display aspect (width/height), for letterboxing
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
};

} // namespace mp
