// SPDX-License-Identifier: BSL-1.0
#include "App.h"

#include "Log.h"
#include "media/ImageDecoder.h"
#include "media/MediaSource.h"

#include <cstdlib>
#include <cstring>

namespace mp {

namespace {
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

    // Load a stereo image if one was given; otherwise the loop falls back to the
    // RED|BLUE L/R test pattern.
    if (mediaPath && *mediaPath) {
        DecodedImage img = ImageDecoder::Load(mediaPath);
        if (img.Valid()) {
            const MediaInfo info = MediaSource::Identify(mediaPath, img.width, img.height);
            if (info.kind == MediaKind::Video) {
                LOG_WARN("Video playback is M2 — '%s' will not display yet", mediaPath);
            } else if (renderer_.UploadTexture(img.pixels.data(),
                                               (uint32_t)img.width, (uint32_t)img.height)) {
                layout_ = info.layout;
                hasImage_ = true;
                // Per-eye display aspect: full SBS packs two eyes across the width,
                // so each eye is half-width; half-SBS/mono use the full width.
                const float eyeW = (info.layout == StereoLayout::SbsFull)
                                       ? (float)img.width * 0.5f : (float)img.width;
                contentAspect_ = eyeW / (float)img.height;
                LOG_INFO("Displaying %s image (%s), per-eye aspect %.3f",
                         MediaSource::LayoutName(info.layout), mediaPath, contentAspect_);
            }
        }
    }
    if (!hasImage_) {
        LOG_INFO("No image loaded — showing RED|BLUE L/R test pattern");
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
    ViewUV uvs[XrSession::kMaxViews];
    XrSession::ViewRect rects[XrSession::kMaxViews];          // full per-view tiles (submitted)
    XrSession::ViewRect contentRects[XrSession::kMaxViews];   // aspect-fit content within each tile
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
                // RED|BLUE fallback colors:
                colors[v] = isLeft ? kLeftImage : kRightImage;
                // SBS texture sampling: left eye = left half, right eye = right half;
                // mono = whole image to every view.
                if (layout_ == StereoLayout::Mono) {
                    uvs[v] = {0.0f, 0.0f, 1.0f, 1.0f};
                } else {
                    uvs[v] = isLeft ? ViewUV{0.0f, 0.0f, 0.5f, 1.0f}
                                    : ViewUV{0.5f, 0.0f, 0.5f, 1.0f};
                }
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
                             (p.x <= centerX) ? "LEFT" : "RIGHT");
                }
            }

            if (hasImage_) {
                // Letterbox in DISPLAY-VIEW space (the canvas), then map into each
                // tile by the mode's view-scale — so non-uniform modes (e.g. Squeezed
                // SBS, scaleX!=scaleY) that the runtime stretches back to the full view
                // still show correct aspect. The render pass clears the rest to black.
                const float sx = xr_.ActiveViewScaleX();
                const float sy = xr_.ActiveViewScaleY();
                const XrSession::ViewRect viewFit =
                    FitRect({0, 0, canvasW, canvasH}, contentAspect_);
                for (uint32_t v = 0; v < frame.viewCount; ++v) {
                    contentRects[v].x = rects[v].x + (int32_t)((float)viewFit.x * sx + 0.5f);
                    contentRects[v].y = rects[v].y + (int32_t)((float)viewFit.y * sy + 0.5f);
                    contentRects[v].w = (uint32_t)((float)viewFit.w * sx + 0.5f);
                    contentRects[v].h = (uint32_t)((float)viewFit.h * sy + 0.5f);
                }
                renderer_.DrawViews(frame.imageIndex, contentRects, uvs, frame.viewCount);
            } else {
                renderer_.ClearViews(frame.imageIndex, colors, rects, frame.viewCount);
            }
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
