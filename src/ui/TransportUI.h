// SPDX-License-Identifier: Apache-2.0
//
// TransportUI — the player's Dear ImGui chrome (top bar + transport + toast +
// dip-to-black), shared VERBATIM by every platform leg.
//
// This is the one place the UI is defined. The desktop legs and the Android leg
// both call BuildTransportUI() with the same state and the same actions, so the
// two can't drift into "looks roughly like the other one" (which is exactly what
// the hand-drawn Android overlay had become — seven-segment digits and no words).
//
// The rules that keep it portable:
//   - no SDL, no FFmpeg, no std::filesystem, no App, no platform headers;
//   - the caller owns ALL the media/plumbing and passes it in as plain data
//     (TransportState) plus callbacks (TransportActions);
//   - a control the platform can't back is declared unavailable in Caps and is
//     then not drawn at all — so a leg is a strict SUBSET of the same UI, never
//     a different one.
#pragma once

#include <functional>
#include <string>

namespace mp::ui {

// Which transport controls this platform can actually back. A false entry HIDES
// the control rather than disabling it: a permanently greyed-out button is worse
// than no button, because it reads as broken instead of absent.
struct TransportCaps {
    bool mode = true;       // display-mode cycle (XR_DXR_display_info)
    bool layout = true;     // stereo-layout override + pin
    bool slideshow = true;  // needs folder enumeration
    bool mute = true;
    bool loop = true;
};

// Everything the widgets read, and the little bit of state they own.
//
// The "owner writes" block is refreshed by the caller every frame from the real
// media objects. The "UI owns" block is scratch state the widget code mutates
// and the caller must NOT clobber — notably the scrubber's drag machine.
struct TransportState {
    // ---- owner writes each frame -------------------------------------------
    bool hasMedia = false;
    bool isVideo = false;
    std::string mediaFilename;  // basename only; the caller does the path work
    std::string modeName;       // e.g. "2x1 Stereo"
    std::string layoutName;     // e.g. "SBS-half"
    std::string layoutTooltip;  // multi-line provenance shown on hover
    bool layoutPinned = false;  // renders a trailing '*'
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    bool paused = true;
    bool muted = false;
    bool loop = false;
    bool openFilePending = false;  // gates the Open button while a picker is up
    bool slideshowActive = false;
    float transitionAlpha = 0.0f;  // dip-to-black; owned by the caller's nav machine

    // ---- UI owns (do not clobber) ------------------------------------------
    // Scrubber. The displayed knob tracks playback EXCEPT while dragging, or
    // while a seek we issued hasn't landed yet — scrubTarget >= 0 holds the knob
    // steady so it can't snap back to the stale position on release.
    float scrubValue = 0.0f;
    bool scrubActive = false;
    float scrubTarget = -1.0f;
    float lastScrubValue = 0.0f;
    bool scrubWasPreview = false;

    // Auto-hide fade (whole-UI alpha) and the independent toast fade.
    float fadeAlpha = 0.0f;
    float toastAlpha = 0.0f;
    std::string toastText;
    double toastSecondsLeft = 0.0;

    // ---- configuration ------------------------------------------------------
    TransportCaps caps{};
    // Touch legs enlarge the INVISIBLE hit targets without changing the drawn
    // glyph, so a finger can hit what a mouse could. 1.0 = desktop.
    float hitTargetScale = 1.0f;
};

// Callbacks into the owner. Every slot is optional — an unset slot makes its
// control inert (but Caps is the right way to remove a control).
struct TransportActions {
    std::function<void()> Open;
    std::function<void()> NextMode;
    std::function<void()> CycleLayout;
    std::function<void()> ToggleSlideshow;
    std::function<void()> TogglePlayback;
    std::function<void()> ToggleMute;
    std::function<void()> ToggleLoop;

    // Scrub machine. Seek(preview=true) is a cheap keyframe seek for a fast
    // sweep; preview=false is the exact frame. ScrubHeld fires once when the
    // drag starts (the owner silences audio); ScrubReleased fires on release
    // (exact seek + audio realign + resume).
    std::function<void(float seconds, bool preview)> Seek;
    std::function<void()> ScrubHeld;
    std::function<void(float seconds)> ScrubReleased;
};

// The player's "dark glass" look. `metricScale` is the ONE sanctioned divergence
// between legs: 1.0 on desktop, ~1.6 on touch. It scales PADDING AND GRAB SIZES ONLY —
// every colour, rounding radius and icon is identical on both, so the design is one
// design. Never branch on it anywhere else.
void ApplyMediaPlayerStyle(float metricScale = 1.0f);

// Advance the purely-visual timers: the auto-hide fade and the toast fade.
// `uiAwake` is the caller's "should the chrome be visible" verdict (master
// toggle AND not idle) — the caller owns what counts as activity, because that
// differs per platform (mouse motion on desktop, touch on Android).
void TickTransportUI(TransportState& s, double dt, bool uiAwake);

// Show a transient message. Fades in over the toast, independent of the bars,
// so it's visible even while the chrome is hidden.
void ShowTransportToast(TransportState& s, std::string msg);

// Emit the whole UI. Must be called between ImGui NewFrame() and Render().
void BuildTransportUI(TransportState& s, const TransportActions& a);

}  // namespace mp::ui
