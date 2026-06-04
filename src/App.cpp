// SPDX-License-Identifier: BSL-1.0
#include "App.h"

#include "Log.h"

#include <cstdlib>

namespace mp {

bool App::Initialize() {
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
    return true;
}

int App::Run() {
    // Stand-in stereo source for M0: a left image (RED) and a right image (BLUE).
    // M1 swaps these for the decoded SBS pair; the *mapping* onto N display views is
    // what we're exercising here.
    const ClearColor kLeftImage{0.85f, 0.10f, 0.10f, 1.0f};   // RED  = left
    const ClearColor kRightImage{0.10f, 0.20f, 0.85f, 1.0f};  // BLUE = right

    // Optional: jump straight to a rendering mode for testing (e.g. =4 for Quad).
    const char* startModeEnv = std::getenv("MEDIAPLAYER_START_MODE");
    const int startMode = startModeEnv ? std::atoi(startModeEnv) : -1;
    bool startModeRequested = false;
    // Optional: dump the submitted swapchain atlas to a PNG once (proves our output).
    const char* dumpPath = std::getenv("MEDIAPLAYER_DUMP_ATLAS");
    bool dumped = false;

    bool keepRunning = true;
    uint64_t frames = 0, rendered = 0;
    ClearColor colors[XrSession::kMaxViews];
    XrSession::ViewRect rects[XrSession::kMaxViews];
    const char* prevMode = "";
    while (keepRunning && !xr_.ExitRequested()) {
        keepRunning = window_.PumpEvents();
        if (window_.TakeCycleModeRequest()) xr_.RequestNextMode();  // 'V'
        xr_.PollEvents();

        if (!xr_.IsRunning()) {
            continue; // session not started yet (or stopping)
        }
        if (startMode >= 0 && !startModeRequested) {
            xr_.RequestMode((uint32_t)startMode);
            startModeRequested = true;
        }

        XrSession::Frame frame;
        if (!xr_.BeginFrame(frame)) {
            continue;
        }
        if (frame.shouldRender) {
            // Compute each active view's tile rect from the window's pixel (canvas)
            // size — per-view extent = canvas_px * mode.viewScale — so the rects we
            // clear and submit match what the runtime samples.
            uint32_t canvasW = 0, canvasH = 0;
            window_.PixelSize(canvasW, canvasH);
            xr_.ComputeViewRects(canvasW, canvasH, rects);

            // Map the 2-view source onto the N active display views: a view left of
            // the eye-box center shows the left image, right of center the right
            // image (nearest source eye). Center = midpoint of active views' X.
            float minX = frame.views[0].pose.position.x;
            float maxX = minX;
            for (uint32_t v = 1; v < frame.viewCount; ++v) {
                const float x = frame.views[v].pose.position.x;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
            }
            const float centerX = 0.5f * (minX + maxX);
            for (uint32_t v = 0; v < frame.viewCount; ++v) {
                const bool isLeft = frame.views[v].pose.position.x <= centerX;
                colors[v] = isLeft ? kLeftImage : kRightImage;
            }

            // Diagnostic: dump the per-view geometry + L/R assignment once per mode.
            const char* mode = xr_.ActiveModeName();
            if (mode != prevMode) {
                prevMode = mode;
                LOG_INFO("Mode '%s': %u views, centerX=%.4f, canvas=%ux%u, tile=%ux%u",
                         mode, frame.viewCount, centerX, canvasW, canvasH, rects[0].w, rects[0].h);
                for (uint32_t v = 0; v < frame.viewCount; ++v) {
                    const auto& p = frame.views[v].pose.position;
                    LOG_INFO("  view %u pos=(%.4f, %.4f, %.4f) rect=(%d,%d %ux%u) -> %s", v,
                             p.x, p.y, p.z, rects[v].x, rects[v].y, rects[v].w, rects[v].h,
                             (p.x <= centerX) ? "LEFT/red" : "RIGHT/blue");
                }
            }

            renderer_.ClearViews(frame.imageIndex, colors, rects, frame.viewCount);
            ++rendered;

            // Dump the atlas once, after the requested mode has settled (~100 frames).
            if (dumpPath && !dumped && rendered >= 100) {
                renderer_.DumpImage(frame.imageIndex, dumpPath);
                dumped = true;
            }
        }
        xr_.EndFrame(frame, rects);
        ++frames;
    }

    LOG_INFO("Exiting render loop after %llu frames (%llu rendered, userQuit=%d xrExit=%d)",
             (unsigned long long)frames, (unsigned long long)rendered,
             !keepRunning, xr_.ExitRequested());
    return 0;
}

void App::Shutdown() {
    renderer_.Shutdown();
    xr_.Shutdown();
    window_.Destroy();
}

} // namespace mp
