// SPDX-License-Identifier: Apache-2.0
//
// MediaBrowser: the in-app gallery (#160, docs/BROWSER.md), drawn with ImGui over the
// whole HUD. A top bar (sources, the current source, view options), a sidebar of sources,
// a grid of square thumbnails with 3D and duration badges (Downloads: its folders first),
// select mode for deleting (long-press), and the access and empty states. It owns the
// library, the thumbnails and the 3D verdicts, and reaches the device's media only through
// a MediaProvider. Render thread only.
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
#include <unordered_map>
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
    // Whether Back() has something to do: leave select mode, or go up a folder.
    bool CanGoBack() const;
    bool Back();

private:
    static constexpr uint32_t kTile = 512;  // thumbnail edge, px
    static constexpr uint32_t kSlots = 64;  // thumbnails on the GPU: a screen and a few rows

    void TakeResults();
    void Rebuild();
    const Source& CurrentSource() const;
    int SourceRows() const;
    float SidebarWidth(float screenW) const;
    void TopBar(float width, float height);
    Action Sidebar(ImVec2 min, ImVec2 max, double now);
    Action Grid(ImVec2 min, ImVec2 max, bool landscape, double now);
    void ViewOptionsPopup(float right, float top);
    void UploadReady(const std::unordered_set<int64_t>& window, uint32_t need);
    void GoTo(const std::string& folder);
    void Up();
    void EndSelection();

    MediaProvider* provider_ = nullptr;
    MediaLibrary library_;
    StereoClassifier classifier_;
    ThumbnailAtlas atlas_;
    ThumbnailCache cache_;

    bool listed_ = false;  // an item list has arrived

    SourceKind sourceKind_ = SourceKind::All;
    std::string folder_;  // the folder shown, in a source browsed as folders
    ViewOptions options_;
    DepthFilter depth_ = DepthFilter::Both;
    bool showUnsupported_ = false;  // the Unsupported source in the sidebar
    bool sidebar_ = true;

    bool selecting_ = false;  // select mode: a tap picks an item, the bin deletes the picked
    std::unordered_set<int64_t> selected_;

    bool dirty_ = true;            // folders_ and shown_ need a rebuild
    std::vector<Folder> folders_;  // the folder's sub-folders: the first tiles
    std::vector<size_t> shown_;    // indices into the library's items, in grid order after the folders
    std::vector<size_t> unknown_;  // under a depth filter: items with no verdict yet
    int64_t openedId_ = -1;        // the item last opened from the grid, or -1

    struct Ready {
        Thumbnail thumbnail;
        StereoLayout layout = StereoLayout::Mono;  // its verdict's: which part a tile shows
    };

    std::unordered_set<int64_t> pending_;      // thumbnails taken from the provider, not yet uploaded
    std::deque<Ready> ready_;                  // classified, waiting for an upload
    std::unordered_map<int64_t, uint32_t> sharpen_;  // shown soft: the edge to ask for again
    std::unordered_set<int64_t> sharpened_;    // asked for again already (once is enough)
    std::unordered_set<int64_t> noThumbnail_;  // the provider could not make one
    std::vector<ThumbnailRequest> wanted_;     // the list the provider has

    TouchScroller gridScroll_, sidebarScroll_;
    ImVec2 gridPress_, sidebarPress_;
    int columns_ = 0;   // the last frame's, to keep the top row on a reflow
    float step_ = 0.0f;
};

} // namespace mp::browser
