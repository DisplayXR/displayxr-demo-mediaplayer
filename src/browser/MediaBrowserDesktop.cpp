// SPDX-License-Identifier: Apache-2.0
//
// The media browser's desktop look (#252, docs/BROWSER.md "Desktop"): a header with a
// clickable path, a toolbar with the filters and the sort, a compact sidebar, and tiles with
// a caption under the picture. Mouse and keyboard first; sizes in design units (ui::Px), the
// type in the desktop scale (ui::DeskText). The touch look stays in MediaBrowser.cpp.
#include "browser/MediaBrowser.h"

#include "ui/Icons.h"
#include "ui/TransportUI.h"
#include "ui/UiScale.h"
#include "ui/UiText.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace mp::browser {

namespace {

using ui::DeskText;
using ui::Glyph;
using ui::Px;

float Round(float v) { return std::floor(v + 0.5f); }

// Draws in a desktop type size while it lives (the UI font where the scale is not built).
struct DeskFont {
    explicit DeskFont(DeskText style) { ImGui::PushFont(ui::DeskFont(style)); }
    ~DeskFont() { ImGui::PopFont(); }
    DeskFont(const DeskFont&) = delete;
    DeskFont& operator=(const DeskFont&) = delete;
};

// The palette: the player's dark HUD, plus a few tints of it.
const ImU32 kPanel = IM_COL32(23, 26, 33, 255);
const ImU32 kToolbar = IM_COL32(13, 15, 20, 255);    // the browser's background
const ImU32 kSurface = IM_COL32(28, 32, 41, 255);      // a picture not here yet, a folder
const ImU32 kHoverFill = IM_COL32(255, 255, 255, 15);  // 6%
const ImU32 kPressFill = IM_COL32(255, 255, 255, 26);  // 10%
const ImU32 kLine = IM_COL32(255, 255, 255, 20);       // 8%
const ImU32 kLineStrong = IM_COL32(255, 255, 255, 41);  // 16%
const ImU32 kText = IM_COL32(235, 240, 247, 255);
const ImU32 kTextDim = IM_COL32(170, 176, 186, 255);
const ImU32 kTextFaint = IM_COL32(110, 117, 129, 255);
const ImU32 kAccent = IM_COL32(51, 166, 255, 255);
const ImU32 kAccentWeak = IM_COL32(51, 166, 255, 41);
const ImU32 kOnAccent = IM_COL32(6, 18, 29, 255);
const ImU32 kBadgeFill = IM_COL32(8, 10, 14, 184);
const ImU32 kWarn = IM_COL32(232, 176, 75, 255);
const ImU32 kWarnFill = IM_COL32(232, 176, 75, 51);
const ImU32 kFocus = IM_COL32(235, 240, 247, 191);

Glyph DeskSourceIcon(SourceKind kind) {
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

// A 28 x 28 icon button: hover and press fills, the glyph dim unless hovered. `enabled`
// false: drawn faint, never clicked.
bool IconBtn(const char* id, Glyph icon, ImVec2 pos, float size, bool enabled = true, const char* tip = nullptr) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size, pos.y + size);
    if (enabled && ImGui::IsItemActive()) dl->AddRectFilled(pos, b, kPressFill, Px(6.0f));
    else if (hovered) dl->AddRectFilled(pos, b, kHoverFill, Px(6.0f));
    // A glyph spans about 60% of its box: a box of 1.0 x the button draws it ~17 px at 28.
    ui::DrawGlyph(dl, icon, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.95f,
                  !enabled ? IM_COL32(110, 117, 129, 128) : hovered ? kText : kTextDim);
    if (hovered && tip) ImGui::SetTooltip("%s", tip);
    return clicked && enabled;
}

// A small rounded badge with `text`, its left edge at `x` (`right` false) or its right edge
// (`right` true). Returns its other edge.
float Badge(ImDrawList* dl, float x, float top, const char* text, ImU32 fill, ImU32 ink, bool right,
            ImU32 outline = 0) {
    DeskFont f(DeskText::Badge);
    const float padX = Round(Px(5.0f)), h = Round(Px(18.0f));
    const float w = Round(ImGui::CalcTextSize(text).x) + padX * 2.0f;
    const float x0 = right ? x - w : x;
    dl->AddRectFilled(ImVec2(x0, top), ImVec2(x0 + w, top + h), fill, Px(4.0f));
    if (outline) dl->AddRect(ImVec2(x0, top), ImVec2(x0 + w, top + h), outline, Px(4.0f), 0, Px(1.0f));
    dl->AddText(ImVec2(x0 + padX, top + Round((h - ImGui::GetFontSize()) * 0.5f)), ink, text);
    return right ? x0 : x0 + w;
}

void Duration(int64_t ms, char* out, size_t n) {
    const int s = (int)(ms / 1000);
    if (s >= 3600) std::snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else std::snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

std::string Upper(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return std::string();
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = (char)std::toupper((unsigned char)c);
    return ext;
}

// "14 Aug 2025", local time; "-" when unknown.
std::string ShortDate(int64_t ms) {
    if (ms <= 0) return "-";
    const std::time_t t = (std::time_t)(ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0) return "-";
#else
    if (!localtime_r(&t, &tm)) return "-";
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%d %b %Y", &tm) == 0) return "-";
    return buf[0] == '0' ? std::string(buf + 1) : std::string(buf);
}

std::string FileSize(int64_t bytes) {
    if (bytes <= 0) return "-";
    char buf[32];
    if (bytes >= (int64_t)1 << 30) std::snprintf(buf, sizeof(buf), "%.1f GB", (double)bytes / (1 << 30));
    else if (bytes >= (int64_t)1 << 20) std::snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1 << 20));
    else std::snprintf(buf, sizeof(buf), "%.0f KB", std::max(1.0, (double)bytes / 1024.0));
    return buf;
}

// A segmented control of icons at (x, y): `count` buttons `w` x `h`, `on` the lit one.
// Returns the one clicked, or -1.
int IconSegments(const char* id, const Glyph* icons, const char* const* tips, int count, int on, float x, float y,
                 float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = Px(6.0f);
    int clicked = -1;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        const float bx = x + w * i;
        ImGui::SetCursorScreenPos(ImVec2(bx, y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(w, h))) clicked = i;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImDrawFlags corners = i == 0           ? ImDrawFlags_RoundCornersLeft
                                    : i == count - 1 ? ImDrawFlags_RoundCornersRight
                                                     : ImDrawFlags_RoundCornersNone;
        if (i == on) dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + w, y + h), kAccentWeak, r, corners);
        else if (hovered) dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + w, y + h), kHoverFill, r, corners);
        if (i > 0) dl->AddLine(ImVec2(bx, y + 1.0f), ImVec2(bx, y + h - 1.0f), kLine, Px(1.0f));
        ui::DrawGlyph(dl, icons[i], ImVec2(bx + w * 0.5f, y + h * 0.5f), h * 0.85f,
                      i == on ? kAccent : hovered ? kText : kTextDim);
        if (hovered && tips) ImGui::SetTooltip("%s", tips[i]);
    }
    ImGui::PopID();
    dl->AddRect(ImVec2(x, y), ImVec2(x + w * count, y + h), kLineStrong, r, 0, Px(1.0f));
    return clicked;
}

} // namespace

float MediaBrowser::DeskCaptionHeight() {
    return Round(Px(6.0f) + ui::DeskTextPx(DeskText::Label) + Px(3.0f) + ui::DeskTextPx(DeskText::Meta) + Px(4.0f));
}

// The header: Up, the path (each part clickable), and the close button. Below 600 px wide a
// menu button opens the sources' drawer first.
void MediaBrowser::DeskHeader(float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float btn = Round(Px(28.0f)), pad = Round(Px(8.0f)), gap = Round(Px(4.0f));
    const float y = Round((height - btn) * 0.5f);
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(width, height), kPanel);
    dl->AddLine(ImVec2(0.0f, height - 0.5f), ImVec2(width, height - 0.5f), kLine, Px(1.0f));
    float x = pad;
    if (!rail_) {
        if (IconBtn("##menu", Glyph::Menu, ImVec2(x, y), btn, true, "Sources")) drawer_ = true;
        x += btn + gap;
    }
    const Source& source = CurrentSource();
    const bool canUp = folder_ != source.folder;
    if (IconBtn("##back", Glyph::Back, ImVec2(x, y), btn, !back_.empty(), "Back (Alt+Left)")) HistoryStep(-1);
    x += btn + gap;
    if (IconBtn("##forward", Glyph::Forward, ImVec2(x, y), btn, !forward_.empty(), "Forward (Alt+Right)"))
        HistoryStep(1);
    x += btn + gap;
    if (IconBtn("##up", Glyph::Up, ImVec2(x, y), btn, canUp, "Up a folder (Backspace)")) Up();
    x += btn + gap;
    dl->AddLine(ImVec2(x + gap, y + Px(4.0f)), ImVec2(x + gap, y + btn - Px(4.0f)), kLine, Px(1.0f));
    x += gap * 3.0f;

    const float closeX = width - pad - btn;
    if (IconBtn("##close", Glyph::Close, ImVec2(closeX, y), btn, true, "Close (Esc)")) closeRequested_ = true;
    // The search box: 240 px, 160 in a narrower window, none in a narrow one (#252 design).
    const float searchW = width >= Px(900.0f) ? Round(Px(240.0f)) : width >= Px(640.0f) ? Round(Px(160.0f)) : 0.0f;
    float crumbEnd = closeX - gap;
    if (searchW > 0.0f) crumbEnd = closeX - Round(Px(12.0f)) - searchW - Round(Px(12.0f));

    // The path: the source, then each folder down to this one. A part's folder is found by
    // walking Parent() up from here, so it is right for every source mode.
    std::vector<std::string> paths;  // folder per level, top first
    {
        std::string f = folder_;
        for (int guard = 0; guard < 64 && f != source.folder; ++guard) {
            paths.insert(paths.begin(), f);
            const std::string up = library_.Parent(source, f);
            if (up == f) break;
            f = up;
        }
        paths.insert(paths.begin(), source.folder);
    }
    std::vector<std::string> labels = {source.name};
    for (const std::string& part : library_.Breadcrumb(source, folder_)) labels.push_back(part);
    if (labels.size() != paths.size()) {  // a breadcrumb the walk does not match: show it, not clickable
        paths.assign(labels.size(), folder_);
    }
    const float room = crumbEnd - x;
    const float segPad = Round(Px(8.0f)), chev = Round(Px(14.0f));
    auto segW = [&](size_t i) {
        DeskFont f(i + 1 == labels.size() ? DeskText::Title : DeskText::Body);
        return Round(ImGui::CalcTextSize(labels[i].c_str()).x) + segPad * 2.0f;
    };
    // Too long: the parts between the first and the last become "...", then only the last
    // is left (cut to fit).
    std::vector<size_t> shown;
    for (size_t i = 0; i < labels.size(); ++i) shown.push_back(i);
    auto total = [&] {
        float t = 0.0f;
        for (size_t k = 0; k < shown.size(); ++k) t += (shown[k] == SIZE_MAX ? Px(28.0f) : segW(shown[k])) + (k ? chev : 0.0f);
        return t;
    };
    if (shown.size() > 2 && total() > room) shown = {0, SIZE_MAX, labels.size() - 1};
    if (shown.size() > 1 && total() > room) shown = {labels.size() - 1};
    const float segH = Round(Px(28.0f));
    for (size_t k = 0; k < shown.size(); ++k) {
        if (k) {
            // A chevron between the parts.
            const float cx = x + chev * 0.5f, cy = y + btn * 0.5f, c = Px(3.0f);
            const ImVec2 v[] = {ImVec2(cx - c * 0.5f, cy - c), ImVec2(cx + c * 0.5f, cy), ImVec2(cx - c * 0.5f, cy + c)};
            dl->AddPolyline(v, 3, kTextFaint, 0, Px(1.5f));
            x += chev;
        }
        if (shown[k] == SIZE_MAX) {
            DeskFont f(DeskText::Body);
            dl->AddText(ImVec2(x + Px(6.0f), y + Round((btn - ImGui::GetFontSize()) * 0.5f)), kTextFaint, "...");
            x += Px(28.0f);
            continue;
        }
        const size_t i = shown[k];
        const bool last = i + 1 == labels.size();
        float w = segW(i);
        if (x + w > crumbEnd) w = std::max(0.0f, crumbEnd - x);
        if (w <= segPad * 2.0f) break;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID((int)i);
        const bool clicked = ImGui::InvisibleButton("##crumb", ImVec2(w, segH));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered && !last) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + segH), kHoverFill, Px(4.0f));
        DeskFont f(last ? DeskText::Title : DeskText::Body);
        const std::string label = ui::FitText(labels[i], w - segPad * 2.0f);
        dl->AddText(ImVec2(x + segPad, y + Round((segH - ImGui::GetFontSize()) * 0.5f)),
                    last ? kText : hovered ? kText : kTextDim, label.c_str());
        if (clicked && !last) GoTo(paths[i]);
        x += w;
    }
    if (searchW > 0.0f) DeskSearch(ImVec2(closeX - Round(Px(12.0f)) - searchW, y), searchW, btn, labels.back());
}

// The search box: a magnifier, "Search <here>", and Ctrl+F's key cap while it is empty. It
// filters the place shown by name; Esc clears it.
void MediaBrowser::DeskSearch(ImVec2 pos, float w, float h, const std::string& place) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DeskFont f(DeskText::Label);
    const float icon = Round(Px(16.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kLineStrong));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(kTextFaint));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, Px(1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(Px(10.0f) + icon + Px(6.0f), std::max(0.0f, Round((h - ImGui::GetFontSize()) * 0.5f))));
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemWidth(w);
    if (focusSearch_) {
        ImGui::SetKeyboardFocusHere();
        focusSearch_ = false;
    }
    const std::string hint = ui::FitText("Search " + place, w - Px(10.0f) - icon - Px(6.0f) - Px(64.0f));
    ImGui::SetNextItemAllowOverlap();  // the clear button lies on it
    if (ImGui::InputTextWithHint("##search", hint.c_str(), search_, sizeof(search_),
                                 ImGuiInputTextFlags_EscapeClearsAll)) {
        query_ = search_;
        dirty_ = true;
        gridScroll_.SetOffset(0.0f);
    }
    const bool active = ImGui::IsItemActive();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ui::DrawGlyph(dl, Glyph::Search, ImVec2(pos.x + Px(10.0f) + icon * 0.5f, pos.y + h * 0.5f), icon * 1.2f,
                  active ? kText : kTextFaint);
    if (search_[0]) {
        // A cross that clears it.
        const float bw = h;
        ImGui::SetCursorScreenPos(ImVec2(pos.x + w - bw, pos.y));
        if (ImGui::InvisibleButton("##search_clear", ImVec2(bw, h))) {
            search_[0] = '\0';
            query_.clear();
            dirty_ = true;
        }
        const bool hov = ImGui::IsItemHovered();
        ui::DrawGlyph(dl, Glyph::Close, ImVec2(pos.x + w - bw * 0.5f, pos.y + h * 0.5f), icon, hov ? kText : kTextFaint);
        if (hov) ImGui::SetTooltip("Clear the search (Esc)");
    } else if (!active) {
        DeskFont k(DeskText::Badge);
        const char* key = "Ctrl+F";
        const ImVec2 ks = ImGui::CalcTextSize(key);
        const float kw = ks.x + Px(8.0f), kh = ks.y + Px(4.0f);
        const float kx = pos.x + w - Px(8.0f) - kw, ky = pos.y + Round((h - kh) * 0.5f);
        dl->AddRect(ImVec2(kx, ky), ImVec2(kx + kw, ky + kh), kLineStrong, Px(3.0f), 0, Px(1.0f));
        dl->AddText(ImVec2(kx + Px(4.0f), ky + Px(2.0f)), kTextFaint, key);
    }
}

// The toolbar: All / 3D / 2D, the type (all, videos, photos), the sort, and the count.
void MediaBrowser::DeskToolbar(ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, kToolbar);  // opaque: flat over the 3D grid too
    dl->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), kLine, Px(1.0f));
    const float h = Round(Px(28.0f)), r = Px(6.0f);
    const float y = Round((min.y + max.y - h) * 0.5f);
    float x = min.x + Round(Px(12.0f));
    float right = max.x - Round(Px(12.0f));
    DeskFont font(DeskText::Label);

    // At the right: the view (grid, details), and for the grid the cards' size.
    {
        const Glyph icons[] = {Glyph::Grid, Glyph::List};
        const char* tips[] = {"Grid", "Details"};
        const float w = h + Round(Px(4.0f));
        const float vx = right - w * 2.0f;
        const int picked = IconSegments("##view", icons, tips, 2, details_ ? 1 : 0, vx, y, w, h);
        if (picked >= 0) details_ = picked == 1;
        right = vx - Round(Px(12.0f));
        const float icon = Round(Px(16.0f)), track = Round(Px(72.0f)), gap = Round(Px(6.0f));
        const float total = icon + gap + track + gap + icon;
        if (!details_ && right - total >= min.x + Px(440.0f)) {
            const float sx = right - total, cy = y + h * 0.5f;
            ui::DrawGlyph(dl, Glyph::Grid, ImVec2(sx + icon * 0.5f, cy), icon * 0.8f, kTextFaint);
            ui::DrawGlyph(dl, Glyph::Grid, ImVec2(sx + total - icon * 0.5f, cy), icon * 1.25f, kTextFaint);
            const float tx0 = sx + icon + gap, tx1 = tx0 + track;
            ImGui::SetCursorScreenPos(ImVec2(tx0 - Px(6.0f), y));
            ImGui::InvisibleButton("##card_size", ImVec2(track + Px(12.0f), h));
            const bool held = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
            if (held) {
                const float f = std::min(1.0f, std::max(0.0f, (ImGui::GetIO().MousePos.x - tx0) / track));
                cardW_ = kCardMin + (kCardMax - kCardMin) * f;
            }
            if (hov || held) ImGui::SetTooltip("Thumbnail size (Ctrl+wheel)");
            const float f = (std::min(kCardMax, std::max(kCardMin, cardW_)) - kCardMin) / (kCardMax - kCardMin);
            const float th = Px(4.0f), kx = tx0 + track * f;
            dl->AddRectFilled(ImVec2(tx0, cy - th * 0.5f), ImVec2(tx1, cy + th * 0.5f), kLineStrong, th * 0.5f);
            dl->AddRectFilled(ImVec2(tx0, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), kAccent, th * 0.5f);
            dl->AddCircleFilled(ImVec2(kx, cy), Px(held || hov ? 7.0f : 6.0f), kText);
            right = sx - Round(Px(12.0f));
        }
    }

    // All · 3D · 2D: a segmented control.
    {
        const char* labels[] = {"All", "3D", "2D"};
        const DepthFilter values[] = {DepthFilter::Both, DepthFilter::Only3D, DepthFilter::Only2D};
        const char* tips[] = {"Every file", "3D photos and videos (the panel goes 3D)", "2D photos and videos"};
        const float x0 = x;
        for (int i = 0; i < 3; ++i) {
            const float w = Round(ImGui::CalcTextSize(labels[i]).x + Px(10.0f) * 2.0f);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##depth", ImVec2(w, h))) SetDepth(values[i]);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = depth_ == values[i];
            const ImDrawFlags corners =
                i == 0 ? ImDrawFlags_RoundCornersLeft : i == 2 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
            if (on) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kAccentWeak, r, corners);
            else if (hovered) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kHoverFill, r, corners);
            if (i > 0) dl->AddLine(ImVec2(x, y + 1.0f), ImVec2(x, y + h - 1.0f), kLine, Px(1.0f));
            const float tw = ImGui::CalcTextSize(labels[i]).x;
            dl->AddText(ImVec2(Round(x + (w - tw) * 0.5f), y + Round((h - ImGui::GetFontSize()) * 0.5f)),
                        on ? kAccent : hovered ? kText : kTextDim, labels[i]);
            if (hovered) ImGui::SetTooltip("%s", tips[i]);
            x += w;
        }
        dl->AddRect(ImVec2(x0, y), ImVec2(x, y + h), kLineStrong, r, 0, Px(1.0f));
    }
    x += Round(Px(8.0f));

    // The type: icons, all / videos / photos.
    {
        const Glyph icons[] = {Glyph::AllMedia, Glyph::Video, Glyph::Image};
        const TypeFilter values[] = {TypeFilter::All, TypeFilter::Videos, TypeFilter::Images};
        const char* tips[] = {"Photos and videos", "Videos only", "Photos only"};
        const float w = h + Round(Px(4.0f));
        if (x + w * 3.0f <= right) {
            const float x0 = x;
            for (int i = 0; i < 3; ++i) {
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                ImGui::PushID(10 + i);
                if (ImGui::InvisibleButton("##type", ImVec2(w, h))) SetType(values[i]);
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool on = options_.type == values[i];
                const ImDrawFlags corners = i == 0   ? ImDrawFlags_RoundCornersLeft
                                            : i == 2 ? ImDrawFlags_RoundCornersRight
                                                     : ImDrawFlags_RoundCornersNone;
                if (on) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kAccentWeak, r, corners);
                else if (hovered) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kHoverFill, r, corners);
                if (i > 0) dl->AddLine(ImVec2(x, y + 1.0f), ImVec2(x, y + h - 1.0f), kLine, Px(1.0f));
                ui::DrawGlyph(dl, icons[i], ImVec2(x + w * 0.5f, y + h * 0.5f), h * 0.85f,
                              on ? kAccent : hovered ? kText : kTextDim);
                if (hovered) ImGui::SetTooltip("%s", tips[i]);
                x += w;
            }
            dl->AddRect(ImVec2(x0, y), ImVec2(x, y + h), kLineStrong, r, 0, Px(1.0f));
            x += Round(Px(8.0f));
        }
    }

    // The sort, as a drop-down: "Sort: Newest first".
    {
        const char* order = options_.order == SortOrder::NewestFirst ? "Newest first" : "Oldest first";
        char label[64];
        std::snprintf(label, sizeof(label), "Sort: %s", order);
        const float chev = Round(Px(8.0f));
        const float w = Round(ImGui::CalcTextSize(label).x + Px(10.0f) * 2.0f + chev + Px(6.0f));
        if (x + w <= right) {
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (ImGui::InvisibleButton("##sort", ImVec2(w, h))) {
                sortMenuAt_ = ImVec2(x, y + h + Px(4.0f));
                ImGui::OpenPopup("##sort_menu");
            }
            const bool hovered = ImGui::IsItemHovered() || ImGui::IsPopupOpen("##sort_menu");
            if (hovered) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kHoverFill, r);
            dl->AddText(ImVec2(x + Px(10.0f), y + Round((h - ImGui::GetFontSize()) * 0.5f)), hovered ? kText : kTextDim,
                        label);
            const float cx = x + w - Px(10.0f) - chev * 0.5f, cy = y + h * 0.5f;
            const ImVec2 v[] = {ImVec2(cx - chev * 0.5f, cy - chev * 0.25f), ImVec2(cx, cy + chev * 0.25f),
                                ImVec2(cx + chev * 0.5f, cy - chev * 0.25f)};
            dl->AddPolyline(v, 3, hovered ? kText : kTextDim, 0, Px(1.5f));
            if (!ViewOptionsAreDefault(options_, showUnsupported_))
                dl->AddCircleFilled(ImVec2(x + w - Px(3.0f), y + Px(3.0f)), Px(3.0f), kAccent);
            x += w + Round(Px(8.0f));
        }
    }

    // The count, right-aligned: what the grid holds, and the scan under a depth filter.
    {
        DeskFont meta(DeskText::Meta);
        char count[96];
        const size_t items = shown_.size();
        if (!unknown_.empty())
            std::snprintf(count, sizeof(count), "%zu %s so far  -  checking %zu more...", items,
                          depth_ == DepthFilter::Only3D ? "in 3D" : "in 2D", unknown_.size());
        else if (!folders_.empty() && items == 0)
            std::snprintf(count, sizeof(count), "%zu folder%s", folders_.size(), folders_.size() == 1 ? "" : "s");
        else if (!folders_.empty())
            std::snprintf(count, sizeof(count), "%zu folder%s  \xC2\xB7  %zu item%s", folders_.size(),
                          folders_.size() == 1 ? "" : "s", items, items == 1 ? "" : "s");
        else
            std::snprintf(count, sizeof(count), "%zu item%s", items, items == 1 ? "" : "s");
        const float tw = ImGui::CalcTextSize(count).x;
        if (right - tw >= x) dl->AddText(ImVec2(right - tw, y + Round((h - ImGui::GetFontSize()) * 0.5f)), kTextFaint, count);
    }
}

void MediaBrowser::DeskSortMenu() {
    ImGui::SetNextWindowPos(sortMenuAt_, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(4.0f), Px(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Px(6.0f));
    const bool open = ImGui::BeginPopup("##sort_menu");
    ImGui::PopStyleVar(2);
    if (!open) return;
    {
        DeskFont f(DeskText::Label);
        const float w = Round(Px(200.0f)), rowH = Round(Px(28.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        auto pick = [&](const char* label, SortOrder o) {
            if (ImGui::Selectable(label, options_.order == o, 0, ImVec2(w, rowH)) && options_.order != o) {
                options_.order = o;
                dirty_ = true;
                gridScroll_.SetOffset(0.0f);
            }
        };
        pick("Newest first", SortOrder::NewestFirst);
        pick("Oldest first", SortOrder::OldestFirst);
        ImGui::Separator();
        if (ImGui::Selectable(showUnsupported_ ? "Hide unsupported files" : "Show unsupported files", false, 0,
                              ImVec2(w, rowH))) {
            showUnsupported_ = !showUnsupported_;
            if (!showUnsupported_ && source_ >= (size_t)SourceRows()) {
                source_ = 0;
                folder_ = CurrentSource().folder;
            }
            dirty_ = true;
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndPopup();
}

// The sources: LIBRARY, then PINNED (each pinned folder a row) with "Add folder...", and
// "Open file..." at the foot.
MediaBrowser::Action MediaBrowser::DeskSidebar(ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::vector<Source>& sources = library_.Sources();
    dl->AddRectFilled(min, max, kPanel);
    dl->AddLine(ImVec2(max.x - 0.5f, min.y), ImVec2(max.x - 0.5f, max.y), kLine, Px(1.0f));
    if (max.x - min.x <= 0.0f || max.y - min.y <= 0.0f) return Action::None;

    const float pad = Round(Px(8.0f)), rowH = Round(Px(32.0f)), headH = Round(Px(28.0f));
    const float icon = Round(Px(16.0f)), r = Px(6.0f);
    const int rows = SourceRows();
    const float footH = rowH + pad * 2.0f;
    const float listBottom = max.y - footH;

    // The rows, in order: kind 0 = a heading, 1 = a source, 2 = Add folder, 3 = a pinned
    // folder (`source` indexes pinnedRows_).
    struct Entry {
        int kind;
        int source;
        const char* label;
    };
    std::vector<Entry> entries;
    entries.push_back({0, -1, "LIBRARY"});
    bool pinnedHeading = false;
    for (int i = 0; i < rows; ++i) {
        if (sources[(size_t)i].kind == SourceKind::Pinned) {
            if (!pinnedHeading) entries.push_back({0, -1, "PINNED"});
            pinnedHeading = true;
            // Its folders, one by one, in place of one "Pinned" row (the path still names
            // "Pinned", and its top lists them).
            if ((size_t)i == pinnedSource_) {
                for (size_t j = 0; j < pinnedRows_.size(); ++j) entries.push_back({3, (int)j, nullptr});
                continue;
            }
        }
        entries.push_back({1, i, nullptr});
    }
    if (!pinnedHeading) entries.push_back({0, -1, "PINNED"});
    entries.push_back({2, -1, "Add folder..."});

    float contentH = pad;
    for (const Entry& e : entries) contentH += e.kind == 0 ? headH : rowH;
    sidebarScroll_.SetLimit(contentH + pad - (listBottom - min.y));
    // The wheel scrolls the list. A hit test, not a button under the rows: an item
    // submitted first takes the hover, and the rows under it could not be clicked.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, ImVec2(max.x, listBottom)) &&
        ImGui::GetIO().MouseWheel != 0.0f)
        sidebarScroll_.SetOffset(sidebarScroll_.Offset() - ImGui::GetIO().MouseWheel * rowH * 2.0f);
    const float offset = Round(sidebarScroll_.Offset());

    Action action = Action::None;
    dl->PushClipRect(min, ImVec2(max.x, listBottom), true);
    float y = min.y + pad - offset;
    for (size_t k = 0; k < entries.size(); ++k) {
        const Entry& e = entries[k];
        if (e.kind == 0) {
            DeskFont f(DeskText::Badge);
            dl->AddText(ImVec2(min.x + pad + Px(10.0f), y + headH - ImGui::GetFontSize() - Px(6.0f)), kTextFaint, e.label);
            y += headH;
            continue;
        }
        const ImVec2 a(min.x + pad, y), b(max.x - pad, y + rowH);
        y += rowH;
        if (b.y < min.y || a.y > listBottom) continue;
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID((int)k);
        const bool clicked = ImGui::InvisibleButton("##src", ImVec2(b.x - a.x, rowH));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();
        const Folder* pin = e.kind == 3 ? &pinnedRows_[(size_t)e.source] : nullptr;
        // A pinned row is lit while its folder, or one inside it, is shown.
        const bool on = (e.kind == 1 && (size_t)e.source == source_) ||
                        (pin && source_ == pinnedSource_ && folder_.compare(0, pin->path.size(), pin->path) == 0);
        if (on) {
            dl->AddRectFilled(a, b, kAccentWeak, r);
            dl->AddRectFilled(ImVec2(a.x, a.y + Px(8.0f)), ImVec2(a.x + Px(3.0f), b.y - Px(8.0f)), kAccent, Px(2.0f));
        } else if (held) {
            dl->AddRectFilled(a, b, kPressFill, r);
        } else if (hovered) {
            dl->AddRectFilled(a, b, kHoverFill, r);
        }
        const Glyph glyph = e.kind == 2 ? Glyph::Folder : pin ? Glyph::Pin : DeskSourceIcon(sources[(size_t)e.source].kind);
        const float textX = a.x + Px(10.0f) + icon + Px(10.0f);
        ui::DrawGlyph(dl, glyph, ImVec2(a.x + Px(10.0f) + icon * 0.5f, a.y + rowH * 0.5f), icon * 1.6f,
                      on ? kAccent : hovered ? kText : kTextDim, on);
        DeskFont f(DeskText::Body);
        const float textY = a.y + Round((rowH - ImGui::GetFontSize()) * 0.5f);
        if (e.kind == 2) {
            dl->AddText(ImVec2(textX, textY), hovered ? kText : kTextDim, ui::FitText(e.label, b.x - textX).c_str());
            if (clicked) action = Action::AddFolder;
            continue;
        }
        if (pin) {
            char count[16];
            std::snprintf(count, sizeof(count), "%d", pin->count);
            float countX = b.x - Px(10.0f);
            {
                DeskFont m(DeskText::Meta);
                countX -= ImGui::CalcTextSize(count).x;
                dl->AddText(ImVec2(countX, a.y + Round((rowH - ImGui::GetFontSize()) * 0.5f)), kTextFaint, count);
            }
            dl->AddText(ImVec2(textX, textY), on || hovered ? kText : kTextDim,
                        ui::FitText(pin->name, std::max(0.0f, countX - Px(8.0f) - textX)).c_str());
            if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                std::string where = pin->path;
                std::replace(where.begin(), where.end(), '/', '\\');
                if (where.size() > 3 && where.back() == '\\') where.pop_back();
                ImGui::SetTooltip("%s", where.c_str());
            }
            // A click shows the folder (from inside it, its top); a right click its menu.
            if (clicked && (source_ != pinnedSource_ || folder_ != pin->path)) {
                source_ = pinnedSource_;
                folder_ = pin->path;
                EndSelection();
                sourcePicked_ = true;
                dirty_ = true;
                gridScroll_.SetOffset(0.0f);
            }
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                pinnedMenu_ = pin->path;
                ImGui::OpenPopup("##pinned_menu");
            }
            continue;
        }
        const Source& s = sources[(size_t)e.source];
        char count[16];
        std::snprintf(count, sizeof(count), "%d", s.count);
        float countX = b.x - Px(10.0f);
        {
            DeskFont m(DeskText::Meta);
            const float cw = ImGui::CalcTextSize(count).x;
            countX -= cw;
            dl->AddText(ImVec2(countX, a.y + Round((rowH - ImGui::GetFontSize()) * 0.5f)), kTextFaint, count);
        }
        dl->AddText(ImVec2(textX, textY), on || hovered ? kText : kTextDim,
                    ui::FitText(s.name, std::max(0.0f, countX - Px(8.0f) - textX)).c_str());
        if (clicked && !on) {
            source_ = (size_t)e.source;
            folder_ = s.folder;
            EndSelection();
            sourcePicked_ = true;
            dirty_ = true;
            gridScroll_.SetOffset(0.0f);
        }
    }
    dl->PopClipRect();
    DeskPinnedMenu();

    // The foot: the system's file dialog, always there.
    dl->AddLine(ImVec2(min.x + pad, listBottom + 0.5f), ImVec2(max.x - pad, listBottom + 0.5f), kLine, Px(1.0f));
    {
        const ImVec2 a(min.x + pad, listBottom + pad), b(max.x - pad, listBottom + pad + rowH);
        ImGui::SetCursorScreenPos(a);
        const bool clicked = ImGui::InvisibleButton("##open_file", ImVec2(b.x - a.x, rowH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive()) dl->AddRectFilled(a, b, kPressFill, r);
        else if (hovered) dl->AddRectFilled(a, b, kHoverFill, r);
        ui::DrawGlyph(dl, Glyph::Files, ImVec2(a.x + Px(10.0f) + icon * 0.5f, a.y + rowH * 0.5f), icon * 1.6f,
                      hovered ? kText : kTextDim);
        const float textX = a.x + Px(10.0f) + icon + Px(10.0f);
        float kx = b.x - Px(10.0f);
        {
            DeskFont m(DeskText::Badge);
            const char* key = "Ctrl+O";
            const ImVec2 ks = ImGui::CalcTextSize(key);
            const float kw = ks.x + Px(8.0f), kh = ks.y + Px(4.0f);
            kx -= kw;
            const float ky = a.y + Round((rowH - kh) * 0.5f);
            dl->AddRect(ImVec2(kx, ky), ImVec2(kx + kw, ky + kh), kLineStrong, Px(3.0f), 0, Px(1.0f));
            dl->AddText(ImVec2(kx + Px(4.0f), ky + Px(2.0f)), kTextFaint, key);
        }
        DeskFont f(DeskText::Body);
        dl->AddText(ImVec2(textX, a.y + Round((rowH - ImGui::GetFontSize()) * 0.5f)), hovered ? kText : kTextDim,
                    ui::FitText("Open file...", std::max(0.0f, kx - Px(8.0f) - textX)).c_str());
        if (hovered) ImGui::SetTooltip("Choose a file with the Windows file dialog");
        if (clicked) {
            openedId_ = -1;
            action = Action::OtherFiles;
        }
    }
    return action;
}

// The status bar (the window's foot, under the sidebar too): what the grid holds, how many are
// selected, the focused file (name, size in pixels, 3D or 2D, size on disk), and a hint at the
// right. Parts drop from the right as the window narrows.
void MediaBrowser::DeskStatusBar(ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, kPanel);
    dl->AddLine(ImVec2(min.x, min.y + 0.5f), ImVec2(max.x, min.y + 0.5f), kLine, Px(1.0f));
    DeskFont f(DeskText::Meta);
    const float pad = Round(Px(12.0f)), gap = Round(Px(16.0f));
    const float ty = min.y + Round((max.y - min.y - ImGui::GetFontSize()) * 0.5f);
    float x = min.x + pad, right = max.x - pad;
    if (!listed_) return;

    // At the right, the hint, when there is room for it beside the rest.
    const char* hint = details_ ? "Click opens  \xC2\xB7  Ctrl/Shift+click selects  \xC2\xB7  Click Date to sort"
                                : "Click opens  \xC2\xB7  Ctrl/Shift+click selects";
    const float hintW = ImGui::CalcTextSize(hint).x;
    if (max.x - min.x >= Px(details_ ? 900.0f : 760.0f)) {
        dl->AddText(ImVec2(right - hintW, ty), kTextFaint, hint);
        right -= hintW + gap;
    }
    auto part = [&](const std::string& text, ImU32 ink) {
        if (text.empty() || x >= right) return;
        const std::string t = ui::FitText(text, right - x);
        dl->AddText(ImVec2(x, ty), ink, t.c_str());
        x += ImGui::CalcTextSize(t.c_str()).x + gap;
    };

    // What the grid holds.
    char count[96];
    const size_t items = shown_.size(), folders = folders_.size();
    const char* what = depth_ == DepthFilter::Only3D ? " in 3D" : depth_ == DepthFilter::Only2D ? " in 2D" : nullptr;
    if (folders > 0 && items > 0)
        std::snprintf(count, sizeof(count), "%zu folder%s  \xC2\xB7  %zu item%s", folders, folders == 1 ? "" : "s", items,
                      items == 1 ? "" : "s");
    else if (folders > 0)
        std::snprintf(count, sizeof(count), "%zu folder%s", folders, folders == 1 ? "" : "s");
    else if (what)
        std::snprintf(count, sizeof(count), "%zu%s", items, what);
    else
        std::snprintf(count, sizeof(count), "%zu item%s", items, items == 1 ? "" : "s");
    part(count, kTextFaint);
    if (!selected_.empty()) {
        char sel[32];
        std::snprintf(sel, sizeof(sel), "%zu selected", selected_.size());
        part(sel, kText);
    }

    // The focused file or folder.
    // ... else the one selected file.
    int64_t focusItem = statusItem_;
    if (focusItem < 0 && statusFolder_ >= folders_.size() && selected_.size() == 1) focusItem = *selected_.begin();
    std::string detail;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        if (!detail.empty()) detail += "  \xC2\xB7  ";
        detail += s;
    };
    if (statusFolder_ < folders_.size()) {
        const Folder& fo = folders_[statusFolder_];
        add(fo.name);
        char n[32];
        std::snprintf(n, sizeof(n), "%d item%s", fo.count, fo.count == 1 ? "" : "s");
        add(n);
    } else if (const MediaItem* item = focusItem >= 0 ? library_.Find(focusItem) : nullptr) {
        add(item->name);
        if (item->width > 0 && item->height > 0) {
            char wh[32];
            std::snprintf(wh, sizeof(wh), "%u\xC3\x97%u", item->width, item->height);
            add(wh);
        }
        if (item->type == MediaType::Video && item->durationMs > 0) {
            char length[24];
            Duration(item->durationMs, length, sizeof(length));
            add(length);
        }
        const StereoVerdict v = classifier_.Get(*item);
        if (v.known && v.stereo) {
            const char* packing = v.layout == StereoLayout::SbsFull || v.layout == StereoLayout::SbsHalf ? "SBS"
                                  : v.layout == StereoLayout::TbFull || v.layout == StereoLayout::TbHalf ? "TB"
                                                                                                         : nullptr;
            const std::string kind = packing ? std::string(packing) : Upper(item->name);
            add(kind.empty() ? std::string("3D") : "3D (" + kind + ")");
        } else if (v.known && v.unchecked && item->cloudOnly) {
            add("3D?");
        } else if (v.known) {
            add("2D");
        }
        if (item->cloudOnly) add("Online only");
        if (item->bytes > 0) add(FileSize(item->bytes));
    }
    part(detail, kTextDim);
}

// A pinned folder's menu (right click its sidebar row): show it, or unpin it.
void MediaBrowser::DeskPinnedMenu() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(4.0f), Px(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Px(6.0f));
    const bool open = ImGui::BeginPopup("##pinned_menu");
    ImGui::PopStyleVar(2);
    if (!open) return;
    {
        DeskFont f(DeskText::Label);
        const float w = Round(Px(200.0f)), rowH = Round(Px(28.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Selectable("Open", false, 0, ImVec2(w, rowH)) && pinnedSource_ != SIZE_MAX) {
            source_ = pinnedSource_;
            folder_ = pinnedMenu_;
            EndSelection();
            dirty_ = true;
            gridScroll_.SetOffset(0.0f);
        }
        if (ImGui::Selectable("Remove from Pinned", false, 0, ImVec2(w, rowH))) {
            folderRequest_ = FolderRequest{FolderRequest::Verb::Unpin, pinnedMenu_};
            folderRequestPending_ = true;
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndPopup();
}

// A folder's card: a surface square with a preview of what it holds (a 2 x 2 mosaic of its
// newest files' thumbnails, or one), else the folder glyph; a badge with a folder and the
// count; the name and where it is under it.
void MediaBrowser::DeskFolderCard(ImDrawList* dl, const Folder& folder, ImVec2 a, ImVec2 b, float pic, bool hovered,
                                  bool pressed, bool focus, const std::vector<size_t>* preview, size_t shown) {
    const float pad = Round(Px(4.0f));
    if (pressed) dl->AddRectFilled(a, b, kPressFill, Px(6.0f));
    else if (hovered) dl->AddRectFilled(a, b, kHoverFill, Px(6.0f));
    const ImVec2 pa(a.x + pad, a.y + pad), pb(pa.x + pic, pa.y + pic);
    dl->AddRectFilled(pa, pb, kSurface, Px(4.0f));
    // The thumbnails here so far, in preview order (null: not here yet).
    const size_t cells = preview ? std::min(shown, preview->size()) : 0;
    ThumbnailAtlas::Thumb thumbs[4];
    bool have[4] = {};
    size_t got = 0;
    for (size_t k = 0; k < cells && k < 4; ++k) {
        int slot = 0;
        have[k] = cache_.Use(library_.Items()[(*preview)[k]].id, slot) && atlas_.Get(slot, thumbs[k]);
        got += have[k];
    }
    const float r = Px(4.0f);
    if (got == 0) {
        ui::DrawGlyph(dl, Glyph::Folder, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), pic * 0.4f, kTextFaint);
    } else if (cells == 1) {
        dl->AddImageRounded(thumbs[0].texture, pa, pb, thumbs[0].uv0, thumbs[0].uv1, IM_COL32_WHITE, r);
    } else {
        // 2 x 2, a 2 px seam; a cell with nothing (yet) stays the surface.
        const float gap = Round(Px(2.0f)), cell = std::floor((pic - gap) * 0.5f);
        const ImDrawFlags corner[4] = {ImDrawFlags_RoundCornersTopLeft, ImDrawFlags_RoundCornersTopRight,
                                       ImDrawFlags_RoundCornersBottomLeft, ImDrawFlags_RoundCornersBottomRight};
        for (size_t k = 0; k < 4; ++k) {
            const ImVec2 c0(k % 2 ? pb.x - cell : pa.x, k / 2 ? pb.y - cell : pa.y), c1(c0.x + cell, c0.y + cell);
            if (k < cells && have[k])
                dl->AddImageRounded(thumbs[k].texture, c0, c1, thumbs[k].uv0, thumbs[k].uv1, IM_COL32_WHITE, r,
                                    corner[k]);
            else
                dl->AddRectFilled(c0, c1, IM_COL32(36, 41, 52, 255), r, corner[k]);
        }
    }
    char count[24];
    std::snprintf(count, sizeof(count), "%d", folder.count);
    {
        // The badge: a small folder, then the count.
        DeskFont f(DeskText::Badge);
        const float padX = Round(Px(5.0f)), h = Round(Px(18.0f)), g = Round(Px(12.0f));
        const float x0 = pa.x + Px(6.0f), y0 = pb.y - Px(6.0f) - h;
        const float w = padX + g + Round(Px(4.0f)) + Round(ImGui::CalcTextSize(count).x) + padX;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), kBadgeFill, Px(4.0f));
        ui::DrawGlyph(dl, Glyph::Folder, ImVec2(x0 + padX + g * 0.5f, y0 + h * 0.5f), g * 1.5f, kText);
        dl->AddText(ImVec2(x0 + padX + g + Round(Px(4.0f)), y0 + Round((h - ImGui::GetFontSize()) * 0.5f)), kText,
                    count);
    }
    float y = pb.y + Px(6.0f);
    {
        DeskFont f(DeskText::Label);
        dl->AddText(ImVec2(pa.x + Px(2.0f), y), kText, ui::FitText(folder.name, pic - Px(4.0f)).c_str());
        y += ImGui::GetFontSize() + Px(3.0f);
    }
    {
        DeskFont f(DeskText::Meta);
        char meta[64];
        std::snprintf(meta, sizeof(meta), "%d item%s", folder.count, folder.count == 1 ? "" : "s");
        const std::string line = folder.detail.empty() ? std::string(meta) : folder.detail;
        dl->AddText(ImVec2(pa.x + Px(2.0f), y), kTextFaint, ui::FitText(line, pic - Px(4.0f)).c_str());
    }
    if (focus)
        dl->AddRect(ImVec2(a.x - Px(2.0f), a.y - Px(2.0f)), ImVec2(b.x + Px(2.0f), b.y + Px(2.0f)), kFocus, Px(8.0f), 0,
                    Px(2.0f));
}

// A file's card, all but the picture (drawn by the grid, or by the 3D renderer behind the HUD):
// the hover fill under it, the badges on it (not in 3D), the caption under it, and the focus.
void MediaBrowser::DeskItemCard(ImDrawList* dl, const MediaItem& item, bool supported, const StereoVerdict& verdict,
                                ImVec2 a, ImVec2 b, float pic, bool hovered, bool pressed, bool focus, bool picture,
                                bool behind) {
    const float pad = Round(Px(4.0f));
    const ImVec2 pa(a.x + pad, a.y + pad), pb(pa.x + pic, pa.y + pic);
    const bool picked = selected_.count(item.id) != 0;
    if (behind) {
        // Under the picture: the card's fill (accent when selected), and a placeholder where
        // there is no picture yet.
        if (picked) dl->AddRectFilled(a, b, kAccentWeak, Px(6.0f));
        if (pressed) dl->AddRectFilled(a, b, kPressFill, Px(6.0f));
        else if (hovered) dl->AddRectFilled(a, b, kHoverFill, Px(6.0f));
        if (!picture) {
            dl->AddRectFilled(pa, pb, kSurface, Px(4.0f));
            const float g = std::min(Px(40.0f), pic * 0.3f);
            ui::DrawGlyph(dl, item.type == MediaType::Video ? Glyph::Video : Glyph::Image,
                          ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), g, IM_COL32(170, 176, 186, 150));
        }
        return;
    }
    char length[24] = "";
    if (item.type == MediaType::Video && item.durationMs > 0) Duration(item.durationMs, length, sizeof(length));
    const float m = Round(Px(6.0f));
    if (!stereo_) {
        // Top left: 3D, or 3D? for a cloud file that was not looked into. Top right: the length,
        // and the format of a file the player cannot open. Bottom right: the cloud.
        if (verdict.known && verdict.stereo)
            Badge(dl, pa.x + m, pa.y + m, "3D", kAccent, kOnAccent, false);
        else if (verdict.known && verdict.unchecked && item.cloudOnly)
            Badge(dl, pa.x + m, pa.y + m, "3D?", kBadgeFill, kText, false, IM_COL32(255, 255, 255, 140));
        float rx = pb.x - m;
        if (length[0]) rx = Badge(dl, rx, pa.y + m, length, kBadgeFill, kText, true) - Px(4.0f);
        if (!supported) {
            const std::string fmt = Upper(item.name);
            if (!fmt.empty()) Badge(dl, rx, pa.y + m, fmt.c_str(), kWarnFill, kWarn, true);
        }
        if (item.cloudOnly) {
            const float s = Round(Px(18.0f));
            const ImVec2 c1(pb.x - m, pb.y - m), c0(c1.x - s - Px(4.0f), c1.y - s);
            dl->AddRectFilled(c0, c1, kBadgeFill, Px(4.0f));
            ui::DrawGlyph(dl, Glyph::Cloud, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), s * 1.2f, kText, true);
        }
    }
    // The caption: the name, then what it is.
    float y = pb.y + Px(6.0f);
    {
        DeskFont f(DeskText::Label);
        float nx = pa.x + Px(2.0f);
        if (stereo_ && item.cloudOnly) {  // no badges on a 3D picture: the cloud goes by the name
            const float s = ImGui::GetFontSize();
            ui::DrawGlyph(dl, Glyph::Cloud, ImVec2(nx + s * 0.5f, y + s * 0.5f), s * 1.3f, kTextDim, true);
            nx += s + Px(4.0f);
        }
        dl->AddText(ImVec2(nx, y), kText, ui::FitText(item.name, pb.x - Px(2.0f) - nx).c_str());
        y += ImGui::GetFontSize() + Px(3.0f);
    }
    {
        DeskFont f(DeskText::Meta);
        std::string meta;
        auto add = [&](const std::string& part) {
            if (part.empty()) return;
            if (!meta.empty()) meta += "  \xC2\xB7  ";
            meta += part;
        };
        if (stereo_ && length[0]) add(length);
        if (!supported) add("Not supported");
        if (item.cloudOnly) add("Online only");
        if (item.width > 0 && item.height > 0) {
            char wh[32];
            std::snprintf(wh, sizeof(wh), "%u\xC3\x97%u", item.width, item.height);
            add(wh);
        } else if (!stereo_ && length[0] && meta.empty()) {
            add("Video");
        }
        if (meta.empty() && item.bytes > 0) add(FileSize(item.bytes));
        dl->AddText(ImVec2(pa.x + Px(2.0f), y), kTextFaint, ui::FitText(meta, pic - Px(4.0f)).c_str());
    }
    if (picked) dl->AddRect(a, b, kAccent, Px(6.0f), 0, Px(2.0f));
    if (focus)
        dl->AddRect(ImVec2(a.x - Px(2.0f), a.y - Px(2.0f)), ImVec2(b.x + Px(2.0f), b.y + Px(2.0f)), kFocus, Px(8.0f), 0,
                    Px(2.0f));
}

// The details view's columns over a row `width` wide from `x0`. The name takes what the
// others leave; in a narrow window type, dimensions, size and then date drop out.
MediaBrowser::DeskColumns MediaBrowser::DeskColumnLayout(float x0, float width) {
    const float want[7] = {0.0f, 64.0f, 110.0f, 110.0f, 72.0f, 110.0f, 84.0f};
    bool on[7] = {true, true, true, true, true, true, true};
    auto others = [&] {
        float s = 0.0f;
        for (int i = 1; i < 7; ++i)
            if (on[i]) s += Round(Px(want[i]));
        return s;
    };
    for (int d : {2, 3, 6, 5}) {
        if (width - others() >= Px(220.0f)) break;
        on[d] = false;
    }
    DeskColumns c;
    float x = x0;
    for (int i = 0; i < 7; ++i) {
        c.on[i] = on[i];
        if (!on[i]) continue;
        c.x[i] = x;
        c.w[i] = i == 0 ? std::max(Px(80.0f), width - others()) : Round(Px(want[i]));
        x += c.w[i];
    }
    return c;
}

// The column headings. Date sorts: a click turns the order round.
void MediaBrowser::DeskColumnHeader(const DeskColumns& c, ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, kToolbar);
    dl->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), kLine, Px(1.0f));
    DeskFont f(DeskText::Meta);
    const char* names[7] = {"Name", "Depth", "Type", "Dimensions", "Length", "Date", "Size"};
    const float h = max.y - min.y, ty = min.y + Round((h - ImGui::GetFontSize()) * 0.5f);
    for (int i = 0; i < 7; ++i) {
        if (!c.on[i]) continue;
        const bool right = i == 4 || i == 6;
        const float tw = ImGui::CalcTextSize(names[i]).x;
        float tx = right ? c.x[i] + c.w[i] - Px(8.0f) - tw : c.x[i] + Px(8.0f);
        if (i == 0) tx += Round(Px(4.0f)) + Round(Px(40.0f)) + Px(2.0f);  // over the names, past the pictures
        ImU32 ink = kTextFaint;
        if (i == 5) {
            ImGui::SetCursorScreenPos(ImVec2(c.x[i], min.y));
            if (ImGui::InvisibleButton("##sort_date", ImVec2(c.w[i], h))) {
                options_.order = options_.order == SortOrder::NewestFirst ? SortOrder::OldestFirst : SortOrder::NewestFirst;
                dirty_ = true;
                gridScroll_.SetOffset(0.0f);
            }
            const bool hov = ImGui::IsItemHovered();
            if (hov) {
                dl->AddRectFilled(ImVec2(c.x[i], min.y + Px(2.0f)), ImVec2(c.x[i] + c.w[i], max.y - Px(2.0f)), kHoverFill,
                                  Px(4.0f));
                ImGui::SetTooltip("%s", options_.order == SortOrder::NewestFirst ? "Newest first: click for oldest first"
                                                                                  : "Oldest first: click for newest first");
            }
            ink = kText;
            // An arrow: down for newest first.
            const float ax = tx + tw + Px(8.0f), ay = min.y + h * 0.5f, s = Px(3.0f);
            const float dir = options_.order == SortOrder::NewestFirst ? 1.0f : -1.0f;
            const ImVec2 v[] = {ImVec2(ax - s, ay - s * 0.5f * dir), ImVec2(ax, ay + s * 0.5f * dir),
                                ImVec2(ax + s, ay - s * 0.5f * dir)};
            dl->AddPolyline(v, 3, ink, 0, Px(1.5f));
        }
        dl->AddText(ImVec2(tx, ty), ink, names[i]);
    }
}

namespace {

// A cell's text in column `i`: cut to fit, left or right aligned, centred in the row.
void Cell(ImDrawList* dl, const MediaBrowser::DeskColumns& c, int i, ImVec2 a, ImVec2 b, const std::string& text,
          ImU32 ink, bool right) {
    if (!c.on[i] || text.empty()) return;
    const std::string t = ui::FitText(text, c.w[i] - Px(16.0f));
    const float tw = ImGui::CalcTextSize(t.c_str()).x;
    const float x = right ? c.x[i] + c.w[i] - Px(8.0f) - tw : c.x[i] + Px(8.0f);
    dl->AddText(ImVec2(x, a.y + Round((b.y - a.y - ImGui::GetFontSize()) * 0.5f)), ink, t.c_str());
}

} // namespace

// A folder's row: the folder in a 40 px square, its name, "Folder", and how many it holds.
void MediaBrowser::DeskFolderRow(ImDrawList* dl, const Folder& folder, const DeskColumns& c, ImVec2 a, ImVec2 b,
                                 float pic, bool hovered, bool pressed, bool focus) {
    if (pressed) dl->AddRectFilled(a, b, kPressFill, Px(4.0f));
    else if (hovered) dl->AddRectFilled(a, b, kHoverFill, Px(4.0f));
    dl->AddLine(ImVec2(a.x, b.y - 0.5f), ImVec2(b.x, b.y - 0.5f), IM_COL32(255, 255, 255, 8), Px(1.0f));
    const ImVec2 pa(a.x + Round(Px(4.0f)), a.y + Round((b.y - a.y - pic) * 0.5f)), pb(pa.x + pic, pa.y + pic);
    dl->AddRectFilled(pa, pb, kSurface, Px(4.0f));
    ui::DrawGlyph(dl, Glyph::Folder, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), pic * 0.6f, kTextDim);
    {
        DeskFont f(DeskText::Body);
        const float nx = pb.x + Px(10.0f);
        dl->AddText(ImVec2(nx, a.y + Round((b.y - a.y - ImGui::GetFontSize()) * 0.5f)), kText,
                    ui::FitText(folder.name, c.x[0] + c.w[0] - Px(8.0f) - nx).c_str());
    }
    DeskFont f(DeskText::Label);
    char count[32];
    std::snprintf(count, sizeof(count), "%d item%s", folder.count, folder.count == 1 ? "" : "s");
    Cell(dl, c, 2, a, b, "Folder", kTextDim, false);
    Cell(dl, c, 6, a, b, count, kTextDim, true);
    if (!c.on[6]) Cell(dl, c, c.on[2] ? 3 : 4, a, b, count, kTextDim, !c.on[2]);
    if (focus) dl->AddRect(a, b, kFocus, Px(4.0f), 0, Px(2.0f));
}

// A file's row, all but the picture (drawn by the grid, or by the 3D renderer behind the HUD):
// the name with a cloud after it, then 3D / 3D? / 2D, the type, the size in pixels, the length,
// the date and the size on disk.
void MediaBrowser::DeskItemRow(ImDrawList* dl, const MediaItem& item, bool supported, const StereoVerdict& verdict,
                               const DeskColumns& c, ImVec2 a, ImVec2 b, float pic, bool hovered, bool pressed,
                               bool focus, bool picture, bool behind) {
    const ImVec2 pa(a.x + Round(Px(4.0f)), a.y + Round((b.y - a.y - pic) * 0.5f)), pb(pa.x + pic, pa.y + pic);
    const bool picked = selected_.count(item.id) != 0;
    if (behind) {
        if (picked) dl->AddRectFilled(a, b, kAccentWeak, Px(4.0f));
        if (pressed) dl->AddRectFilled(a, b, kPressFill, Px(4.0f));
        else if (hovered) dl->AddRectFilled(a, b, kHoverFill, Px(4.0f));
        if (!picture) {
            dl->AddRectFilled(pa, pb, kSurface, Px(4.0f));
            ui::DrawGlyph(dl, item.type == MediaType::Video ? Glyph::Video : Glyph::Image,
                          ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), pic * 0.55f, IM_COL32(170, 176, 186, 150));
        }
        return;
    }
    dl->AddLine(ImVec2(a.x, b.y - 0.5f), ImVec2(b.x, b.y - 0.5f), IM_COL32(255, 255, 255, 8), Px(1.0f));
    {
        DeskFont f(DeskText::Body);
        const float nx = pb.x + Px(10.0f), fs = ImGui::GetFontSize();
        const float cloudW = item.cloudOnly ? fs + Px(6.0f) : 0.0f;
        const std::string name = ui::FitText(item.name, c.x[0] + c.w[0] - Px(8.0f) - nx - cloudW);
        const float ty = a.y + Round((b.y - a.y - fs) * 0.5f);
        dl->AddText(ImVec2(nx, ty), kText, name.c_str());
        if (item.cloudOnly) {
            const float cx = nx + ImGui::CalcTextSize(name.c_str()).x + Px(6.0f) + fs * 0.5f;
            ui::DrawGlyph(dl, Glyph::Cloud, ImVec2(cx, ty + fs * 0.5f), fs * 1.3f, kTextDim, false);
            if (ImGui::IsMouseHoveringRect(ImVec2(cx - fs * 0.5f, ty), ImVec2(cx + fs * 0.5f, ty + fs)))
                ImGui::SetTooltip("Online only: opening it downloads it");
        }
    }
    DeskFont f(DeskText::Label);
    // Depth: a badge for 3D, an outlined one for a cloud file not looked into, else text.
    if (c.on[1]) {
        const float bh = Round(Px(18.0f)), by = a.y + Round((b.y - a.y - bh) * 0.5f), bx = c.x[1] + Px(8.0f);
        if (verdict.known && verdict.stereo)
            Badge(dl, bx, by, "3D", kAccent, kOnAccent, false);
        else if (verdict.known && verdict.unchecked && item.cloudOnly)
            Badge(dl, bx, by, "3D?", kBadgeFill, kText, false, IM_COL32(255, 255, 255, 140));
        else
            Cell(dl, c, 1, a, b, verdict.known ? "2D" : "-", kTextFaint, false);
    }
    const std::string ext = Upper(item.name);
    if (c.on[2]) {
        if (!supported && !ext.empty()) {
            const float bh = Round(Px(18.0f));
            Badge(dl, c.x[2] + Px(8.0f), a.y + Round((b.y - a.y - bh) * 0.5f), ext.c_str(), kWarnFill, kWarn, false);
        } else {
            std::string type = item.type == MediaType::Video ? "Video" : "Photo";
            if (!ext.empty()) type += "  \xC2\xB7  " + ext;
            Cell(dl, c, 2, a, b, type, kTextDim, false);
        }
    }
    char wh[48] = "-";
    if (item.width > 0 && item.height > 0) std::snprintf(wh, sizeof(wh), "%u \xC3\x97 %u", item.width, item.height);
    Cell(dl, c, 3, a, b, wh, kTextDim, false);
    char length[24] = "-";
    if (item.type == MediaType::Video && item.durationMs > 0) Duration(item.durationMs, length, sizeof(length));
    Cell(dl, c, 4, a, b, length, kTextDim, true);
    Cell(dl, c, 5, a, b, ShortDate(item.dateMs), kTextDim, false);
    Cell(dl, c, 6, a, b, FileSize(item.bytes), kTextDim, true);
    if (picked) dl->AddRect(a, b, kAccent, Px(4.0f), 0, Px(2.0f));
    if (focus) dl->AddRect(a, b, kFocus, Px(4.0f), 0, Px(2.0f));
}

// A file's menu (right click): open it, show it in File Explorer, copy its path.
void MediaBrowser::DeskItemMenu() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(4.0f), Px(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Px(6.0f));
    const bool open = ImGui::BeginPopup("##item_menu");
    ImGui::PopStyleVar(2);
    if (!open) return;
    {
        DeskFont f(DeskText::Label);
        const float w = Round(Px(240.0f)), rowH = Round(Px(28.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        auto row = [&](const char* label, const char* key) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const bool hit = ImGui::Selectable(label, false, 0, ImVec2(w, rowH));
            if (key) {
                DeskFont k(DeskText::Meta);
                const float kw = ImGui::CalcTextSize(key).x;
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(at.x + w - Px(8.0f) - kw, at.y + Round((rowH - ImGui::GetFontSize()) * 0.5f)), kTextFaint, key);
            }
            return hit;
        };
        auto ask = [&](ItemRequest::Verb verb) {
            itemRequest_ = ItemRequest{verb, menuItem_, {}};
            itemRequestPending_ = true;
        };
        if (row("Open", "Enter")) menuOpen_ = true;
        if (row("Show in File Explorer", nullptr)) ask(ItemRequest::Verb::Reveal);
        // On a selection of several: copy all their paths, one a line.
        if (selected_.size() > 1 && selected_.count(menuItem_)) {
            char label[48];
            std::snprintf(label, sizeof(label), "Copy %zu paths", selected_.size());
            if (row(label, "Ctrl+Shift+C")) {
                itemRequest_ = ItemRequest{ItemRequest::Verb::CopyPath, menuItem_, SelectedInOrder()};
                itemRequestPending_ = true;
            }
        } else if (row("Copy path", "Ctrl+Shift+C")) {
            ask(ItemRequest::Verb::CopyPath);
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndPopup();
}

} // namespace mp::browser
