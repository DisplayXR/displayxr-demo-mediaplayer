// SPDX-License-Identifier: Apache-2.0
#include "ui/TransportUI.h"

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#endif

namespace mp::ui {

namespace {
constexpr double kFadeSeconds = 0.20;       // UI fade in/out duration
constexpr double kToastFadeSeconds = 0.30;  // toast fade in/out duration
constexpr double kToastSeconds = 1.5;       // how long a toast stays fully lit
// Scrub velocity threshold: a sweep faster than this (in seconds of media per
// frame) previews keyframes instead of decoding exact frames.
constexpr float kFastScrubSeconds = 1.0f;
}  // namespace

void TickTransportUI(TransportState& s, double dt, bool uiAwake) {
    if (dt < 0.0 || dt > 0.25) dt = 0.0;  // ignore the first frame and big hitches

    // Auto-hide fade.
    const float fadeTarget = uiAwake ? 1.0f : 0.0f;
    const float fadeStep = (float)(dt / kFadeSeconds);
    if (s.fadeAlpha < fadeTarget) s.fadeAlpha = std::min(fadeTarget, s.fadeAlpha + fadeStep);
    else if (s.fadeAlpha > fadeTarget) s.fadeAlpha = std::max(fadeTarget, s.fadeAlpha - fadeStep);

    // Toast fade: ease toward 1 while live, toward 0 once expired; clear when gone.
    if (s.toastSecondsLeft > 0.0) s.toastSecondsLeft -= dt;
    const bool toastLive = !s.toastText.empty() && s.toastSecondsLeft > 0.0;
    const float toastTarget = toastLive ? 1.0f : 0.0f;
    const float toastStep = (float)(dt / kToastFadeSeconds);
    if (s.toastAlpha < toastTarget) s.toastAlpha = std::min(toastTarget, s.toastAlpha + toastStep);
    else if (s.toastAlpha > toastTarget) s.toastAlpha = std::max(toastTarget, s.toastAlpha - toastStep);
    if (!toastLive && s.toastAlpha <= 0.001f) s.toastText.clear();
}

void ShowTransportToast(TransportState& s, std::string msg) {
    s.toastText = std::move(msg);
    s.toastSecondsLeft = kToastSeconds;
}

#if defined(MEDIAPLAYER_WITH_IMGUI)

// Polished "dark glass" look: generous rounding, padded controls, faint translucent
// surfaces, one cyan accent. Centralizes all styling so the per-widget code stays clean.
//
// Moved here from ImGuiLayer so both legs share it. `k` scales metrics for touch:
// ImGui derives the icon-button size from GetFrameHeight() (= FontSize + FramePadding.y*2)
// and every glyph inside IconButton is a fraction of that, so one scalar enlarges the
// whole control set correctly with no per-widget touch code. TouchExtraPadding grows hit
// rectangles without moving a single pixel.
void ApplyMediaPlayerStyle(float k) {
    if (!(k > 0.0f)) k = 1.0f;
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    // ---- identical on every leg ----
    s.WindowRounding = 16.0f;
    s.FrameRounding = 10.0f;
    s.GrabRounding = 10.0f;
    s.PopupRounding = 10.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;

    // ---- the one divergence: metrics scale, design does not ----
    s.WindowPadding = ImVec2(18.0f * k, 14.0f * k);
    s.FramePadding = ImVec2(14.0f * k, 9.0f * k);
    s.ItemSpacing = ImVec2(12.0f * k, 10.0f * k);
    s.GrabMinSize = 18.0f * k;
    // Hit-only padding: enlarges the grab/press rectangles, draws nothing.
    s.TouchExtraPadding = (k > 1.0f) ? ImVec2(10.0f, 14.0f) : ImVec2(0.0f, 0.0f);
    // Legible against the HUD canvas (the desktop's long-standing 1.9), nudged up
    // slightly for arm's-length tablet viewing.
    ImGui::GetIO().FontGlobalScale = 1.9f * (k > 1.0f ? 1.15f : 1.0f);

    const ImVec4 accent(0.20f, 0.65f, 1.00f, 1.00f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.62f);
    c[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.16f);
    c[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    c[ImGuiCol_ButtonHovered] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.78f, 1.00f, 1.00f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
}

namespace {
constexpr float kPi = 3.14159265358979f;  // IM_PI lives in imgui_internal.h; keep our own
enum class Icon { Play, Pause, Loop, Slideshow, Speaker, SpeakerMuted };

// A borderless icon button: an invisible hit-target with a hand-drawn glyph centered
// in it (no icon font needed). `active` tints it with the accent color (toggle-on);
// hover brightens. Returns true on click. Glyph colors go through GetColorU32 so the
// surrounding ImGui Alpha (the auto-hide fade) applies for free.
//
// `hitScale` grows the invisible hit target WITHOUT changing the drawn glyph, so a
// touch leg gets finger-sized targets while looking identical to the desktop.
bool IconButton(const char* id, Icon kind, float size, bool active = false,
                float hitScale = 1.0f) {
    const float hit = size * (hitScale > 1.0f ? hitScale : 1.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(hit, hit));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();   // pointer held down — immediate press feedback
    // Center the glyph in the (possibly enlarged) hit box so the drawn size is
    // independent of the touch padding.
    const ImVec2 c(p.x + hit * 0.5f, p.y + hit * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Toggle state (accent) must read THROUGH hover — hover/press only brighten the base,
    // they don't replace the accent, so an active toggle stays clearly cyan when hovered.
    ImVec4 col = active ? ImVec4(0.20f, 0.65f, 1.00f, 1.0f)       // toggled on  -> accent
                        : ImVec4(0.88f, 0.91f, 0.95f, 1.0f);      // off         -> light grey
    if (held) col = active ? ImVec4(0.55f, 0.85f, 1.00f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    else if (hovered) col = active ? ImVec4(0.42f, 0.78f, 1.00f, 1.0f)
                                   : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    const ImU32 u = ImGui::GetColorU32(col);
    const float r = size * 0.30f;
    const float th = size * 0.11f;

    switch (kind) {
        case Icon::Play: {
            const float h = size * 0.30f;
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.7f, c.y - h),
                                  ImVec2(c.x - h * 0.7f, c.y + h),
                                  ImVec2(c.x + h, c.y), u);
            break;
        }
        case Icon::Pause: {
            const float bw = size * 0.13f, bh = size * 0.30f, gap = size * 0.10f;
            dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh), ImVec2(c.x - gap, c.y + bh), u,
                              th * 0.4f);
            dl->AddRectFilled(ImVec2(c.x + gap, c.y - bh), ImVec2(c.x + gap + bw, c.y + bh), u,
                              th * 0.4f);
            break;
        }
        case Icon::Loop: {
            // Two arcs forming a near-circle, with a small arrowhead at each open end.
            dl->PathArcTo(c, r, kPi * 0.30f, kPi * 0.95f);
            dl->PathStroke(u, 0, th);
            dl->PathArcTo(c, r, kPi * 1.30f, kPi * 1.95f);
            dl->PathStroke(u, 0, th);
            const float ah = size * 0.13f;
            const ImVec2 e1(c.x + r * std::cos(kPi * 0.95f), c.y + r * std::sin(kPi * 0.95f));
            dl->AddTriangleFilled(ImVec2(e1.x - ah, e1.y), ImVec2(e1.x + ah * 0.4f, e1.y - ah),
                                  ImVec2(e1.x + ah * 0.4f, e1.y + ah), u);
            const ImVec2 e2(c.x + r * std::cos(kPi * 1.95f), c.y + r * std::sin(kPi * 1.95f));
            dl->AddTriangleFilled(ImVec2(e2.x + ah, e2.y), ImVec2(e2.x - ah * 0.4f, e2.y - ah),
                                  ImVec2(e2.x - ah * 0.4f, e2.y + ah), u);
            break;
        }
        case Icon::Slideshow: {
            // A photo frame with a small play triangle inside (auto-advancing stills).
            dl->AddRect(ImVec2(c.x - r, c.y - r * 0.78f), ImVec2(c.x + r, c.y + r * 0.78f), u,
                        size * 0.10f, 0, th * 0.8f);
            const float h = size * 0.16f;
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.5f, c.y - h), ImVec2(c.x - h * 0.5f, c.y + h),
                                  ImVec2(c.x + h, c.y), u);
            break;
        }
        case Icon::Speaker:
        case Icon::SpeakerMuted: {
            // Body box + cone (triangle pointing right).
            const float bx = -r * 0.35f;
            dl->AddRectFilled(ImVec2(c.x - r * 0.95f, c.y - r * 0.30f),
                              ImVec2(c.x + bx, c.y + r * 0.30f), u);
            dl->AddTriangleFilled(ImVec2(c.x + bx, c.y - r * 0.62f),
                                  ImVec2(c.x + bx, c.y + r * 0.62f), ImVec2(c.x + r * 0.25f, c.y), u);
            if (kind == Icon::Speaker) {  // two sound-wave arcs
                dl->PathArcTo(ImVec2(c.x + r * 0.1f, c.y), r * 0.58f, -kPi * 0.28f, kPi * 0.28f);
                dl->PathStroke(u, 0, th * 0.7f);
                dl->PathArcTo(ImVec2(c.x + r * 0.1f, c.y), r * 0.92f, -kPi * 0.28f, kPi * 0.28f);
                dl->PathStroke(u, 0, th * 0.7f);
            } else {                      // muted: a small X
                const float xc = c.x + r * 0.72f, d = r * 0.26f;
                dl->AddLine(ImVec2(xc - d, c.y - d), ImVec2(xc + d, c.y + d), u, th * 0.9f);
                dl->AddLine(ImVec2(xc - d, c.y + d), ImVec2(xc + d, c.y - d), u, th * 0.9f);
            }
            break;
        }
    }
    return ImGui::IsItemClicked();
}

// A media-player scrubber: a THIN track with a round knob, not ImGui's
// SliderFloat. A SliderFloat is a full-height frame widget (FontSize +
// FramePadding.y*2), so at any legible font size it renders as a fat bar — and at
// touch scale it dominates the transport. This draws a slim track while keeping
// the HIT area a full row tall, so it stays easy to grab with a finger.
//
// Returns true on a value change; `outActive` reports the held state. Both have
// exactly the semantics the caller's scrub machine expects from SliderFloat.
bool ScrubBar(const char* id, float* v, float vmax, float width, bool* outActive) {
    if (width < 8.0f) width = 8.0f;
    const float rowH = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(width, rowH));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    *outActive = active;

    bool changed = false;
    if (active) {
        const float t = (ImGui::GetIO().MousePos.x - p.x) / width;
        const float nv = std::min(1.0f, std::max(0.0f, t)) * vmax;
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }

    const float frac = (vmax > 0.0f) ? std::min(1.0f, std::max(0.0f, *v / vmax)) : 0.0f;
    const float cy = p.y + rowH * 0.5f;
    // Track thickness scales with the row so the touch build stays proportionate,
    // but stays genuinely thin.
    const float th = std::max(3.0f, rowH * 0.085f);
    const float knobR = th * ((hovered || active) ? 2.2f : 1.7f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Unplayed track, then the played portion in the accent colour.
    dl->AddRectFilled(ImVec2(p.x, cy - th * 0.5f), ImVec2(p.x + width, cy + th * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), th * 0.5f);
    if (frac > 0.0f) {
        dl->AddRectFilled(ImVec2(p.x, cy - th * 0.5f), ImVec2(p.x + width * frac, cy + th * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_SliderGrab), th * 0.5f);
    }
    dl->AddCircleFilled(ImVec2(p.x + width * frac, cy),
                        knobR,
                        ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive
                                                  : ImGuiCol_SliderGrab));
    return changed;
}

void Fire(const std::function<void()>& fn) { if (fn) fn(); }
}  // namespace

void BuildTransportUI(TransportState& s, const TransportActions& a) {
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const ImGuiWindowFlags pill = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_AlwaysAutoResize;

    // The bars fade in/out together with the idle timer.
    if (s.fadeAlpha > 0.001f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s.fadeAlpha);
        const float iconSz = ImGui::GetFrameHeight();

        // --- Top window-space bar: full width, flush to the top of the window.
        //     Open / Mode on the left, Slideshow on the right. ---
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(W, 0.0f), ImGuiCond_Always);
        const ImGuiWindowFlags topbar = pill & ~ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);  // flat bar, flush to edges
        if (ImGui::Begin("##wsui_top", nullptr, topbar)) {
            ImGui::BeginDisabled(s.openFilePending);
            if (ImGui::Button("Open")) Fire(a.Open);
            ImGui::EndDisabled();
            if (s.caps.mode) {
                ImGui::SameLine();
                char modeLabel[96];
                std::snprintf(modeLabel, sizeof(modeLabel), "Mode: %s", s.modeName.c_str());
                if (ImGui::Button(modeLabel)) Fire(a.NextMode);
            }
            // Stereo layout + override (#45). The trailing '*' marks a manual pin.
            if (s.caps.layout && s.hasMedia) {
                char layoutBtn[64];
                std::snprintf(layoutBtn, sizeof(layoutBtn), "Layout: %s%s", s.layoutName.c_str(),
                              s.layoutPinned ? "*" : "");
                ImGui::SameLine();
                if (ImGui::Button(layoutBtn)) Fire(a.CycleLayout);
                if (ImGui::IsItemHovered() && !s.layoutTooltip.empty()) {
                    ImGui::SetTooltip("%s\nL cycles: auto / mono / SBS-full / SBS-half",
                                      s.layoutTooltip.c_str());
                }
            }
            // Current filename, centered in the bar.
            if (!s.mediaFilename.empty()) {
                const float tw = ImGui::CalcTextSize(s.mediaFilename.c_str()).x;
                ImGui::SameLine();
                // Clamp, don't set: with a third button on the left the centred position
                // can fall BEHIND the cursor, and SetCursorPosX would happily back-track
                // and overlap the buttons. The filename may only ever slide right.
                ImGui::SetCursorPosX(
                    std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - tw) * 0.5f));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(s.mediaFilename.c_str());
            }
            // Right-align the slideshow toggle. (Close the app via ESC or the window's
            // own close button — no in-UI X needed.)
            if (s.caps.slideshow) {
                ImGui::SameLine(ImGui::GetWindowWidth() - iconSz -
                                ImGui::GetStyle().WindowPadding.x);
                if (IconButton("##slideshow", Icon::Slideshow, iconSz, s.slideshowActive,
                               s.hitTargetScale))
                    Fire(a.ToggleSlideshow);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();

        // --- Bottom transport: play/pause + scrubber + loop (video only) ---
        if (s.isVideo && s.durationSeconds > 0.0) {
            ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.955f), ImGuiCond_Always,
                                    ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(W * 0.8f, 0.0f), ImGuiCond_Always);
            const ImGuiWindowFlags bar = pill & ~ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::Begin("##transport", nullptr, bar)) {
                const float dur = (float)s.durationSeconds;
                const float pos = (float)s.positionSeconds;
                // Track playback into the knob, but not while dragging, and not while a
                // seek we issued is still landing — else it snaps back on release.
                if (!s.scrubActive) {
                    if (s.scrubTarget >= 0.0f) {
                        if (std::fabs(pos - s.scrubTarget) < 0.3f) s.scrubTarget = -1.0f;
                    } else {
                        s.scrubValue = pos;
                    }
                }
                auto mmss = [](double sec, char* out, size_t n) {
                    if (sec < 0.0) sec = 0.0;
                    const int t = (int)sec;
                    std::snprintf(out, n, "%d:%02d", t / 60, t % 60);
                };
                char cur[16], tot[16];
                mmss(s.scrubValue, cur, sizeof(cur));
                mmss(dur, tot, sizeof(tot));

                if (IconButton("##playpause", s.paused ? Icon::Play : Icon::Pause, iconSz, false,
                               s.hitTargetScale))
                    Fire(a.TogglePlayback);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", cur);
                ImGui::SameLine();
                // Leave room on the right for the total-time label + mute + loop icons.
                // Derived from the live style rather than a magic constant, so the
                // touch build spaces correctly instead of crowding the total time.
                const ImGuiStyle& st = ImGui::GetStyle();
                const int rightIcons = (s.caps.mute ? 1 : 0) + (s.caps.loop ? 1 : 0);
                const float rightW = ImGui::CalcTextSize(tot).x +
                                     (float)rightIcons * (iconSz * s.hitTargetScale) +
                                     (float)(rightIcons + 2) * st.ItemSpacing.x +
                                     st.WindowPadding.x;
                bool active = false;
                const bool changed = ScrubBar("##scrub", &s.scrubValue, dur,
                                              ImGui::GetContentRegionAvail().x - rightW, &active);
                const float scrubDelta = std::fabs(s.scrubValue - s.lastScrubValue);
                if (changed) {
                    // Fast sweep -> keyframe preview (responsive); slow/fine drag -> exact
                    // frame (precise). Audio goes silent while scrubbing.
                    const bool fast = active && scrubDelta > kFastScrubSeconds;
                    if (a.Seek) a.Seek(s.scrubValue, fast);
                    s.scrubWasPreview = fast;
                    s.scrubTarget = s.scrubValue;  // hold the knob until the decode lands
                    if (active) Fire(a.ScrubHeld);
                } else if (active && s.scrubWasPreview) {
                    // Settled after a fast sweep -> resolve to the exact frame.
                    if (a.Seek) a.Seek(s.scrubValue, false);
                    s.scrubWasPreview = false;
                }
                if (s.scrubActive && !active) {
                    // Released: settle on the exact frame and realign + resume audio.
                    if (a.ScrubReleased) a.ScrubReleased(s.scrubValue);
                    s.scrubWasPreview = false;
                }
                s.scrubActive = active;
                s.lastScrubValue = s.scrubValue;
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", tot);
                if (s.caps.mute) {
                    ImGui::SameLine();
                    if (IconButton("##mute", s.muted ? Icon::SpeakerMuted : Icon::Speaker, iconSz,
                                   s.muted, s.hitTargetScale))
                        Fire(a.ToggleMute);
                }
                if (s.caps.loop) {
                    ImGui::SameLine();
                    if (IconButton("##loop", Icon::Loop, iconSz, s.loop, s.hitTargetScale))
                        Fire(a.ToggleLoop);
                }
            }
            ImGui::End();
        }
        ImGui::PopStyleVar();
    }

    // --- Toast (convergence readout, nav filename): own alpha, shows even when hidden ---
    if (s.toastAlpha > 0.001f && !s.toastText.empty()) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s.toastAlpha);
        ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.14f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("##toast", nullptr, pill)) {
            ImGui::TextUnformatted(s.toastText.c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // --- Slideshow dip-to-black: a full-canvas overlay above everything. Because the
    //     HUD layer composites on top of the content layer, this dims the whole view. ---
    if (s.transitionAlpha > 0.001f) {
        const int alpha = (int)(std::min(1.0f, s.transitionAlpha) * 255.0f + 0.5f);
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H),
                                                      IM_COL32(0, 0, 0, alpha));
    }
}

#else  // !MEDIAPLAYER_WITH_IMGUI

void ApplyMediaPlayerStyle(float) {}
void BuildTransportUI(TransportState&, const TransportActions&) {}

#endif

}  // namespace mp::ui
