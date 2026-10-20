// SPDX-License-Identifier: Apache-2.0
//
// DisplayXR Stereo Media Player — entry point.
// M0: open a window, bring up an OpenXR stereo session against the DisplayXR
// runtime, and clear the two eye views to distinct colors each frame.
#include "App.h"
#if MEDIAPLAYER_PRO
#include "pro/DeckLinkSource.h"
#endif
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
// and the stream-URL flag (#93):
//   --url <u> / --url=<u>            start on a live network stream
bool TakeCameraArgs(std::vector<std::string>& args, mp::App::LaunchOptions& o, bool& list,
                    bool& listDeckLink) {
    std::vector<std::string> rest;
    struct {
        bool on = false;
        std::string dev = "0", right, opts;
    } decklink;  // Pro: --decklink* collected into one station input
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--url" && i + 1 < args.size()) {
            o.url = args[++i];
        } else if (a.rfind("--url=", 0) == 0) {
            o.url = a.substr(6);
        } else if (a == "--camera") {
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
        } else if (a.rfind("--input=", 0) == 0) {          // Media Player Pro station input
            o.proInputs.push_back(a.substr(8));
        } else if (a == "--decklink" || a.rfind("--decklink=", 0) == 0) {
            decklink.on = true;
            if (a.size() > 10) decklink.dev = a.substr(11);
        } else if (a.rfind("--decklink-right=", 0) == 0) {
            decklink.on = true;
            decklink.right = a.substr(17);
        } else if (a.rfind("--decklink-input=", 0) == 0) {
            decklink.opts += "," + a.substr(17);
        } else if (a.rfind("--decklink-mode=", 0) == 0) {
            decklink.opts += ",mode=" + a.substr(16);
        } else if (a.rfind("--decklink-stereo=", 0) == 0) {
            decklink.opts += "," + a.substr(18);
        } else if (a == "--decklink-list") {
            listDeckLink = true;
        } else if (a == "--test-signal" || a.rfind("--test-signal=", 0) == 0) {
            o.proInputs.push_back("Test rig=test:" + (a.size() > 13 ? a.substr(14) : std::string()));
        } else if (a.rfind("--depth-budget=", 0) == 0) {
            if (std::sscanf(a.c_str() + 15, "%f,%f", &o.proBudget[0], &o.proBudget[1]) != 2 ||
                o.proBudget[0] >= o.proBudget[1]) {
                LOG_ERROR("--depth-budget: expected near,far in %% of eye width (e.g. -2,1)");
                return false;
            }
        } else if (a.rfind("--vertical-limit=", 0) == 0) {
            o.proBudget[2] = (float)std::atof(a.c_str() + 17);
        } else if (a.rfind("--colour-limit=", 0) == 0) {
            o.proBudget[3] = (float)std::atof(a.c_str() + 15);
        } else if (a.rfind("--edge-limit=", 0) == 0) {
            o.proBudget[4] = (float)std::atof(a.c_str() + 13);
        } else if (a == "--no-timecode") {
            o.proTimecode = false;
        } else if (a.rfind("--seq-fps=", 0) == 0) {
            o.proSeqFps = std::atof(a.c_str() + 10);
            if (!(o.proSeqFps > 0.0 && o.proSeqFps <= 240.0)) {
                LOG_ERROR("--seq-fps: expected a rate in fps, e.g. 24 or 23.976");
                return false;
            }
        } else if (a.rfind("--compare=", 0) == 0) {
            o.proCompare = a.substr(10);
        } else if (a.rfind("--ocio=", 0) == 0) {
            o.ocio = true;
            o.ocioConfig = a.substr(7);
        } else if (a.rfind("--ocio-src=", 0) == 0) {
            o.ocio = true;
            o.ocioSrc = a.substr(11);
        } else if (a.rfind("--ocio-display=", 0) == 0) {
            o.ocioDisplay = a.substr(15);
        } else if (a.rfind("--ocio-view=", 0) == 0) {
            o.ocioView = a.substr(12);
        } else if (a.rfind("--ocio-dst=", 0) == 0) {
            o.ocioDst = a.substr(11);
        } else if (a.rfind("--ocio-looks=", 0) == 0) {
            o.ocioLooks = a.substr(13);
        } else if (a == "--lut" && i + 1 < args.size()) {   // Media Player Pro look
            o.lutPath = args[++i];
        } else if (a.rfind("--lut=", 0) == 0) {
            o.lutPath = a.substr(6);
        } else if (a == "--cdl" && i + 1 < args.size()) {
            o.cdlPath = args[++i];
        } else if (a.rfind("--cdl=", 0) == 0) {
            o.cdlPath = a.substr(6);
        } else if (a == "--camera-list") {
            list = true;
        } else {
            rest.push_back(a);
        }
    }
    args.swap(rest);
    if (decklink.on)  // the DeckLink input goes first: the station opens input 0
        o.proInputs.insert(o.proInputs.begin(), "DeckLink=decklink:" + decklink.dev +
                                                    (decklink.right.empty() ? "" : "+" + decklink.right) +
                                                    decklink.opts);
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
    bool listDeckLink = false;
    if (!TakeCameraArgs(args, launch, listCameras, listDeckLink)) return 2;
#if MEDIAPLAYER_PRO
    if (listDeckLink) {
#if defined(_WIN32)
        // Same rule as --camera-list: borrow the parent's console only when stdout has
        // none, so `> file` keeps working (an unconditional CONOUT$ swallowed it).
        const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if ((h == nullptr || h == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
        }
#endif
        mp::DeckLinkSource::PrintDeviceList(stdout);
        return 0;
    }
#endif
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
