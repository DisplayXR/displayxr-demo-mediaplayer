// SPDX-License-Identifier: Apache-2.0
#include "ui/SettingsPanel.h"

#include "media/Settings.h"
#include "ui/UiScale.h"

#include <algorithm>

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include "imgui.h"
#endif

namespace mp::ui {

void SettingsPanel::AddToggle(const std::string& group, const std::string& label, const std::string& key, bool def,
                              const std::string& tooltip, std::function<bool()> enabled, const std::string& note) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    rows_.push_back({group, [this, label, key, def, tooltip, enabled, note] {
                         const bool on = !enabled || enabled();
                         bool v = store_ ? store_->GetBool(key, def) : def;
                         ImGui::BeginDisabled(!on);
                         if (ImGui::Checkbox(label.c_str(), &v) && store_) store_->SetBool(key, v);
                         ImGui::EndDisabled();
                         if (!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                             ImGui::SetTooltip("%s", tooltip.c_str());
                         if (!on && !note.empty()) {
                             ImGui::SameLine();
                             ImGui::TextDisabled("(%s)", note.c_str());
                         }
                     }});
#else
    (void)group, (void)label, (void)key, (void)def, (void)tooltip, (void)enabled, (void)note;
#endif
}

void SettingsPanel::AddCustom(const std::string& group, std::function<void()> draw) {
    rows_.push_back({group, std::move(draw)});
}

bool SettingsPanel::Build(float rect[4]) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!open_) {
        wasOpen_ = false;
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (!wasOpen_) {
        // Each opening starts centred and focused, so Esc reaches it at once.
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowFocus();
        wasOpen_ = true;
    }
    const float w = std::min(Px(420.0f), io.DisplaySize.x - Px(16.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, io.DisplaySize.y - Px(16.0f)));
    ImGui::SetNextWindowBgAlpha(1.0f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    bool open = true;
    if (ImGui::Begin("Settings###settings", &open, flags)) {
        // Groups in the order they first appear; each group's rows in registration order.
        std::vector<std::string> groups;
        for (const Row& r : rows_) {
            bool seen = false;
            for (const std::string& g : groups) seen = seen || g == r.group;
            if (!seen) groups.push_back(r.group);
        }
        if (groups.empty()) ImGui::TextDisabled("Nothing to set here yet.");
        for (const std::string& g : groups) {
            if (!ImGui::CollapsingHeader(g.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::PushID(g.c_str());
            int i = 0;
            for (const Row& r : rows_) {
                if (r.group != g || !r.draw) continue;
                ImGui::PushID(i++);
                r.draw();
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        if (store_ && !store_->Path().empty()) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Saved to %s", store_->Path().c_str());
            ImGui::PopStyleColor();
        }
        const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
        if (rect) {
            rect[0] = p.x;
            rect[1] = p.y;
            rect[2] = s.x;
            rect[3] = s.y;
        }
    }
    ImGui::End();
    // Esc closes it unless a popup or text field inside has it.
    if (!open || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !io.WantTextInput && !ImGui::IsPopupOpen(
                                                                     "", ImGuiPopupFlags_AnyPopupId)))
        open_ = false;
    return open_;
#else
    (void)rect;
    return false;
#endif
}

}  // namespace mp::ui
