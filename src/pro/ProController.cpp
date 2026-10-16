// SPDX-License-Identifier: Apache-2.0
#include "pro/ProController.h"

#include "Log.h"

#include <SDL3/SDL.h>

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace mp::pro {

namespace {

constexpr float kMaxZoom = 16.0f;
constexpr float kWheelStep = 1.15f;

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Ext(const std::string& path) {
    return Lower(std::filesystem::path(path).extension().string());
}

bool IsLutPath(const std::string& p) { return Ext(p) == ".cube"; }
bool IsCdlPath(const std::string& p) {
    const std::string e = Ext(p);
    return e == ".cc" || e == ".ccc" || e == ".cdl";
}

std::string BaseName(const std::string& p) { return std::filesystem::path(p).filename().string(); }

}  // namespace

const char* ViewModeName(ViewMode m) {
    switch (m) {
        case ViewMode::Stereo3D: return "3D";
        case ViewMode::LeftOnly: return "Left eye";
        case ViewMode::RightOnly: return "Right eye";
        case ViewMode::Difference: return "Difference";
        case ViewMode::Anaglyph: return "Anaglyph";
        default: return "?";
    }
}

void ProController::Init(VulkanRenderer* renderer, SDL_Window* window, Host host) {
    renderer_ = renderer;
    window_ = window;
    host_ = std::move(host);
}

void ProController::ApplyTestEnv() {
    if (const char* v = std::getenv("MEDIAPLAYER_PRO_VIEW")) {
        const int m = std::atoi(v);
        if (m >= 0 && m < (int)ViewMode::Count) mode_ = (ViewMode)m;  // stereo check happens per frame
    }
    if (const char* z = std::getenv("MEDIAPLAYER_PRO_ZOOM")) SetZoom((float)std::atof(z));
}

// ---------------------------------------------------------------------------
// Looks
// ---------------------------------------------------------------------------

void ProController::LoadLut(const std::string& path) {
    color::CubeLut lut;
    std::string err;
    if (!color::LoadCube(path, lut, err)) {
        LOG_WARN("Pro: LUT %s rejected: %s", path.c_str(), err.c_str());
        if (host_.toast) host_.toast("LUT not loaded: " + err);
        return;
    }
    lut_ = std::move(lut);
    lutPath_ = path;
    lutName_ = lut_.title.empty() ? BaseName(path) : lut_.title;
    lutPending_ = true;  // uploaded on the render thread in Tick()
    grade_.lutOn = true;
    LOG_INFO("Pro: LUT %s (%d^3) loaded", path.c_str(), lut_.size);
    if (host_.toast) host_.toast("LUT: " + lutName_);
}

void ProController::LoadCdl(const std::string& path) {
    color::Cdl cdl;
    std::string err;
    if (!color::LoadCdl(path, cdl, err)) {
        LOG_WARN("Pro: CDL %s rejected: %s", path.c_str(), err.c_str());
        if (host_.toast) host_.toast("CDL not loaded: " + err);
        return;
    }
    cdl_ = cdl;
    cdlPath_ = path;
    cdlName_ = cdl_.id.empty() ? BaseName(path) : cdl_.id;
    for (int i = 0; i < 3; ++i) {
        grade_.slope[i] = cdl_.slope[i];
        grade_.offset[i] = cdl_.offset[i];
        grade_.power[i] = cdl_.power[i];
    }
    grade_.saturation = cdl_.saturation;
    grade_.cdlOn = true;
    LOG_INFO("Pro: CDL %s (%s) loaded", path.c_str(), cdlName_.c_str());
    if (host_.toast) host_.toast("CDL: " + cdlName_);
}

bool ProController::HandleDroppedPath(const std::string& path) {
    if (IsLutPath(path)) { LoadLut(path); return true; }
    if (IsCdlPath(path)) { LoadCdl(path); return true; }
    return false;
}

void ProController::DialogCallback(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* self = static_cast<ProController*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->dialogOpen_ = false;
    if (filelist && filelist[0]) {
        self->dialogPath_ = filelist[0];
        self->dialogIsCdl_ = self->pendingDialogIsCdl_;
        self->dialogReady_ = true;
    }
}

void ProController::OpenLookDialog(bool cdl) {
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (dialogOpen_) return;
        dialogOpen_ = true;
        pendingDialogIsCdl_ = cdl;
    }
    static const SDL_DialogFileFilter kLutFilter[] = {{"3D LUT (.cube)", "cube"}};
    static const SDL_DialogFileFilter kCdlFilter[] = {{"ASC CDL (.cc, .ccc, .cdl)", "cc;ccc;cdl"}};
    const std::string& last = cdl ? cdlPath_ : lutPath_;
    std::string dir = last.empty() ? std::string() : std::filesystem::path(last).parent_path().string();
    SDL_ShowOpenFileDialog(&ProController::DialogCallback, this, window_, cdl ? kCdlFilter : kLutFilter, 1,
                           dir.empty() ? nullptr : dir.c_str(), false);
}

// ---------------------------------------------------------------------------
// View: mode, pan, zoom
// ---------------------------------------------------------------------------

void ProController::SetMode(ViewMode m, bool announce) {
    if ((m == ViewMode::Difference || m == ViewMode::Anaglyph) && !stereoSource_) {
        if (host_.toast) host_.toast(std::string(ViewModeName(m)) + " needs a stereo source");
        return;
    }
    mode_ = m;
    if (announce && host_.toast) host_.toast(std::string("View: ") + ViewModeName(m));
}

void ProController::SetZoom(float z) {
    // Keep the centre of the visible window fixed.
    const float cx = panX_ + 0.5f / zoom_, cy = panY_ + 0.5f / zoom_;
    zoom_ = std::clamp(z, 1.0f, kMaxZoom);
    panX_ = cx - 0.5f / zoom_;
    panY_ = cy - 0.5f / zoom_;
    ClampPan();
}

void ProController::ZoomAt(float factor, float u, float v) {
    // Keep the source point under (u, v) fixed: s = pan + u/zoom.
    const float sx = panX_ + u / zoom_, sy = panY_ + v / zoom_;
    zoom_ = std::clamp(zoom_ * factor, 1.0f, kMaxZoom);
    panX_ = sx - u / zoom_;
    panY_ = sy - v / zoom_;
    ClampPan();
}

void ProController::ClampPan() {
    const float maxPan = 1.0f - 1.0f / zoom_;
    panX_ = std::clamp(panX_, 0.0f, maxPan);
    panY_ = std::clamp(panY_, 0.0f, maxPan);
    if (zoom_ <= 1.0001f) { zoom_ = 1.0f; panX_ = panY_ = 0.0f; }
}

void ProController::ApplyViewTransform(ViewUV* uvs, uint32_t n, bool stereo, bool eyesSwapped) {
    stereoSource_ = stereo;
    if (!stereo && mode_ != ViewMode::Stereo3D) {
        // Eye-specific views mean nothing for a mono picture.
        if (mode_ == ViewMode::Difference || mode_ == ViewMode::Anaglyph) mode_ = ViewMode::Stereo3D;
    }

    // The two eyes' halves of a side-by-side frame. For mono both are the whole frame.
    const ViewUV halfA{0.0f, 0.0f, 0.5f, 1.0f}, halfB{0.5f, 0.0f, 0.5f, 1.0f};
    const ViewUV leftUv = !stereo ? uvs[0] : (eyesSwapped ? halfB : halfA);
    const ViewUV rightUv = !stereo ? uvs[0] : (eyesSwapped ? halfA : halfB);

    for (uint32_t v = 0; v < n; ++v) {
        ViewUV u = uvs[v];
        switch (mode_) {
            case ViewMode::LeftOnly: u = leftUv; break;
            case ViewMode::RightOnly: u = rightUv; break;
            case ViewMode::Difference:
            case ViewMode::Anaglyph:
                // Every view samples the left eye and looks up the right eye at the same
                // point, so both eyes show the same QC picture (a flat image).
                u = leftUv;
                u.otherDX = rightUv.offX - leftUv.offX;
                u.otherDY = rightUv.offY - leftUv.offY;
                break;
            default: break;
        }
        // Pan / zoom inside the eye's sub-rect; identical for both eyes, so the
        // disparity scales with the picture and depth reads correctly while zoomed.
        if (zoom_ > 1.0001f) {
            u.offX += u.scaleX * panX_;
            u.offY += u.scaleY * panY_;
            u.scaleX /= zoom_;
            u.scaleY /= zoom_;
        }
        uvs[v] = u;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool ProController::HandleEvent(const void* ev, bool imguiWantsMouse, bool imguiWantsKeyboard) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
    switch (e.type) {
        case SDL_EVENT_MOUSE_WHEEL: {
            if (imguiWantsMouse || e.wheel.y == 0.0f) return false;
            float u = 0.5f, v = 0.5f;
            if (host_.pointToContent && !host_.pointToContent(e.wheel.mouse_x, e.wheel.mouse_y, u, v)) {
                u = 0.5f;
                v = 0.5f;
            }
            ZoomAt(std::pow(kWheelStep, e.wheel.y), u, v);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (imguiWantsMouse) return false;
            const bool middle = e.button.button == SDL_BUTTON_MIDDLE;
            const bool leftPlain = e.button.button == SDL_BUTTON_LEFT &&
                                   !(SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) && zoom_ > 1.0001f;
            if (middle || leftPlain) {
                panning_ = true;
                panLastX_ = e.button.x;
                panLastY_ = e.button.y;
                return true;
            }
            if (e.button.button == SDL_BUTTON_LEFT && e.button.clicks == 2 && host_.toggleFullscreen) {
                host_.toggleFullscreen();  // double-click the picture = full screen
                return true;
            }
            return false;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (panning_ && (e.button.button == SDL_BUTTON_MIDDLE || e.button.button == SDL_BUTTON_LEFT)) {
                panning_ = false;
                return true;
            }
            return false;
        case SDL_EVENT_MOUSE_MOTION: {
            if (!panning_) return false;
            float w = 0.0f, h = 0.0f;
            if (host_.contentSizePoints && host_.contentSizePoints(w, h) && w > 1.0f && h > 1.0f) {
                // Drag the picture with the pointer: the visible window spans the picture
                // rect on screen and 1/zoom of the eye's UV range.
                panX_ -= (e.motion.x - panLastX_) / (w * zoom_);
                panY_ -= (e.motion.y - panLastY_) / (h * zoom_);
                ClampPan();
            }
            panLastX_ = e.motion.x;
            panLastY_ = e.motion.y;
            return true;
        }
        case SDL_EVENT_KEY_DOWN: {
            if (imguiWantsKeyboard || e.key.repeat) return false;
            const SDL_Keymod mod = e.key.mod;
            if (mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) return false;
            switch (e.key.key) {
                case SDLK_1: SetMode(ViewMode::Stereo3D, true); return true;
                case SDLK_2: SetMode(ViewMode::LeftOnly, true); return true;
                case SDLK_3: SetMode(ViewMode::RightOnly, true); return true;
                case SDLK_4: SetMode(ViewMode::Difference, true); return true;
                case SDLK_5: SetMode(ViewMode::Anaglyph, true); return true;
                case SDLK_H:  // home: fit
                    SetZoom(1.0f);
                    if (host_.toast) host_.toast("Zoom: fit");
                    return true;
                case SDLK_Z: {  // toggle 1:1 <-> fit
                    const float one = host_.oneToOneZoom ? host_.oneToOneZoom() : 1.0f;
                    if (zoom_ > 1.0001f || one <= 1.0001f) SetZoom(1.0f);
                    else SetZoom(one);
                    char t[48];
                    std::snprintf(t, sizeof(t), zoom_ > 1.0001f ? "Zoom: 1:1 (%.0f%%)" : "Zoom: fit", zoom_ * 100.f);
                    if (host_.toast) host_.toast(t);
                    return true;
                }
                case SDLK_G:
                    bypass_ = !bypass_;
                    if (host_.toast) host_.toast(bypass_ ? "Grade bypassed" : "Grade on");
                    return true;
                case SDLK_TAB:
                    if (mod & SDL_KMOD_SHIFT) return false;  // Shift+Tab is the trunk's HUD master
                    inspectorPinned_ = !inspectorPinned_;
                    return true;
                default: return false;
            }
        }
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

void ProController::Tick(const Status& st) {
    stereoSource_ = st.stereo;
    PollDeckLink();
    std::string path;
    bool isCdl = false;
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (dialogReady_) {
            path = dialogPath_;
            isCdl = dialogIsCdl_;
            dialogReady_ = false;
        }
    }
    if (!path.empty()) {
        if (isCdl) LoadCdl(path);
        else LoadLut(path);
    }
    if (lutPending_ && renderer_) {
        lutPending_ = false;
        const float dmin = lut_.domainMin[0], dmax = lut_.domainMax[0];
        if (lut_.domainMin[0] != lut_.domainMin[1] || lut_.domainMin[0] != lut_.domainMin[2] ||
            lut_.domainMax[0] != lut_.domainMax[1] || lut_.domainMax[0] != lut_.domainMax[2])
            LOG_WARN("Pro: LUT has a per-channel domain; using the red channel's (%.3f..%.3f)", dmin, dmax);
        if (!renderer_->UploadLut3D(lut_.rgb.data(), (uint32_t)lut_.size, dmin, dmax)) {
            if (host_.toast) host_.toast("LUT upload failed");
            grade_.lutOn = false;
        }
    }
    if (!renderer_) return;
    GradeParams g = grade_;
    if (bypass_ || st.isLogo) {
        g = GradeParams{};
    }
    g.qcMode = st.isLogo ? 0 : mode_ == ViewMode::Difference ? 1 : mode_ == ViewMode::Anaglyph ? 2 : 0;
    renderer_->SetGrade(g);
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

void ProController::BuildUI(const Status& st, float uiAlpha) {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    const float alpha = inspectorPinned_ ? 1.0f : uiAlpha;
    if (alpha < 0.01f) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const float panelW = std::clamp(W * 0.24f, 280.0f, 440.0f);
    const float top = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f + 8.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::SetNextWindowPos(ImVec2(W - panelW - 8.0f, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 0.0f), ImVec2(panelW, H - top - 90.0f));
    ImGui::SetNextWindowBgAlpha(0.82f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("Inspector  (Tab)", nullptr, flags)) {
        // --- Source ---------------------------------------------------------
        if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (st.sourceLabel.empty() || st.isLogo) {
                ImGui::TextDisabled("Nothing loaded. Open a file or a live source.");
            } else {
                ImGui::Text("%s  %s", st.sourceKind.c_str(), st.sourceLabel.c_str());
                ImGui::TextDisabled("%dx%d  %s  %s", st.width, st.height, st.layoutName.c_str(),
                                    st.stereo ? "stereo" : "mono");
                if (st.sourceFps > 0.0f)
                    ImGui::TextDisabled("source %.1f fps  panel %.0f fps  age %.0f ms", st.sourceFps,
                                        st.panelFps, st.frameAgeMs);
                else
                    ImGui::TextDisabled("panel %.0f fps  mode %s", st.panelFps, st.modeName.c_str());
            }
        }

        BuildDeckLinkUI();

        // --- View -----------------------------------------------------------
        if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < (int)ViewMode::Count; ++i) {
                const ViewMode m = (ViewMode)i;
                const bool needsStereo = m == ViewMode::Difference || m == ViewMode::Anaglyph;
                if (i > 0) ImGui::SameLine();
                ImGui::BeginDisabled(needsStereo && !st.stereo);
                char label[32];
                std::snprintf(label, sizeof(label), "%d %s", i + 1,
                              m == ViewMode::Stereo3D ? "3D"
                              : m == ViewMode::LeftOnly ? "L"
                              : m == ViewMode::RightOnly ? "R"
                              : m == ViewMode::Difference ? "Diff" : "Ana");
                if (ImGui::RadioButton(label, mode_ == m)) SetMode(m, false);
                ImGui::EndDisabled();
            }
            ImGui::Text("Zoom %.0f%%", zoom_ * 100.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("Fit (H)")) SetZoom(1.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("1:1 (Z)")) SetZoom(host_.oneToOneZoom ? host_.oneToOneZoom() : 1.0f);
            ImGui::SameLine();
            const bool fs = host_.isFullscreen && host_.isFullscreen();
            if (ImGui::SmallButton(fs ? "Windowed (F11)" : "Full screen (F11)") && host_.toggleFullscreen)
                host_.toggleFullscreen();
            ImGui::TextDisabled("Wheel zooms, middle-drag pans");
        }

        // --- Stereo ---------------------------------------------------------
        if (ImGui::CollapsingHeader("Stereo", ImGuiTreeNodeFlags_DefaultOpen)) {
            float conv = host_.getConvergence ? host_.getConvergence() * 100.0f : 0.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            if (ImGui::SliderFloat("Convergence", &conv, -5.0f, 5.0f, "%+.2f%%") && host_.setConvergence)
                host_.setConvergence(conv / 100.0f);
            bool swap = host_.getSwapEyes && host_.getSwapEyes();
            if (ImGui::Checkbox("Swap eyes (X)", &swap) && host_.setSwapEyes) host_.setSwapEyes(swap);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset conv. (0)") && host_.setConvergence) host_.setConvergence(0.0f);
        }

        // --- Color ----------------------------------------------------------
        if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Bypass all (G)", &bypass_);
            ImGui::BeginDisabled(bypass_);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::SliderFloat("Exposure", &grade_.exposure, -4.0f, 4.0f, "%+.2f EV");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) grade_.exposure = 0.0f;

            // LUT
            ImGui::SeparatorText("LUT");
            if (ImGui::Button("Load .cube...")) OpenLookDialog(false);
            if (!lutName_.empty()) {
                ImGui::SameLine();
                ImGui::Checkbox("On##lut", &grade_.lutOn);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##lut")) {
                    lutName_.clear();
                    lutPath_.clear();
                    grade_.lutOn = false;
                    if (renderer_) renderer_->ClearLut3D();
                }
                ImGui::TextDisabled("%s  (%d^3)", lutName_.c_str(), lut_.size);
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("or drop one on the window");
            }

            // CDL
            ImGui::SeparatorText("ASC CDL");
            if (ImGui::Button("Load .cdl / .cc...")) OpenLookDialog(true);
            ImGui::SameLine();
            ImGui::Checkbox("On##cdl", &grade_.cdlOn);
            if (!cdlName_.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", cdlName_.c_str());
            }
            ImGui::BeginDisabled(!grade_.cdlOn);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::DragFloat3("Slope", grade_.slope, 0.005f, 0.0f, 4.0f, "%.3f");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::DragFloat3("Offset", grade_.offset, 0.002f, -1.0f, 1.0f, "%.3f");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::DragFloat3("Power", grade_.power, 0.005f, 0.1f, 4.0f, "%.3f");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            ImGui::DragFloat("Saturation", &grade_.saturation, 0.005f, 0.0f, 4.0f, "%.3f");
            if (ImGui::SmallButton("Reset CDL")) {
                for (int i = 0; i < 3; ++i) {
                    grade_.slope[i] = cdl_.slope[i];
                    grade_.offset[i] = cdl_.offset[i];
                    grade_.power[i] = cdl_.power[i];
                }
                grade_.saturation = cdl_.saturation;
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }

        // --- Rig check (Phase 2) --------------------------------------------
        if (ImGui::CollapsingHeader("Rig check")) {
            ImGui::TextWrapped("Use 4 (Difference) to check alignment and sync: a well-aligned, in-sync "
                               "pair shows edges only where there is depth. Measured vertical error, eye "
                               "sync and colour match are Phase 2 (docs/PRO.md).");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
#else
    (void)st;
    (void)uiAlpha;
#endif
}

// ---------------------------------------------------------------------------
// DeckLink input
// ---------------------------------------------------------------------------

bool ProController::StartDeckLink(DeckLinkSource::Mode mode, int left, int right) {
    if (!deckLink_.Open(mode, left, right)) {
        if (host_.toast) host_.toast("DeckLink: " + deckLink_.LastError());
        return false;
    }
    deckLinkActive_ = true;
    deckLinkSeen_ = DeckLinkSource::State::Opening;
    if (host_.deckLinkStarted) host_.deckLinkStarted();
    if (host_.toast) host_.toast("DeckLink: " + deckLink_.Description());
    return true;
}

bool ProController::StartDeckLinkSpec(const std::string& spec) {
    int a = 0, b = -1;
    const int n = std::sscanf(spec.c_str(), "%d,%d", &a, &b);
    if (n < 1) {
        if (host_.toast) host_.toast("DeckLink: bad --decklink value '" + spec + "'");
        return false;
    }
    return n == 2 ? StartDeckLink(DeckLinkSource::Mode::Dual, a, b)
                  : StartDeckLink(DeckLinkSource::Mode::Single, a, a);
}

void ProController::StopDeckLink() {
    const bool was = deckLinkActive_;
    deckLink_.Stop();
    deckLinkActive_ = false;
    deckLinkSeen_ = DeckLinkSource::State::Closed;
    if (was && host_.deckLinkStopped) host_.deckLinkStopped();
}

void ProController::PollDeckLink() {
    if (!deckLinkActive_) return;
    const DeckLinkSource::State s = deckLink_.GetState();
    if (s == deckLinkSeen_) return;
    deckLinkSeen_ = s;
    if (!host_.toast) return;
    switch (s) {
        case DeckLinkSource::State::NoSignal: host_.toast("DeckLink: no signal"); break;
        case DeckLinkSource::State::Failed: host_.toast("DeckLink: " + deckLink_.LastError()); break;
        case DeckLinkSource::State::Streaming: host_.toast("DeckLink: live"); break;
        default: break;
    }
}

void ProController::BuildDeckLinkUI() {
#if defined(MEDIAPLAYER_WITH_IMGUI)
    if (!ImGui::CollapsingHeader("SDI / HDMI input (DeckLink)")) return;
    if (!dlScanned_ || ImGui::SmallButton("Rescan")) {
        dlDevices_ = DeckLinkSource::Enumerate();
        dlUnavailable_ = dlDevices_.empty() ? DeckLinkSource::Unavailable() : std::string();
        dlScanned_ = true;
    }
    if (dlDevices_.empty()) {
        ImGui::TextWrapped("%s", dlUnavailable_.c_str());
        return;
    }
    auto deviceCombo = [&](const char* label, int& sel) {
        const char* cur = "?";
        for (const auto& d : dlDevices_)
            if (d.index == sel) cur = d.name.c_str();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
        if (ImGui::BeginCombo(label, cur)) {
            for (const auto& d : dlDevices_) {
                char row[160];
                std::snprintf(row, sizeof(row), "%s%s##dl%d", d.name.c_str(),
                              d.formatDetection ? "" : "  (no auto-detect)", d.index);
                if (ImGui::Selectable(row, sel == d.index)) sel = d.index;
            }
            ImGui::EndCombo();
        }
    };
    ImGui::BeginDisabled(deckLinkActive_);
    ImGui::RadioButton("One input, side by side", &dlMode_, 0);
    ImGui::RadioButton("Two inputs, left + right camera", &dlMode_, 1);
    deviceCombo(dlMode_ == 1 ? "Left" : "Input", dlLeft_);
    if (dlMode_ == 1) deviceCombo("Right", dlRight_);
    ImGui::EndDisabled();
    if (!deckLinkActive_) {
        if (ImGui::Button("Start"))
            StartDeckLink(dlMode_ == 1 ? DeckLinkSource::Mode::Dual : DeckLinkSource::Mode::Single, dlLeft_,
                          dlRight_);
    } else {
        if (ImGui::Button("Stop")) StopDeckLink();
        const DeckLinkSource::Stats s = deckLink_.GetStats();
        ImGui::SameLine();
        ImGui::Text("%s", DeckLinkSource::StateName(deckLink_.GetState()));
        ImGui::TextDisabled("%s  %dx%d  %s  %.1f fps", s.modeName.c_str(), s.width, s.height,
                            s.tenBit ? "10-bit" : "8-bit", s.fps);
        if (!s.timecode.empty()) ImGui::TextDisabled("TC %s", s.timecode.c_str());
        if (dlMode_ == 1)
            ImGui::TextDisabled("eye skew %.2f ms (max %.2f)  unpaired %llu", s.lastSkewMs, s.maxSkewMs,
                                (unsigned long long)s.unpaired);
    }
#endif
}

}  // namespace mp::pro
