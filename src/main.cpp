// SPDX-License-Identifier: Apache-2.0
//
// DisplayXR Stereo Media Player — entry point.
// M0: open a window, bring up an OpenXR stereo session against the DisplayXR
// runtime, and clear the two eye views to distinct colors each frame.
#include "App.h"
#include "Log.h"

// SDL3 redefines main on some platforms; including SDL_main keeps the entry point
// portable (and is a no-op where not needed).
#include <SDL3/SDL_main.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include "dxr_linux_window.h"   // displayxr-common: --platform parsing (the one rule)
#include "platform/Window.h"
#include <string>
#include <vector>
#endif

int main(int argc, char* argv[]) {
    const char* mediaPath = (argc > 1) ? argv[1] : nullptr;
#if defined(__linux__) && !defined(__ANDROID__)
    // --platform=x11|wayland|auto (default auto, a capability probe). Taken out
    // of the argument list first: the media path is the first argument left.
    std::vector<std::string> args(argv + 1, argv + argc);
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
    mediaPath = args.empty() ? nullptr : args[0].c_str();
#endif
    mp::App app;
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
