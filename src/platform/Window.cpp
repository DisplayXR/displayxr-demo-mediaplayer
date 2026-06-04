// SPDX-License-Identifier: BSL-1.0
#include "Window.h"

#include "Log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

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

Window::~Window() { Destroy(); }

bool Window::Create(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // The runtime presents into our view via Vulkan/MoltenVK; on macOS we still
    // request a Metal-capable window so SDL gives us a CAMetalLayer-backed NSView.
    // Resizable from the start — the render path reads the live pixel size each
    // frame, so tiles + letterboxing re-fit automatically.
    Uint32 flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
    window_ = SDL_CreateWindow(title, width, height, flags);
    if (!window_) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

#if defined(__APPLE__)
    // SDL_Metal_CreateView returns an NSView* (CAMetalLayer-backed) — exactly what
    // XR_EXT_cocoa_window_binding wants as its viewHandle.
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

void Window::SetTitle(const char* title) {
    if (window_) SDL_SetWindowTitle(window_, title);
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
        if (e.type == SDL_EVENT_QUIT) return false;
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.key == SDLK_ESCAPE) return false;
            if (e.key.key == SDLK_V) cycleModeRequested_ = true;
            if (e.key.key == SDLK_TAB && (e.key.mod & SDL_KMOD_SHIFT)) toggleHudRequested_ = true;
        }
    }
    return true;
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
