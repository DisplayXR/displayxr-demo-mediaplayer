// SPDX-License-Identifier: Apache-2.0
//
// Window — the app window + native-handle extraction. The DisplayXR runtime
// renders into a handle the app owns (the `_handle` app class), so we must hand
// it the platform-native view/window:
//   macOS  -> NSView* (CAMetalLayer-backed) via SDL_Metal_CreateView
//   Windows-> HWND via SDL window properties
//   Linux  -> &LinuxHandles: displayxr-common's DxrLinuxWindow — THE desktop-
//             Linux window every DisplayXR demo uses (X11 or native Wayland,
//             picked by its capability probe). It owns the window chrome
//             (the client-side title bar on GNOME Wayland, the X11 header bar),
//             the phase-snapped drags (title bar, right-drag anywhere), F11 to
//             the panel and the Wayland geometry feed. SDL still provides audio,
//             camera and the file dialog on Linux, but no window.
// macOS / Windows: one SDL codebase; only the handle extraction is per-platform.
// Desktop Linux: src/platform/WindowLinux.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
#if defined(__linux__) && !defined(__ANDROID__)
class DxrLinuxWindow;   // displayxr-common (dxr_linux_window.h)
#endif

namespace mp {

class Window {
public:
#if defined(__linux__) && !defined(__ANDROID__)
    // What NativeHandle() points at on desktop Linux. `wayland` is known from
    // Create() (the capability probe's verdict — it picks the binding extension
    // before the instance exists); `win` exists from RealizeOnPanel() on, which
    // XrSession runs after the system-properties query and before
    // xrCreateSession — the window helper's ordering contract.
    struct LinuxHandles {
        bool wayland = false;           // true: XR_DXR_wayland_surface_binding; false: xlib
        DxrLinuxWindow* win = nullptr;  // null until RealizeOnPanel(), or with no platform
    };
    //! --platform=x11|wayland|auto from the command line (default auto). Read
    //! by Create(); 0 = auto, 1 = x11, 2 = wayland.
    static void SetLinuxPlatformRequest(int request);
#endif

    // Create the native window now that the 3D panel is known (desktop Linux;
    // a no-op elsewhere, where Create() already made it). (left, top) is the
    // panel's top-left in virtual-desktop px, (panelW, panelH) its size (0 =
    // unknown). The window opens where SetPosition() last asked, if it did.
    void RealizeOnPanel(int32_t left, int32_t top, uint32_t panelW, uint32_t panelH);

    // Pointer position in window points (the PointSize() space).
    void MousePosition(float& x, float& y) const;

    // The pointer shape ImGui wants (an ImGuiMouseCursor value; -1 = none).
    // Desktop Linux only — the SDL ImGui backend sets cursors itself elsewhere.
    void SetImGuiCursor(int imguiCursor);

    Window();   // out of line: a member of incomplete type (desktop Linux)
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

    // --- Drag and drop (#44) ---
    // Paths dropped on the window since the last read, in drop order; moved into `out`
    // and the latch cleared. Returns false (leaving `out` untouched) if nothing was
    // dropped. SDL3 delivers one SDL_EVENT_DROP_FILE per file, so a multi-file drop
    // arrives as several paths — the caller decides what that means.
    bool TakeDroppedPaths(std::vector<std::string>& out);

    // --- Folder navigation / slideshow ---
    bool TakePrevMediaRequest();      // Left arrow — previous asset in the folder
    bool TakeNextMediaRequest();      // Right arrow — next asset in the folder
    bool TakeToggleSlideshowRequest(); // `S` — toggle slideshow ("diaporama")
    bool TakeToggleMuteRequest();      // `M` — toggle audio mute
    bool TakeCycleLayoutRequest();     // `L` — cycle the stereo-layout override (#45)
    bool TakeToggleCameraRequest();    // `C` — live camera on/off (#90)
    bool TakeToggleAutoConvRequest();  // `A` — auto-convergence on/off (#92)
    bool TakeCycleAutoConvPolicyRequest();  // Shift+A — next subject policy (#92)
    // Ctrl+left-click since the last read, in window POINT coordinates (#92: pin the
    // auto-convergence subject). False if none.
    bool TakeCtrlClick(float& x, float& y);
    bool TakeOpenUrlRequest();         // Ctrl+U — the stream-URL popup (#93)
    // While an ImGui text field has focus the app's single-key shortcuts (Space, L, V,
    // ESC-quits, ...) must not fire on what the user types. The app sets this each frame
    // from ImGui's WantTextInput; PumpEvents then leaves key-downs to ImGui alone.
    void SetKeyboardCaptured(bool c) { keyboardCaptured_ = c; }
    // SDL camera events since the last read, as {SDL event type, SDL_CameraID} pairs
    // (ADDED / REMOVED / APPROVED / DENIED). Moved into `out`; false if none. They only
    // arrive once the app has initialised SDL's camera subsystem.
    bool TakeCameraEvents(std::vector<std::pair<uint32_t, uint32_t>>& out);

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
    LinuxHandles linux_;          // nativeHandle_ points here when a platform resolved
    std::unique_ptr<DxrLinuxWindow> lwin_;
    int linuxBackend_ = 0;        // the probe's verdict: 0 none, 1 X11, 2 Wayland
    std::string title_;
    int reqW_ = 0, reqH_ = 0;     // requested content size (px)
    bool hasPos_ = false;
    int posX_ = 0, posY_ = 0;     // SetPosition() before RealizeOnPanel()
    float mouseX_ = -1.0f, mouseY_ = -1.0f;
    int lastCursor_ = -2;
    // Wayland sends no key repeat (the client repeats): the held key that
    // repeats the app's nudge keys, and when it next fires.
    uint32_t heldKeysym_ = 0;
    uint32_t heldMods_ = 0;
    int64_t heldNextNs_ = 0;
    void HandleLinuxKey(uint32_t keysym, uint32_t mods, bool repeat, bool* quit);
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
    bool cycleLayoutRequested_ = false;
    bool toggleCameraRequested_ = false;
    bool toggleAutoConvRequested_ = false;
    bool cycleAutoConvPolicyRequested_ = false;
    bool ctrlClick_ = false;
    float ctrlClickX_ = 0.0f, ctrlClickY_ = 0.0f;
    bool openUrlRequested_ = false;
    bool keyboardCaptured_ = false;
    std::vector<std::pair<uint32_t, uint32_t>> cameraEvents_;
    std::vector<std::string> dropBatch_;
    bool mouseActivity_ = false;
    bool mouseLeft_ = false;
    bool mouseInWindow_ = true;
    bool fullscreen_ = false;
    std::function<void()> liveResizeCb_;
    std::function<void(void*)> eventHook_;
    bool eventWatchAdded_ = false;
};

} // namespace mp
