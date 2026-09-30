// SPDX-License-Identifier: Apache-2.0
//
// DisplayXR Stereo Media Player — entry point.
// M0: open a window, bring up an OpenXR stereo session against the DisplayXR
// runtime, and clear the two eye views to distinct colors each frame.
#include "App.h"
#include "Log.h"
#include "media/CameraSource.h"

// SDL3 redefines main on some platforms; including SDL_main keeps the entry point
// portable (and is a no-op where not needed).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // AttachConsole: --camera-list prints from a WIN32_EXECUTABLE
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#include "dxr_linux_window.h"   // displayxr-common: --platform parsing (the one rule)
#include "platform/Window.h"
#endif

namespace {
// Live camera flags (#90), taken out of `args` so the media path is still the first
// argument left:
//   --camera / --camera=<selector>   start live (selector: auto | index | name substring)
//   --camera-fps=<N>                 requested capture rate
//   --camera-list                    print the cameras and exit (no window, no runtime)
//   --auto-conv[=nearest|sharp|centre]  live auto-convergence on (#92; default OFF)
bool TakeCameraArgs(std::vector<std::string>& args, mp::App::LaunchOptions& o, bool& list) {
    std::vector<std::string> rest;
    for (const std::string& a : args) {
        if (a == "--camera") {
            o.camera = true;
        } else if (a.rfind("--camera=", 0) == 0) {
            o.camera = true;
            o.cameraSelector = a.substr(9);
        } else if (a.rfind("--camera-fps=", 0) == 0) {
            o.cameraFps = std::atoi(a.c_str() + 13);
        } else if (a == "--auto-conv") {  // #92: auto-convergence on (default policy)
            o.autoConv = true;
        } else if (a.rfind("--auto-conv=", 0) == 0) {
            o.autoConv = true;
            o.autoConvPolicy = a.substr(12);
        } else if (a == "--camera-list") {
            list = true;
        } else {
            rest.push_back(a);
        }
    }
    args.swap(rest);
    return true;
}

int PrintCameraList() {
#if defined(_WIN32)
    // A WIN32_EXECUTABLE has no console: borrow the parent's, if any, and point stdout
    // at it. A redirected stdout (`> cams.txt`) already works and is left alone.
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((h == nullptr || h == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
#endif
    if (!SDL_Init(0)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    mp::CameraSource::PrintDeviceList(stdout);
    SDL_Quit();
    return 0;
}
}  // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
#if defined(__linux__) && !defined(__ANDROID__)
    // --platform=x11|wayland|auto (default auto, a capability probe). Taken out
    // of the argument list first: the media path is the first argument left.
    {
        DxrWindowBackend requested = DxrWindowBackend::Auto;
        std::string err;
        if (!DxrLinuxWindow::take_platform_args(&args, &requested, &err)) {
            LOG_ERROR("%s", err.c_str());
            return 1;
        }
        mp::Window::SetLinuxPlatformRequest(requested == DxrWindowBackend::X11       ? 1
                                            : requested == DxrWindowBackend::Wayland ? 2
                                                                                     : 0);
    }
#endif
    mp::App::LaunchOptions launch;
    bool listCameras = false;
    TakeCameraArgs(args, launch, listCameras);
    if (listCameras) return PrintCameraList();

    // The media path is the first remaining argument that is not a flag.
    const char* mediaPath = nullptr;
    for (const std::string& a : args) {
        if (a.rfind("--", 0) == 0) continue;
        mediaPath = a.c_str();
        break;
    }

    mp::App app;
    app.SetLaunchOptions(launch);
    if (!app.Initialize(mediaPath)) {
        LOG_ERROR("Initialization failed — is the DisplayXR runtime installed and "
                  "XR_RUNTIME_JSON pointed at it?");
        app.Shutdown();
        return 1;
    }
    int rc = app.Run();
    app.Shutdown();
    LOG_INFO("Clean shutdown (rc=%d)", rc);
    return rc;
}
