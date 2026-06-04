// SPDX-License-Identifier: BSL-1.0
//
// Window — SDL3 window + native-handle extraction. The DisplayXR runtime renders
// into a handle the app owns (the `_handle` app class), so we must hand it the
// platform-native view/window:
//   macOS  -> NSView* (CAMetalLayer-backed) via SDL_Metal_CreateView
//   Windows-> HWND via SDL window properties
// One SDL codebase; only the handle extraction is per-platform.
#pragma once

#include <cstdint>
#include <functional>

struct SDL_Window;

namespace mp {

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const char* title, int width, int height);
    void Destroy();

    // NSView* (macOS) or HWND (Windows), to pass into the OpenXR window binding.
    void* NativeHandle() const { return nativeHandle_; }

    // Backing-store size in pixels (Retina-aware). This is the runtime's canvas.
    void PixelSize(uint32_t& width, uint32_t& height) const;

    void SetTitle(const char* title);

    // Pump SDL events; returns false when the user asked to quit (close / Esc).
    bool PumpEvents();

    // True once per V keypress (cycle rendering mode). Clears the latch on read.
    bool TakeCycleModeRequest();
    // True once per SHIFT+TAB (toggle HUD). Clears the latch on read.
    bool TakeToggleHudRequest();

    // Called to render a frame *during* the macOS modal resize loop (which would
    // otherwise block our main loop), giving continuous live resize.
    void SetLiveResizeCallback(std::function<void()> cb);

private:
    SDL_Window* window_ = nullptr;
    void* metalView_ = nullptr;   // SDL_MetalView (macOS only); owned, destroyed on Destroy
    void* nativeHandle_ = nullptr;
    bool cycleModeRequested_ = false;
    bool toggleHudRequested_ = false;
    std::function<void()> liveResizeCb_;
    bool eventWatchAdded_ = false;
};

} // namespace mp
