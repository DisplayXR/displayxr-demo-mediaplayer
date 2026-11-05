// SPDX-License-Identifier: Apache-2.0
//
// MediaBrowser: the in-app gallery (#160, docs/BROWSER.md), drawn with ImGui over the
// whole HUD. A top bar (sources, the current source, view options), a sidebar of sources,
// a grid of square thumbnails with 3D and duration badges, and the access and empty
// states. It owns the library, the thumbnails and the 3D verdicts, and reaches the
// device's media only through a MediaProvider. Render thread only.
#pragma once

#include "browser/MediaLibrary.h"
#include "browser/MediaProvider.h"
#include "browser/StereoClassifier.h"
#include "browser/ThumbnailAtlas.h"
#include "browser/ThumbnailCache.h"
#include "browser/TouchScroller.h"

#include <vulkan/vulkan.h>

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace mp::browser {

enum class DepthFilter { Both, Only3D, Only2D };

class MediaBrowser {
public:
    // What the user picked in Build().
    enum class Action {
        None,
        Opened,      // an item, already handed to MediaProvider::Open
        OtherFiles,  // "Other files...": the caller opens the system picker
    };

    MediaBrowser() = default;
    ~MediaBrowser();

    MediaBrowser(const MediaBrowser&) = delete;
    MediaBrowser& operator=(const MediaBrowser&) = delete;

    // `provider` must outlive the browser. The thumbnails take their descriptors from
    // ImGuiLayer, so initialize after ImGuiLayer::Init and shut down before
    // ImGuiLayer::Shutdown. `cachePath` is StereoClassifier's file of verdicts.
    bool Initialize(MediaProvider* provider, VkPhysicalDevice physicalDevice, VkDevice device,
                    uint32_t framesInFlight, const std::string& cachePath);
    void Shutdown();

    // Read the library again, e.g. each time the browser is shown.
    void Refresh();
    // Write the 3D verdicts to disk, e.g. when the app goes to the background.
    void Save() { classifier_.Save(); }
    // Emit the browser, between ImGuiLayer::BeginFrame and RenderToHud.
    Action Build();
    // Record this frame's thumbnail uploads into the frame command buffer, before
    // ImGuiLayer::RenderToHud.
    void RecordUploads(VkCommandBuffer cmd) { atlas_.RecordUploads(cmd); }
    // Open the item `step` places after the one last opened, in grid order (-1: the one
    // before). False at either end, under Unsupported, or after "Other files...".
    bool OpenNext(int step);
    // The item OpenNext(step) would open.
    bool NextId(int step, int64_t& id) const;
    // Make the item `step` places away the one last opened, without opening it: the player
    // shows it already.
    bool StepTo(int step);

private:
    static constexpr uint32_t kTile = 512;  // thumbnail edge, px
    static constexpr uint32_t kSlots = 64;  // thumbnails on the GPU: a screen and a few rows

    void TakeResults();
    void Rebuild();
    const Source& CurrentSource() const;
    float SidebarWidth(float screenW) const;
    void TopBar(float width, float height);
    Action Sidebar(ImVec2 min, ImVec2 max, double now);
    Action Grid(ImVec2 min, ImVec2 max, bool landscape, double now);
    void ViewOptionsPopup(float right, float top);
    void UploadReady(const std::unordered_set<int64_t>& window);

    MediaProvider* provider_ = nullptr;
    MediaLibrary library_;
    StereoClassifier classifier_;
    ThumbnailAtlas atlas_;
    ThumbnailCache cache_;

    bool listed_ = false;  // an item list has arrived

    SourceKind sourceKind_ = SourceKind::All;
    ViewOptions options_;
    DepthFilter depth_ = DepthFilter::Both;
    bool sidebar_ = true;

    bool dirty_ = true;            // shown_ needs a rebuild
    std::vector<size_t> shown_;    // indices into the library's items, in grid order
    std::vector<size_t> unknown_;  // under a depth filter: items with no verdict yet
    int64_t openedId_ = -1;        // the item last opened from the grid, or -1

    std::unordered_set<int64_t> pending_;      // thumbnails taken from the provider, not yet uploaded
    std::deque<Thumbnail> ready_;              // classified, waiting for an upload
    std::unordered_set<int64_t> noThumbnail_;  // the provider could not make one
    std::vector<ThumbnailRequest> wanted_;     // the list the provider has

    TouchScroller gridScroll_, sidebarScroll_;
    ImVec2 gridPress_, sidebarPress_;
    int columns_ = 0;   // the last frame's, to keep the top row on a reflow
    float step_ = 0.0f;
};

} // namespace mp::browser
