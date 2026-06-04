// SPDX-License-Identifier: BSL-1.0
//
// DisplayXR Stereo Media Player — entry point.
// M0: open a window, bring up an OpenXR stereo session against the DisplayXR
// runtime, and clear the two eye views to distinct colors each frame.
#include "App.h"
#include "Log.h"

// SDL3 redefines main on some platforms; including SDL_main keeps the entry point
// portable (and is a no-op where not needed).
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[]) {
    const char* mediaPath = (argc > 1) ? argv[1] : nullptr;
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
