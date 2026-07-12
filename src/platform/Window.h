// SPDX-License-Identifier: Apache-2.0
//
// Window — SDL3 window + native-handle extraction. The DisplayXR runtime renders
// into a handle the app owns (the `_handle` app class), so we must hand it the
// platform-native view/window:
//   macOS  -> NSView* (CAMetalLayer-backed) via SDL_Metal_CreateView
//   Windows-> HWND via SDL window properties
//   Linux  -> &X11Handles (Display* + XID pair) via SDL X11 window properties;
//             XR_DXR_xlib_window_binding needs both, unlike the single-pointer
//             handles of the other platforms
// One SDL codebase; only the handle extraction is per-platform.
#pragma once

#include <cstdint>
#include <functional>

struct SDL_Window;

namespace mp {

class Window {
public:
#if defined(__linux__) && !defined(__ANDROID__)
    // What NativeHandle() points at on desktop Linux. XR_DXR_xlib_window_binding
    // takes the pair (Display*, Window XID); SDL exposes them as two window
    // properties, so they're bundled here to fit the one-void* handle plumbing.
    // Both are borrowed from SDL — valid until Destroy().
    struct X11Handles {
        void* display = nullptr;      // Display* (Xlib connection SDL opened)
        unsigned long window = 0;     // X11 Window (XID)
    };
#endif

    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const char* title, int width, int height);
    void Destroy();

    // NSView* (macOS), HWND (Windows), or &X11Handles (desktop Linux), to pass
    // into the OpenXR window binding.
    void* NativeHandle() const { return nativeHandle_; }
    // The SDL_Window itself (for the ImGui SDL3 backend).
    SDL_Window* SdlWindow() const { return window_; }

    // Backing-store size in pixels (Retina-aware). This is the runtime's canvas.
    void PixelSize(uint32_t& width, uint32_t& height) const;
    // Logical (point) size — the space SDL/ImGui mouse coordinates live in.
    void PointSize(uint32_t& width, uint32_t& height) const;

    void SetTitle(const char* title);

    // Move the window to (x, y) in SDL global desktop coordinates — top-down
    // virtual-desktop pixels on every platform (Windows virtual screen, X11 root,
    // and macOS SDL global coordinates alike), matching the convention of
    // XrDisplayDesktopPositionDXR directly. No per-platform flip needed.
    void SetPosition(int x, int y);

    // Make the window visible. Deferred until after XR setup so the window doesn't flash on
    // top of the workspace shell while the runtime is still binding/hiding the HWND.
    void Show();

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
    bool TakeAutoConvergeRequest(); // Backspace — apply auto-convergence (LIF w/o convergence)
    // Net single-frame steps since last read: `]` = +1, `[` = -1 (key-repeat counts).
    int TakeFrameStep();
    bool TakeSwapEyesRequest();    // `X` — toggle L/R eye assignment
    bool TakeCaptureRequest();     // `I` — snapshot the composed atlas to a PNG
    bool TakeTogglePauseRequest(); // Space — play/pause
    bool TakeOpenFileRequest();    // Ctrl+O — open the file picker

    // --- Folder navigation / slideshow ---
    bool TakePrevMediaRequest();      // Left arrow — previous asset in the folder
    bool TakeNextMediaRequest();      // Right arrow — next asset in the folder
    bool TakeToggleSlideshowRequest(); // `S` — toggle slideshow ("diaporama")
    bool TakeToggleMuteRequest();      // `M` — toggle audio mute

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
#if defined(__linux__) && !defined(__ANDROID__)
    X11Handles x11_;              // nativeHandle_ points here when X11 props resolve
#endif
    bool cycleModeRequested_ = false;
    bool toggleHudRequested_ = false;
    int convergenceSteps_ = 0;
    int frameStepRequest_ = 0;
    bool resetConvergenceRequested_ = false;
    bool autoConvergeRequested_ = false;
    bool swapEyesRequested_ = false;
    bool captureRequested_ = false;
    bool togglePauseRequested_ = false;
    bool openFileRequested_ = false;
    bool prevMediaRequested_ = false;
    bool nextMediaRequested_ = false;
    bool toggleSlideshowRequested_ = false;
    bool toggleMuteRequested_ = false;
    bool mouseActivity_ = false;
    bool mouseLeft_ = false;
    bool mouseInWindow_ = true;
    bool fullscreen_ = false;
    std::function<void()> liveResizeCb_;
    std::function<void(void*)> eventHook_;
    bool eventWatchAdded_ = false;
};

} // namespace mp
