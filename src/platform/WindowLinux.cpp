// SPDX-License-Identifier: Apache-2.0
//
// Window on desktop Linux: displayxr-common's DxrLinuxWindow, the one window
// helper every DisplayXR demo uses (dxr_linux_window.h). The player used to open
// an SDL3 window here; on GNOME's native Wayland that window had NO title bar
// (mutter offers no server-side decorations and the static SDL3 is built
// without libdecor) and no content drag, so only Super+drag moved it. The helper
// gives it what the other demos have:
//   - the client-side title bar on Wayland (xdg_toplevel.move, the compositor's
//     move the runtime move-syncs), the X11 header bar with the client-owned,
//     phase-snapped drag, double-click maximise, close / minimise;
//   - right-drag anywhere in the content moves the window (mutter: the
//     window-geometry extension's pointer drag; X11: the snapped client drag);
//   - F11 fullscreen onto the 3D panel's output, the Wayland geometry feed.
// The left button, the wheel and the keyboard stay the player's (and ImGui's).
// SDL is still the player's audio, camera and file-dialog layer — it just opens
// no window any more (events subsystem only).
#if defined(__linux__) && !defined(__ANDROID__)

#include "Window.h"

#include "Log.h"

#include "dxr_linux_window.h"

#include <SDL3/SDL.h>
#include <X11/keysym.h>
#include <openxr/openxr.h>
#include <openxr/XR_DXR_wayland_surface_binding.h>
#include <openxr/XR_DXR_xlib_window_binding.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace mp {

namespace {

int s_linuxPlatformRequest = 0;  // 0 auto, 1 x11, 2 wayland

//! Which window platform: the helper's capability probe, fed by which binding
//! extensions the runtime advertises (no instance needed).
DxrWindowBackend ResolveLinuxPlatform() {
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
        LOG_WARN("No usable window platform (%s) — no window binding (the runtime opens its own)", why.c_str());
    } else {
        LOG_INFO("Window platform: %s (requested %s) — %s", DxrLinuxWindow::backend_name(b),
                 DxrLinuxWindow::backend_name(requested), why.c_str());
    }
    return b;
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Wayland has no server-side key repeat: the client repeats. Same feel as SDL's.
constexpr int64_t kRepeatDelayNs = 400LL * 1000 * 1000;
constexpr int64_t kRepeatPeriodNs = 33LL * 1000 * 1000;

//! ImGuiMouseCursor (imgui.h, v1.91) -> the helper's shape. Numeric so this file
//! needs no ImGui header: Arrow 0, TextInput 1, ResizeAll 2, ResizeNS 3,
//! ResizeEW 4, ResizeNESW 5, ResizeNWSE 6, Hand 7, NotAllowed 8; None = -1.
DxrCursor CursorFromImGui(int c) {
    switch (c) {
        case -1: return DxrCursor::Hidden;
        case 1: return DxrCursor::Text;
        case 2: return DxrCursor::Move;
        case 3: return DxrCursor::ResizeNS;
        case 4: return DxrCursor::ResizeEW;
        case 5: return DxrCursor::ResizeNESW;
        case 6: return DxrCursor::ResizeNWSE;
        case 7: return DxrCursor::Pointer;
        case 8: return DxrCursor::NotAllowed;
        default: return DxrCursor::Default;
    }
}

}  // namespace

void Window::SetLinuxPlatformRequest(int request) { s_linuxPlatformRequest = request; }

Window::Window() = default;
Window::~Window() { Destroy(); }

bool Window::Create(const char* title, int width, int height) {
    // SDL: events only — camera hot-plug / permission events and the file
    // dialog's callback. Audio and camera bring up their own subsystems.
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    title_ = title ? title : "";
    reqW_ = width;
    reqH_ = height;
    const DxrWindowBackend b = ResolveLinuxPlatform();
    if (b == DxrWindowBackend::Auto) return true;   // no binding: the runtime's own window
    linuxBackend_ = b == DxrWindowBackend::X11 ? 1 : 2;
    linux_.wayland = b == DxrWindowBackend::Wayland;
    nativeHandle_ = &linux_;
    LOG_INFO("Window: displayxr-common window (%s), created once the 3D panel is known",
             DxrLinuxWindow::backend_name(b));
    return true;
}

void Window::RealizeOnPanel(int32_t left, int32_t top, uint32_t panelW, uint32_t panelH) {
    if (linuxBackend_ == 0 || lwin_) return;
    DxrLinuxWindowDesc d;
    d.width = (uint32_t)reqW_;
    d.height = (uint32_t)reqH_;
    d.panel_left = left;
    d.panel_top = top;
    d.panel_width = panelW;
    d.panel_height = panelH;
    d.title = title_.c_str();
    d.app_id = "com.displayxr.mediaplayer";
    d.fullscreen_on_wayland = false;  // windowed, like every other desktop leg (F / F11 = fullscreen)
    d.x11_header_bar = true;          // the snapped drag needs a client-side bar on X11
    d.x11_drag_button = 3;            // right-drag anywhere moves the window; the left
    d.wayland_drag_button = 3;        // button stays the player's (scrubber, buttons, Ctrl+click)
    d.has_position = hasPos_;
    d.x = posX_;
    d.y = posY_;
    if (!hasPos_ && panelW > 0 && panelH > 0) {
        // A panel at (0, 0) — the primary monitor — gets no SetPosition() from
        // the app (SDL centred its windows there by itself): centre it on the
        // panel, as the other demos do. (X11 only; Wayland's compositor places.)
        d.has_position = true;
        d.x = left + ((int)panelW - reqW_) / 2;
        d.y = top + ((int)panelH - reqH_) / 2;
    }
    const DxrWindowBackend b = linuxBackend_ == 1 ? DxrWindowBackend::X11 : DxrWindowBackend::Wayland;
    lwin_ = std::make_unique<DxrLinuxWindow>();
    // The stream-URL field: dead keys / Compose produce text (XIM / xkb compose).
    lwin_->set_text_input(true);
    if (!lwin_->create(b, d)) {
        LOG_ERROR("Could not create the %s window — no window binding", DxrLinuxWindow::backend_name(b));
        lwin_.reset();
        return;
    }
    linux_.win = lwin_.get();
    uint32_t w = 0, h = 0;
    lwin_->current_size(&w, &h);
    LOG_INFO("Window created: %s, %ux%u px — title bar + right-drag move it",
             lwin_->connection_description().c_str(), w, h);
}

void Window::PixelSize(uint32_t& width, uint32_t& height) const {
    uint32_t w = 0, h = 0;
    if (lwin_ && lwin_->current_size(&w, &h)) {
        width = w;
        height = h;
        return;
    }
    width = (uint32_t)(reqW_ > 0 ? reqW_ : 0);
    height = (uint32_t)(reqH_ > 0 ? reqH_ : 0);
}

// The helper reports pointer positions in content BUFFER px, so the "point"
// space is the pixel space here.
void Window::PointSize(uint32_t& width, uint32_t& height) const { PixelSize(width, height); }

void Window::SetTitle(const char* title) {
    title_ = title ? title : "";
    if (lwin_) lwin_->set_title(title_.c_str());
}

void Window::SetPosition(int x, int y) {
    // Before RealizeOnPanel only: X11 opens the window there (a Wayland client
    // cannot place itself; the compositor does).
    hasPos_ = true;
    posX_ = x;
    posY_ = y;
}

void Window::Show() {}   // the helper maps the window at create

void Window::MousePosition(float& x, float& y) const {
    x = mouseX_;
    y = mouseY_;
}

void Window::SetImGuiCursor(int imguiCursor) {
    if (!lwin_ || imguiCursor == lastCursor_) return;
    lastCursor_ = imguiCursor;
    lwin_->set_cursor(CursorFromImGui(imguiCursor));
}

void Window::ToggleFullscreen() {
    if (!lwin_) return;
    lwin_->toggle_fullscreen();
    fullscreen_ = lwin_->is_fullscreen();
    LOG_INFO("Fullscreen %s", fullscreen_ ? "on" : "off");
}

// The player's key bindings, by X11 keysym (shift level 0 on both backends — the
// same keys as SDL's keycodes on the other desktops).
void Window::HandleLinuxKey(uint32_t ks, uint32_t mods, bool repeat, bool* quit) {
    const bool ctrl = (mods & DxrModCtrl) != 0;
    const bool shift = (mods & DxrModShift) != 0;
    // Convergence nudges and frame steps repeat while held; everything else is one-shot.
    if (ks == XK_equal) { ++convergenceSteps_; return; }
    if (ks == XK_minus) { --convergenceSteps_; return; }
    if (ks == XK_bracketright) { ++frameStepRequest_; return; }
    if (ks == XK_bracketleft) { --frameStepRequest_; return; }
    if (repeat) return;
    switch (ks) {
        case XK_Escape: *quit = true; break;
        case XK_v: cycleModeRequested_ = true; break;
        case XK_Tab: if (shift) toggleHudRequested_ = true; break;
        case XK_0: resetConvergenceRequested_ = true; break;
        case XK_BackSpace: autoConvergeRequested_ = true; break;
        case XK_x: swapEyesRequested_ = true; break;
        case XK_space: togglePauseRequested_ = true; break;
        case XK_Left: prevMediaRequested_ = true; break;
        case XK_Right: nextMediaRequested_ = true; break;
        case XK_s: toggleSlideshowRequested_ = true; break;
        case XK_m: toggleMuteRequested_ = true; break;
        case XK_l: cycleLayoutRequested_ = true; break;          // stereo layout override
        case XK_c: if (!ctrl) toggleCameraRequested_ = true; break;  // live camera (#90)
        case XK_i: captureRequested_ = true; break;               // snapshot the atlas
        case XK_a:                                                // auto-convergence (#92)
            if (!ctrl) {
                if (shift) cycleAutoConvPolicyRequested_ = true;
                else toggleAutoConvRequested_ = true;
            }
            break;
        case XK_o: if (ctrl) openFileRequested_ = true; break;    // Ctrl+O — open
        case XK_u: if (ctrl) openUrlRequested_ = true; break;     // Ctrl+U — stream URL (#93)
        case XK_f: ToggleFullscreen(); break;
        // F11 is the window helper's own (it toggles fullscreen inside the pump).
        default: break;
    }
}

bool Window::PumpEvents() {
    bool quit = false;

    // SDL carries no window here: only the camera's hot-plug / permission
    // events (and a quit signal) arrive through it.
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) quit = true;
        if (e.type == SDL_EVENT_CAMERA_DEVICE_ADDED || e.type == SDL_EVENT_CAMERA_DEVICE_REMOVED ||
            e.type == SDL_EVENT_CAMERA_DEVICE_APPROVED || e.type == SDL_EVENT_CAMERA_DEVICE_DENIED) {
            cameraEvents_.emplace_back((uint32_t)e.type, (uint32_t)e.cdevice.which);
            if (e.type == SDL_EVENT_CAMERA_DEVICE_REMOVED || e.type == SDL_EVENT_CAMERA_DEVICE_DENIED)
                mouseActivity_ = true;  // wake the UI so the status change is seen
        }
    }
    if (!lwin_) return !quit;

    bool running = true;
    lwin_->pump_events(
        [this, &quit](const DxrWindowEvent& ev) {
            if (eventHook_) eventHook_((void*)&ev);   // feed ImGui first (it may want the input)
            switch (ev.type) {
                case DxrWindowEvent::Type::KeyDown:
                    if (!keyboardCaptured_) HandleLinuxKey(ev.keysym, ev.mods, ev.repeat, &quit);
                    if (!ev.repeat) {  // Wayland: we repeat a held key ourselves
                        heldKeysym_ = ev.keysym;
                        heldMods_ = ev.mods;
                        heldNextNs_ = NowNs() + kRepeatDelayNs;
                    }
                    break;
                case DxrWindowEvent::Type::KeyUp:
                    if (ev.keysym == heldKeysym_) heldKeysym_ = 0;
                    break;
                case DxrWindowEvent::Type::FocusLost: heldKeysym_ = 0; break;
                case DxrWindowEvent::Type::Motion:
                    mouseX_ = (float)ev.x;
                    mouseY_ = (float)ev.y;
                    if (!mouseInWindow_) { mouseInWindow_ = true; mouseActivity_ = true; }
                    break;
                case DxrWindowEvent::Type::PointerLeave:
                    mouseInWindow_ = false;
                    mouseLeft_ = true;
                    break;
                case DxrWindowEvent::Type::ButtonDown:
                    // Discrete pointer activity wakes the auto-hide UI (motion is
                    // polled by the app, jitter-immune). A window drag still counts.
                    mouseActivity_ = true;
                    if (ev.button == 1 && !ev.window_drag && (ev.mods & DxrModCtrl)) {
                        ctrlClick_ = true;  // #92: pin the auto-convergence subject
                        ctrlClickX_ = (float)ev.x;
                        ctrlClickY_ = (float)ev.y;
                    }
                    break;
                case DxrWindowEvent::Type::Scroll: mouseActivity_ = true; break;
                case DxrWindowEvent::Type::Drop:
                    // Drag and drop (#44): one event per drop, the whole batch.
                    dropBatch_ = ev.paths;
                    mouseActivity_ = true;
                    break;
                default: break;
            }
        },
        &running);
    if (!running) return false;   // the title bar's close button / the WM's close

    // Wayland key repeat (X11 delivers its own, marked `repeat`). Only the keys
    // that repeat at all act on it (HandleLinuxKey's nudges / frame steps).
    if (heldKeysym_ != 0 && linuxBackend_ == 2 && !keyboardCaptured_) {
        const int64_t now = NowNs();
        if (now >= heldNextNs_) {
            HandleLinuxKey(heldKeysym_, heldMods_, /*repeat=*/true, &quit);
            heldNextNs_ = now + kRepeatPeriodNs;
        }
    }
    return !quit;
}

void Window::Destroy() {
    nativeHandle_ = nullptr;
    linux_ = LinuxHandles{};
    // LAST, after the XR session and its Vulkan instance are gone (App::Shutdown
    // order): the runtime's VkSurfaceKHR borrowed this window's connection.
    if (lwin_) {
        lwin_->destroy();
        lwin_.reset();
    }
    SDL_Quit();
}

}  // namespace mp

#endif  // desktop Linux
