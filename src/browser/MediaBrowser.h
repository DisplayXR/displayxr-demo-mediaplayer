// SPDX-License-Identifier: Apache-2.0
//
// MediaBrowser: the in-app gallery (#160, docs/BROWSER.md), drawn with ImGui over the
// whole HUD. A top bar (sources, the current source, view options), a row of filter chips,
// the sources as a rail (wide landscape) or a drawer, a grid of square thumbnails with 3D
// and duration badges (date sections; Downloads: its folders first), select mode for
// deleting (long-press), and the access and empty states. It owns the
// library, the thumbnails and the 3D verdicts, and reaches the device's media only through
// a MediaProvider. Render thread only.
#pragma once

#include "browser/BrowserLayout.h"
#include "browser/MediaLibrary.h"
#include "browser/MediaProvider.h"
#include "browser/StereoClassifier.h"
#include "browser/StereoGrid.h"
#include "browser/StereoPairs.h"
#include "browser/ThumbnailAtlas.h"
#include "browser/ThumbnailCache.h"
#include "browser/ThumbnailDepth.h"
#include "browser/TouchScroller.h"

#include <vulkan/vulkan.h>

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mp::browser {

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
    // The item last opened from the grid: its index in grid order and how many the grid
    // holds. False when none was (the system picker) or it is no longer in the grid.
    bool OpenedPosition(int& index, int& count) const;
    // The item last opened from the grid, if any.
    bool OpenedItem(MediaItem& out) const;
    // Whether Back() has something to do: close the drawer or a sheet, leave select mode, or
    // go up a folder.
    bool CanGoBack() const;
    bool Back();

    // The 3D grid (#180, StereoGrid.h). The "3D only" filter is on: the caller takes the
    // panel to 3D for it.
    bool WantsStereo() const { return depth_ == DepthFilter::Only3D; }
    // The panel is in 3D and the caller draws Stereo() into the projection layer, so
    // Build() leaves the tiles' pictures and the background behind them out of the HUD.
    // `viewScale` is the 3D mode's (the useful size of an eye is a tile times it).
    void SetStereo(bool on, float viewScale) {
        stereo_ = on;
        if (viewScale > 0.0f) viewScale_ = viewScale;
    }
    // Make pairs for 3D photos whose thumbnail is one view (LIF, MPO: StereoPairs.h).
    // `read` / `close` work on the file descriptors the provider hands over. Call after
    // Initialize; without it those tiles stay flat.
    void SetPairMaker(ScaledJpegDecoder decode, FileReader read, std::function<void(int)> close);
    // The tiles Build() laid out this frame, at this frame's rects; inactive unless set.
    const StereoFrame& Stereo() const { return stereoFrame_; }

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
    Action Drawer(float width, float height, double now);
    void ChipRow(ImVec2 min, ImVec2 max);
    Action Grid(ImVec2 min, ImVec2 max, double now);
    void ViewOptionsSheet(float width, float top, float bottom);
    void TypeMenu();
    void SetDepth(DepthFilter depth);
    void SetType(TypeFilter type);
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
    bool sidebar_ = true;   // the rail shows (a wide landscape screen)
    bool rail_ = false;     // this frame: the sources are a rail, not a drawer
    bool drawer_ = false;   // the drawer is open (anywhere else)
    bool sourcePicked_ = false;  // Sidebar() picked a source this frame
    bool sheetOpen_ = false;     // the view options sheet or the type menu is open
    bool closeSheets_ = false;   // Back: close them in the next Build()
    ImVec2 typeChip_;       // where the type chip's menu opens, its bottom left

    bool selecting_ = false;  // select mode: a tap picks an item, the bin deletes the picked
    std::unordered_set<int64_t> selected_;

    bool dirty_ = true;            // folders_ and shown_ need a rebuild
    std::vector<Folder> folders_;  // the folder's sub-folders: the first tiles
    std::vector<size_t> shown_;    // indices into the library's items, in grid order after the folders
    std::vector<size_t> unknown_;  // under a depth filter: items with no verdict yet
    // The grid's date sections (All media, Camera: "Today", "October 2026"): how many of
    // shown_ each holds, in order, and their labels. Empty: one section, no headers.
    std::vector<size_t> sectionSizes_;
    std::vector<std::string> sectionLabels_;
    GridLayout layout_;
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
    float thumbGrab_ = 0.0f;  // where the fast-scroll thumb was taken, from its top

    bool stereo_ = false;        // SetStereo
    StereoFrame stereoFrame_;    // this frame's tiles, when stereo_
    ThumbnailDepth thumbDepth_;  // per-thumbnail convergence for the 3D grid
    std::unordered_set<int64_t> refetched_;  // the 3D grid: asked again once for a right eye
    float viewScale_ = 1.0f;
    StereoPairs pairs_;                      // the 3D grid: pairs for one-view 3D photos
    std::function<void(int)> closeFile_;
    std::unordered_set<int64_t> paired_;     // their slot holds a made pair (full side by side)
    std::unordered_set<int64_t> noPair_;     // no pair could be made: flat

    // The packing the slot of `item` holds: a made pair is full side by side.
    StereoLayout SlotLayout(const MediaItem& item, const StereoVerdict& v) const {
        return paired_.count(item.id) ? StereoLayout::SbsFull : v.layout;
    }
    // A 3D photo whose thumbnail is one view, which the 3D grid makes a pair for.
    bool NeedsPair(const MediaItem& item, const StereoVerdict& v) const {
        return pairs_.Ready() && item.type == MediaType::Image && v.known && v.stereo && !Packed(v.layout) &&
               !noPair_.count(item.id);
    }
};

} // namespace mp::browser
