// SPDX-License-Identifier: Apache-2.0
#include "browser/MediaBrowser.h"

#include "browser/TileCrop.h"
#include "ui/Icons.h"
#include "ui/TransportUI.h"
#include "ui/UiScale.h"
#include "ui/UiText.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <utility>

namespace mp::browser {

namespace {

using ui::Dp;
using ui::Glyph;
using ui::Px;
using ui::TextStyle;

constexpr float kGutter = 4.0f;    // between tiles, design units
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
const ImU32 kSelected = IM_COL32(51, 166, 255, 72);     // the UI's accent
const ImU32 kAccent = IM_COL32(51, 166, 255, 255);
const ImU32 kPicked = IM_COL32(0, 0, 0, 110);           // over a tile picked in select mode
const ImU32 kBadge = IM_COL32(0, 0, 0, 140);
const ImU32 kBadgeText = IM_COL32(255, 255, 255, 255);
const ImU32 kFolder = IM_COL32(190, 192, 198, 255);
const ImU32 kScrim = IM_COL32(0, 0, 0, 140);            // under the drawer and the sheets
const ImU32 kDim = IM_COL32(170, 176, 186, 255);        // secondary text: counts, headers

float Round(float v) { return std::floor(v + 0.5f); }

bool Same(const std::vector<ThumbnailRequest>& a, const std::vector<ThumbnailRequest>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].checkFile != b[i].checkFile || a[i].maxEdge != b[i].maxEdge) return false;
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
float Badge(ImDrawList* dl, float right, float top, const char* text) {
    FontScope small(TextStyle::Small);
    const float font = ImGui::GetFontSize();
    const float padX = Round(font * 0.3f), padY = Round(font * 0.1f);
    const float w = Round(ImGui::CalcTextSize(text).x) + padX * 2.0f;
    const float h = font + padY * 2.0f;
    dl->AddRectFilled(ImVec2(right - w, top), ImVec2(right, top + h), kBadge, h * 0.5f);
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
    provider_ = provider;
    cache_.Reset((int)atlas_.SlotCount());
    library_.SetItems({});
    return true;
}

void MediaBrowser::Shutdown() {
    if (!provider_) return;
    classifier_.Shutdown();
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
    if (provider_->TakeItems(items)) {
        // An edited file keeps its id: forget its old thumbnail.
        for (const MediaItem& item : items) {
            const MediaItem* old = library_.Find(item.id);
            if (old && old->modified != item.modified) {
                cache_.Remove(item.id);
                noThumbnail_.erase(item.id);
                sharpen_.erase(item.id);
                sharpened_.erase(item.id);
            }
        }
        library_.SetItems(std::move(items));
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
        if (!item) continue;
        pending_.insert(thumbnail.id);
        classifier_.Classify(*item, std::move(thumbnail));
        thumbnail = Thumbnail{};
    }
    StereoVerdict verdict;
    while (classifier_.TakeResult(thumbnail, verdict)) {
        if (depth_ != DepthFilter::Both) dirty_ = true;  // it may join the grid now
        ready_.push_back(Ready{std::move(thumbnail), verdict.layout});
        thumbnail = Thumbnail{};
    }
}

void MediaBrowser::Rebuild() {
    dirty_ = false;
    shown_.clear();
    unknown_.clear();
    const std::vector<MediaItem>& items = library_.Items();
    folders_ = folder_.empty() ? std::vector<Folder>() : library_.Folders(CurrentSource(), options_, folder_);
    for (size_t i : library_.Query(CurrentSource(), options_, folder_)) {
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
    if ((sourceKind_ == SourceKind::All || sourceKind_ == SourceKind::Camera) && folders_.empty()) {
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
    return (int)library_.Sources().size() - (showUnsupported_ ? 0 : 1);
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

const Source& MediaBrowser::CurrentSource() const { return library_.Sources()[(size_t)sourceKind_]; }

MediaBrowser::Action MediaBrowser::Build() {
    if (!provider_) return Action::None;
    TakeResults();
    if (dirty_) Rebuild();
    cache_.NextFrame();

    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const double now = ImGui::GetTime();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float bar = Round(ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f);
    const float chips = Round(Dp(56.0f));
    // Landscape and wide: the sources are a rail that stays; anywhere else a drawer (#180).
    rail_ = UseRail(w / Dp(1.0f), h / Dp(1.0f));
    if (rail_) drawer_ = false;
    const float sidebarW = SidebarWidth(w);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
    const bool open = ImGui::Begin("##browser", nullptr,
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    Action action = Action::None;
    if (open) {
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
    if (action == Action::Opened) classifier_.Save();  // the app may be stopped while playing
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
    const float more = width - pad - icon;
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
    size_t i = CurrentSource().folder.size();
    for (size_t slash; (slash = folder_.find('/', i)) != std::string::npos; i = slash + 1)
        title += " / " + folder_.substr(i, slash - i);
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
        action = Sidebar(ImVec2(0.0f, ui::g_systemBarTop + Dp(8.0f)), ImVec2(w, height - ui::g_systemBarBottom), now);
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
    const float top = min.y + gap;
    sidebarScroll_.SetSlop(Px(kSlop));
    sidebarScroll_.SetLimit((files + 1) * rowH + gap * 4.0f - size.y);
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##sidebar", size);
    const bool tap = Track(sidebarScroll_, sidebarPress_, now);
    const float offset = Round(sidebarScroll_.Offset());

    auto rowTop = [&](int r) { return top + r * rowH + (r == files ? gap * 2.0f : 0.0f) - offset; };
    auto rowAt = [&](ImVec2 p) {
        for (int r = 0; r <= files; ++r) {
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
    for (int r = 0; r <= files; ++r) {
        const float y = rowTop(r);
        if (r == files) {
            const float line = y - gap;
            dl->AddLine(ImVec2(min.x + pad, line), ImVec2(right, line), dim, Round(Dp(1.0f)));
        }
        if (y + rowH < min.y || y > max.y) continue;
        const ImVec2 a(min.x, y), b(max.x, y + rowH);
        const Source* s = r < files ? &sources[r] : nullptr;
        if (s && s->kind == sourceKind_) dl->AddRectFilled(a, b, kSelected);
        if (r == pressed) dl->AddRectFilled(a, b, kPressed);
        ui::DrawGlyph(dl, s ? SourceIcon(s->kind) : Glyph::Files, ImVec2(min.x + pad + icon * 0.5f, y + rowH * 0.5f),
                      icon, text, s && s->kind == sourceKind_);
        const float textY = y + Round((rowH - ImGui::GetFontSize()) * 0.5f);
        if (!s) {
            dl->AddText(ImVec2(textX, textY), text, ui::FitText("Other files...", right - textX).c_str());
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
    sourceKind_ = sources[r].kind;
    folder_ = sources[r].folder;
    EndSelection();
    sourcePicked_ = true;  // closes the drawer; the rail stays (#180)
    dirty_ = true;
    gridScroll_.SetOffset(0.0f);
    return Action::None;
}

MediaBrowser::Action MediaBrowser::Grid(ImVec2 min, ImVec2 max, double now) {
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

    // Square tiles, about one column per 180 dp of the grid's width (#180): beside the rail
    // that is one fewer. The drawer lies over the grid and leaves it as it is.
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const int cols = GridColumns(size.x / Dp(1.0f));
    const float gutter = Round(Px(kGutter));
    const float tile = std::floor((size.x - gutter * (cols - 1)) / cols);
    if (tile <= 0.0f || size.y <= 0.0f) return Action::None;
    const float step = tile + gutter;
    const float x0 = min.x + Round((size.x - (cols * step - gutter)) * 0.5f);
    const size_t nf = folders_.size();
    const size_t n = nf + shown_.size();  // tiles: the folders, then the items
    // The tiles in sections: the date sections, or one with the folders then the items.
    const bool headers = !sectionSizes_.empty();
    const float headerH = Round(Dp(44.0f));
    const size_t topTile = layout_.TileAtOffset(gridScroll_.Offset());  // before the rebuild
    layout_.Build(headers ? sectionSizes_ : std::vector<size_t>{n}, headers, cols, tile, gutter, headerH);
    if (cols != columns_ && columns_ > 0) {
        // A reflow (rotation, the rail): keep the top row's first item in the top row.
        gridScroll_.SetLimit(layout_.Height() - size.y);
        gridScroll_.SetOffset(layout_.OffsetOf(topTile));
    }
    columns_ = cols;
    gridScroll_.SetSlop(Px(kSlop));
    gridScroll_.SetLimit(layout_.Height() - size.y);
    const float limit = std::max(0.0f, layout_.Height() - size.y);

    // A fast-scroll thumb on the right edge of a long grid, submitted before the grid so it
    // wins the press.
    const bool fast = n > kFastScrollItems && limit > 0.0f;
    const float trackTop = min.y + Dp(8.0f), trackH = size.y - Dp(16.0f), thumbH = Round(Dp(48.0f));
    bool thumbHeld = false;
    if (fast && n > 0) {
        const float frac = std::min(1.0f, std::max(0.0f, gridScroll_.Offset() / limit));
        const float ty = trackTop + frac * (trackH - thumbH);
        const float hitW = Round(Dp(40.0f));
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
    if (n > 0) {
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##grid", size);
        tap = Track(gridScroll_, gridPress_, now);
    }
    const float offset = Round(gridScroll_.Offset());

    // The visible rows, and a few more either side as far as the slots go.
    size_t visFirst = 0, visEnd = 0;
    layout_.Range(offset, offset + size.y, visFirst, visEnd);
    const int visRows = (int)((size.y + step - 1.0f) / step) + 1;
    const int spare = (int)atlas_.SlotCount() - visRows * cols;
    const int extra = std::max(0, std::min(2, spare / (2 * cols)));
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
    }
    // A thumbnail big enough that the part a tile shows covers it pixel for pixel, as far as
    // an atlas slot holds.
    const uint32_t need = std::min(atlas_.Tile(), (uint32_t)tile);
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
            edge = ThumbnailEdge(item.width, item.height, classifier_.Get(item).layout, need, atlas_.Tile(), kFetchCap);
        }
        want.push_back(ThumbnailRequest{item.id, classifier_.NeedsFileCheck(item), edge});
    };
    for (size_t p = std::max(visFirst, nf); p < winEnd; ++p) ask(items[shown_[p - nf]]);
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
    // A long press on an item starts select mode with it. In select mode it is a tap.
    if (pressed >= nf && pressed < n && !selecting_ && provider_->CanDelete() && gridScroll_.TakeLongPress(now)) {
        selecting_ = true;
        selected_ = {items[shown_[pressed - nf]].id};
        provider_->Haptic();
    }

    const float margin = Round(Px(6.0f));
    const float badgeGap = Round(Px(4.0f));
    const float box = Round(ImGui::GetFontSize() * 1.2f);
    dl->PushClipRect(min, max, true);
    for (size_t p = visFirst; p < visEnd; ++p) {
        float tx = 0.0f, ty = 0.0f;
        if (!layout_.TilePos(p, tx, ty)) continue;
        const ImVec2 a(x0 + tx, min.y + ty - offset);
        const ImVec2 b(a.x + tile, a.y + tile);
        if (p < nf) {
            FolderTile(dl, folders_[p], a, b, margin);
            if (p == pressed) dl->AddRectFilled(a, b, kPressed);
            continue;
        }
        const MediaItem& item = items[shown_[p - nf]];
        const StereoVerdict verdict = classifier_.Get(item);
        int slot = 0;
        ThumbnailAtlas::Thumb thumb;
        if (cache_.Use(item.id, slot) && atlas_.Get(slot, thumb)) {
            dl->AddImage(thumb.texture, a, b, thumb.uv0, thumb.uv1);  // the slot holds what the tile shows
        } else {
            dl->AddRectFilled(a, b, kTileEmpty);
            if (noThumbnail_.count(item.id)) {
                ui::DrawGlyph(dl, item.type == MediaType::Video ? Glyph::Video : Glyph::Image,
                         ImVec2(a.x + tile * 0.5f, a.y + tile * 0.5f), tile * 0.3f,
                         ImGui::GetColorU32(ImGuiCol_TextDisabled));
            }
        }
        // Badges. Top left "3D", beside select mode's box. Top right the length of a video,
        // then to its left the format of a file the player cannot open.
        if (verdict.known && verdict.stereo) {
            LeftBadge(dl, a.x + margin + (selecting_ ? box + badgeGap : 0.0f), a.y + margin, "3D");
        }
        float right = b.x - margin;
        if (item.type == MediaType::Video && item.durationMs > 0) {
            char length[24];
            Duration(item.durationMs, length, sizeof(length));
            right = Badge(dl, right, a.y + margin, length) - badgeGap;
        }
        const std::string format = library_.Kind(shown_[p - nf]) == SourceKind::Unsupported ? Format(item.name) : "";
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
    }
    // The date sections' headers.
    if (headers) {
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
        const float tw = Round(Dp(thumbHeld ? 8.0f : 6.0f));
        const float tx = max.x - Round(Dp(6.0f)) - tw;
        dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + thumbH),
                          thumbHeld ? kAccent : IM_COL32(255, 255, 255, 150), tw * 0.5f);
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

    size_t picked = 0;
    if (!tap || !itemAt(gridPress_, picked)) return Action::None;
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

void MediaBrowser::GoTo(const std::string& folder) {
    folder_ = folder;
    dirty_ = true;
    gridScroll_.SetOffset(0.0f);
}

void MediaBrowser::Up() { GoTo(folder_.substr(0, folder_.rfind('/', folder_.size() - 2) + 1)); }

void MediaBrowser::EndSelection() {
    selecting_ = false;
    selected_.clear();
}

bool MediaBrowser::CanGoBack() const {
    return provider_ && (drawer_ || sheetOpen_ || selecting_ || folder_ != CurrentSource().folder);
}

bool MediaBrowser::Back() {
    if (!CanGoBack()) return false;
    if (drawer_) {
        drawer_ = false;
    } else if (sheetOpen_) {
        closeSheets_ = true;  // the next Build() closes it: popups are ImGui's, within a frame
    } else if (selecting_) {
        EndSelection();
    } else {
        Up();
    }
    return true;
}

bool MediaBrowser::NextId(int step, int64_t& id) const {
    if (openedId_ < 0 || sourceKind_ == SourceKind::Unsupported) return false;
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
        // Shown soft (3D found from the thumbnail itself, so it was asked for as 2D; black bars
        // taken off; a size the provider did not report): ask once more, larger.
        sharpen_.erase(t.id);
        const uint32_t shorter = std::min(r.w, r.h), longer = std::max(r.w, r.h);
        const uint32_t own = item ? std::max(item->width, item->height) : 0;
        const uint32_t made = std::max(t.width, t.height);
        if (!sharpened_.count(t.id) && shorter < kSoft * (float)need && longer < tile && (own == 0 || made < own)) {
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
        if (!showUnsupported_ && sourceKind_ == SourceKind::Unsupported) {
            sourceKind_ = SourceKind::All;
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
