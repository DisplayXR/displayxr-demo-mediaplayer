// SPDX-License-Identifier: Apache-2.0
#include "browser/MediaBrowser.h"

#include "ui/Icons.h"
#include "ui/UiScale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>

namespace mp::browser {

namespace {

using ui::Glyph;
using ui::Px;

constexpr float kGutter = 4.0f;    // between tiles, design units
constexpr float kSlop = 12.0f;     // how far a tap may move, design units
constexpr size_t kScanBatch = 16;  // under a depth filter: items with no verdict asked for at once

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

float Round(float v) { return std::floor(v + 0.5f); }

bool Same(const std::vector<ThumbnailRequest>& a, const std::vector<ThumbnailRequest>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].checkFile != b[i].checkFile) return false;
    }
    return true;
}

// `text` cut to fit in `width`, ending in an ellipsis if it was cut.
std::string Fit(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const char* kEllipsis = "\xE2\x80\xA6";
    const float ellipsis = ImGui::CalcTextSize(kEllipsis).x;
    size_t n = text.size();
    while (n > 0 && ImGui::CalcTextSize(text.c_str(), text.c_str() + n).x + ellipsis > width) {
        --n;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) --n;  // whole UTF-8 characters
    }
    return text.substr(0, n) + kEllipsis;
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

// The part of a thumbnail a tile shows: one eye, centre-cropped to a square on screen.
void Crop(const ThumbnailAtlas::Thumb& t, StereoLayout layout, ImVec2& uv0, ImVec2& uv1) {
    float x0 = 0.0f, x1 = 1.0f, y0 = 0.0f, y1 = 1.0f;
    float aspect = (float)t.w / (float)t.h;  // the eye's width over height, on screen
    switch (layout) {
        case StereoLayout::SbsFull:
            x1 = 0.5f;
            aspect *= 0.5f;
            break;
        case StereoLayout::SbsHalf:  // a squeezed eye is shown at the whole frame's aspect
            x1 = 0.5f;
            break;
        case StereoLayout::TbFull:
            y1 = 0.5f;
            aspect *= 2.0f;
            break;
        case StereoLayout::TbHalf:
            y1 = 0.5f;
            break;
        default:
            break;
    }
    if (aspect > 1.0f) {
        const float mid = (x0 + x1) * 0.5f, half = (x1 - x0) * 0.5f / aspect;
        x0 = mid - half;
        x1 = mid + half;
    } else {
        const float mid = (y0 + y1) * 0.5f, half = (y1 - y0) * 0.5f * aspect;
        y0 = mid - half;
        y1 = mid + half;
    }
    const ImVec2 span(t.uv1.x - t.uv0.x, t.uv1.y - t.uv0.y);
    uv0 = ImVec2(t.uv0.x + span.x * x0, t.uv0.y + span.y * y0);
    uv1 = ImVec2(t.uv0.x + span.x * x1, t.uv0.y + span.y * y1);
}

void Duration(int64_t ms, char* out, size_t n) {
    const int s = (int)(ms / 1000);
    if (s >= 3600) {
        std::snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    } else {
        std::snprintf(out, n, "%d:%02d", s / 60, s % 60);
    }
}

// A translucent dark pill holding `text`, its right edge at `right`. Returns its left edge.
float Badge(ImDrawList* dl, float right, float top, const char* text) {
    const float font = ImGui::GetFontSize();
    const float padX = Round(font * 0.3f), padY = Round(font * 0.1f);
    const float w = Round(ImGui::CalcTextSize(text).x) + padX * 2.0f;
    const float h = font + padY * 2.0f;
    dl->AddRectFilled(ImVec2(right - w, top), ImVec2(right, top + h), kBadge, h * 0.5f);
    dl->AddText(ImVec2(right - w + padX, top + padY), kBadgeText, text);
    return right - w;
}

// A folder's tile: the folder, its name along the bottom and its count top right.
void FolderTile(ImDrawList* dl, const Folder& folder, ImVec2 a, ImVec2 b, float margin) {
    const float tile = b.x - a.x, font = ImGui::GetFontSize();
    dl->AddRectFilled(a, b, kTileEmpty);
    ui::DrawGlyph(dl, Glyph::Folder, ImVec2(a.x + tile * 0.5f, a.y + Round((tile - font) * 0.45f)), tile * 0.5f, kFolder);
    const std::string name = Fit(folder.name, tile - margin * 2.0f);
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

// Centred `text`, and a button under it if `button` is set. True when the button is pressed.
bool Message(ImVec2 min, ImVec2 max, const char* text, const char* button) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::string line = Fit(text, max.x - min.x - style.WindowPadding.x * 2.0f);
    const ImVec2 size = ImGui::CalcTextSize(line.c_str());
    const float gap = Round(style.ItemSpacing.y * 2.0f);
    const float buttonH = button ? gap + ImGui::GetFrameHeight() : 0.0f;
    const float y = Round((min.y + max.y - size.y - buttonH) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(Round((min.x + max.x - size.x) * 0.5f), y),
                                        ImGui::GetColorU32(ImGuiCol_Text), line.c_str());
    if (!button) return false;
    const float w = ImGui::CalcTextSize(button).x + style.FramePadding.x * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(Round((min.x + max.x - w) * 0.5f), y + size.y + gap));
    return ImGui::Button(button);
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
        ready_.push_back(std::move(thumbnail));
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
}

// The sources the sidebar lists: Unsupported, the last, only when the view options show it.
int MediaBrowser::SourceRows() const {
    return (int)library_.Sources().size() - (showUnsupported_ ? 0 : 1);
}

float MediaBrowser::SidebarWidth(float screenW) const {
    // Wide enough for the longest row, within 11 font sizes and 30% of the screen.
    const ImGuiStyle& style = ImGui::GetStyle();
    float widest = ImGui::CalcTextSize("Other files...").x;
    const std::vector<Source>& sources = library_.Sources();
    for (int r = 0, rows = SourceRows(); r < rows; ++r) {
        const Source& s = sources[(size_t)r];
        char count[16];
        std::snprintf(count, sizeof(count), "%d", s.count);
        widest = std::max(widest, ImGui::CalcTextSize(s.name.c_str()).x + style.ItemSpacing.x + ImGui::CalcTextSize(count).x);
    }
    const float row = style.WindowPadding.x * 2.0f + ImGui::GetFrameHeight() + style.ItemSpacing.x + widest;
    return Round(std::min(std::max(row, ImGui::GetFontSize() * 11.0f), screenW * 0.3f));
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
        const float left = sidebar_ ? sidebarW : 0.0f;
        const float top = ui::g_systemBarTop + bar, bottom = h - ui::g_systemBarBottom;
        action = Grid(ImVec2(left, top), ImVec2(w, bottom), w >= h, now);
        if (sidebar_) {
            const Action picked = Sidebar(ImVec2(0.0f, top), ImVec2(sidebarW, bottom), now);
            if (picked != Action::None) action = picked;
        }
        TopBar(w, bar);
        ViewOptionsPopup(w - style.WindowPadding.x, top);
    }
    ImGui::End();
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
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(width, top + height), kPanel);
    if (selecting_) {
        // As the stock tablet player's: a cross, the count, and the bin.
        if (IconButton("##cancel", Glyph::Close, ImVec2(pad, y), icon)) EndSelection();
        char count[32];
        std::snprintf(count, sizeof(count), "%d selected", (int)selected_.size());
        dl->AddText(ImVec2(pad + icon + Round(style.ItemSpacing.x), top + Round((height - ImGui::GetFontSize()) * 0.5f)),
                    ImGui::GetColorU32(ImGuiCol_Text), count);
        if (IconButton("##delete", Glyph::Delete, ImVec2(width - pad - icon, y), icon))
            provider_->Delete(std::vector<int64_t>(selected_.begin(), selected_.end()));
        return;
    }
    if (IconButton("##menu", Glyph::Menu, ImVec2(pad, y), icon)) sidebar_ = !sidebar_;
    if (IconButton("##options", Glyph::More, ImVec2(width - pad - icon, y), icon)) ImGui::OpenPopup("##view_options");
    if (folder_ != CurrentSource().folder &&
        IconButton("##up", Glyph::Back, ImVec2(pad + icon + Round(style.ItemSpacing.x), y), icon))
        Up();

    // The current source and the folder in it, "Downloads / MEDIA / ZTEPics", in a pill in
    // the middle.
    std::string title = CurrentSource().name;
    size_t i = CurrentSource().folder.size();
    for (size_t slash; (slash = folder_.find('/', i)) != std::string::npos; i = slash + 1)
        title += " / " + folder_.substr(i, slash - i);
    const std::string name = Fit(title, width - (pad + icon) * 4.0f);
    const ImVec2 text = ImGui::CalcTextSize(name.c_str());
    const float padX = Round(style.FramePadding.x);
    const float pillW = Round(text.x) + padX * 2.0f;
    const float x = Round((width - pillW) * 0.5f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + pillW, y + icon), ImGui::GetColorU32(ImGuiCol_FrameBg), icon * 0.5f);
    dl->AddText(ImVec2(x + padX, y + Round((icon - text.y) * 0.5f)), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
}

MediaBrowser::Action MediaBrowser::Sidebar(ImVec2 min, ImVec2 max, double now) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::vector<Source>& sources = library_.Sources();
    const ImVec2 size(max.x - min.x, max.y - min.y);
    dl->AddRectFilled(min, max, kPanel);
    if (size.x <= 0.0f || size.y <= 0.0f) return Action::None;

    const float rowH = Round(ImGui::GetFrameHeight() + style.ItemSpacing.y);
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
            dl->AddLine(ImVec2(min.x + pad, line), ImVec2(right, line), dim, Round(Px(1.0f)));
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
            dl->AddText(ImVec2(textX, textY), text, Fit("Other files...", right - textX).c_str());
            continue;
        }
        char count[16];
        std::snprintf(count, sizeof(count), "%d", s->count);
        const float countX = right - Round(ImGui::CalcTextSize(count).x);
        dl->AddText(ImVec2(countX, textY), dim, count);
        dl->AddText(ImVec2(textX, textY), text, Fit(s->name, countX - Round(style.ItemSpacing.x) - textX).c_str());
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
    sidebar_ = false;
    dirty_ = true;
    gridScroll_.SetOffset(0.0f);
    return Action::None;
}

MediaBrowser::Action MediaBrowser::Grid(ImVec2 min, ImVec2 max, bool landscape, double now) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const MediaAccess access = provider_->Access();
    if (access == MediaAccess::None) {
        if (Message(min, max, "Allow access to your photos and videos.", "Allow access")) provider_->RequestAccess();
        return Action::None;
    }
    if (access == MediaAccess::Denied) {
        if (Message(min, max, "Photo and video access is off.", "Open Settings")) provider_->RequestAccess();
        return Action::None;
    }
    if (access == MediaAccess::Unknown) return Action::None;
    if (access == MediaAccess::Partial) {
        const char* kSelectMore = "Select more";
        const float h = Round(ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f);
        const float pad = Round(style.WindowPadding.x);
        const float buttonX = max.x - pad - Round(ImGui::CalcTextSize(kSelectMore).x + style.FramePadding.x * 2.0f);
        dl->AddRectFilled(min, ImVec2(max.x, min.y + h), kPanel);
        dl->AddText(ImVec2(min.x + pad, min.y + Round((h - ImGui::GetFontSize()) * 0.5f)),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    Fit("Showing only the items you picked.", buttonX - Round(style.ItemSpacing.x) - min.x - pad).c_str());
        ImGui::SetCursorScreenPos(ImVec2(buttonX, min.y + Round(style.ItemSpacing.y)));
        if (ImGui::Button(kSelectMore)) provider_->RequestAccess();
        min.y += h;
    }
    if (!listed_) {
        Message(min, max, "Loading...", nullptr);
        return Action::None;
    }

    // Square tiles, landscape 4 columns beside the sidebar and 5 without, portrait 2 and 3.
    const int cols = landscape ? (sidebar_ ? 4 : 5) : (sidebar_ ? 2 : 3);
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const float gutter = Round(Px(kGutter));
    const float tile = std::floor((size.x - gutter * (cols - 1)) / cols);
    if (tile <= 0.0f || size.y <= 0.0f) return Action::None;
    const float step = tile + gutter;
    const float x0 = min.x + Round((size.x - (cols * step - gutter)) * 0.5f);
    const size_t nf = folders_.size();
    const size_t n = nf + shown_.size();  // tiles: the folders, then the items
    const int rows = (int)((n + cols - 1) / cols);
    if (cols != columns_ && columns_ > 0 && step_ > 0.0f) {
        // A reflow (rotation, the sidebar): keep the top row's first item in the top row.
        const size_t first = (size_t)(gridScroll_.Offset() / step_) * columns_;
        gridScroll_.SetLimit(rows * step - size.y);
        gridScroll_.SetOffset((float)(first / cols) * step);
    }
    columns_ = cols;
    step_ = step;
    gridScroll_.SetSlop(Px(kSlop));
    gridScroll_.SetLimit(rows * step - size.y);
    bool tap = false;
    if (n > 0) {
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##grid", size);
        tap = Track(gridScroll_, gridPress_, now);
    }
    const float offset = Round(gridScroll_.Offset());

    // The visible rows, and a few more either side as far as the slots go.
    const int firstRow = (int)(offset / step);
    const int lastRow = std::min(rows - 1, (int)((offset + size.y) / step));
    const size_t visFirst = (size_t)firstRow * cols;
    const size_t visEnd = std::min(n, (size_t)(lastRow + 1) * cols);
    const int spare = (int)atlas_.SlotCount() - (lastRow - firstRow + 1) * cols;
    const int extra = std::max(0, std::min(2, spare / (2 * cols)));
    const size_t winFirst = (size_t)std::max(0, firstRow - extra) * cols;
    const size_t winEnd = std::min(n, (size_t)(lastRow + 1 + extra) * cols);

    const std::vector<MediaItem>& items = library_.Items();
    std::unordered_set<int64_t> window;
    for (size_t p = std::max(winFirst, nf); p < winEnd; ++p) {
        const int64_t id = items[shown_[p - nf]].id;
        window.insert(id);
        int slot = 0;
        cache_.Use(id, slot);  // so a new thumbnail never takes a slot the window still shows
    }
    UploadReady(window);

    // Ask for what the window lacks: the visible rows, then those below, then above. Under a
    // depth filter also for the next few items with no verdict, so they can be sorted in.
    std::vector<ThumbnailRequest> want;
    auto ask = [&](const MediaItem& item) {
        if (cache_.Contains(item.id) || pending_.count(item.id) || noThumbnail_.count(item.id)) return;
        want.push_back(ThumbnailRequest{item.id, classifier_.NeedsFileCheck(item)});
    };
    for (size_t p = std::max(visFirst, nf); p < winEnd; ++p) ask(items[shown_[p - nf]]);
    for (size_t p = visFirst; p > std::max(winFirst, nf); --p) ask(items[shown_[p - 1 - nf]]);
    for (size_t k = 0; k < unknown_.size() && k < kScanBatch; ++k) ask(items[unknown_[k]]);
    if (!Same(want, wanted_)) {
        provider_->WantThumbnails(want, atlas_.Tile());
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
        Message(min, max, text, nullptr);
        return Action::None;
    }

    // The item under a point, false over a gutter or past the end.
    auto itemAt = [&](ImVec2 pt, size_t& pos) {
        const float x = pt.x - x0, y = pt.y - min.y + offset;
        if (x < 0.0f || pt.y < min.y) return false;
        const int c = (int)(x / step), r = (int)(y / step);
        if (c >= cols || x - c * step > tile || y - r * step > tile) return false;
        pos = (size_t)r * cols + c;
        return pos < n;
    };
    size_t pressed = n;
    if (gridScroll_.Pressed() && !gridScroll_.Dragging() && !itemAt(gridPress_, pressed)) pressed = n;
    // A long press on an item starts select mode with it. In select mode it is a tap.
    if (pressed >= nf && pressed < n && !selecting_ && provider_->CanDelete() && gridScroll_.TakeLongPress(now)) {
        selecting_ = true;
        selected_ = {items[shown_[pressed - nf]].id};
    }

    const float margin = Round(Px(6.0f));
    const float badgeGap = Round(Px(4.0f));
    const float box = Round(ImGui::GetFontSize() * 1.2f);
    dl->PushClipRect(min, max, true);
    for (size_t p = visFirst; p < visEnd; ++p) {
        const ImVec2 a(x0 + (float)(p % cols) * step, min.y + (float)(p / cols) * step - offset);
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
            ImVec2 uv0, uv1;
            Crop(thumb, verdict.layout, uv0, uv1);
            dl->AddImage(thumb.texture, a, b, uv0, uv1);
        } else {
            dl->AddRectFilled(a, b, kTileEmpty);
            if (noThumbnail_.count(item.id)) {
                ui::DrawGlyph(dl, item.type == MediaType::Video ? Glyph::Video : Glyph::Image,
                         ImVec2(a.x + tile * 0.5f, a.y + tile * 0.5f), tile * 0.3f,
                         ImGui::GetColorU32(ImGuiCol_TextDisabled));
            }
        }
        // Badges, top right: the length of a video, "3D" to its left, then the format of a
        // file the player cannot open.
        float right = b.x - margin;
        if (item.type == MediaType::Video && item.durationMs > 0) {
            char length[24];
            Duration(item.durationMs, length, sizeof(length));
            right = Badge(dl, right, a.y + margin, length) - badgeGap;
        }
        if (verdict.known && verdict.stereo) right = Badge(dl, right, a.y + margin, "3D") - badgeGap;
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
    dl->PopClipRect();

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

bool MediaBrowser::CanGoBack() const { return provider_ && (selecting_ || folder_ != CurrentSource().folder); }

bool MediaBrowser::Back() {
    if (!CanGoBack()) return false;
    if (selecting_) {
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

void MediaBrowser::UploadReady(const std::unordered_set<int64_t>& window) {
    bool budget = true;
    for (auto it = ready_.begin(); it != ready_.end();) {
        const Thumbnail& t = *it;
        const bool wanted = window.count(t.id) != 0;
        const bool fits = t.width > 0 && t.height > 0 && t.width <= atlas_.Tile() && t.height <= atlas_.Tile();
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
        if (!atlas_.Upload(slot, t.rgba.data(), t.width, t.height)) {
            cache_.Remove(t.id);  // this frame's uploads are spent: the rest wait for the next
            budget = false;
            ++it;
            continue;
        }
        pending_.erase(t.id);
        it = ready_.erase(it);
    }
}

void MediaBrowser::ViewOptionsPopup(float right, float top) {
    ImGui::SetNextWindowPos(ImVec2(right, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    if (!ImGui::BeginPopup("##view_options")) return;
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, Round(style.ItemSpacing.y * 0.5f)));
    const float rowH = Round(ImGui::GetFontSize() + style.FramePadding.y);
    bool changed = false;
    // A finger-sized row that keeps the popup open, so several options can be set.
    auto option = [&](const char* label, bool selected) {
        const bool picked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_DontClosePopups,
                                              ImVec2(0.0f, rowH));
        changed = changed || picked;
        return picked;
    };
    ImGui::TextDisabled("Show");
    if (option("3D and 2D", depth_ == DepthFilter::Both)) depth_ = DepthFilter::Both;
    if (option("3D only", depth_ == DepthFilter::Only3D)) depth_ = DepthFilter::Only3D;
    if (option("2D only", depth_ == DepthFilter::Only2D)) depth_ = DepthFilter::Only2D;
    ImGui::Separator();
    ImGui::TextDisabled("Type");
    if (option("Photos and videos", options_.type == TypeFilter::All)) options_.type = TypeFilter::All;
    if (option("Videos", options_.type == TypeFilter::Videos)) options_.type = TypeFilter::Videos;
    if (option("Photos", options_.type == TypeFilter::Images)) options_.type = TypeFilter::Images;
    ImGui::Separator();
    ImGui::TextDisabled("Sort by date");
    if (option("Newest first", options_.order == SortOrder::NewestFirst)) options_.order = SortOrder::NewestFirst;
    if (option("Oldest first", options_.order == SortOrder::OldestFirst)) options_.order = SortOrder::OldestFirst;
    ImGui::Separator();
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
