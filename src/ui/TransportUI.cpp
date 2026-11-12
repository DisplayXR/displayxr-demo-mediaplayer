// SPDX-License-Identifier: Apache-2.0
#include "ui/TransportUI.h"
#include "ui/Icons.h"
#include "ui/UiScale.h"
#include "ui/UiText.h"

#if defined(MEDIAPLAYER_WITH_IMGUI)
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

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

void ShowTransportToast(TransportState& s, std::string msg, double seconds) {
    s.toastText = std::move(msg);
    s.toastSecondsLeft = seconds > 0.0 ? seconds : kToastSeconds;
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
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();  // start from defaults so re-applying at a new UI scale never compounds
    ImGui::StyleColorsDark();

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

    s.ScaleAllSizes(g_uiScale);
}

float UiFontPx(float k) {
    // Touch is bigger for arm's-length tablet viewing: 30 is 40 px on the 320 dpi NP02J,
    // 16 is 40 px at Windows' 250%. Rounded up to a whole pixel; the epsilon keeps float
    // error (320 / 240 = 1.3333334f) from pushing a whole size to the next one.
    return std::ceil((k > 1.0f ? 30.0f : 16.0f) * g_uiScale - 0.001f);
}

float SwitchFontPx(float k) { return std::ceil(UiFontPx(k) * 1.2f - 0.001f); }

float TextStylePx(TextStyle style, float k) {
    if (!(k > 1.0f)) return UiFontPx(k);  // desktop: one size, as before
    const float dp = style == TextStyle::Title ? 22.0f : style == TextStyle::Row ? 18.0f : 14.0f;
    return std::ceil(Dp(dp) - 0.001f);
}

ImFont* StyleFont(TextStyle style) {
    switch (style) {
        case TextStyle::Title: return g_titleFont;
        case TextStyle::Row: return g_rowFont;
        case TextStyle::Small: return g_smallFont;
    }
    return nullptr;
}

namespace {
// A borderless icon button: an invisible hit-target with a hand-drawn glyph centered
// in it (ui/Icons.h, no icon font needed). `active` tints it with the accent color
// (toggle-on); hover brightens. Returns true on click. Glyph colors go through
// GetColorU32 so the surrounding ImGui Alpha (the auto-hide fade) applies for free.
//
// `hitScale` grows the invisible hit target WITHOUT changing the drawn glyph, so a
// touch leg gets finger-sized targets while looking identical to the desktop.
bool IconButton(const char* id, Glyph kind, float size, bool active = false,
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
    DrawGlyph(dl, kind, c, size, ImGui::GetColorU32(col));
    return ImGui::IsItemClicked();
}

// The touch player's 2D/3D switch (#180), drawn like the stock player's on the same tablets: a
// pill track and a round knob naming the mode in force, on the right for 3D and on the
// left for 2D. `size` is the knob's diameter; the hit box grows like IconButton's.
bool ModeSwitch(const char* id, bool on3D, float size, float hitScale) {
    const float pad = size * (std::max(1.0f, hitScale) - 1.0f) * 0.5f;
    const float trackW = size * 1.5f, trackH = size * 0.45f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(trackW + pad * 2.0f, size + pad * 2.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = p.x + pad, cy = p.y + pad + size * 0.5f, r = size * 0.5f;
    dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(x0 + trackW, cy + trackH * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_WindowBg), trackH * 0.5f);  // the bars' background
    const ImVec2 c(on3D ? x0 + trackW - r : x0 + r, cy);
    // A 1 px dark grey ring parts the knob from the track.
    dl->AddCircleFilled(c, r, ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.32f, 1.0f)));
    dl->AddCircleFilled(c, r - 1.0f, ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.06f, 1.0f)));
    const char* label = on3D ? "3D" : "2D";
    const float fontPx = g_switchFont->FontSize;
    const ImVec2 ts = g_switchFont->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, label);
    dl->AddText(g_switchFont, fontPx,
                ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)),
                ImGui::GetColorU32(ImVec4(0.96f, 0.96f, 0.97f, 1.0f)), label);
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
    if (width < Px(8.0f)) width = Px(8.0f);
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
    const float th = std::max(Px(3.0f), rowH * 0.085f);
    const float knobR = th * ((hovered || active) ? 2.2f : 1.7f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Unplayed track, then the played portion in the accent colour.
    // FrameBg is tuned for widgets on a panel and all but vanishes on the dark strip: a
    // translucent text tint reads on any background and stays clearly below the accent.
    dl->AddRectFilled(ImVec2(p.x, cy - th * 0.5f), ImVec2(p.x + width, cy + th * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_Text, (hovered || active) ? 0.38f : 0.26f), th * 0.5f);
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

// Track playback into the scrubber's knob, but not while dragging, and not while a seek we
// issued is still landing — else it snaps back on release.
void TrackScrubKnob(TransportState& s, float pos) {
    if (!s.scrubActive) {
        if (s.scrubTarget >= 0.0f) {
            if (std::fabs(pos - s.scrubTarget) < 0.3f) s.scrubTarget = -1.0f;
        } else {
            s.scrubValue = pos;
        }
    }
}

// The scrubber at the cursor, `width` wide, and the seek machine behind it.
void DriveScrub(TransportState& s, const TransportActions& a, float dur, float width) {
    bool active = false;
    const bool changed = ScrubBar("##scrub", &s.scrubValue, dur, width, &active);
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
}

// The top bar's live-source picker: a combo next to Open. Live cameras (#90, refreshed
// once each time it opens, so enumeration stays lazy) and the "Stream URL..." row
// (#93), each behind its own cap.
void CameraCombo(TransportState& s, const TransportActions& a) {
    const bool live = s.caps.camera && s.isLive;
    const bool stream = s.caps.url && s.isStream;
    const char* preview = live ? "Camera" : stream ? "Stream" : "Live...";
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Live...").x + st.FramePadding.x * 2.0f +
                            ImGui::GetFrameHeight());
    const bool open = ImGui::BeginCombo("##camera", preview, ImGuiComboFlags_HeightLarge);
    if (open) {
        if (s.caps.camera) {
            if (!s.cameraComboWasOpen && a.RefreshCameras) a.RefreshCameras(false);
            if (s.cameraNames.empty()) {
                ImGui::BeginDisabled();
                ImGui::Selectable("No cameras found", false);
                ImGui::EndDisabled();
            }
            for (int row = 0; row < (int)s.cameraNames.size(); ++row) {
                ImGui::PushID(row);
                if (ImGui::Selectable(s.cameraNames[(size_t)row].c_str(),
                                      live && row == s.cameraCurrent) &&
                    a.OpenCamera)
                    a.OpenCamera(row);
                ImGui::PopID();
            }
            ImGui::Separator();
        }
        // Opens the URL popup (drawn at top level by UrlPopup, after this combo closes).
        if (s.caps.url && ImGui::Selectable("Stream URL...", stream)) s.urlPopupRequest = true;
        if (s.caps.camera &&
            ImGui::Selectable("Rescan", false, ImGuiSelectableFlags_DontClosePopups) &&
            a.RefreshCameras)
            a.RefreshCameras(true);
        if ((live || stream) && ImGui::Selectable("Stop live")) Fire(a.StopCamera);
        ImGui::EndCombo();
    }
    s.cameraComboWasOpen = open;
}

// Stream URL popup (#93): a text field prefilled with the last URL, Enter / Connect
// opens it, Esc / Cancel closes. Top level (outside the fading bars), so Ctrl+U works
// while the chrome is hidden.
void UrlPopup(TransportState& s, const TransportActions& a) {
    const char* kTitle = "Stream URL";
    if (s.urlPopupRequest) {
        s.urlPopupRequest = false;
        std::snprintf(s.urlBuf, sizeof(s.urlBuf), "%s", s.urlSeed.c_str());
        ImGui::OpenPopup(kTitle);
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.35f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::TextUnformatted("rtsp:// rtmp:// srt:// udp:// rtp:// http(s):// .m3u8 or .mpd");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
    bool connect = ImGui::InputText("##url", s.urlBuf, sizeof(s.urlBuf),
                                    ImGuiInputTextFlags_EnterReturnsTrue |
                                        ImGuiInputTextFlags_AutoSelectAll);
    const bool empty = (s.urlBuf[0] == '\0');
    ImGui::BeginDisabled(empty);
    connect = ImGui::Button("Connect") || connect;
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (connect && !empty) {
        const std::string url = s.urlBuf;
        ImGui::CloseCurrentPopup();
        if (a.OpenUrl) a.OpenUrl(url);
    } else if (cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// A status pill in the filename slot: dot + text, click = TogglePlayback (freeze).
// Shared by the LIVE (camera, #90) and STREAM (#93) pills.
void StatusPill(const char* id, const char* text, const ImVec4& dot, bool frozen,
                const TransportActions& a) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float dotR = ImGui::GetFontSize() * 0.22f;
    const float gap = st.ItemInnerSpacing.x * 2.0f;
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float w = st.FramePadding.x * 2.0f + dotR * 2.0f + gap + ts.x;

    ImGui::SameLine();
    // Same centre-clamp as the filename: it may only ever slide right of the buttons.
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - w) * 0.5f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(id, ImVec2(w, h))) Fire(a.TogglePlayback);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetTooltip("%s", frozen ? "Space: resume" : "Space: freeze");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                      st.FrameRounding);
    const float cx = p.x + st.FramePadding.x + dotR;
    const float cy = p.y + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, cy), dotR, ImGui::GetColorU32(dot));
    dl->AddText(ImVec2(cx + dotR + gap, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
                text);
}

const ImVec4 kDotGrey(0.55f, 0.57f, 0.60f, 1.0f);   // gone / blocked / error / failed
const ImVec4 kDotAmber(0.96f, 0.70f, 0.20f, 1.0f);  // frozen / opening / (re)connecting

// The LIVE pill (#90): replaces the centred filename while a camera is the source.
// A status dot (red = live, amber = frozen / opening / no signal, grey = gone), the
// device, and the three numbers that matter for a live feed. Click = freeze/resume.
void LivePill(const TransportState& s, const TransportActions& a) {
    char text[192];
    const bool streaming = (s.liveStatus == "LIVE" || s.liveStatus == "FROZEN");
    if (streaming) {
        // U+00B7 middle dot, spelled as UTF-8 bytes (ImGui's default font covers Latin-1).
        std::snprintf(text, sizeof(text),
                      "%s  %s  \xC2\xB7  cam %.0f  \xC2\xB7  panel %.0f fps  \xC2\xB7  %.0f ms",
                      s.liveStatus.c_str(), s.liveDevice.c_str(), s.cameraFps, s.panelFps,
                      s.frameAgeMs);
    } else {
        std::snprintf(text, sizeof(text), "%s  %s", s.liveStatus.c_str(), s.liveDevice.c_str());
    }
    ImVec4 dot = kDotGrey;
    if (s.liveStatus == "LIVE")
        dot = ImVec4(235.0f / 255.0f, 64.0f / 255.0f, 52.0f / 255.0f, 1.0f);  // red
    else if (s.liveStatus == "FROZEN" || s.liveStatus == "OPENING" || s.liveStatus == "NO SIGNAL")
        dot = kDotAmber;
    StatusPill("##livepill", text, dot, s.livePaused, a);
}

// The STREAM pill (#93): host, decoded fps and bitrate. Blue-ish dot while streaming,
// amber while frozen / connecting / reconnecting, grey once the stream failed.
void StreamPill(const TransportState& s, const TransportActions& a) {
    char text[192];
    if (s.streamStatus == "STREAM" || s.streamStatus == "FROZEN") {
        std::snprintf(text, sizeof(text), "%s  %s  \xC2\xB7  %.0f fps  \xC2\xB7  %.1f Mbit/s%s%s",
                      s.streamStatus.c_str(), s.streamHost.c_str(), s.streamFps,
                      s.streamKbps / 1000.0f, s.streamQuality.empty() ? "" : "  \xC2\xB7  ",
                      s.streamQuality.c_str());
    } else {
        std::snprintf(text, sizeof(text), "%s  %s", s.streamStatus.c_str(), s.streamHost.c_str());
    }
    ImVec4 dot = kDotGrey;
    if (s.streamStatus == "STREAM")
        dot = ImVec4(0.30f, 0.62f, 1.00f, 1.0f);  // blue
    else if (s.streamStatus != "FAILED")
        dot = kDotAmber;
    StatusPill("##streampill", text, dot, s.streamStatus == "FROZEN", a);
}

// ---- The touch layout (#180) ----------------------------------------------------------

constexpr float kScrimAlpha = 0.60f;  // the scrims' darkest, at the screen's edge
constexpr float kScrimDp = 112.0f;    // how far they reach in from the system bar

// A font's pixel size, the UI font's for null.
float FontPx(ImFont* f) { return f ? f->FontSize : ImGui::GetFontSize(); }

// The width of a time label in `font` that can never grow: `sample` with every digit as
// the font's widest, so neither the label nor the scrubber beside it moves as it counts.
float TimeSlotW(ImFont* font, const char* sample) {
    ImFont* f = font ? font : ImGui::GetFont();
    const float px = FontPx(font);
    char wide = '0';
    float wideW = 0.0f;
    for (char d = '0'; d <= '9'; d++) {
        const char ds[2] = {d, '\0'};
        const float w = f->CalcTextSizeA(px, FLT_MAX, 0.0f, ds).x;
        if (w > wideW) {
            wideW = w;
            wide = d;
        }
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s", sample);
    for (char* p = buf; *p; p++)
        if (*p >= '0' && *p <= '9') *p = wide;
    return std::ceil(f->CalcTextSizeA(px, FLT_MAX, 0.0f, buf).x);
}

// `text` in `font` at `pos`, right-aligned in `slotW` when `right`.
void TextAt(ImFont* font, ImVec2 pos, const char* text, ImU32 col, float slotW = 0.0f, bool right = false) {
    ImFont* f = font ? font : ImGui::GetFont();
    const float px = FontPx(font);
    if (right) pos.x += slotW - f->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
    ImGui::GetWindowDrawList()->AddText(f, px, ImVec2(std::round(pos.x), std::round(pos.y)), col, text);
}

// The 2D/3D switch's width (SegmentSwitch).
float SegmentSwitchW() {
    ImFont* f = StyleFont(TextStyle::Row);
    ImFont* font = f ? f : ImGui::GetFont();
    const float seg = font->CalcTextSizeA(FontPx(f), FLT_MAX, 0.0f, "3D").x + Dp(28.0f);
    return std::round(seg) * 2.0f;
}

// The touch layout's 2D/3D switch: a labelled two-segment control, "2D | 3D", the mode in
// force filled with the accent. A dark fill and a light 2 dp outline make it read over a
// dark picture and a light one alike. Drawn `h` tall in a `hitH` tall hit box at the
// cursor. True when the other segment is tapped.
bool SegmentSwitch(const char* id, bool on3D, float h, float hitH) {
    const float w = SegmentSwitchW(), seg = w * 0.5f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, hitH));
    const bool tapped = ImGui::IsItemClicked();
    const bool tapped3D = ImGui::GetIO().MouseClickedPos[0].x >= p.x + seg;
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, std::round(p.y + (hitH - h) * 0.5f)), b(p.x + w, a.y + h);
    const float r = h * 0.5f;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, held ? 0.55f : 0.40f)), r);
    const ImVec2 on0(on3D ? a.x + seg : a.x, a.y), on1(on3D ? b.x : a.x + seg, b.y);
    dl->AddRectFilled(on0, on1, ImGui::GetColorU32(ImGuiCol_SliderGrab), r,
                      on3D ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft);
    const float stroke = std::max(Px(2.0f), g_iconStroke);
    dl->AddRect(a, b, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.85f)), r, 0, stroke);

    ImFont* f = StyleFont(TextStyle::Row);
    ImFont* font = f ? f : ImGui::GetFont();
    const float px = FontPx(f);
    for (int i = 0; i < 2; ++i) {
        const char* label = i ? "3D" : "2D";
        const bool on = (i == 1) == on3D;
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, label);
        const ImVec2 at(a.x + seg * (float)i + (seg - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f);
        dl->AddText(font, px, ImVec2(std::round(at.x), std::round(at.y)),
                    ImGui::GetColorU32(on ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.86f, 0.88f, 0.92f, 1.0f)),
                    label);
    }
    return tapped && tapped3D != on3D;
}

// The touch player's chrome: scrims instead of slabs, the back arrow and the file's name
// at the top left, the transport (or, for a still, the 2D/3D switch) at the bottom.
//
// 3D comfort: everything is drawn once into the HUD, which both views share, so it sits
// flat at the screen plane. A gradient scrim instead of an opaque bar means no hard edge
// cuts a flat plane across the picture's depth.
void TouchChrome(TransportState& s, const TransportActions& a, float W, float H) {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    const float iconSz = ImGui::GetFrameHeight();
    const float hit = iconSz * std::max(1.0f, s.hitTargetScale);
    const float top = g_systemBarTop, bottom = H - g_systemBarBottom;
    const float edge = std::round(Dp(8.0f));
    const bool video = s.isVideo && s.durationSeconds > 0.0;
    ImFont* titleFont = StyleFont(TextStyle::Title);
    ImFont* smallFont = StyleFont(TextStyle::Small);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim = ImGui::GetColorU32(ImVec4(0.80f, 0.83f, 0.88f, 1.0f));

    // Scrims: darkest under the system bars, so their icons read, then fading out.
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const ImU32 dark = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, kScrimAlpha));
    const ImU32 clear = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const float ramp = Dp(kScrimDp);
    if (top > 0.0f) bg->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(W, top), dark);
    bg->AddRectFilledMultiColor(ImVec2(0.0f, top), ImVec2(W, top + ramp), dark, dark, clear, clear);
    if (video || s.caps.modeSwitch) {
        bg->AddRectFilledMultiColor(ImVec2(0.0f, bottom - ramp), ImVec2(W, bottom), clear, clear, dark, dark);
        if (bottom < H) bg->AddRectFilled(ImVec2(0.0f, bottom), ImVec2(W, H), dark);
    }

    // --- Top: back, then the name and its place in the list, left-aligned ---
    ImGui::SetNextWindowPos(ImVec2(0.0f, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(edge, edge));
    if (ImGui::Begin("##touch_top", nullptr, flags)) {
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(s.openFilePending);
        if (IconButton("##back", Glyph::Back, iconSz, false, s.hitTargetScale)) Fire(a.Open);
        ImGui::EndDisabled();
        const float x = row.x + hit + ImGui::GetStyle().ItemSpacing.x * 0.5f;
        const float right = W - edge;
        if (!s.mediaFilename.empty() && right > x) {
            ImGui::PushFont(titleFont);
            const std::string name = FitText(s.mediaFilename, right - x);
            ImGui::PopFont();
            const float titleH = FontPx(titleFont);
            const float posH = s.mediaPosition.empty() ? 0.0f : FontPx(smallFont) + Dp(2.0f);
            const float y = row.y + (hit - titleH - posH) * 0.5f;
            TextAt(titleFont, ImVec2(x, y), name.c_str(), text);
            if (posH > 0.0f) TextAt(smallFont, ImVec2(x, y + titleH + Dp(2.0f)), s.mediaPosition.c_str(), dim);
        }
    }
    ImGui::End();

    // --- Bottom: the transport for a video, the 2D/3D switch docked at its end ---
    const float switchH = std::round(iconSz * 0.82f);
    if (video) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(W, 0.0f), ImGuiCond_Always);
        if (ImGui::Begin("##touch_transport", nullptr, flags)) {
            if (s.fadeAlpha > 0.01f) g_transportTop = ImGui::GetWindowPos().y;
            const float dur = (float)s.durationSeconds;
            TrackScrubKnob(s, (float)s.positionSeconds);
            auto mmss = [](double sec, char* out, size_t n) {
                if (sec < 0.0) sec = 0.0;
                const int t = (int)sec;
                if (t >= 3600)
                    std::snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
                else
                    std::snprintf(out, n, "%d:%02d", t / 60, t % 60);
            };
            char cur[16], tot[16];
            mmss(s.scrubValue, cur, sizeof(cur));
            mmss(dur, tot, sizeof(tot));
            const ImGuiStyle& st = ImGui::GetStyle();
            const ImVec2 row = ImGui::GetCursorScreenPos();
            if (IconButton("##playpause", s.paused ? Glyph::Play : Glyph::Pause, iconSz, false, s.hitTargetScale))
                Fire(a.TogglePlayback);
            const float slot = TimeSlotW(smallFont, tot);
            const float textY = row.y + (hit - FontPx(smallFont)) * 0.5f;
            ImGui::SameLine();
            float x = ImGui::GetCursorScreenPos().x;
            TextAt(smallFont, ImVec2(x, textY), cur, text, slot, true);
            x += slot + st.ItemSpacing.x;
            const float switchW = s.caps.modeSwitch ? SegmentSwitchW() + st.ItemSpacing.x : 0.0f;
            const float scrubW = W - edge - switchW - slot - st.ItemSpacing.x * 2.0f - x;
            ImGui::SetCursorScreenPos(ImVec2(x, row.y + (hit - ImGui::GetFrameHeight()) * 0.5f));
            DriveScrub(s, a, dur, scrubW);
            x += std::max(Px(8.0f), scrubW) + st.ItemSpacing.x;
            TextAt(smallFont, ImVec2(x, textY), tot, text);
            x += slot + st.ItemSpacing.x;
            if (s.caps.modeSwitch) {
                ImGui::SetCursorScreenPos(ImVec2(x, row.y));
                if (SegmentSwitch("##mode3d", s.mode3D, switchH, hit)) Fire(a.NextMode);
            }
            ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + hit));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
        ImGui::End();
    } else if (s.caps.modeSwitch) {
        // A still: the switch alone, bottom right.
        ImGui::SetNextWindowPos(ImVec2(W, bottom), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        if (ImGui::Begin("##touch_modeswitch", nullptr, flags | ImGuiWindowFlags_AlwaysAutoResize) &&
            SegmentSwitch("##mode3d", s.mode3D, switchH, hit))
            Fire(a.NextMode);
        ImGui::End();
    }
    ImGui::PopStyleVar();
}
}  // namespace

void BuildTransportUI(TransportState& s, const TransportActions& a) {
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    g_transportTop = 0.0f;
    const ImGuiWindowFlags pill = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_AlwaysAutoResize;

    // The bars fade in/out together with the idle timer.
    if (s.fadeAlpha > 0.001f && s.caps.touchLayout) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s.fadeAlpha);
        TouchChrome(s, a, W, H);
        ImGui::PopStyleVar();
    } else if (s.fadeAlpha > 0.001f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s.fadeAlpha);
        const float iconSz = ImGui::GetFrameHeight();
        const float barBottom = H - g_systemBarBottom;
        const float rowY = H * 0.955f - g_systemBarBottom;  // the bottom row's lower edge
        // A black band behind each system bar keeps its icons readable.
        if (g_systemBarTop > 0.0f || g_systemBarBottom > 0.0f) {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            const ImU32 black = ImGui::GetColorU32(IM_COL32_BLACK);
            dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(W, g_systemBarTop), black);
            dl->AddRectFilled(ImVec2(0.0f, barBottom), ImVec2(W, H), black);
        }

        // --- Top window-space bar: full width, flush to the top of the window.
        //     Open / Mode on the left, Slideshow on the right. ---
        ImGui::SetNextWindowPos(ImVec2(0.0f, g_systemBarTop), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(W, 0.0f), ImGuiCond_Always);
        const ImGuiWindowFlags topbar = pill & ~ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);  // flat bar, flush to edges
        if (!s.stripOnly && ImGui::Begin("##wsui_top", nullptr, topbar)) {
            ImGui::BeginDisabled(s.openFilePending);
            if (s.caps.gallery ? IconButton("##gallery", Glyph::Back, iconSz)
                               : ImGui::Button("Open"))
                Fire(a.Open);
            ImGui::EndDisabled();
            if (s.caps.camera || s.caps.url) {
                ImGui::SameLine();
                CameraCombo(s, a);
            }
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
                    ImGui::SetTooltip("%s\nL cycles: auto / mono / SBS-full / SBS-half / TB-full / TB-half",
                                      s.layoutTooltip.c_str());
                }
            }
            // Convert to 3D: mono content only (caps). Highlighted while on; greyed with
            // the reason while the conversion cannot serve (a temporary state). The "###"
            // keeps one widget id across the label changes.
            if (s.caps.convert && s.hasMedia) {
                ImGui::SameLine();
                const char* label = "Convert to 3D###convert3d";
                if (s.convertWarming) label = "Convert to 3D (warming)###convert3d";
                if (s.convertUnavailable) label = "Convert to 3D (unavailable)###convert3d";
                ImGui::BeginDisabled(!s.convertEnabled);
                if (s.convertOn)
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(label)) Fire(a.ToggleConvert);
                if (s.convertOn) ImGui::PopStyleColor();
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
                    !s.convertTooltip.empty())
                    ImGui::SetTooltip("%s", s.convertTooltip.c_str());
                // Provider selector: the runtime's candidates, names verbatim. A pick is a
                // per-session hint (nothing is persisted; the default lives in DisplayXR).
                if (!s.convertProviders.empty()) {
                    const int cur = s.convertProviderCurrent;
                    const bool valid = cur >= 0 && cur < (int)s.convertProviders.size();
                    const char* preview =
                        valid ? s.convertProviders[cur].preview.c_str() : "Default";
                    ImGui::SameLine();
                    const ImGuiStyle& st = ImGui::GetStyle();
                    ImGui::SetNextItemWidth(std::min(ImGui::CalcTextSize(preview).x +
                                                         ImGui::GetFrameHeight() +
                                                         st.FramePadding.x * 2.0f,
                                                     Px(280.0f)));
                    if (ImGui::BeginCombo("##convert3d_provider", preview)) {
                        for (int i = 0; i < (int)s.convertProviders.size(); ++i) {
                            const ConvertProviderRow& row = s.convertProviders[i];
                            ImGui::PushID(i);
                            ImGui::BeginDisabled(!row.enabled);
                            if (ImGui::Selectable(row.label.c_str(), i == cur) && i != cur &&
                                a.SelectConvertProvider)
                                a.SelectConvertProvider(i);
                            ImGui::EndDisabled();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "2D->3D provider for this session\n"
                            "(the default is set in DisplayXR's settings)");
                }
            }
            // Input providers: one button, a small popup with a combo per role family and the
            // "Remember for this app" checkbox. Names are the runtime's, verbatim.
            if (s.caps.input) {
                ImGui::SameLine();
                if (ImGui::Button("Input")) ImGui::OpenPopup("##input_providers");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Which input device drives this app (controllers, hands)");
                if (ImGui::BeginPopup("##input_providers")) {
                    for (int f = 0; f < (int)s.inputFamilies.size(); ++f) {
                        const InputFamilyRow& fam = s.inputFamilies[f];
                        ImGui::PushID(f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(fam.label.c_str());
                        ImGui::SameLine(Px(110.0f));
                        const bool valid =
                            fam.enabled && fam.current >= 0 && fam.current < (int)fam.rows.size();
                        ImGui::BeginDisabled(!fam.enabled);
                        ImGui::SetNextItemWidth(Px(260.0f));
                        if (ImGui::BeginCombo("##family", valid ? fam.rows[fam.current].c_str()
                                                                : "no provider")) {
                            for (int r = 0; r < (int)fam.rows.size(); ++r) {
                                ImGui::PushID(r);
                                if (ImGui::Selectable(fam.rows[r].c_str(), r == fam.current) &&
                                    r != fam.current && a.SelectInputProvider)
                                    a.SelectInputProvider(f, r);
                                ImGui::PopID();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::EndDisabled();
                        if (!fam.enabled &&
                            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("No installed input provider supplies %s",
                                              fam.label.c_str());
                        ImGui::PopID();
                    }
                    bool remember = s.inputRemember;
                    if (ImGui::Checkbox("Remember for this app", &remember) && a.SetInputRemember)
                        a.SetInputRemember(remember);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Saves your pick as this app's input setting\n"
                            "(the same one DisplayXR's settings edit)");
                    ImGui::EndPopup();
                }
            }
            // Auto-convergence (#92): live, or a stereo file. Highlighted while on.
            if (((s.caps.camera && s.isLive) || (s.caps.url && s.isStream) || s.autoConvFile) &&
                !s.autoConvLabel.empty()) {
                ImGui::SameLine();
                if (s.autoConvOn)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(s.autoConvLabel.c_str())) Fire(a.ToggleAutoConv);
                if (s.autoConvOn) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A: auto-convergence on/off\nShift+A: nearest / sharp / centre\n"
                                      "Ctrl+click the picture: pin the subject");
            }
            // Current filename, centered in the bar - or the LIVE pill in its place.
            if (s.caps.camera && s.isLive) {
                LivePill(s, a);
            } else if (s.caps.url && s.isStream) {
                StreamPill(s, a);
            } else if (!s.mediaFilename.empty()) {
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
                if (IconButton("##slideshow", Glyph::Slideshow, iconSz, s.slideshowActive,
                               s.hitTargetScale))
                    Fire(a.ToggleSlideshow);
            }
        }
        if (!s.stripOnly) ImGui::End();
        ImGui::PopStyleVar();

        // --- Bottom transport: play/pause + scrubber + loop (video only) ---
        if (s.isVideo && s.durationSeconds > 0.0) {
            // The strip sits flush on the bottom edge, full width and flat, so it covers as
            // little of the picture as possible (usually only the letterbox). Pro's full UI
            // (side panels up) uses it too: the floating 80%-wide pill ran under the
            // Station / Inspector panels, while the strip lands in the gap below them
            // (the panels stop 90 px above the bottom edge, the scopes 52 px).
            const bool strip = s.stripOnly || s.panelsOn;
            if (strip) {
                ImGui::SetNextWindowPos(ImVec2(W * 0.5f, barBottom), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
                ImGui::SetNextWindowSize(ImVec2(W, 0.0f), ImGuiCond_Always);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(ImGui::GetStyle().WindowPadding.x, 4.0f));
            } else {
                ImGui::SetNextWindowPos(ImVec2(W * 0.5f, rowY), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
                ImGui::SetNextWindowSize(ImVec2(W * 0.8f, 0.0f), ImGuiCond_Always);
            }
            const ImGuiWindowFlags bar = pill & ~ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::Begin("##transport", nullptr, bar)) {
                if (s.fadeAlpha > 0.01f) g_transportTop = ImGui::GetWindowPos().y;
                const float dur = (float)s.durationSeconds;
                const float pos = (float)s.positionSeconds;
                TrackScrubKnob(s, pos);
                auto mmss = [](double sec, char* out, size_t n) {
                    if (sec < 0.0) sec = 0.0;
                    const int t = (int)sec;
                    std::snprintf(out, n, "%d:%02d", t / 60, t % 60);
                };
                char cur[16], tot[16], frm[32] = "";
                mmss(s.scrubValue, cur, sizeof(cur));
                mmss(dur, tot, sizeof(tot));
                const bool showFrame = s.frameRate > 0.0;  // Pro: the frame number after the time
                if (showFrame)
                    std::snprintf(frm, sizeof(frm), "f %lld",
                                  (long long)std::llround(s.scrubValue * s.frameRate));

                if (IconButton("##playpause", s.paused ? Glyph::Play : Glyph::Pause, iconSz, false,
                               s.hitTargetScale))
                    Fire(a.TogglePlayback);
                // A touch leg's icon hit boxes are taller than a frame: drop the frame-high
                // items (times, scrubber) to the middle of the row.
                const float rowPad = iconSz * (std::max(1.0f, s.hitTargetScale) - 1.0f) * 0.5f;
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowPad);
                // Time and frame number each sit in their OWN fixed slot, sized for the
                // widest value this clip can produce (its duration / last frame, every
                // digit as the font's widest digit). Drawn at natural width the label
                // pushed the scrubber sideways as digits changed; drawn as one string,
                // the time slid left whenever the frame number gained a digit. Now the
                // time is right-aligned in its slot and the frame number left-aligned in
                // its own, so neither moves the other and nothing moves the scrubber.
                char wideDigit = '0';
                float wideDigitW = 0.0f;
                for (char d = '0'; d <= '9'; d++) {
                    const char ds[2] = {d, '\0'};
                    const float w = ImGui::CalcTextSize(ds).x;
                    if (w > wideDigitW) {
                        wideDigitW = w;
                        wideDigit = d;
                    }
                }
                auto slotW = [wideDigit](const char* sample) {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "%s", sample);
                    for (char* p = buf; *p; p++)
                        if (*p >= '0' && *p <= '9') *p = wideDigit;
                    return ImGui::CalcTextSize(buf).x;
                };
                const float timeSlotW = std::max(slotW(tot), ImGui::CalcTextSize(cur).x);
                float frameSlotW = 0.0f;
                if (showFrame) {
                    char lastFrm[32];
                    std::snprintf(lastFrm, sizeof(lastFrm), "f %lld",
                                  (long long)std::llround((double)dur * s.frameRate));
                    frameSlotW = std::max(slotW(lastFrm), ImGui::CalcTextSize(frm).x);
                }
                const float labelGap = ImGui::CalcTextSize("  ").x;
                const float curX = ImGui::GetCursorPosX();
                ImGui::SetCursorPosX(curX + timeSlotW - ImGui::CalcTextSize(cur).x);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", cur);
                float labelW = timeSlotW;
                if (showFrame) {
                    ImGui::SameLine(curX + timeSlotW + labelGap);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowPad);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("%s", frm);
                    labelW += labelGap + frameSlotW;
                }
                ImGui::SameLine(curX + labelW + ImGui::GetStyle().ItemSpacing.x);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowPad);
                // Leave room on the right for the total-time label + mute + loop icons.
                // Derived from the live style rather than a magic constant, so the
                // touch build spaces correctly instead of crowding the total time.
                const ImGuiStyle& st = ImGui::GetStyle();
                const int rightIcons = (s.caps.mute ? 1 : 0) + (s.caps.loop ? 1 : 0);
                const float panelsW = s.panelsButton ? ImGui::CalcTextSize("Panels").x +
                                                           st.FramePadding.x * 2.0f + st.ItemSpacing.x
                                                     : 0.0f;
                const float rightW = ImGui::CalcTextSize(tot).x +
                                     (float)rightIcons * (iconSz * s.hitTargetScale) +
                                     (float)(rightIcons + 2) * st.ItemSpacing.x +
                                     st.WindowPadding.x + panelsW;
                DriveScrub(s, a, dur, ImGui::GetContentRegionAvail().x - rightW);
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowPad);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", tot);
                if (s.caps.mute) {
                    ImGui::SameLine();
                    if (IconButton("##mute", s.muted ? Glyph::SpeakerMuted : Glyph::Speaker, iconSz,
                                   s.muted, s.hitTargetScale))
                        Fire(a.ToggleMute);
                }
                if (s.caps.loop) {
                    ImGui::SameLine();
                    if (IconButton("##loop", Glyph::Loop, iconSz, s.loop, s.hitTargetScale))
                        Fire(a.ToggleLoop);
                }
                if (s.panelsButton) {
                    ImGui::SameLine();
                    if (s.panelsOn)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button("Panels")) Fire(a.TogglePanels);
                    if (s.panelsOn) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(s.panelsOn ? "Hide panels (Tab)" : "Show panels (Tab)");
                }
            }
            ImGui::End();
            if (strip) ImGui::PopStyleVar(2);
        }
        // The 2D/3D switch: alone at the right edge, level with the transport's row.
        if (s.caps.modeSwitch) {
            ImGui::SetNextWindowPos(ImVec2(W, rowY), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            if (ImGui::Begin("##modeswitch", nullptr, pill | ImGuiWindowFlags_NoBackground) &&
                ModeSwitch("##mode3d", s.mode3D, iconSz, s.hitTargetScale))
                Fire(a.NextMode);
            ImGui::End();
        }
        ImGui::PopStyleVar();
    }

    // --- Stream URL popup (#93): independent of the bar fade, like the toast ---
    if (s.caps.url) UrlPopup(s, a);

    // --- Toast (convergence readout, nav filename): own alpha, shows even when hidden ---
    BuildTransportToast(s);

    // --- Slideshow dip-to-black: a full-canvas overlay above everything. Because the
    //     HUD layer composites on top of the content layer, this dims the whole view. ---
    if (s.transitionAlpha > 0.001f) {
        const int alpha = (int)(std::min(1.0f, s.transitionAlpha) * 255.0f + 0.5f);
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H),
                                                      IM_COL32(0, 0, 0, alpha));
    }
}

void BuildTransportToast(const TransportState& s) {
    if (!(s.toastAlpha > 0.001f) || s.toastText.empty()) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const ImGuiWindowFlags pill = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s.toastAlpha);
    if (s.caps.touchLayout) {
        // Touch: low on the screen, above the transport, clear of the picture's middle, and
        // cut to the screen's width.
        const float margin = Dp(16.0f);
        ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H - g_systemBarBottom - Dp(96.0f)), ImGuiCond_Always,
                                ImVec2(0.5f, 1.0f));
        if (ImGui::Begin("##toast", nullptr, pill)) {
            const float pad = ImGui::GetStyle().WindowPadding.x * 2.0f;
            ImGui::TextUnformatted(FitText(s.toastText, W - margin * 2.0f - pad).c_str());
        }
    } else {
        ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.14f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("##toast", nullptr, pill)) {
            ImGui::TextUnformatted(s.toastText.c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

#else  // !MEDIAPLAYER_WITH_IMGUI

void ApplyMediaPlayerStyle(float) {}
void BuildTransportUI(TransportState&, const TransportActions&) {}
void BuildTransportToast(const TransportState&) {}

#endif

}  // namespace mp::ui
