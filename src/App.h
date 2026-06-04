// SPDX-License-Identifier: BSL-1.0
//
// App — wires the SDL3 window, OpenXR session, and Vulkan renderer, and owns the
// frame loop. For M0 the loop clears the two eyes to distinct colors; M1+ adds
// decode + UI (PRD §4).
#pragma once

#include "media/MediaSource.h"
#include "platform/Window.h"
#include "rhi/VulkanRenderer.h"
#include "xr/XrSession.h"

namespace mp {

class App {
public:
    // `mediaPath` may be null/empty — then the app falls back to the RED|BLUE
    // L/R test pattern instead of a loaded stereo image.
    bool Initialize(const char* mediaPath);
    int Run();          // returns process exit code
    void Shutdown();

private:
    Window window_;
    XrSession xr_;
    VulkanRenderer renderer_;

    bool hasImage_ = false;
    StereoLayout layout_ = StereoLayout::Mono;
};

} // namespace mp
