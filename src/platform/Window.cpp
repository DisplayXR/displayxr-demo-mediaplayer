// SPDX-License-Identifier: Apache-2.0
#include "Window.h"

#include "Log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#if defined(__linux__) && !defined(__ANDROID__)
// The platform DECISION is displayxr-common's (displayxr::linux_window's
// capability probe — the one rule every DisplayXR app uses: an explicit
// --platform wins; auto = native Wayland when the compositor is ready, else
// X11; never read from session env vars). SDL still owns the window: the probe
// only picks SDL's video driver, and the handles below feed the runtime.
#include "dxr_linux_window.h"
#include <openxr/openxr.h>
#include <openxr/XR_DXR_xlib_window_binding.h>
#include <openxr/XR_DXR_wayland_surface_binding.h>
#include <cstring>
#include <string>
#include <vector>
#endif

namespace mp {

namespace {
// SDL event watch — fires synchronously on the main thread as events are pumped,
// INCLUDING during the macOS modal live-resize loop. We re-render from here so the
// window keeps updating while the user drags, instead of freezing until mouse-up.
bool SDLCALL ResizeEventWatch(void* userdata, SDL_Event* e) {
    if (e->type == SDL_EVENT_WINDOW_RESIZED ||
        e->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        e->type == SDL_EVENT_WINDOW_EXPOSED) {
        auto* cb = static_cast<std::function<void()>*>(userdata);
        if (cb && *cb) (*cb)();
    }
    return true;  // keep the event in the queue for normal handling
}
} // namespace

#if defined(__linux__) && !defined(__ANDROID__)
static int s_linuxPlatformRequest = 0; // 0 auto, 1 x11, 2 wayland
void Window::SetLinuxPlatformRequest(int request) { s_linuxPlatformRequest = request; }

//! Which window platform to give SDL: the helper's capability probe, fed by
//! which binding extensions the runtime advertises (no instance needed).
static DxrWindowBackend ResolveLinuxPlatform() {
    bool hasXlib = false, hasWayland = false;
    uint32_t n = 0;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)) && n > 0) {
        std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()))) {
            for (const auto& e : exts) {
                if (strcmp(e.extensionName, XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME) == 0) hasXlib = true;
                if (strcmp(e.extensionName, XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME) == 0) hasWayland = true;
            }
        }
    }
    const DxrWindowBackend requested = s_linuxPlatformRequest == 1   ? DxrWindowBackend::X11
                                       : s_linuxPlatformRequest == 2 ? DxrWindowBackend::Wayland
                                                                     : DxrWindowBackend::Auto;
    std::string why;
    const DxrWindowBackend b = DxrLinuxWindow::select(requested, hasXlib, hasWayland, &why);
    if (b == DxrWindowBackend::Auto) {
        LOG_WARN("No usable window platform (%s) — SDL picks its own driver; no window binding", why.c_str());
    } else {
        LOG_INFO("Window platform: %s (requested %s) — %s", DxrLinuxWindow::backend_name(b),
                 DxrLinuxWindow::backend_name(requested), why.c_str());
    }
    return b;
}
#endif

Window::~Window() { Destroy(); }

bool Window::Create(const char* title, int width, int height) {
#if defined(__linux__) && !defined(__ANDROID__)
    // SDL's video driver = the platform the capability probe chose (X11 —
    // XWayland included — or native Wayland; see ResolveLinuxPlatform). A
    // hint, so a user's own SDL_VIDEO_DRIVER still wins; what SDL actually
    // picked is verified after SDL_Init.
    const DxrWindowBackend linuxPlatform = ResolveLinuxPlatform();
    if (linuxPlatform == DxrWindowBackend::X11) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    if (linuxPlatform == DxrWindowBackend::Wayland) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // The runtime presents into our view via Vulkan/MoltenVK; on macOS we still
    // request a Metal-capable window so SDL gives us a CAMetalLayer-backed NSView.
    // Resizable from the start — the render path reads the live pixel size each
    // frame, so tiles + letterboxing re-fit automatically.
    // Created HIDDEN: in workspace mode the runtime hides/binds the HWND during session
    // creation, which takes a couple of seconds. A window shown up-front flashes on top of
    // the shell during that init; we show it (via Show()) only after XR setup completes.
    Uint32 flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    // Verify what SDL actually picked (GLFW-style): the handles and the
    // binding follow the live driver, not the request.
    const char* driver = SDL_GetCurrentVideoDriver();
    const bool onWayland = driver != nullptr && strcmp(driver, "wayland") == 0;
    if (linuxPlatform != DxrWindowBackend::Auto &&
        onWayland != (linuxPlatform == DxrWindowBackend::Wayland)) {
        LOG_WARN("SDL video driver is '%s', not the %s the probe chose (SDL_VIDEO_DRIVER set?) — "
                 "binding what SDL actually opened",
                 driver ? driver : "?", DxrLinuxWindow::backend_name(linuxPlatform));
    }
    // Native Wayland: the runtime's Vulkan WSI presents into SDL's surface —
    // SDL's supported external-Vulkan case, which makes SDL map the pixel-size
    // buffer onto the logical window and never attach a buffer of its own.
    if (onWayland) flags |= SDL_WINDOW_VULKAN;
#endif
    window_ = SDL_CreateWindow(title, width, height, flags);
    if (!window_) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

#if defined(__APPLE__)
    // SDL_Metal_CreateView returns an NSView* (CAMetalLayer-backed) — exactly what
    // XR_DXR_cocoa_window_binding wants as its viewHandle.
    metalView_ = SDL_Metal_CreateView(window_);
    if (!metalView_) {
        LOG_ERROR("SDL_Metal_CreateView failed: %s", SDL_GetError());
        return false;
    }
    nativeHandle_ = metalView_;
#elif defined(_WIN32)
    SDL_PropertiesID props = SDL_GetWindowProperties(window_);
    nativeHandle_ = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!nativeHandle_) {
        LOG_ERROR("Could not get Win32 HWND from SDL window");
        return false;
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    // Bundle the live driver's handles behind the single void* the XR plumbing
    // carries: X11 (Display*, Window XID) or Wayland (wl_display*, wl_surface*).
    SDL_PropertiesID props = SDL_GetWindowProperties(window_);
    if (onWayland) {
        linux_.wayland = true;
        linux_.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        linux_.surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        // The runtime creates its VkSurfaceKHR inside xrCreateSession and the
        // WSI attaches a buffer on the first present, so the surface must
        // already have its xdg role and an acked configure: SDL gives it both
        // on show. (The X11 window stays hidden until XR setup is done; on
        // Wayland it has to be up first.)
        SDL_ShowWindow(window_);
        SDL_SyncWindow(window_);
        if (linux_.display && linux_.surface) nativeHandle_ = &linux_;
    } else {
        linux_.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        linux_.window =
            (unsigned long)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (linux_.display && linux_.window) nativeHandle_ = &linux_;
    }
    if (nativeHandle_ == nullptr) {
        LOG_WARN("No %s window handles from SDL (driver '%s') — window binding unavailable",
                 onWayland ? "Wayland" : "X11", driver ? driver : "?");
    } else {
        LOG_INFO("Window platform: SDL driver '%s' verified — %s", driver ? driver : "?",
                 onWayland ? "XR_DXR_wayland_surface_binding" : "XR_DXR_xlib_window_binding");
    }
#else
    LOG_WARN("No native window-handle extraction for this platform");
#endif

    LOG_INFO("Window created (%dx%d), native handle=%p", width, height, nativeHandle_);
    return true;
}

void Window::PixelSize(uint32_t& width, uint32_t& height) const {
    int w = 0, h = 0;
    if (window_) SDL_GetWindowSizeInPixels(window_, &w, &h);
    width = (uint32_t)(w > 0 ? w : 0);
    height = (uint32_t)(h > 0 ? h : 0);
}

void Window::PointSize(uint32_t& width, uint32_t& height) const {
    int w = 0, h = 0;
    if (window_) SDL_GetWindowSize(window_, &w, &h);
    width = (uint32_t)(w > 0 ? w : 0);
    height = (uint32_t)(h > 0 ? h : 0);
}

void Window::SetTitle(const char* title) {
    if (window_) SDL_SetWindowTitle(window_, title);
}

void Window::SetPosition(int x, int y) {
    if (window_) SDL_SetWindowPosition(window_, x, y);
}

void Window::Show() {
    if (window_) SDL_ShowWindow(window_);
}

void Window::SetEventHook(std::function<void(void*)> hook) {
    eventHook_ = std::move(hook);
}

void Window::SetLiveResizeCallback(std::function<void()> cb) {
    liveResizeCb_ = std::move(cb);
    if (!eventWatchAdded_) {
        SDL_AddEventWatch(ResizeEventWatch, &liveResizeCb_);
        eventWatchAdded_ = true;
    }
}

bool Window::PumpEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (eventHook_) eventHook_(&e);   // feed ImGui first (it may want the input)
        if (e.type == SDL_EVENT_QUIT) return false;
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        // Discrete pointer activity wakes the auto-hide UI. Continuous motion is handled
        // by polling in the app (jitter-immune), NOT here — a noisy sensor emits a stream
        // of tiny MOTION events that would otherwise pin the UI permanently visible.
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_WHEEL)
            mouseActivity_ = true;
        if (e.type == SDL_EVENT_WINDOW_MOUSE_ENTER) { mouseInWindow_ = true; mouseActivity_ = true; }
        if (e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) { mouseInWindow_ = false; mouseLeft_ = true; }
        // Drag and drop (#44). SDL3 delivers this on Windows, macOS and X11 through the
        // same event, so there is no per-platform handler here.
        //
        // TRAP: in SDL3 the path is `drop.data` and SDL OWNS the string — it is valid only
        // for the duration of this event, so it must be COPIED now. (SDL2 used `drop.file`
        // and required SDL_free(); most examples online are still SDL2 and would leak or
        // double-free here.)
        //
        // Readiness is deliberately NOT keyed off SDL_EVENT_DROP_COMPLETE: every event of
        // one drop is delivered inside this single SDL_PollEvent drain, so the batch is
        // already whole by the time the app drains it later in the same frame. DROP_BEGIN
        // is used only to discard a stale partial batch. SDL_EVENT_DROP_TEXT is ignored.
        if (e.type == SDL_EVENT_DROP_BEGIN) dropBatch_.clear();
        if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
            if (!window_ || e.drop.windowID == SDL_GetWindowID(window_)) {
                dropBatch_.emplace_back(e.drop.data);
                mouseActivity_ = true;   // a drop is input: wake the auto-hide UI
            }
        }
        if (e.type == SDL_EVENT_KEY_DOWN) {
            // Convergence nudges repeat while held; everything else is one-shot. The
            // convergence keys form the contiguous `0 - =` cluster: `=`/`-` nudge, `0` resets.
            if (e.key.key == SDLK_EQUALS) ++convergenceSteps_;
            else if (e.key.key == SDLK_MINUS) --convergenceSteps_;
            else if (e.key.key == SDLK_RIGHTBRACKET) ++frameStepRequest_;  // ']' next frame
            else if (e.key.key == SDLK_LEFTBRACKET) --frameStepRequest_;   // '[' prev frame
            else if (!e.key.repeat) {
                if (e.key.key == SDLK_ESCAPE) return false;
                if (e.key.key == SDLK_V) cycleModeRequested_ = true;
                if (e.key.key == SDLK_TAB && (e.key.mod & SDL_KMOD_SHIFT)) toggleHudRequested_ = true;
                if (e.key.key == SDLK_0) resetConvergenceRequested_ = true;
                if (e.key.key == SDLK_BACKSPACE) autoConvergeRequested_ = true;
                if (e.key.key == SDLK_X) swapEyesRequested_ = true;
                if (e.key.key == SDLK_SPACE) togglePauseRequested_ = true;
                if (e.key.key == SDLK_LEFT) prevMediaRequested_ = true;
                if (e.key.key == SDLK_RIGHT) nextMediaRequested_ = true;
                if (e.key.key == SDLK_S) toggleSlideshowRequested_ = true;
                if (e.key.key == SDLK_M) toggleMuteRequested_ = true;
                if (e.key.key == SDLK_L) cycleLayoutRequested_ = true;  // stereo layout override
                if (e.key.key == SDLK_I) captureRequested_ = true;  // snapshot the atlas
                if (e.key.key == SDLK_O && (e.key.mod & SDL_KMOD_CTRL)) openFileRequested_ = true; // Ctrl+O — open
                if (e.key.key == SDLK_F || e.key.key == SDLK_F11) ToggleFullscreen();
            }
        }
    }
    return true;
}

void Window::ToggleFullscreen() {
    fullscreen_ = !fullscreen_;
    if (window_) SDL_SetWindowFullscreen(window_, fullscreen_);
    LOG_INFO("Fullscreen %s", fullscreen_ ? "on" : "off");
}

bool Window::TakeCycleModeRequest() {
    bool v = cycleModeRequested_;
    cycleModeRequested_ = false;
    return v;
}

bool Window::TakeToggleHudRequest() {
    bool v = toggleHudRequested_;
    toggleHudRequested_ = false;
    return v;
}

int Window::TakeConvergenceSteps() {
    int v = convergenceSteps_;
    convergenceSteps_ = 0;
    return v;
}

bool Window::TakeResetConvergence() {
    bool v = resetConvergenceRequested_;
    resetConvergenceRequested_ = false;
    return v;
}

bool Window::TakeAutoConvergeRequest() {
    bool v = autoConvergeRequested_;
    autoConvergeRequested_ = false;
    return v;
}

int Window::TakeFrameStep() {
    int v = frameStepRequest_;
    frameStepRequest_ = 0;
    return v;
}

bool Window::TakeSwapEyesRequest() {
    bool v = swapEyesRequested_;
    swapEyesRequested_ = false;
    return v;
}

bool Window::TakeCaptureRequest() {
    bool v = captureRequested_;
    captureRequested_ = false;
    return v;
}

bool Window::TakeTogglePauseRequest() {
    bool v = togglePauseRequested_;
    togglePauseRequested_ = false;
    return v;
}

bool Window::TakeOpenFileRequest() {
    bool v = openFileRequested_;
    openFileRequested_ = false;
    return v;
}

bool Window::TakeCycleLayoutRequest() {
    bool v = cycleLayoutRequested_;
    cycleLayoutRequested_ = false;
    return v;
}

bool Window::TakeDroppedPaths(std::vector<std::string>& out) {
    if (dropBatch_.empty()) return false;
    out = std::move(dropBatch_);
    dropBatch_.clear();   // `moved-from` is unspecified, not guaranteed empty
    return true;
}

bool Window::TakePrevMediaRequest() {
    bool v = prevMediaRequested_;
    prevMediaRequested_ = false;
    return v;
}

bool Window::TakeNextMediaRequest() {
    bool v = nextMediaRequested_;
    nextMediaRequested_ = false;
    return v;
}

bool Window::TakeToggleSlideshowRequest() {
    bool v = toggleSlideshowRequested_;
    toggleSlideshowRequested_ = false;
    return v;
}

bool Window::TakeToggleMuteRequest() {
    bool v = toggleMuteRequested_;
    toggleMuteRequested_ = false;
    return v;
}

bool Window::TakeMouseActivity() {
    bool v = mouseActivity_;
    mouseActivity_ = false;
    return v;
}

bool Window::TakeMouseLeft() {
    bool v = mouseLeft_;
    mouseLeft_ = false;
    return v;
}

void Window::Destroy() {
    if (eventWatchAdded_) {
        SDL_RemoveEventWatch(ResizeEventWatch, &liveResizeCb_);
        eventWatchAdded_ = false;
    }
#if defined(__APPLE__)
    if (metalView_) {
        SDL_Metal_DestroyView((SDL_MetalView)metalView_);
        metalView_ = nullptr;
    }
#endif
    nativeHandle_ = nullptr;
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

} // namespace mp
