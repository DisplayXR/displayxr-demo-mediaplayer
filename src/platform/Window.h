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
    // The SDL_Window itself (for the ImGui SDL3 backend).
    SDL_Window* SdlWindow() const { return window_; }

    // Backing-store size in pixels (Retina-aware). This is the runtime's canvas.
    void PixelSize(uint32_t& width, uint32_t& height) const;
    // Logical (point) size — the space SDL/ImGui mouse coordinates live in.
    void PointSize(uint32_t& width, uint32_t& height) const;

    void SetTitle(const char* title);

    // Forward every SDL event to a sink (the ImGui backend) before our own handling.
    // The argument is an `SDL_Event*` (void to keep SDL out of this header).
    void SetEventHook(std::function<void(void*)> hook);

    void ToggleFullscreen();      // `F` key, and the ImGui button

    // Pump SDL events; returns false when the user asked to quit (close / Esc).
    bool PumpEvents();

    // True once per V keypress (cycle rendering mode). Clears the latch on read.
    bool TakeCycleModeRequest();
    // True once per SHIFT+TAB (toggle HUD). Clears the latch on read.
    bool TakeToggleHudRequest();

    // --- M4 transport / stereo controls (keyboard; ImGui adds pointer UI later) ---
    // Net convergence steps since last read: `=` = +1, `-` = -1 (key-repeat counts),
    // reset to 0 on read. The caller scales each step into its parallax budget.
    int TakeConvergenceSteps();
    bool TakeResetConvergence();   // `0` — convergence back to 0
    bool TakeSwapEyesRequest();    // `X` — toggle L/R eye assignment
    bool TakeTogglePauseRequest(); // Space — play/pause

    // --- Folder navigation / slideshow ---
    bool TakePrevMediaRequest();      // Left arrow — previous asset in the folder
    bool TakeNextMediaRequest();      // Right arrow — next asset in the folder
    bool TakeToggleSlideshowRequest(); // `S` — toggle slideshow ("diaporama")

    // Discrete pointer activity (click / wheel / window-enter) since last read — wakes
    // the auto-hide UI. Continuous motion is detected by polling (jitter-immune).
    bool TakeMouseActivity();
    // True once when the cursor left the window (auto-hide should drop the UI).
    bool TakeMouseLeft();
    // Is the cursor currently inside the window? (gates motion polling.)
    bool MouseInWindow() const { return mouseInWindow_; }

    // Called to render a frame *during* the macOS modal resize loop (which would
    // otherwise block our main loop), giving continuous live resize.
    void SetLiveResizeCallback(std::function<void()> cb);

private:
    SDL_Window* window_ = nullptr;
    void* metalView_ = nullptr;   // SDL_MetalView (macOS only); owned, destroyed on Destroy
    void* nativeHandle_ = nullptr;
    bool cycleModeRequested_ = false;
    bool toggleHudRequested_ = false;
    int convergenceSteps_ = 0;
    bool resetConvergenceRequested_ = false;
    bool swapEyesRequested_ = false;
    bool togglePauseRequested_ = false;
    bool prevMediaRequested_ = false;
    bool nextMediaRequested_ = false;
    bool toggleSlideshowRequested_ = false;
    bool mouseActivity_ = false;
    bool mouseLeft_ = false;
    bool mouseInWindow_ = true;
    bool fullscreen_ = false;
    std::function<void()> liveResizeCb_;
    std::function<void(void*)> eventHook_;
    bool eventWatchAdded_ = false;
};

} // namespace mp
