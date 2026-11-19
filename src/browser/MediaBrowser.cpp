// SPDX-License-Identifier: Apache-2.0
#include "browser/MediaBrowser.h"

#include "Log.h"

#include "browser/TileCrop.h"
#include "ui/Icons.h"
#include "ui/TransportUI.h"
#include "ui/UiScale.h"
#include "ui/UiText.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <utility>

namespace mp::browser {

namespace {

using ui::Glyph;

// Android's dp is 1.5 design units (ui::Dp). On desktop (SetDesktop) a dp is one design
// unit: the browser is drawn at the size of the rest of the desktop UI (#252).
float g_dpUnits = 1.5f;
float Dp(float v) { return ui::Px(v * g_dpUnits); }
constexpr float kDesktopRailMinDp = 600.0f;  // desktop: the sources stay as a rail from here

// Whether `text` holds `part`, ignoring ASCII case (desktop search).
bool ContainsNoCase(const std::string& text, const std::string& part) {
    auto low = [](unsigned char c) { return (char)std::tolower(c); };
    return std::search(text.begin(), text.end(), part.begin(), part.end(),
                       [&](char x, char y) { return low((unsigned char)x) == low((unsigned char)y); }) != text.end();
}
using ui::Px;
using ui::TextStyle;

constexpr float kGutter = 4.0f;    // between tiles, design units
constexpr float kStereoGutter = 12.0f;  // ...in the 3D grid: flat space between the depths
constexpr float kStereoFade = 3.0f;     // the 3D grid: each tile fades to flat at its border
constexpr float kSlop = 12.0f;     // how far a tap may move, design units
constexpr size_t kScanBatch = 16;  // under a depth filter: items with no verdict asked for at once
constexpr size_t kFastScrollItems = 200;  // more than this: a fast-scroll thumb
constexpr uint32_t kFetchCap = 2048;  // the largest thumbnail asked for, px (a wide panorama's)
// A tile shown softer than this fraction of 1:1 asks once more, for a sharper thumbnail.
constexpr float kSoft = 0.8f;

const ImVec4 kBackground(0.05f, 0.06f, 0.08f, 1.0f);    // the UI's window colour, opaque
const ImU32 kPanel = IM_COL32(23, 26, 33, 255);         // top bar and sidebar
const ImU32 kTileEmpty = IM_COL32(255, 255, 255, 16);
const ImU32 kPressed = IM_COL32(255, 255, 255, 40);
const ImU32 kHover = IM_COL32(255, 255, 255, 150);      // desktop: an outline on the tile under the pointer
const ImU32 kSelected = IM_COL32(51, 166, 255, 72);     // the UI's accent
const ImU32 kAccent = IM_COL32(51, 166, 255, 255);
const ImU32 kPicked = IM_COL32(0, 0, 0, 110);           // over a tile picked in select mode
const ImU32 kBadge = IM_COL32(0, 0, 0, 140);
const ImU32 kBadgeOpaque = IM_COL32(16, 17, 20, 255);  // the 3D grid: flat over a stereo tile
const ImU32 kBadgeText = IM_COL32(255, 255, 255, 255);
const ImU32 kFolder = IM_COL32(190, 192, 198, 255);
const ImU32 kScrim = IM_COL32(0, 0, 0, 140);            // under the drawer and the sheets
const ImU32 kDim = IM_COL32(170, 176, 186, 255);        // secondary text: counts, headers

float Round(float v) { return std::floor(v + 0.5f); }

bool Same(const std::vector<ThumbnailRequest>& a, const std::vector<ThumbnailRequest>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].checkFile != b[i].checkFile || a[i].maxEdge != b[i].maxEdge ||
            a[i].pairFile != b[i].pairFile)
            return false;
    }
    return true;
}

Glyph SourceIcon(SourceKind kind) {
    switch (kind) {
        case SourceKind::All: return Glyph::AllMedia;
        case SourceKind::Camera: return Glyph::Camera;
        case SourceKind::Downloads: return Glyph::Download;
        case SourceKind::Pictures: return Glyph::Image;
        case SourceKind::Movies: return Glyph::Video;
        case SourceKind::Unsupported: return Glyph::Unsupported;
        case SourceKind::Recents: return Glyph::Recent;
        case SourceKind::Folders: return Glyph::Folder;
        case SourceKind::Pinned: return Glyph::Pin;
    }
    return Glyph::Unsupported;
}

// A file's format as its badge shows it, e.g. "BMP" for "scan.bmp", or "" if it has none.
std::string Format(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return std::string();
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = (char)std::toupper((unsigned char)c);
    return ext;
}

void Duration(int64_t ms, char* out, size_t n) {
    const int s = (int)(ms / 1000);
    if (s >= 3600) {
        std::snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    } else {
        std::snprintf(out, n, "%d:%02d", s / 60, s % 60);
    }
}

// Draws in a type-scale font while it lives (ui/TransportUI.h; the UI font where none).
struct FontScope {
    explicit FontScope(TextStyle style) { ImGui::PushFont(ui::StyleFont(style)); }
    ~FontScope() { ImGui::PopFont(); }
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};

// A translucent dark pill holding `text`, its right edge at `right`. Returns its left edge.
float Badge(ImDrawList* dl, float right, float top, const char* text, ImU32 fill = kBadge) {
    FontScope small(TextStyle::Small);
    const float font = ImGui::GetFontSize();
    const float padX = Round(font * 0.3f), padY = Round(font * 0.1f);
    const float w = Round(ImGui::CalcTextSize(text).x) + padX * 2.0f;
    const float h = font + padY * 2.0f;
    dl->AddRectFilled(ImVec2(right - w, top), ImVec2(right, top + h), fill, h * 0.5f);
    dl->AddText(ImVec2(right - w + padX, top + padY), kBadgeText, text);
    return right - w;
}

// The same pill, its left edge at `left`.
void LeftBadge(ImDrawList* dl, float left, float top, const char* text) {
    FontScope small(TextStyle::Small);  // Badge's size, so the edge lands where it is drawn
    const float padX = Round(ImGui::GetFontSize() * 0.3f);
    Badge(dl, left + Round(ImGui::CalcTextSize(text).x) + padX * 2.0f, top, text);
}

// A folder's tile: the folder, its name along the bottom and its count top right.
void FolderTile(ImDrawList* dl, const Folder& folder, ImVec2 a, ImVec2 b, float margin) {
    FontScope row(TextStyle::Row);
    const float tile = b.x - a.x, font = ImGui::GetFontSize();
    dl->AddRectFilled(a, b, kTileEmpty);
    ui::DrawGlyph(dl, Glyph::Folder, ImVec2(a.x + tile * 0.5f, a.y + Round((tile - font) * 0.45f)), tile * 0.5f, kFolder);
    const std::string name = ui::FitText(folder.name, tile - margin * 2.0f);
    const float w = Round(ImGui::CalcTextSize(name.c_str()).x);
    dl->AddText(ImVec2(Round(a.x + (tile - w) * 0.5f), b.y - margin - font), ImGui::GetColorU32(ImGuiCol_Text),
                name.c_str());
    if (!folder.detail.empty()) {
        // Where it is, above the name: two grouped folders may share one (#252).
        FontScope small(TextStyle::Small);
        const std::string where = ui::FitText(folder.detail, tile - margin * 2.0f);
        const float dw = Round(ImGui::CalcTextSize(where.c_str()).x);
        dl->AddText(ImVec2(Round(a.x + (tile - dw) * 0.5f), b.y - margin - font - ImGui::GetFontSize()), kDim,
                    where.c_str());
    }
    char count[16];
    std::snprintf(count, sizeof(count), "%d", folder.count);
    Badge(dl, b.x - margin, a.y + margin, count);
}

// Select mode's box on a tile, `size` square at `min`: an outline, or filled and ticked.
void CheckBox(ImDrawList* dl, ImVec2 min, float size, bool on) {
    const ImVec2 max(min.x + size, min.y + size);
    const float th = Round(std::max(1.0f, size * 0.1f)), r = size * 0.15f;
    if (!on) {
        dl->AddRectFilled(min, max, kBadge, r);  // so the outline reads on a light picture
        dl->AddRect(min, max, kBadgeText, r, 0, th);
        return;
    }
    dl->AddRectFilled(min, max, kAccent, r);
    const ImVec2 tick[] = {ImVec2(min.x + size * 0.22f, min.y + size * 0.52f),
                           ImVec2(min.x + size * 0.42f, min.y + size * 0.72f),
                           ImVec2(min.x + size * 0.78f, min.y + size * 0.30f)};
    dl->AddPolyline(tick, 3, kBadgeText, 0, th * 1.5f);
}

bool IconButton(const char* id, Glyph icon, ImVec2 pos, float size) {
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    if (ImGui::IsItemActive()) dl->AddCircleFilled(c, size * 0.5f, ImGui::GetColorU32(ImGuiCol_FrameBgActive));
    ui::DrawGlyph(dl, icon, c, size, ImGui::GetColorU32(ImGuiCol_Text));
    return clicked;
}

// A state in the middle of `min`..`max`: a 48 dp icon (unless `icon` is null), `text`
// wrapped to 80% of the width, and a 48 dp button under it if `button` is set. True when
// the button is pressed.
bool Message(ImVec2 min, ImVec2 max, const Glyph* icon, const char* text, const char* button) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::vector<std::string> lines = ui::WrapText(text, (max.x - min.x) * 0.8f);
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float iconH = icon ? Dp(48.0f) : 0.0f, gap = Dp(16.0f), buttonH = Dp(48.0f);
    const float total = (icon ? iconH + gap : 0.0f) + lineH * (float)lines.size() - ImGui::GetStyle().ItemSpacing.y +
                        (button ? gap + buttonH : 0.0f);
    float y = Round((min.y + max.y - total) * 0.5f);
    const float cx = (min.x + max.x) * 0.5f;
    if (icon) {
        // A glyph spans about 60% of its box: a box of 80 dp draws it 48 dp.
        ui::DrawGlyph(dl, *icon, ImVec2(cx, y + iconH * 0.5f), Dp(80.0f), kDim);
        y += iconH + gap;
    }
    for (const std::string& line : lines) {
        const float w = ImGui::CalcTextSize(line.c_str()).x;
        dl->AddText(ImVec2(Round(cx - w * 0.5f), y), ImGui::GetColorU32(ImGuiCol_Text), line.c_str());
        y += lineH;
    }
    if (!button) return false;
    y += gap - ImGui::GetStyle().ItemSpacing.y;
    const float w = Round(ImGui::CalcTextSize(button).x + Dp(24.0f) * 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(Round(cx - w * 0.5f), y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = ImGui::Button(button, ImVec2(w, buttonH));
    ImGui::PopStyleColor();
    return pressed;
}

// A text button in the accent colour, `height` tall at `pos`. True when tapped.
bool TextButton(const char* id, const char* label, ImVec2 pos, float height) {
    const float w = Round(ImGui::CalcTextSize(label).x + Dp(12.0f) * 2.0f);
    ImGui::SetCursorScreenPos(pos);
    const bool tapped = ImGui::InvisibleButton(id, ImVec2(w, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemActive())
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + height), kPressed, height * 0.5f);
    dl->AddText(ImVec2(pos.x + Dp(12.0f), pos.y + Round((height - ImGui::GetFontSize()) * 0.5f)), kAccent, label);
    return tapped;
}

// The text button's width for `label`.
float TextButtonWidth(const char* label) { return Round(ImGui::CalcTextSize(label).x + Dp(12.0f) * 2.0f); }

// The local time's offset from UTC now, seconds: for the date sections.
int64_t UtcOffsetSeconds(std::time_t now) {
    std::tm* utc = std::gmtime(&now);
    if (!utc) return 0;
    std::tm asLocal = *utc;
    asLocal.tm_isdst = -1;  // let mktime work out whether DST applies
    const std::time_t t = std::mktime(&asLocal);
    return t == (std::time_t)-1 ? 0 : (int64_t)(now - t);
}

// Feed the pointer to `scroller` through the invisible button just submitted. True on a tap.
bool Track(TouchScroller& scroller, ImVec2& press, double now) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsItemActivated()) {
        press = mouse;
        scroller.Press(mouse.x, mouse.y, now);
    } else if (ImGui::IsItemActive()) {
        scroller.Move(mouse.x, mouse.y, now);
    }
    const bool tap = ImGui::IsItemDeactivated() && scroller.Release(now);
    scroller.Update(now);
    return tap;
}

} // namespace

MediaBrowser::~MediaBrowser() { Shutdown(); }

bool MediaBrowser::Initialize(MediaProvider* provider, VkPhysicalDevice physicalDevice, VkDevice device,
                              uint32_t framesInFlight, const std::string& cachePath) {
    if (provider_ || !provider) return false;
    if (!atlas_.Initialize(physicalDevice, device, kTile, kSlots, framesInFlight)) return false;
    if (!classifier_.Initialize(cachePath)) {
        atlas_.Shutdown();
        return false;
    }
    thumbDepth_.Initialize();  // without it the 3D grid biases every tile back instead
    provider_ = provider;
    cache_.Reset((int)atlas_.SlotCount());
    library_.SetItems({});
    return true;
}

void MediaBrowser::SetPairMaker(ScaledJpegDecoder decode, FileReader read, std::function<void(int)> close,
                                const std::string& diskDir) {
    closeFile_ = close;
    pairs_.Initialize(std::move(decode), std::move(read), std::move(close), diskDir);
}

void MediaBrowser::Shutdown() {
    if (!provider_) return;
    classifier_.Shutdown();
    thumbDepth_.Shutdown();
    pairs_.Shutdown();
    paired_.clear();
    noPair_.clear();
    atlas_.Shutdown();
    provider_ = nullptr;
    listed_ = false;
    dirty_ = true;
    pending_.clear();
    ready_.clear();
    noThumbnail_.clear();
    sharpen_.clear();
    sharpened_.clear();
    wanted_.clear();
}

void MediaBrowser::Refresh() {
    if (provider_) provider_->Refresh();
}

void MediaBrowser::TakeResults() {
    std::vector<MediaItem> items;
    std::vector<Source> sources;
    if (provider_->TakeSources(sources)) {
        // Keep the source shown when it is still there (by kind and name), else the first.
        const Source current = CurrentSource();
        library_.SetSources(sources);
        size_t keep = 0;
        for (size_t i = 0; i < sources.size(); ++i)
            if (sources[i].kind == current.kind && sources[i].name == current.name) keep = i;
        if (keep != source_) {
            source_ = keep;
            folder_ = sources.empty() ? std::string() : sources[keep].folder;
        }
        dirty_ = true;
    }
    if (provider_->TakeItems(items)) {
        // An edited file keeps its id: forget its old thumbnail.
        for (const MediaItem& item : items) {
            const MediaItem* old = library_.Find(item.id);
            if (old && old->modified != item.modified) {
                cache_.Remove(item.id);
                noThumbnail_.erase(item.id);
                cache_.Remove(RightEyeKey(item.id));
                sharpen_.erase(item.id);
                sharpened_.erase(item.id);
                thumbDepth_.Forget(item.id);
                paired_.erase(item.id);
                noPair_.erase(item.id);
            }
        }
        library_.SetItems(std::move(items));
        UpdatePinnedRows();
        // An empty source at its top (Recents before anything was opened): show the first
        // one that holds something instead.
        // Only Recents: an empty source the user picked (Pictures still listing, or empty
        // Downloads) stays picked.
        if (CurrentSource().kind == SourceKind::Recents && CurrentSource().count == 0 &&
            folder_ == CurrentSource().folder) {
            const std::vector<Source>& all = library_.Sources();
            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].count == 0 || all[i].kind == SourceKind::Unsupported) continue;
                source_ = i;
                folder_ = all[i].folder;
                break;
            }
        }
        listed_ = true;
        dirty_ = true;
        // Deleted items leave the selection, and with the last one select mode ends. A
        // cancelled delete keeps both.
        for (auto it = selected_.begin(); it != selected_.end();) {
            it = library_.Find(*it) ? std::next(it) : selected_.erase(it);
        }
        if (selected_.empty()) selecting_ = false;
    }

    Thumbnail thumbnail;
    while (provider_->TakeThumbnail(thumbnail)) {
        const MediaItem* item = library_.Find(thumbnail.id);
        if (thumbnail.file >= 0) {
            // Asked for its pair: the file goes to StereoPairs (which closes it) with the view.
            if (item && WantsStereo())
                pairs_.Make(item->id, item->modified, thumbnail.file, thumbnail.fileSize, thumbnail.rgba.data(),
                            thumbnail.width, thumbnail.height, viewScale_);
            else if (closeFile_)
                closeFile_(thumbnail.file);
            thumbnail.file = -1;
        }
        if (!item) continue;
        pending_.insert(thumbnail.id);
        classifier_.Classify(*item, std::move(thumbnail));
        thumbnail = Thumbnail{};
    }
    StereoVerdict verdict;
    while (classifier_.TakeResult(thumbnail, verdict)) {
        if (depth_ != DepthFilter::Both) dirty_ = true;  // it may join the grid now
        // The 3D grid: a one-view 3D photo with a pair made already shows the pair.
        const MediaItem* item = library_.Find(thumbnail.id);
        Thumbnail pair;
        if (item && WantsStereo() && NeedsPair(*item, verdict) && pairs_.Cached(item->id, item->modified, pair)) {
            paired_.insert(item->id);
            ready_.push_back(Ready{std::move(pair), StereoLayout::SbsFull});
        } else {
            if (item && !Packed(verdict.layout)) paired_.erase(item->id);  // a one-view thumbnail again
            ready_.push_back(Ready{std::move(thumbnail), verdict.layout});
        }
        thumbnail = Thumbnail{};
    }
    std::vector<Thumbnail> made;
    std::vector<int64_t> failed;
    pairs_.TakeResults(made, failed);
    for (Thumbnail& t : made) {
        if (!WantsStereo() || !library_.Find(t.id)) continue;  // cached for later
        paired_.insert(t.id);
        pending_.insert(t.id);
        ready_.push_back(Ready{std::move(t), StereoLayout::SbsFull});
    }
    noPair_.insert(failed.begin(), failed.end());
    thumbDepth_.TakeResults();
}

void MediaBrowser::Rebuild() {
    dirty_ = false;
    cursor_ = SIZE_MAX;  // a new list: the keyboard starts again from the first tile
    shown_.clear();
    unknown_.clear();
    const std::vector<MediaItem>& items = library_.Items();
    folders_ = folder_.empty() && CurrentSource().mode == SourceMode::Flat
                   ? std::vector<Folder>()
                   : library_.Folders(CurrentSource(), options_, folder_);
    // Desktop search: only the names holding the query, any case.
    auto matches = [&](const std::string& name) { return query_.empty() || ContainsNoCase(name, query_); };
    folders_.erase(std::remove_if(folders_.begin(), folders_.end(), [&](const Folder& f) { return !matches(f.name); }),
                   folders_.end());
    folderPreviews_.clear();
    if (desktop_) folderPreviews_ = library_.FolderPreviews(CurrentSource(), options_, folders_, 4);
    for (size_t i : library_.Query(CurrentSource(), options_, folder_)) {
        if (!matches(items[i].name)) continue;
        if (depth_ == DepthFilter::Both) {
            shown_.push_back(i);
            continue;
        }
        const StereoVerdict v = classifier_.Get(items[i]);
        if (!v.known) {
            unknown_.push_back(i);
        } else if (v.stereo == (depth_ == DepthFilter::Only3D)) {
            shown_.push_back(i);
        }
    }

    // Date sections, for the sources that are a camera roll (#180). shown_ is in date order,
    // so a section is a run of items with one key.
    sectionSizes_.clear();
    sectionLabels_.clear();
    if (CurrentSource().dateSections && folders_.empty()) {
        const std::time_t now = std::time(nullptr);
        const int64_t nowMs = (int64_t)now * 1000, offset = UtcOffsetSeconds(now);
        int64_t key = 0;
        for (size_t i : shown_) {
            const DateSection s = DateBucket(items[i].dateMs, nowMs, offset);
            if (sectionSizes_.empty() || s.key != key) {
                key = s.key;
                sectionSizes_.push_back(0);
                sectionLabels_.push_back(s.label);
            }
            ++sectionSizes_.back();
        }
    }
}

// The sources the sidebar lists: Unsupported, the last, only when the view options show it.
int MediaBrowser::SourceRows() const {
    const std::vector<Source>& s = library_.Sources();
    const bool hide = !showUnsupported_ && !s.empty() && s.back().kind == SourceKind::Unsupported;
    return (int)s.size() - (hide ? 1 : 0);
}

float MediaBrowser::SidebarWidth(float screenW) const {
    // The rail: wide enough for the longest row as Sidebar() draws it (names in the Row size,
    // counts in the Small size), at least 240 dp and at most 40% of the screen.
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::vector<Source>& sources = library_.Sources();
    const int rows = SourceRows();
    std::vector<float> names((size_t)rows + 1), counts((size_t)rows + 1, 0.0f);
    {
        FontScope row(TextStyle::Row);
        names[(size_t)rows] = ImGui::CalcTextSize("Other files...").x;
        for (int r = 0; r < rows; ++r) names[(size_t)r] = ImGui::CalcTextSize(sources[(size_t)r].name.c_str()).x;
    }
    {
        FontScope small(TextStyle::Small);
        for (int r = 0; r < rows; ++r) {
            char count[16];
            std::snprintf(count, sizeof(count), "%d", sources[(size_t)r].count);
            counts[(size_t)r] = ImGui::CalcTextSize(count).x;
        }
    }
    float widest = 0.0f;
    for (size_t r = 0; r < names.size(); ++r)
        widest = std::max(widest, names[r] + (counts[r] > 0.0f ? Dp(16.0f) + counts[r] : 0.0f));
    const float content = style.WindowPadding.x * 2.0f + ImGui::GetFrameHeight() + style.ItemSpacing.x + widest;
    return Round(RailWidth(content, screenW, Dp(240.0f), 0.4f));
}

const Source& MediaBrowser::CurrentSource() const {
    static const Source kNone;
    const std::vector<Source>& s = library_.Sources();
    return source_ < s.size() ? s[source_] : (s.empty() ? kNone : s.front());
}

MediaBrowser::Action MediaBrowser::Build() {
    if (!provider_) return Action::None;
    TakeResults();
    if (!startSource_.empty() && listed_) {
        const std::vector<Source>& s = library_.Sources();
        bool found = false;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i].name != startSource_) continue;
            source_ = i;
            folder_ = s[i].folder;
            // A folder kept from the last launch: only one inside this source's top.
            if (!startFolder_.empty() && startFolder_.compare(0, folder_.size(), folder_) == 0) folder_ = startFolder_;
            dirty_ = true;
            found = true;
        }
        // Else a pinned folder by its name (desktop tests: a pinned row lit).
        for (size_t j = 0; !found && j < pinnedRows_.size(); ++j) {
            if (pinnedRows_[j].name != startSource_) continue;
            source_ = pinnedSource_;
            folder_ = pinnedRows_[j].path;
            dirty_ = true;
            found = true;
        }
        startSource_.clear();
        startFolder_.clear();
    }
    if (desktop_) TrackHistory();
    if (dirty_) {
        // A slow rebuild stalls the whole player (it runs on the render thread): say so.
        const auto t0 = std::chrono::steady_clock::now();
        Rebuild();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 50.0)
            LOG_WARN("browser: rebuild took %.0f ms (%zu folders, %zu items, %zu unknown)", ms, folders_.size(),
                     shown_.size(), unknown_.size());
    }
    cache_.NextFrame();

    g_dpUnits = desktop_ ? 1.0f : 1.5f;
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const double now = ImGui::GetTime();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float bar = Round(ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f);
    const float chips = Round(Dp(56.0f));
    // Landscape and wide: the sources are a rail that stays; anywhere else a drawer (#180).
    rail_ = desktop_ ? w / Dp(1.0f) >= kDesktopRailMinDp : UseRail(w / Dp(1.0f), h / Dp(1.0f));
    if (rail_) drawer_ = false;
    const float sidebarW = SidebarWidth(w);
    std::vector<StereoTile> tiles = std::move(stereoFrame_.tiles);  // keep the capacity
    tiles.clear();
    stereoFrame_ = StereoFrame{};
    stereoFrame_.tiles = std::move(tiles);
    stereoFrame_.active = stereo_;  // even with no tiles: the caller then clears behind the HUD
    const float bg[3] = {kBackground.x, kBackground.y, kBackground.z};
    std::copy(bg, bg + 3, stereoFrame_.background);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // The 3D grid: the projection layer behind the HUD draws the background and the tiles.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, stereo_ ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f) : kBackground);
    const bool open = ImGui::Begin("##browser", nullptr,
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    Action action = Action::None;
    if (open && desktop_) {
        // The desktop look (#252): a header, a toolbar over the grid, a compact sidebar.
        ImGui::PushFont(ui::DeskFont(ui::DeskText::Body));
        const float headerH = Round(Px(44.0f)), toolbarH = Round(Px(40.0f));
        // The status bar along the foot: not in a short window.
        const float statusH = h >= Px(420.0f) ? Round(Px(24.0f)) : 0.0f;
        rail_ = w >= Px(kDesktopRailMinDp);
        if (rail_) drawer_ = false;
        const float sideW = rail_ ? Round(std::min(Px(220.0f), w * 0.34f)) : 0.0f;
        action = Grid(ImVec2(sideW, headerH + toolbarH), ImVec2(w, h - statusH), now);
        DeskToolbar(ImVec2(sideW, headerH), ImVec2(w, headerH + toolbarH));
        if (rail_) {
            const Action picked = DeskSidebar(ImVec2(0.0f, headerH), ImVec2(sideW, h - statusH));
            if (picked != Action::None) action = picked;
        }
        if (statusH > 0.0f) DeskStatusBar(ImVec2(0.0f, h - statusH), ImVec2(w, h));
        DeskHeader(w, headerH);
        DeskSortMenu();
        closeSheets_ = false;
        sheetOpen_ = ImGui::IsPopupOpen("##sort_menu") || ImGui::IsPopupOpen("##folder_menu") ||
                     ImGui::IsPopupOpen("##pinned_menu");
        ImGui::PopFont();
    } else if (open) {
        const bool railShown = rail_ && sidebar_;
        const float left = railShown ? sidebarW : 0.0f;
        const float top = ui::g_systemBarTop + bar, bottom = h - ui::g_systemBarBottom;
        const bool chipRow = provider_->Access() == MediaAccess::Full || provider_->Access() == MediaAccess::Partial;
        action = Grid(ImVec2(left, chipRow ? top + chips : top), ImVec2(w, bottom), now);
        if (chipRow) ChipRow(ImVec2(left, top), ImVec2(w, top + chips));
        if (railShown) {
            const Action picked = Sidebar(ImVec2(0.0f, top), ImVec2(sidebarW, bottom), now);
            if (picked != Action::None) action = picked;
        }
        TopBar(w, bar);
        ViewOptionsSheet(w, top, bottom);
        TypeMenu();
        closeSheets_ = false;
        sheetOpen_ = ImGui::IsPopupOpen("##view_options") || ImGui::IsPopupOpen("##type_menu");
        // The sheet sits over a dimmed grid.
        if (ImGui::IsPopupOpen("##view_options")) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, kScrim);
    }
    ImGui::End();
    if (drawer_) {
        const Action picked = Drawer(w, h, now);
        if (picked != Action::None) action = picked;
    }
    if (closeRequested_ && action == Action::None) action = Action::Closed;
    closeRequested_ = false;
    // A slideshow asked for this frame (#252), unless something else won it.
    if (playlistPending_) {
        if (action == Action::None) action = Action::Slideshow;
        else if (action != Action::Slideshow) playlistPending_ = false;
    }
    // The app may be stopped while playing.
    if (action == Action::Opened || action == Action::Slideshow) classifier_.Save();
    if (action != Action::None && !wanted_.empty()) {
        // The grid is going away: stop the thumbnail work, so it does not compete with
        // opening the file and decoding its neighbours. The grid asks again when it shows.
        wanted_.clear();
        provider_->WantThumbnails(wanted_);
    }
    return action;
}

void MediaBrowser::TopBar(float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float icon = ImGui::GetFrameHeight();
    const float top = ui::g_systemBarTop;  // the bar sits under the status bar
    const float y = top + Round((height - icon) * 0.5f);
    const float pad = Round(style.WindowPadding.x);
    const float gap = Round(style.ItemSpacing.x);
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(width, top + height), kPanel);
    if (selecting_) {
        // As the stock tablet player's: a cross, the count, select all, and the bin.
        if (IconButton("##cancel", Glyph::Close, ImVec2(pad, y), icon)) EndSelection();
        char count[32];
        std::snprintf(count, sizeof(count), "%d selected", (int)selected_.size());
        {
            FontScope title(TextStyle::Title);
            dl->AddText(ImVec2(pad + icon + gap, top + Round((height - ImGui::GetFontSize()) * 0.5f)),
                        ImGui::GetColorU32(ImGuiCol_Text), count);
        }
        // Select all the files shown (not the folders), or none once they all are.
        const std::vector<MediaItem>& items = library_.Items();
        size_t picked = 0;
        for (size_t i : shown_) picked += selected_.count(items[i].id);
        const bool all = !shown_.empty() && picked == shown_.size();
        const char* label = all ? "Select none" : "Select all";
        FontScope row(TextStyle::Row);
        const float bx = width - pad - icon - gap - TextButtonWidth(label);
        if (TextButton("##select_all", label, ImVec2(bx, y), icon)) {
            if (all) {
                selected_.clear();
                selecting_ = false;
            } else {
                for (size_t i : shown_) selected_.insert(items[i].id);
            }
        }
        if (IconButton("##delete", Glyph::Delete, ImVec2(width - pad - icon, y), icon))
            provider_->Delete(std::vector<int64_t>(selected_.begin(), selected_.end()));
        return;
    }
    if (IconButton("##menu", Glyph::Menu, ImVec2(pad, y), icon)) {
        if (rail_) {
            sidebar_ = !sidebar_;
        } else {
            drawer_ = true;
        }
    }
    // Desktop: a close button at the far right, back to the player (#252).
    if (desktop_ && IconButton("##close", Glyph::Close, ImVec2(width - pad - icon, y), icon)) closeRequested_ = true;
    const float more = width - pad - icon - (desktop_ ? icon + gap : 0.0f);
    if (IconButton("##options", Glyph::More, ImVec2(more, y), icon)) ImGui::OpenPopup("##view_options");
    // An accent dot on the menu when an option in it is not as it starts.
    if (!ViewOptionsAreDefault(options_, showUnsupported_))
        dl->AddCircleFilled(ImVec2(more + icon * 0.80f, y + icon * 0.22f), Round(Dp(4.0f)), kAccent);
    float x = pad + icon + gap;
    if (folder_ != CurrentSource().folder) {
        if (IconButton("##up", Glyph::Back, ImVec2(x, y), icon)) Up();
        x += icon + gap;
    }

    // The current source and the folder in it, "Downloads / MEDIA / ZTEPics": plain text,
    // left-aligned, not a pill that looks like a button and does nothing. The sources are
    // the menu's.
    std::string title = CurrentSource().name;
    for (const std::string& part : library_.Breadcrumb(CurrentSource(), folder_)) title += " / " + part;
    FontScope font(TextStyle::Title);
    const std::string name = ui::FitText(title, more - gap - x);
    dl->AddText(ImVec2(x, top + Round((height - ImGui::GetFontSize()) * 0.5f)), ImGui::GetColorU32(ImGuiCol_Text),
                name.c_str());
}

// The filters, up front (#180): a 3D / 2D segmented control and the type chip.
void MediaBrowser::ChipRow(ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    FontScope font(TextStyle::Row);
    const float pad = Round(ImGui::GetStyle().WindowPadding.x);
    const float h = Round(Dp(36.0f)), hit = Round(Dp(48.0f));
    const float y = Round((min.y + max.y - hit) * 0.5f), y0 = y + Round((hit - h) * 0.5f);
    const float stroke = std::max(Px(2.0f), ui::g_iconStroke);
    const float r = h * 0.5f;
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 outline = IM_COL32(255, 255, 255, 110);

    // All · 3D · 2D
    const char* labels[] = {"All", "3D", "2D"};
    const DepthFilter values[] = {DepthFilter::Both, DepthFilter::Only3D, DepthFilter::Only2D};
    float segW = 0.0f;
    for (const char* l : labels) segW = std::max(segW, ImGui::CalcTextSize(l).x);
    segW = Round(segW + Dp(20.0f) * 2.0f);
    float x = min.x + pad;
    const ImVec2 a(x, y0), b(x + segW * 3.0f, y0 + h);
    for (int i = 0; i < 3; ++i) {
        const ImVec2 s0(x + segW * (float)i, y0), s1(s0.x + segW, y0 + h);
        ImGui::SetCursorScreenPos(ImVec2(s0.x, y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##depth", ImVec2(segW, hit))) SetDepth(values[i]);
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();
        const bool on = depth_ == values[i];
        const ImDrawFlags corners =
            i == 0 ? ImDrawFlags_RoundCornersLeft : i == 2 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
        if (on) dl->AddRectFilled(s0, s1, kSelected, r, corners);
        if (held) dl->AddRectFilled(s0, s1, kPressed, r, corners);
        const float tw = ImGui::CalcTextSize(labels[i]).x;
        dl->AddText(ImVec2(Round(s0.x + (segW - tw) * 0.5f), Round(y0 + (h - ImGui::GetFontSize()) * 0.5f)),
                    on ? kAccent : text, labels[i]);
        if (i > 0) dl->AddLine(ImVec2(s0.x, y0), ImVec2(s0.x, y0 + h), outline, stroke);
    }
    dl->AddRect(a, b, outline, r, 0, stroke);

    // The type chip: what it shows, and a chevron for its menu.
    x = b.x + Round(Dp(12.0f));
    const char* type = TypeLabel(options_.type);
    const float chev = Round(Dp(10.0f));
    const float chipW = Round(ImGui::CalcTextSize(type).x + Dp(16.0f) * 2.0f + chev + Dp(8.0f));
    if (x + chipW <= max.x - pad) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ImGui::InvisibleButton("##type_chip", ImVec2(chipW, hit))) {
            typeChip_ = ImVec2(x, y0 + h + Dp(4.0f));
            ImGui::OpenPopup("##type_menu");
        }
        const bool held = ImGui::IsItemActive();
        const bool on = options_.type != TypeFilter::All;
        const ImVec2 c0(x, y0), c1(x + chipW, y0 + h);
        if (on) dl->AddRectFilled(c0, c1, kSelected, r);
        if (held) dl->AddRectFilled(c0, c1, kPressed, r);
        dl->AddRect(c0, c1, outline, r, 0, stroke);
        const ImU32 col = on ? kAccent : text;
        dl->AddText(ImVec2(x + Round(Dp(16.0f)), Round(y0 + (h - ImGui::GetFontSize()) * 0.5f)), col, type);
        const float cx = x + chipW - Round(Dp(16.0f)) - chev * 0.5f, cy = y0 + h * 0.5f;
        const ImVec2 v[] = {ImVec2(cx - chev * 0.5f, cy - chev * 0.25f), ImVec2(cx, cy + chev * 0.25f),
                            ImVec2(cx + chev * 0.5f, cy - chev * 0.25f)};
        dl->AddPolyline(v, 3, col, 0, stroke);
    }
}

void MediaBrowser::SetDepth(DepthFilter depth) {
    if (depth == depth_) return;
    depth_ = depth;
    dirty_ = true;
    gridScroll_.SetOffset(0.0f);
}

void MediaBrowser::SetType(TypeFilter type) {
    if (type == options_.type) return;
    options_.type = type;
    dirty_ = true;
    gridScroll_.SetOffset(0.0f);
}

// The type chip's menu: three 48 dp rows under the chip.
void MediaBrowser::TypeMenu() {
    ImGui::SetNextWindowPos(typeChip_, ImGuiCond_Always);
    if (!ImGui::BeginPopup("##type_menu")) return;
    if (closeSheets_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    {
        // Popped before EndPopup: ImGui asserts on a font still pushed at End().
        FontScope font(TextStyle::Row);
        const TypeFilter types[] = {TypeFilter::All, TypeFilter::Videos, TypeFilter::Images};
        const float w = Round(ImGui::CalcTextSize("All types").x + Dp(48.0f) * 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        for (TypeFilter t : types) {
            if (ImGui::Selectable(TypeLabel(t), options_.type == t, 0, ImVec2(w, Dp(48.0f)))) SetType(t);
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndPopup();
}

// The sources as a modal drawer over a dimmed screen: a pick, a tap on the scrim or Back
// closes it.
MediaBrowser::Action MediaBrowser::Drawer(float width, float height, double now) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    const bool open = ImGui::Begin("##drawer", nullptr,
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    Action action = Action::None;
    if (open) {
        const float w = Round(std::min(Dp(280.0f), width * 0.85f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(w, 0.0f), ImVec2(width, height), kScrim);
        // The scrim, first so the panel's rows win the press.
        ImGui::SetCursorScreenPos(ImVec2(w, 0.0f));
        if (width > w && ImGui::InvisibleButton("##scrim", ImVec2(width - w, height))) drawer_ = false;
        dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(w, height), kPanel);
        sourcePicked_ = false;
        if (desktop_) {
            ImGui::PushFont(ui::DeskFont(ui::DeskText::Body));
            action = DeskSidebar(ImVec2(0.0f, 0.0f), ImVec2(w, height));
            ImGui::PopFont();
        } else {
            action = Sidebar(ImVec2(0.0f, ui::g_systemBarTop + Dp(8.0f)), ImVec2(w, height - ui::g_systemBarBottom), now);
        }
        if (action != Action::None || sourcePicked_) drawer_ = false;
    }
    ImGui::End();
    return action;
}

MediaBrowser::Action MediaBrowser::Sidebar(ImVec2 min, ImVec2 max, double now) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::vector<Source>& sources = library_.Sources();
    const ImVec2 size(max.x - min.x, max.y - min.y);
    dl->AddRectFilled(min, max, kPanel);
    if (size.x <= 0.0f || size.y <= 0.0f) return Action::None;
    FontScope font(TextStyle::Row);

    const float rowH = Round(std::max(Dp(52.0f), ImGui::GetFrameHeight() + style.ItemSpacing.y));
    const float gap = Round(style.ItemSpacing.y);  // around the line above "Other files..."
    const int files = SourceRows();                // the row of "Other files..."
    const int last = files + (desktop_ ? 1 : 0);   // desktop: then "Add folder..."
    const float top = min.y + gap;
    sidebarScroll_.SetSlop(Px(kSlop));
    sidebarScroll_.SetLimit((last + 1) * rowH + gap * 4.0f - size.y);
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##sidebar", size);
    const bool sidebarHovered = ImGui::IsItemHovered();
    const bool tap = Track(sidebarScroll_, sidebarPress_, now);
    if (desktop_ && sidebarHovered && ImGui::GetIO().MouseWheel != 0.0f)
        sidebarScroll_.SetOffset(sidebarScroll_.Offset() - ImGui::GetIO().MouseWheel * Dp(52.0f));
    const float offset = Round(sidebarScroll_.Offset());

    auto rowTop = [&](int r) { return top + r * rowH + (r >= files ? gap * 2.0f : 0.0f) - offset; };
    auto rowAt = [&](ImVec2 p) {
        for (int r = 0; r <= last; ++r) {
            if (p.y >= rowTop(r) && p.y < rowTop(r) + rowH) return r;
        }
        return -1;
    };
    const int pressed = sidebarScroll_.Pressed() && !sidebarScroll_.Dragging() ? rowAt(sidebarPress_) : -1;

    const float icon = ImGui::GetFrameHeight();
    const float pad = Round(style.WindowPadding.x);
    const float textX = min.x + pad + icon + Round(style.ItemSpacing.x);
    const float right = max.x - pad;
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->PushClipRect(min, max, true);
    for (int r = 0; r <= last; ++r) {
        const float y = rowTop(r);
        if (r == files) {
            const float line = y - gap;
            dl->AddLine(ImVec2(min.x + pad, line), ImVec2(right, line), dim, Round(Dp(1.0f)));
        }
        if (y + rowH < min.y || y > max.y) continue;
        const ImVec2 a(min.x, y), b(max.x, y + rowH);
        const Source* s = r < files ? &sources[r] : nullptr;
        if (s && (size_t)r == source_) dl->AddRectFilled(a, b, kSelected);
        if (r == pressed) dl->AddRectFilled(a, b, kPressed);
        ui::DrawGlyph(dl, s ? SourceIcon(s->kind) : r == files ? Glyph::Files : Glyph::Pin,
                      ImVec2(min.x + pad + icon * 0.5f, y + rowH * 0.5f), icon, text, s && (size_t)r == source_);
        const float textY = y + Round((rowH - ImGui::GetFontSize()) * 0.5f);
        if (!s) {
            const char* label = r == files ? "Other files..." : "Add folder...";
            dl->AddText(ImVec2(textX, textY), text, ui::FitText(label, right - textX).c_str());
            continue;
        }
        char count[16];
        std::snprintf(count, sizeof(count), "%d", s->count);
        // The label up to a gap before the count, cut with an ellipsis there: never over it.
        SourceRowLayout row;
        {
            FontScope small(TextStyle::Small);
            row = SourceRow(textX, right, Round(ImGui::CalcTextSize(count).x), Round(Dp(16.0f)));
            dl->AddText(ImVec2(row.countX, y + Round((rowH - ImGui::GetFontSize()) * 0.5f)), dim, count);
        }
        dl->AddText(ImVec2(textX, textY), text, ui::FitText(s->name, row.labelMax).c_str());
    }
    dl->PopClipRect();

    const int r = tap ? rowAt(sidebarPress_) : -1;
    if (r < 0) return Action::None;
    if (r == files) {
        openedId_ = -1;
        return Action::OtherFiles;
    }
    if (r > files) return Action::AddFolder;
    source_ = (size_t)r;
    folder_ = sources[r].folder;
    EndSelection();
    sourcePicked_ = true;  // closes the drawer; the rail stays (#180)
    dirty_ = true;
    gridScroll_.SetOffset(0.0f);
    return Action::None;
}

MediaBrowser::Action MediaBrowser::Grid(ImVec2 min, ImVec2 max, double now) {
    statusItem_ = -1;
    statusFolder_ = SIZE_MAX;
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const MediaAccess access = provider_->Access();
    if (access == MediaAccess::None) {
        const Glyph icon = Glyph::Image;
        if (Message(min, max, &icon, "Allow access to your photos and videos to see them here.", "Allow access"))
            provider_->RequestAccess();
        return Action::None;
    }
    if (access == MediaAccess::Denied) {
        const Glyph icon = Glyph::Unsupported;
        if (Message(min, max, &icon, "Photo and video access is off. Turn it on in Settings to see them here.",
                    "Open Settings"))
            provider_->RequestAccess();
        return Action::None;
    }
    if (access == MediaAccess::Unknown) return Action::None;
    if (access == MediaAccess::Partial) {
        const char* kSelectMore = "Select more";
        const float h = Round(std::max(Dp(56.0f), ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f));
        const float pad = Round(style.WindowPadding.x);
        const float buttonX = max.x - pad - TextButtonWidth(kSelectMore);
        dl->AddRectFilled(min, ImVec2(max.x, min.y + h), kPanel);
        dl->AddText(ImVec2(min.x + pad, min.y + Round((h - ImGui::GetFontSize()) * 0.5f)),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    ui::FitText("Showing only the items you picked.", buttonX - Round(style.ItemSpacing.x) - min.x - pad).c_str());
        if (TextButton("##select_more", kSelectMore, ImVec2(buttonX, min.y + Round((h - Dp(48.0f)) * 0.5f)), Dp(48.0f)))
            provider_->RequestAccess();
        min.y += h;
    }
    if (!listed_) {
        Message(min, max, nullptr, "Loading...", nullptr);
        return Action::None;
    }
    // The provider's notice (desktop: the Search index is off), as a strip over the grid.
    const std::string notice = provider_->Notice();
    if (!notice.empty()) {
        const float h = Round(std::max(Dp(40.0f), ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f));
        const float pad = Round(style.WindowPadding.x);
        dl->AddRectFilled(min, ImVec2(max.x, min.y + h), kPanel);
        dl->AddText(ImVec2(min.x + pad, min.y + Round((h - ImGui::GetFontSize()) * 0.5f)), kDim,
                    ui::FitText(notice, max.x - min.x - pad * 2.0f).c_str());
        min.y += h;
    }

    // Square tiles, about one column per 180 dp of the grid's width (#180): beside the rail
    // that is one fewer. The drawer lies over the grid and leaves it as it is. Desktop
    // (#252): cards about 176 px wide, a picture with a caption under it.
    const bool desk = desktop_;
    const bool list = desk && details_;  // the details view: a row per file, under column headings
    const float colH = list ? Round(Px(32.0f)) : 0.0f;
    const ImVec2 listTop = min;
    if (desk) min.y += list ? colH : Round(Px(8.0f));
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const float deskPad = desk ? Round(Px(4.0f)) : 0.0f;
    const float usableW = size.x - (desk ? Round(Px(12.0f)) * 2.0f : 0.0f);
    const float gutter = list ? 0.0f : Round(desk ? Px(stereo_ ? 16.0f : 8.0f) : Px(stereo_ ? kStereoGutter : kGutter));
    // Desktop cards: the size slider's width, eased so a Ctrl+wheel notch glides. A card keeps
    // that width (the rows end short of the right edge, as File Explorer's): stretched to fill
    // the row, the cards only changed size when a column came or went.
    const float cardTarget = std::min(kCardMax, std::max(kCardMin, cardW_));
    if (cardShown_ <= 0.0f || std::fabs(cardShown_ - cardTarget) < 0.5f) {
        cardShown_ = cardTarget;
    } else {
        const float k = std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
        cardShown_ += (cardTarget - cardShown_) * k;
    }
    const float cardW = Round(Px(cardShown_));
    const int cols = list   ? 1
                     : desk ? std::max(2, (int)((usableW + gutter) / (cardW + gutter)))
                            : GridColumns(size.x / Dp(1.0f));
    const float tile = desk && !list ? std::min(cardW, std::floor((usableW - gutter * (cols - 1)) / cols))
                                     : std::floor((usableW - gutter * (cols - 1)) / cols);
    if (tile <= deskPad * 2.0f || size.y <= 0.0f) return Action::None;
    // The picture's edge: the tile's on touch, inside the card on desktop, 40 px in a row.
    const float pic = list ? Round(Px(40.0f)) : tile - deskPad * 2.0f;
    const float tileH = list ? Round(Px(48.0f)) : desk ? deskPad + pic + DeskCaptionHeight() : tile;
    const float step = tileH + gutter;  // the row pitch
    // Centred on touch; on desktop from the left, so the cards do not slide sideways as they
    // grow.
    const float x0 = desk ? min.x + Round(Px(12.0f))
                          : min.x + Round((size.x - (cols * (tile + gutter) - gutter)) * 0.5f);
    DeskColumns columns;
    if (list) {
        columns = DeskColumnLayout(x0, tile);
        DeskColumnHeader(columns, listTop, ImVec2(max.x, listTop.y + colH));
    }
    const size_t nf = folders_.size();
    const size_t n = nf + shown_.size();  // tiles: the folders, then the items
    // The tiles in sections: the date sections, or one with the folders then the items.
    const bool headers = !sectionSizes_.empty();
    const float headerH = Round(desk ? Px(30.0f) : Dp(44.0f));
    const size_t topTile = layout_.TileAtOffset(gridScroll_.Offset());  // before the rebuild
    // How far into its row the view's top was, as a share of the row: kept on a resize.
    const float into = lastStep_ > 0.0f ? (gridScroll_.Offset() - layout_.OffsetOf(topTile)) / lastStep_ : 0.0f;
    layout_.Build(headers ? sectionSizes_ : std::vector<size_t>{n}, headers, cols, tile, gutter, headerH,
                  desk ? tileH : -1.0f);
    if (cols != columns_ && columns_ > 0) {
        // A reflow (rotation, the rail): keep the top row's first item in the top row.
        gridScroll_.SetLimit(layout_.Height() - size.y);
        gridScroll_.SetOffset(layout_.OffsetOf(topTile) + (desk ? std::max(0.0f, into) * step : 0.0f));
    } else if (desk && lastStep_ > 0.0f && step != lastStep_ && gridScroll_.Offset() > 0.0f) {
        // The cards grew or shrank: the same item stays at the top of the view.
        gridScroll_.SetLimit(layout_.Height() - size.y);
        gridScroll_.SetOffset(layout_.OffsetOf(topTile) + std::max(0.0f, into) * step);
    }
    columns_ = cols;
    lastStep_ = step;
    gridScroll_.SetSlop(Px(kSlop));
    gridScroll_.SetLimit(layout_.Height() - size.y);
    const float limit = std::max(0.0f, layout_.Height() - size.y);

    // A fast-scroll thumb on the right edge of a long grid, submitted before the grid so it
    // wins the press.
    const bool fast = desk ? limit > 0.0f : n > kFastScrollItems && limit > 0.0f;
    const float trackTop = min.y + Dp(8.0f), trackH = size.y - Dp(16.0f);
    const float thumbH = desk ? Round(std::max(Px(32.0f), trackH * size.y / std::max(1.0f, layout_.Height())))
                              : Round(Dp(48.0f));
    bool thumbHeld = false;
    if (fast && n > 0) {
        const float frac = std::min(1.0f, std::max(0.0f, gridScroll_.Offset() / limit));
        const float ty = trackTop + frac * (trackH - thumbH);
        const float hitW = Round(desk ? Px(14.0f) : Dp(40.0f));
        ImGui::SetCursorScreenPos(ImVec2(max.x - hitW, ty));
        ImGui::InvisibleButton("##fast_scroll", ImVec2(hitW, thumbH));
        const float mouseY = ImGui::GetIO().MousePos.y;
        if (ImGui::IsItemActivated()) thumbGrab_ = mouseY - ty;
        thumbHeld = ImGui::IsItemActive();
        if (thumbHeld && trackH > thumbH) {
            const float f = (mouseY - thumbGrab_ - trackTop) / (trackH - thumbH);
            gridScroll_.SetOffset(std::min(1.0f, std::max(0.0f, f)) * limit);
        }
    }
    bool tap = false;
    bool gridHovered = false;
    if (n > 0) {
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##grid", size);
        gridHovered = ImGui::IsItemHovered();
        tap = Track(gridScroll_, gridPress_, now);
        // Desktop: the wheel scrolls by about half a row a notch.
        if (desktop_ && gridHovered && ImGui::GetIO().MouseWheel != 0.0f) {
            if (ImGui::GetIO().KeyCtrl) {
                // Ctrl+wheel: the cards' size, as the toolbar's slider (the grid only).
                if (!list)
                    cardW_ = std::min(kCardMax, std::max(kCardMin, cardW_ * (1.0f + 0.1f * ImGui::GetIO().MouseWheel)));
            } else {
                gridScroll_.SetOffset(gridScroll_.Offset() - ImGui::GetIO().MouseWheel * (list ? step * 3.0f : step * 0.5f));
            }
        }
    }
    const float offset = Round(gridScroll_.Offset());

    // The visible rows, and a few more either side as far as the slots go.
    size_t visFirst = 0, visEnd = 0;
    layout_.Range(offset, offset + size.y, visFirst, visEnd);
    const int visRows = (int)((size.y + step - 1.0f) / step) + 1;
    const int perTile = WantsStereo() ? 2 : 1;  // the 3D grid keeps each eye in a slot
    const int spare = (int)atlas_.SlotCount() - visRows * cols * perTile;
    const int extra = std::max(0, std::min(2, spare / (2 * cols * perTile)));
    size_t winFirst = 0, winEnd = 0;
    layout_.Range(offset - (float)extra * step, offset + size.y + (float)extra * step, winFirst, winEnd);
    winFirst = std::min(winFirst, visFirst);
    winEnd = std::max(winEnd, visEnd);

    const std::vector<MediaItem>& items = library_.Items();
    std::unordered_set<int64_t> window;
    for (size_t p = std::max(winFirst, nf); p < winEnd; ++p) {
        const int64_t id = items[shown_[p - nf]].id;
        window.insert(id);
        int slot = 0;
        cache_.Use(id, slot);  // so a new thumbnail never takes a slot the window still shows
        if (perTile > 1) cache_.Use(RightEyeKey(id), slot);
    }
    // Desktop grid: the visible folder cards preview what they hold, 2 x 2, from the slots the
    // files' window leaves: four each while they fit, else one each, else none (the glyph).
    // Not in the 3D grid, whose files take two slots each.
    size_t previewEach = 0;
    const size_t visFolders = visFirst < nf ? std::min(visEnd, nf) - visFirst : 0;
    if (desk && !list && perTile == 1 && visFolders > 0 && folderPreviews_.size() == nf) {
        const size_t used = winEnd > std::max(winFirst, nf) ? winEnd - std::max(winFirst, nf) : 0;
        const size_t spare = atlas_.SlotCount() > used ? atlas_.SlotCount() - used : 0;
        previewEach = spare >= 4 * visFolders ? 4 : spare >= visFolders ? 1 : 0;
    }
    for (size_t p = visFirst; p < visFirst + visFolders && previewEach > 0; ++p) {
        for (size_t k = 0; k < folderPreviews_[p].size() && k < previewEach; ++k) {
            const int64_t id = items[folderPreviews_[p][k]].id;
            window.insert(id);
            int slot = 0;
            cache_.Use(id, slot);
        }
    }
    // A thumbnail big enough that the part a tile shows covers it pixel for pixel, as far as
    // an atlas slot holds.
    // On desktop in steps of 128 px: the card size eases through every width, and a new edge
    // each frame would ask the provider for the whole window again each frame.
    const uint32_t picPx = desk ? (uint32_t)(std::ceil(pic / 128.0f) * 128.0f) : (uint32_t)pic;
    const uint32_t need = std::min(atlas_.Tile(), picPx);
    UploadReady(window, need);

    // Ask for what the window lacks: the visible rows, then those below, then above. Under a
    // depth filter also for the next few items with no verdict, so they can be sorted in.
    // Also for the ones shown soft, sharper (UploadReady). The size goes by the verdict known
    // now: an item found 3D by its thumbnail is asked for again if its eye came out soft.
    std::vector<ThumbnailRequest> want;
    auto ask = [&](const MediaItem& item) {
        if (pending_.count(item.id) || noThumbnail_.count(item.id)) return;
        uint32_t edge = 0;
        if (cache_.Contains(item.id)) {
            const auto it = sharpen_.find(item.id);
            if (it == sharpen_.end()) return;
            edge = it->second;
        } else {
            // Photos keep the system's cached thumbnail at one slot, as before #223: they were
            // sharp enough, and decoding the file for each tile takes CPU that opening one needs.
            edge = item.type == MediaType::Video
                       ? ThumbnailEdge(item.width, item.height, classifier_.Get(item).layout, need, atlas_.Tile(), kFetchCap)
                       : atlas_.Tile();
        }
        ThumbnailRequest r{item.id, classifier_.NeedsFileCheck(item), edge};
        // The 3D grid: the file too, for a one-view 3D photo whose pair is not made yet.
        const StereoVerdict v = classifier_.Get(item);
        r.pairFile = WantsStereo() && NeedsPair(item, v) && !pairs_.Busy(item.id) && !pairs_.Has(item.id, item.modified);
        want.push_back(r);
    };
    for (size_t p = std::max(visFirst, nf); p < winEnd; ++p) ask(items[shown_[p - nf]]);
    for (size_t p = visFirst; p < visFirst + visFolders && previewEach > 0; ++p)
        for (size_t k = 0; k < folderPreviews_[p].size() && k < previewEach; ++k) ask(items[folderPreviews_[p][k]]);
    for (size_t p = visFirst; p > std::max(winFirst, nf); --p) ask(items[shown_[p - 1 - nf]]);
    for (size_t k = 0; k < unknown_.size() && k < kScanBatch; ++k) ask(items[unknown_[k]]);
    if (!Same(want, wanted_)) {
        provider_->WantThumbnails(want);
        wanted_ = std::move(want);
    }

    if (n == 0) {
        const char* text = "No photos or videos here.";
        if (!unknown_.empty()) {
            text = depth_ == DepthFilter::Only3D ? "Looking for 3D photos and videos..."
                                                 : "Looking for 2D photos and videos...";
        } else if (depth_ != DepthFilter::Both || options_.type != TypeFilter::All) {
            text = "Nothing here matches the view options.";
        }
        const Glyph icon = unknown_.empty() ? Glyph::Image : Glyph::AllMedia;
        Message(min, max, &icon, text, nullptr);
        return Action::None;
    }

    // The item under a point, false over a gutter, a header or past the end.
    auto itemAt = [&](ImVec2 pt, size_t& pos) {
        if (pt.y < min.y) return false;
        return layout_.TileAt(pt.x - x0, pt.y - min.y + offset, pos);
    };
    size_t pressed = n;
    if (gridScroll_.Pressed() && !gridScroll_.Dragging() && !itemAt(gridPress_, pressed)) pressed = n;
    // Desktop: the tile under the pointer, lightly lit.
    size_t hovered = n;
    if (desktop_ && gridHovered && !gridScroll_.Pressed() && !itemAt(ImGui::GetIO().MousePos, hovered)) hovered = n;
    // Desktop keyboard: arrows, Page Up / Down, Home / End move a focus ring; Enter opens.
    // The first key shows the ring on the first visible tile. The view follows the ring.
    bool keyOpen = false;
    if (desktop_ && !sheetOpen_ && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl) {
        const int page = std::max(1, (int)(size.y / step)) * cols;
        long move = 0;
        bool jump = false;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = cols;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -cols;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move = page;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move = -page;
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) jump = true, move = -(long)n;
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) jump = true, move = (long)n;
        if (move != 0) {
            if (cursor_ >= n) {
                cursor_ = jump ? (move < 0 ? 0 : n - 1) : std::min(n - 1, visFirst);
            } else {
                cursor_ = (size_t)std::max(0L, std::min((long)n - 1, (long)cursor_ + move));
            }
            float tx = 0.0f, ty = 0.0f;
            if (layout_.TilePos(cursor_, tx, ty)) {
                if (ty < offset) gridScroll_.SetOffset(ty);
                else if (ty + tileH > offset + size.y) gridScroll_.SetOffset(ty + tileH - size.y);
            }
        }
        if (move != 0) focusId_ = -1;  // the user moves the ring: no FocusItem() to come
        keyOpen = cursor_ < n && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        // Shift+Enter: a folder's slideshow, or the grid's from this file (#252).
        if (keyOpen && slideshows_ && ImGui::GetIO().KeyShift) {
            keyOpen = false;
            if (cursor_ < nf) AskSlideshow(folders_[cursor_].path, false);
            else AskSlideshowFrom(items[shown_[cursor_ - nf]].id);
        }
    }
    // Ctrl+Enter: the place shown as a slideshow, as the header's play button (#252).
    if (desktop_ && slideshows_ && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) &&
        CanPlayHere())
        AskSlideshow(folder_, true);
    // Back from a slideshow (FocusItem): the ring on the file it ended on, in view.
    if (focusId_ >= 0 && !dirty_) {
        for (size_t p = 0; p < shown_.size(); ++p) {
            if (items[shown_[p]].id != focusId_) continue;
            cursor_ = nf + p;
            focusId_ = -1;
            float tx = 0.0f, ty = 0.0f;
            if (layout_.TilePos(cursor_, tx, ty)) {
                if (ty < offset) gridScroll_.SetOffset(ty);
                else if (ty + tileH > offset + size.y) gridScroll_.SetOffset(ty + tileH - size.y);
            }
            break;
        }
    }
    // Tests (SetStartSlideshow): play a tile once the grid holds it.
    if (startSlideshow_ != SIZE_MAX && slideshows_ && startSlideshow_ < n && !dirty_) {
        if (startSlideshow_ < nf) AskSlideshow(folders_[startSlideshow_].path, false);
        else AskSlideshowFrom(items[shown_[startSlideshow_ - nf]].id);
        startSlideshow_ = SIZE_MAX;
    }
    if (desktop_ && cursor_ >= n && startFocus_ < n) cursor_ = startFocus_;
    if (desktop_ && !startSelection_.empty() && !shown_.empty()) {
        for (size_t f : startSelection_)
            if (f < shown_.size()) selected_.insert(items[shown_[f]].id);
        startSelection_.clear();
    }
    // The status bar's tile: the focus ring's, else the one under the pointer.
    if (desktop_) {
        const size_t at = cursor_ < n ? cursor_ : hovered;
        if (at < nf) statusFolder_ = at;
        else if (at < n) statusItem_ = items[shown_[at - nf]].id;
    }
    // Desktop: Ctrl+Shift+C copies the paths of the selection, else the path of the file with
    // the focus ring (else under the pointer).
    if (desktop_ && ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        const size_t at = cursor_ < n ? cursor_ : hovered;
        if (!selected_.empty()) {
            itemRequest_ = ItemRequest{ItemRequest::Verb::CopyPath, -1, SelectedInOrder()};
            itemRequestPending_ = true;
        } else if (at >= nf && at < n) {
            itemRequest_ = ItemRequest{ItemRequest::Verb::CopyPath, items[shown_[at - nf]].id, {}};
            itemRequestPending_ = true;
        }
    }
    // Desktop: Ctrl+A selects every file shown (not in the search box, where it is the text's).
    if (desktop_ && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        for (size_t i : shown_) selected_.insert(items[i].id);
    }
    // A long press on a folder opens its menu (touch's right click, #252), where the caller
    // plays slideshows.
    if (slideshows_ && pressed < nf && !selecting_ && gridScroll_.TakeLongPress(now)) {
        menuFolder_ = folders_[pressed].path;
        ImGui::OpenPopup("##folder_menu");
        provider_->Haptic();
    }
    // A long press on an item starts select mode with it. In select mode it is a tap.
    if (pressed >= nf && pressed < n && !selecting_ && provider_->CanDelete() && gridScroll_.TakeLongPress(now)) {
        selecting_ = true;
        selected_ = {items[shown_[pressed - nf]].id};
        provider_->Haptic();
    }

    const float margin = Round(Px(6.0f));
    const float badgeGap = Round(Px(4.0f));
    const float box = Round(ImGui::GetFontSize() * 1.2f);
    if (stereo_) {
        stereoFrame_.hudW = ImGui::GetIO().DisplaySize.x;
        stereoFrame_.hudH = ImGui::GetIO().DisplaySize.y;
        const float clip[4] = {min.x, min.y, max.x, max.y};
        std::copy(clip, clip + 4, stereoFrame_.clip);
        stereoFrame_.fade = Round(Px(kStereoFade));
    }
    dl->PushClipRect(min, max, true);
    for (size_t p = visFirst; p < visEnd; ++p) {
        float tx = 0.0f, ty = 0.0f;
        if (!layout_.TilePos(p, tx, ty)) continue;
        const ImVec2 a(x0 + tx, min.y + ty - offset);
        const ImVec2 b(a.x + tile, a.y + tileH);
        if (desk && p < nf) {
            if (list)
                DeskFolderRow(dl, folders_[p], columns, a, b, pic, p == hovered, p == pressed, p == cursor_);
            else
                DeskFolderCard(dl, folders_[p], a, b, pic, p == hovered, p == pressed, p == cursor_,
                               p < folderPreviews_.size() ? &folderPreviews_[p] : nullptr, previewEach);
            continue;
        }
        if (p < nf) {
            FolderTile(dl, folders_[p], a, b, margin);
            if (p == pressed) dl->AddRectFilled(a, b, kPressed);
            if (p == hovered) dl->AddRect(a, b, kHover, 0.0f, 0, Round(Px(2.0f)));
            if (p == cursor_) dl->AddRect(a, b, kAccent, 0.0f, 0, Round(Px(3.0f)));
            continue;
        }
        const MediaItem& item = items[shown_[p - nf]];
        const StereoVerdict verdict = classifier_.Get(item);
        int slot = 0;
        ThumbnailAtlas::Thumb thumb;
        const bool shown = cache_.Use(item.id, slot) && atlas_.Get(slot, thumb);
        // The picture's square: the tile on touch, inside the card on desktop.
        const ImVec2 pa(a.x + deskPad, list ? a.y + Round((tileH - pic) * 0.5f) : a.y + deskPad), pb(pa.x + pic, pa.y + pic);
        const bool supported = library_.Supported(shown_[p - nf]);
        if (list)
            DeskItemRow(dl, item, supported, verdict, columns, a, b, pic, p == hovered, p == pressed, p == cursor_, shown,
                        /*behind=*/true);
        else if (desk)
            DeskItemCard(dl, item, supported, verdict, a, b, pic, p == hovered, p == pressed, p == cursor_, shown,
                         /*behind=*/true);
        if (shown && stereo_) {
            // Drawn into the views by the caller, at this rect: the left eye's square from
            // this slot, the right eye's from its own slot (flat until it has one).
            StereoTile t;
            t.x0 = pa.x, t.y0 = pa.y, t.x1 = pb.x, t.y1 = pb.y;
            ThumbnailAtlas::Thumb right = thumb;
            int rslot = 0;
            const StereoLayout layout = SlotLayout(item, verdict);
            const bool pair = Packed(layout) && cache_.Use(RightEyeKey(item.id), rslot) && atlas_.Get(rslot, right);
            DepthEstimate depth;
            const bool measured = thumbDepth_.Get(item.id, depth);
            UvRect l, r;
            if (pair) SlotCrops(measured ? &depth : nullptr, DepthPolicy{}, l, r);
            t.texture[0] = (uint64_t)thumb.texture;
            t.texture[1] = (uint64_t)right.texture;
            t.uv[0] = InSlot(l, thumb.uv0.x, thumb.uv0.y, thumb.uv1.x, thumb.uv1.y);
            t.uv[1] = InSlot(pair ? r : l, right.uv0.x, right.uv0.y, right.uv1.x, right.uv1.y);
            stereoFrame_.tiles.push_back(t);
            // Uploaded before the 3D filter without its right eye, or a one-view 3D photo whose
            // pair is not made: fetch it again (once), which asks for the pair too.
            const bool wantsPair = Packed(layout) || (NeedsPair(item, verdict) && !pairs_.Busy(item.id));
            if (wantsPair && !pair && !pending_.count(item.id) && refetched_.insert(item.id).second)
                cache_.Remove(item.id);
        } else if (shown && desk) {
            dl->AddImageRounded(thumb.texture, pa, pb, thumb.uv0, thumb.uv1, IM_COL32_WHITE, Px(4.0f));
        } else if (shown) {
            dl->AddImage(thumb.texture, a, b, thumb.uv0, thumb.uv1);  // the slot holds what the tile shows
        } else if (!desk) {
            dl->AddRectFilled(a, b, kTileEmpty);
            if (noThumbnail_.count(item.id)) {
                ui::DrawGlyph(dl, item.type == MediaType::Video ? Glyph::Video : Glyph::Image,
                         ImVec2(a.x + tile * 0.5f, a.y + tile * 0.5f), tile * 0.3f,
                         ImGui::GetColorU32(ImGuiCol_TextDisabled));
            }
        }
        if (desk) {
            if (list)
                DeskItemRow(dl, item, supported, verdict, columns, a, b, pic, p == hovered, p == pressed, p == cursor_,
                            shown, /*behind=*/false);
            else
                DeskItemCard(dl, item, supported, verdict, a, b, pic, p == hovered, p == pressed, p == cursor_, shown,
                             /*behind=*/false);
            if (p == hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                if (item.cloudOnly)
                    ImGui::SetTooltip("%s\nOnline only: opening it downloads it", item.name.c_str());
                else
                    ImGui::SetTooltip("%s", item.name.c_str());
            }
            continue;
        }
        // Badges. Top left "3D", beside select mode's box. Top right the length of a video,
        // then to its left the format of a file the player cannot open.
        if (verdict.known && verdict.stereo && !stereo_) {  // the 3D grid: every tile is 3D
            LeftBadge(dl, a.x + margin + (selecting_ ? box + badgeGap : 0.0f), a.y + margin, "3D");
        } else if (verdict.known && verdict.unchecked && item.cloudOnly && !stereo_) {
            // A cloud file whose 3D-ness needs the file: not looked at, so not downloaded (#252).
            LeftBadge(dl, a.x + margin, a.y + margin, "3D?");
        }
        if (item.cloudOnly) {
            // Bottom left: a cloud, kept online only (desktop, #252).
            const float s = Round(ImGui::GetFontSize() * 1.25f);
            const ImVec2 c0(a.x + margin, b.y - margin - s), c1(c0.x + Round(s * 1.5f), b.y - margin);
            dl->AddRectFilled(c0, c1, kBadge, s * 0.5f);
            ui::DrawGlyph(dl, Glyph::Cloud, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), s * 1.4f,
                          kBadgeText, true);
        }
        if (p == hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            if (item.cloudOnly)
                ImGui::SetTooltip("%s\nIn the cloud: opening it downloads it", item.name.c_str());
            else
                ImGui::SetTooltip("%s", item.name.c_str());
        }
        float right = b.x - margin;
        if (item.type == MediaType::Video && item.durationMs > 0) {
            char length[24];
            Duration(item.durationMs, length, sizeof(length));
            right = Badge(dl, right, a.y + margin, length, stereo_ ? kBadgeOpaque : kBadge) - badgeGap;
        }
        const std::string format = library_.Supported(shown_[p - nf]) ? "" : Format(item.name);
        if (!format.empty()) Badge(dl, right, a.y + margin, format.c_str());
        if (selecting_) {
            // As the stock tablet player's: a box on every tile; a picked one dimmed, ticked and outlined.
            const bool on = selected_.count(item.id) != 0;
            if (on) {
                dl->AddRectFilled(a, b, kPicked);
                dl->AddRect(a, b, kAccent, 0.0f, 0, Round(Px(3.0f)));
            }
            CheckBox(dl, ImVec2(a.x + margin, a.y + margin), box, on);
        }
        if (p == pressed) dl->AddRectFilled(a, b, kPressed);
        if (p == hovered) dl->AddRect(a, b, kHover, 0.0f, 0, Round(Px(2.0f)));
        if (p == cursor_) dl->AddRect(a, b, kAccent, 0.0f, 0, Round(Px(3.0f)));
    }
    // The date sections' headers.
    if (headers && desk) {
        // Desktop: the label, then how many it holds, faint.
        ImGui::PushFont(ui::DeskFont(ui::DeskText::Meta));
        for (size_t h = 0; h < layout_.Headers() && h < sectionLabels_.size(); ++h) {
            const float y = min.y + layout_.HeaderY(h) - offset;
            if (y + headerH < min.y || y > max.y) continue;
            const float ty = y + Round((headerH - ImGui::GetFontSize()) * 0.5f);
            const float lx = x0 + Round(Px(6.0f));
            dl->AddText(ImVec2(lx, ty), kDim, sectionLabels_[h].c_str());
            char count[32];
            const size_t c = h < sectionSizes_.size() ? sectionSizes_[h] : 0;
            std::snprintf(count, sizeof(count), "%zu item%s", c, c == 1 ? "" : "s");
            dl->AddText(ImVec2(lx + ImGui::CalcTextSize(sectionLabels_[h].c_str()).x + Px(8.0f), ty),
                        IM_COL32(110, 117, 129, 255), count);
        }
        ImGui::PopFont();
    } else if (headers) {
        FontScope row(TextStyle::Row);
        for (size_t h = 0; h < layout_.Headers() && h < sectionLabels_.size(); ++h) {
            const float y = min.y + layout_.HeaderY(h) - offset;
            if (y + headerH < min.y || y > max.y) continue;
            dl->AddText(ImVec2(x0 + Round(Dp(4.0f)), y + Round((headerH - ImGui::GetFontSize()) * 0.5f)), kDim,
                        sectionLabels_[h].c_str());
        }
    }
    dl->PopClipRect();

    // The fast-scroll thumb, and while it is held the section at the top of the view.
    if (fast) {
        const float frac = std::min(1.0f, std::max(0.0f, offset / limit));
        const float ty = trackTop + frac * (trackH - thumbH);
        const float tw = Round(desk ? Px(thumbHeld ? 10.0f : 6.0f) : Dp(thumbHeld ? 8.0f : 6.0f));
        const float tx = max.x - Round(desk ? Px(3.0f) : Dp(6.0f)) - tw;
        dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + thumbH),
                          thumbHeld ? kAccent : IM_COL32(255, 255, 255, desk ? 70 : 150), tw * 0.5f);
        if (thumbHeld && headers) {
            // The section of the tile at the top: count the sections' sizes up to it.
            const size_t top = layout_.TileAtOffset(offset);
            size_t s = 0, sum = 0;
            while (s + 1 < sectionSizes_.size() && sum + sectionSizes_[s] <= top) sum += sectionSizes_[s++];
            FontScope row(TextStyle::Row);
            const char* label = sectionLabels_[s].c_str();
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const float px = Round(Dp(14.0f)), bh = Round(Dp(40.0f));
            const ImVec2 b1(tx - Round(Dp(12.0f)), ty + thumbH * 0.5f + bh * 0.5f);
            const ImVec2 b0(b1.x - ts.x - px * 2.0f, b1.y - bh);
            dl->AddRectFilled(b0, b1, kPanel, bh * 0.5f);
            dl->AddText(ImVec2(b0.x + px, b0.y + Round((bh - ts.y) * 0.5f)), ImGui::GetColorU32(ImGuiCol_Text), label);
        }
    }

    // Desktop: right click a folder tile for its menu (pin, unpin, hide), a file for its own
    // (open, show in File Explorer, copy the path).
    size_t clicked = n;
    if (desktop_ && gridHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        itemAt(ImGui::GetIO().MousePos, clicked)) {
        if (clicked < nf && (slideshows_ || folders_[clicked].path != kNewestFolder)) {
            menuFolder_ = folders_[clicked].path;
            ImGui::OpenPopup("##folder_menu");
        } else if (clicked >= nf && clicked < n) {
            menuItem_ = items[shown_[clicked - nf]].id;
            // Outside the selection, the menu is the file's alone (as File Explorer's).
            if (!selected_.count(menuItem_)) selected_.clear();
            ImGui::OpenPopup("##item_menu");
        }
    }
    // Desktop: Shift+F10 or the Menu key opens the menu of the tile with the focus ring, at it.
    if (desktop_ && cursor_ < n && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Menu, false) ||
         (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false)))) {
        float tx = 0.0f, ty = 0.0f;
        if (layout_.TilePos(cursor_, tx, ty)) {
            menuAt_ = ImVec2(x0 + tx + Round(Px(16.0f)), min.y + ty - offset + Round(Px(16.0f)));
            menuAtSet_ = true;
        }
        if (cursor_ < nf && (slideshows_ || folders_[cursor_].path != kNewestFolder)) {
            menuFolder_ = folders_[cursor_].path;
            ImGui::OpenPopup("##folder_menu");
        } else if (cursor_ >= nf) {
            menuItem_ = items[shown_[cursor_ - nf]].id;
            if (!selected_.count(menuItem_)) selected_.clear();
            ImGui::OpenPopup("##item_menu");
        } else {
            menuAtSet_ = false;
        }
    }
    FolderMenu();
    if (desktop_) DeskItemMenu();
    if (menuOpen_) {
        menuOpen_ = false;
        if (!selecting_ && library_.Find(menuItem_)) {
            openedId_ = menuItem_;
            provider_->Open(openedId_);
            return Action::Opened;
        }
    }

    size_t picked = 0;
    if (keyOpen) {
        picked = cursor_;
    } else if (!tap || !itemAt(gridPress_, picked)) {
        return Action::None;
    }
    // Desktop: Ctrl / Shift + click selects (folders are left out); a plain click opens, and
    // leaves the selection behind.
    if (desktop_ && !keyOpen && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift)) {
        if (picked >= nf && picked < n) {
            std::vector<int64_t> ids;
            ids.reserve(shown_.size());
            for (size_t i : shown_) ids.push_back(items[i].id);
            ClickSelect(ids, picked - nf, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift, selAnchor_, selected_);
        }
        return Action::None;
    }
    if (desktop_) selected_.clear();
    if (picked < nf) {
        if (!selecting_) GoTo(folders_[picked].path);
        return Action::None;
    }
    const int64_t id = items[shown_[picked - nf]].id;
    if (selecting_) {
        if (!selected_.erase(id)) selected_.insert(id);
        if (selected_.empty()) selecting_ = false;
        return Action::None;
    }
    openedId_ = id;
    provider_->Open(openedId_);
    return Action::Opened;
}

void MediaBrowser::FolderMenu() {
    if (menuAtSet_ && ImGui::IsPopupOpen("##folder_menu")) {
        ImGui::SetNextWindowPos(menuAt_, ImGuiCond_Appearing);
        menuAtSet_ = false;
    }
    if (!ImGui::BeginPopup("##folder_menu")) return;
    const Source& s = CurrentSource();
    const bool top = folder_ == s.folder;
    const bool newest = menuFolder_ == kNewestFolder;
    auto ask = [&](FolderRequest::Verb verb) {
        folderRequest_ = FolderRequest{verb, menuFolder_};
        folderRequestPending_ = true;
    };
    {
        FontScope row(TextStyle::Row);
        if (slideshows_) {
            // Play it (#252): what the tile counts, it and its sub-folders. Open is the click.
            if (ImGui::MenuItem("Play slideshow", desktop_ ? "Shift+Enter" : nullptr)) AskSlideshow(menuFolder_, false);
            if (ImGui::MenuItem("Open", desktop_ ? "Enter" : nullptr)) GoTo(menuFolder_);
        }
        if (desktop_ && !newest) {
            if (slideshows_) ImGui::Separator();
            if (s.kind == SourceKind::Pinned && top) {
                if (ImGui::MenuItem("Remove from Pinned")) ask(FolderRequest::Verb::Unpin);
            } else if (ImGui::MenuItem("Pin this folder")) {
                ask(FolderRequest::Verb::Pin);
            }
            if (s.mode == SourceMode::Grouped && top && ImGui::MenuItem("Hide from Media folders"))
                ask(FolderRequest::Verb::Hide);
        }
    }
    ImGui::EndPopup();
}

bool MediaBrowser::CanPlayHere() const {
    const Source& s = CurrentSource();
    if (!listed_ || s.kind == SourceKind::Unsupported) return false;
    for (const Folder& f : folders_)
        if (f.count > 0) return true;
    return !shown_.empty();
}

void MediaBrowser::AskSlideshow(const std::string& folder, bool here) {
    const std::vector<MediaItem>& items = library_.Items();
    playlist_.clear();
    playlistStart_ = 0;
    playlistSkipped_ = 0;
    for (size_t i : library_.QueryTree(CurrentSource(), options_, folder)) {
        const MediaItem& item = items[i];
        if (here && !query_.empty() && !ContainsNoCase(item.name, query_)) continue;
        if (depth_ != DepthFilter::Both) {
            // As the grid: the 3D / 2D filter keeps the files whose verdict is known and fits.
            const StereoVerdict v = classifier_.Get(item);
            if (!v.known || v.stereo != (depth_ == DepthFilter::Only3D)) continue;
        }
        if (item.cloudOnly) {
            ++playlistSkipped_;  // a slideshow never downloads
            continue;
        }
        playlist_.push_back(item.id);
    }
    playlistPending_ = true;  // empty too: the caller says there is nothing to play
    LOG_INFO("browser: slideshow of '%s'%s: %zu file(s), %d cloud-only left out", folder.c_str(),
             here ? " (the place shown)" : "", playlist_.size(), playlistSkipped_);
}

void MediaBrowser::AskSlideshowFrom(int64_t id) {
    const std::vector<MediaItem>& items = library_.Items();
    playlist_.clear();
    playlistStart_ = 0;
    playlistSkipped_ = 0;
    bool reached = false;
    for (size_t i : shown_) {
        const MediaItem& item = items[i];
        if (item.id == id) {
            reached = true;
            playlistStart_ = playlist_.size();  // the file, or (cloud-only) the next local one
        }
        if (item.cloudOnly) {
            ++playlistSkipped_;
            continue;
        }
        playlist_.push_back(item.id);
    }
    if (!reached || playlistStart_ >= playlist_.size()) playlistStart_ = 0;
    playlistPending_ = true;
    LOG_INFO("browser: slideshow of the grid from item %lld: %zu file(s), start %zu, %d cloud-only left out",
             (long long)id, playlist_.size(), playlistStart_, playlistSkipped_);
}

std::vector<int64_t> MediaBrowser::SelectedInOrder() const {
    std::vector<int64_t> out;
    const std::vector<MediaItem>& items = library_.Items();
    for (size_t i : shown_)
        if (selected_.count(items[i].id)) out.push_back(items[i].id);
    return out;
}

void MediaBrowser::GoTo(const std::string& folder) {
    focusId_ = -1;
    folder_ = folder;
    dirty_ = true;
    gridScroll_.SetOffset(0.0f);
}

void MediaBrowser::Up() { GoTo(library_.Parent(CurrentSource(), folder_)); }

// The Pinned source's folders, for the desktop sidebar: one row per pinned folder, with what it
// holds (as the source rows' counts: every supported file, whatever the view options). A
// folder shown that is no longer pinned goes back to the source's top.
void MediaBrowser::UpdatePinnedRows() {
    pinnedRows_.clear();
    pinnedSource_ = SIZE_MAX;
    const std::vector<Source>& sources = library_.Sources();
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].kind != SourceKind::Pinned) continue;
        pinnedSource_ = i;
        pinnedRows_ = library_.Folders(sources[i], ViewOptions{}, sources[i].folder);
        break;
    }
    if (source_ != pinnedSource_ || pinnedSource_ == SIZE_MAX || folder_ == CurrentSource().folder) return;
    for (const Folder& f : pinnedRows_)
        if (folder_.compare(0, f.path.size(), f.path) == 0) return;
    folder_ = CurrentSource().folder;
    dirty_ = true;
}

void MediaBrowser::SetViewPrefs(const DeskViewPrefs& prefs) {
    details_ = prefs.details;
    cardW_ = std::min(kCardMax, std::max(kCardMin, prefs.cardW));
    cardShown_ = 0.0f;  // drawn at that size at once, not eased to it
    if (options_.order != prefs.order) {
        options_.order = prefs.order;
        dirty_ = true;
    }
    // The place last shown, once the list arrives (a test's SetStartSource still wins).
    if (startSource_.empty() && !prefs.source.empty()) {
        startSource_ = prefs.source;
        startFolder_ = prefs.folder;
    }
}

void MediaBrowser::SetQuery(const std::string& query) {
    std::snprintf(search_, sizeof(search_), "%s", query.c_str());
    query_ = search_;
    dirty_ = true;
}

// Each change of place (a source, a folder, by any route) goes on the Back list. A new place
// clears Forward, and the search: it was for the place left.
void MediaBrowser::TrackHistory() {
    if (!listed_) return;
    if (!placed_) {
        here_ = Place{source_, folder_};
        placed_ = true;
        return;
    }
    if (here_.source == source_ && here_.folder == folder_) return;
    back_.push_back(here_);
    if (back_.size() > 64) back_.erase(back_.begin());
    forward_.clear();
    here_ = Place{source_, folder_};
    selected_.clear();  // a selection belongs to the place it was made in
    if (search_[0]) {
        search_[0] = '\0';
        query_.clear();
        dirty_ = true;
    }
}

bool MediaBrowser::HistoryStep(int dir) {
    std::vector<Place>& from = dir < 0 ? back_ : forward_;
    std::vector<Place>& to = dir < 0 ? forward_ : back_;
    TrackHistory();  // a change made this frame goes on the list first
    while (!from.empty() && from.back().source >= library_.Sources().size()) from.pop_back();
    if (from.empty()) return false;
    to.push_back(here_);
    here_ = from.back();
    from.pop_back();
    source_ = here_.source;
    folder_ = here_.folder;
    EndSelection();
    if (search_[0]) {
        search_[0] = '\0';
        query_.clear();
    }
    dirty_ = true;
    gridScroll_.SetOffset(0.0f);
    return true;
}

void MediaBrowser::EndSelection() {
    selecting_ = false;
    selected_.clear();
}

bool MediaBrowser::CanGoBack() const {
    return provider_ && (drawer_ || sheetOpen_ || selecting_ || (desktop_ && !selected_.empty()) ||
                         folder_ != CurrentSource().folder);
}

bool MediaBrowser::Back() {
    if (!CanGoBack()) return false;
    if (drawer_) {
        drawer_ = false;
    } else if (sheetOpen_) {
        closeSheets_ = true;  // the next Build() closes it: popups are ImGui's, within a frame
    } else if (selecting_ || (desktop_ && !selected_.empty())) {
        EndSelection();
    } else {
        Up();
    }
    return true;
}

bool MediaBrowser::NextId(int step, int64_t& id) const {
    if (openedId_ < 0 || CurrentSource().kind == SourceKind::Unsupported) return false;
    const std::vector<MediaItem>& items = library_.Items();
    for (size_t p = 0; p < shown_.size(); ++p) {
        if (items[shown_[p]].id != openedId_) continue;
        const long next = (long)p + step;
        if (next < 0 || next >= (long)shown_.size()) return false;
        id = items[shown_[(size_t)next]].id;
        return true;
    }
    return false;
}

bool MediaBrowser::OpenedPosition(int& index, int& count) const {
    if (openedId_ < 0) return false;
    const std::vector<MediaItem>& items = library_.Items();
    for (size_t p = 0; p < shown_.size(); ++p) {
        if (items[shown_[p]].id != openedId_) continue;
        index = (int)p;
        count = (int)shown_.size();
        return true;
    }
    return false;
}

bool MediaBrowser::OpenedItem(MediaItem& out) const {
    const MediaItem* item = openedId_ >= 0 ? library_.Find(openedId_) : nullptr;
    if (!item) return false;
    out = *item;
    return true;
}

bool MediaBrowser::StepTo(int step) {
    int64_t id = 0;
    if (!NextId(step, id)) return false;
    openedId_ = id;
    return true;
}

bool MediaBrowser::OpenNext(int step) {
    if (!provider_ || !StepTo(step)) return false;
    provider_->Open(openedId_);
    return true;
}

void MediaBrowser::UploadReady(const std::unordered_set<int64_t>& window, uint32_t need) {
    bool budget = true;
    const uint32_t tile = atlas_.Tile();
    for (auto it = ready_.begin(); it != ready_.end();) {
        const Thumbnail& t = it->thumbnail;
        const bool wanted = window.count(t.id) != 0;
        const bool fits = t.width > 0 && t.height > 0 && t.rgba.size() >= (size_t)t.width * t.height * 4;
        if (!wanted || !fits) {
            // Scrolled away meanwhile, or nothing to show.
            if (wanted) noThumbnail_.insert(t.id);
            pending_.erase(t.id);
            it = ready_.erase(it);
            continue;
        }
        int slot = 0;
        if (!budget || !cache_.Assign(t.id, slot)) {
            ++it;
            continue;
        }
        // Only the part the tile shows goes to the GPU, at most a slot's worth of it. Black
        // bars come off a video's frame only: a still with a dark surround is more often a
        // screenshot of a dialog than a letterboxed picture.
        const MediaItem* item = library_.Find(t.id);
        const bool bars = item && item->type == MediaType::Video;
        TileRegion r = ShownRegion(bars ? t.rgba.data() : nullptr, t.width, t.height, it->layout);
        if (r.w > tile) {
            r.x += (r.w - tile) / 2;
            r.w = tile;
        }
        if (r.h > tile) {
            r.y += (r.h - tile) / 2;
            r.h = tile;
        }
        const uint8_t* origin = t.rgba.data() + ((size_t)r.y * t.width + r.x) * 4;
        if (!atlas_.Upload(slot, origin, r.w, r.h, (size_t)t.width * 4)) {
            cache_.Remove(t.id);  // this frame's uploads are spent: the rest wait for the next
            budget = false;
            ++it;
            continue;
        }
        if (Packed(it->layout)) {
            // The 3D grid: the right eye's same square in a slot of its own (only under the
            // "3D only" filter, so the 2D grid's slots are not halved), and the pair's depth.
            uint32_t rx = 0, ry = 0;
            int rslot = 0;
            if (WantsStereo() && OtherEyeRegion(it->layout, t.width, t.height, r.x, r.y, r.w, r.h, rx, ry) &&
                cache_.Assign(RightEyeKey(t.id), rslot)) {
                const uint8_t* rorigin = t.rgba.data() + ((size_t)ry * t.width + rx) * 4;
                if (!atlas_.Upload(rslot, rorigin, r.w, r.h, (size_t)t.width * 4)) {
                    cache_.Remove(RightEyeKey(t.id));  // spent: both eyes again next frame
                    cache_.Remove(t.id);
                    budget = false;
                    ++it;
                    continue;
                }
            }
            const uint32_t eyeW = (it->layout == StereoLayout::SbsFull || it->layout == StereoLayout::SbsHalf)
                                      ? t.width / 2
                                      : t.width;
            thumbDepth_.Measure(t.id, t.rgba.data(), t.width, t.height, it->layout,
                                eyeW > 0 ? (float)r.w / (float)eyeW : 1.0f);
        }
        // A video shown soft (3D found from the thumbnail itself, so it was asked for as 2D;
        // black bars taken off; a size the provider did not report): ask once more, larger.
        sharpen_.erase(t.id);
        if (paired_.count(t.id)) sharpened_.insert(t.id);  // a made pair is sized for the 3D mode
        const uint32_t shorter = std::min(r.w, r.h), longer = std::max(r.w, r.h);
        const uint32_t own = item ? std::max(item->width, item->height) : 0;
        const uint32_t made = std::max(t.width, t.height);
        if (bars && !sharpened_.count(t.id) && shorter < kSoft * (float)need && longer < tile && (own == 0 || made < own)) {
            const double scale = std::min((double)need / std::max(1u, shorter), (double)tile / std::max(1u, longer));
            uint32_t edge = (uint32_t)std::ceil(scale * made);
            edge = std::min(edge, own > 0 ? std::min(own, kFetchCap) : kFetchCap);
            if (edge > made + made / 8) sharpen_[t.id] = edge;
            sharpened_.insert(t.id);
        }
        pending_.erase(t.id);
        it = ready_.erase(it);
    }
}

// The view options as a sheet (#180): from the bottom in portrait, from the right in
// landscape, over a dimmed grid, with 48 dp rows. The filters are in the chip row.
void MediaBrowser::ViewOptionsSheet(float width, float top, float bottom) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool portrait = io.DisplaySize.x < io.DisplaySize.y;
    if (portrait) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    } else {
        const float w = Round(std::min(Dp(360.0f), width * 0.5f));
        ImGui::SetNextWindowPos(ImVec2(width, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(w, bottom - top), ImGuiCond_Always);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(8.0f), Dp(12.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(16.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(kPanel));
    const bool open = ImGui::BeginPopup("##view_options");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (closeSheets_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
    const float rowH = Round(Dp(48.0f));
    bool changed = false;
    // A row that keeps the sheet open, so several options can be set.
    auto option = [&](const char* label, bool selected) {
        FontScope row(TextStyle::Row);
        const bool picked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_DontClosePopups, ImVec2(0.0f, rowH));
        changed = changed || picked;
        return picked;
    };
    auto heading = [&](const char* label) {
        FontScope small(TextStyle::Small);
        ImGui::Dummy(ImVec2(0.0f, Dp(6.0f)));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDim), "%s", label);
        ImGui::Dummy(ImVec2(0.0f, Dp(4.0f)));
    };
    heading("Sort by date");
    if (option("Newest first", options_.order == SortOrder::NewestFirst)) options_.order = SortOrder::NewestFirst;
    if (option("Oldest first", options_.order == SortOrder::OldestFirst)) options_.order = SortOrder::OldestFirst;
    heading("Files");
    if (option("Show unsupported files", showUnsupported_)) {
        showUnsupported_ = !showUnsupported_;
        if (!showUnsupported_ && CurrentSource().kind == SourceKind::Unsupported) {
            source_ = 0;
            GoTo(CurrentSource().folder);
            EndSelection();
        }
    }
    ImGui::PopStyleVar(2);
    ImGui::EndPopup();
    if (changed) {
        dirty_ = true;
        gridScroll_.SetOffset(0.0f);
    }
}

} // namespace mp::browser
