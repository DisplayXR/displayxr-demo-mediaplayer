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
        Closed,      // desktop: the close button; the caller goes back to the player
        AddFolder,   // desktop: "Add folder...": the caller asks for a folder to pin
    };

    // Desktop: what the user asked of a folder tile's menu (right click).
    struct FolderRequest {
        enum class Verb { Pin, Unpin, Hide } verb = Verb::Pin;
        std::string path;  // as the provider reported it ('/'-separated, trailing '/')
    };
    bool TakeFolderRequest(FolderRequest& out) {
        if (!folderRequestPending_) return false;
        folderRequestPending_ = false;
        out = folderRequest_;
        return true;
    }

    // Desktop: what the user asked of a file's menu (right click) or its keys.
    struct ItemRequest {
        enum class Verb { Reveal, CopyPath } verb = Verb::Reveal;
        int64_t id = -1;
    };
    bool TakeItemRequest(ItemRequest& out) {
        if (!itemRequestPending_) return false;
        itemRequestPending_ = false;
        out = itemRequest_;
        return true;
    }

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

    // Desktop (#252): a close button in the top bar, mouse-wheel scrolling and a hover
    // highlight. Off on Android.
    void SetDesktop(bool on) { desktop_ = on; }

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
    // Desktop: back (-1) or forward (+1) through the places shown, as a file manager's
    // Back / Forward. False with nowhere to go.
    bool HistoryStep(int dir);
    // Desktop: the details view (one row per file) instead of the grid.
    void SetDetails(bool on) { details_ = on; }
    // Desktop: show only the names holding `query` (any case), as the search box does.
    void SetQuery(const std::string& query);
    // Ctrl+F: the search box takes the keyboard.
    void FocusSearch() { focusSearch_ = true; }
    // Desktop: grid or details, the cards' size and the sort, as the caller keeps them
    // between launches (BrowserLayout.h).
    DeskViewPrefs ViewPrefs() const { return DeskViewPrefs{details_, cardW_, options_.order}; }
    void SetViewPrefs(const DeskViewPrefs& prefs);
    static constexpr float kCardMin = 120.0f, kCardMax = 320.0f;  // desktop: the size slider's range

    // The 3D grid (#180, StereoGrid.h). The "3D only" filter is on: the caller takes the
    // panel to 3D for it.
    bool WantsStereo() const { return depth_ == DepthFilter::Only3D; }
    // The 2D / 3D filter, as the chip sets it (tests start on "3D only").
    void SetDepthFilter(DepthFilter depth) { SetDepth(depth); }
    // Show the source named `name` once the list arrives (tests: MEDIAPLAYER_BROWSER_SOURCE).
    void SetStartSource(std::string name) { startSource_ = std::move(name); }
    // The panel is in 3D and the caller draws Stereo() into the projection layer, so
    // Build() leaves the tiles' pictures and the background behind them out of the HUD.
    // `viewScale` is the 3D mode's (the useful size of an eye is a tile times it).
    void SetStereo(bool on, float viewScale) {
        stereo_ = on;
        if (viewScale > 0.0f) viewScale_ = viewScale;
    }
    // Make pairs for 3D photos whose thumbnail is one view (LIF, MPO: StereoPairs.h).
    // `read` / `close` work on the file descriptors the provider hands over. Call after
    // Initialize; without it those tiles stay flat. `diskDir`: where the pairs are kept
    // between launches (PairDiskCache.h; empty = memory only).
    void SetPairMaker(ScaledJpegDecoder decode, FileReader read, std::function<void(int)> close,
                      const std::string& diskDir = std::string());
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
    void FolderMenu();
    void Up();
    void EndSelection();
    // The desktop look (MediaBrowserDesktop.cpp, #252).
    void DeskHeader(float width, float height);
    void DeskToolbar(ImVec2 min, ImVec2 max);
    void DeskSortMenu();
    Action DeskSidebar(ImVec2 min, ImVec2 max);
    // `preview`: indices into the library's items to show a mosaic of (up to `shown` of them,
    // as far as their thumbnails are here); none: the folder glyph.
    void DeskFolderCard(ImDrawList* dl, const Folder& folder, ImVec2 a, ImVec2 b, float pic, bool hovered, bool pressed,
                        bool focus, const std::vector<size_t>* preview, size_t shown);
    void DeskItemCard(ImDrawList* dl, const MediaItem& item, bool supported, const StereoVerdict& verdict, ImVec2 a,
                      ImVec2 b, float pic, bool hovered, bool pressed, bool focus, bool picture, bool behind);
    static float DeskCaptionHeight();
    // The details view's columns: 0 name, 1 depth, 2 type, 3 dimensions, 4 length, 5 date,
    // 6 size. Narrow windows drop type, dimensions, size, then date.
public:
    struct DeskColumns {
        float x[7] = {};
        float w[7] = {};
        bool on[7] = {};
    };

private:
    static DeskColumns DeskColumnLayout(float x0, float width);
    void DeskColumnHeader(const DeskColumns& c, ImVec2 min, ImVec2 max);
    void DeskFolderRow(ImDrawList* dl, const Folder& folder, const DeskColumns& c, ImVec2 a, ImVec2 b, float pic,
                       bool hovered, bool pressed, bool focus);
    void DeskItemRow(ImDrawList* dl, const MediaItem& item, bool supported, const StereoVerdict& verdict,
                     const DeskColumns& c, ImVec2 a, ImVec2 b, float pic, bool hovered, bool pressed, bool focus,
                     bool picture, bool behind);
    void DeskSearch(ImVec2 pos, float w, float h, const std::string& place);
    void DeskItemMenu();
    void DeskPinnedMenu();
    void TrackHistory();
    void UpdatePinnedRows();

    MediaProvider* provider_ = nullptr;
    bool desktop_ = false;         // SetDesktop
    bool closeRequested_ = false;  // the close button, this frame
    FolderRequest folderRequest_;
    bool folderRequestPending_ = false;
    std::string menuFolder_;       // the folder tile whose menu is open
    size_t cursor_ = SIZE_MAX;     // desktop: the keyboard's tile (SIZE_MAX: none yet)
    bool details_ = false;         // desktop: the details view
    float cardW_ = 176.0f;         // desktop grid: a card's width, design px (the size slider)
    float cardShown_ = 0.0f;       // ... as drawn: eases to cardW_ (0: not drawn yet)
    float lastStep_ = 0.0f;        // the last frame's row pitch, to hold the view on a resize
    char search_[256] = {};        // desktop: the search box's text
    std::string query_;            // ... as the grid filters by it
    bool focusSearch_ = false;     // Ctrl+F: the search box takes the keyboard
    // Desktop: the places shown, for Back / Forward.
    struct Place {
        size_t source = 0;
        std::string folder;
    };
    std::vector<Place> back_, forward_;
    Place here_;
    bool placed_ = false;
    ItemRequest itemRequest_;
    bool itemRequestPending_ = false;
    // Desktop: the Pinned source's folders, a sidebar row each (UpdatePinnedRows).
    std::vector<Folder> pinnedRows_;
    size_t pinnedSource_ = SIZE_MAX;  // the Pinned source's index, SIZE_MAX: none
    std::string pinnedMenu_;          // the pinned row whose menu is open
    int64_t menuItem_ = -1;        // the file whose menu is open
    bool menuOpen_ = false;        // its menu's "Open", this frame
    MediaLibrary library_;
    StereoClassifier classifier_;
    ThumbnailAtlas atlas_;
    ThumbnailCache cache_;

    bool listed_ = false;  // an item list has arrived

    size_t source_ = 0;   // index into the library's sources
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
    ImVec2 sortMenuAt_;     // desktop: where the sort menu opens
    std::string startSource_;  // SetStartSource, until the list arrives

    bool selecting_ = false;  // select mode: a tap picks an item, the bin deletes the picked
    std::unordered_set<int64_t> selected_;

    bool dirty_ = true;            // folders_ and shown_ need a rebuild
    std::vector<Folder> folders_;  // the folder's sub-folders: the first tiles
    // Desktop: per folder tile, the files its card previews (MediaLibrary::FolderPreviews).
    std::vector<std::vector<size_t>> folderPreviews_;
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
