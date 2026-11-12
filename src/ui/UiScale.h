// SPDX-License-Identifier: Apache-2.0
//
// The UI is laid out in HUD pixels so text is pixel-exact at every display scale.
// Hard-coded sizes are written in design units (pixels at 100% scaling) and go
// through Px(); text and ImGui style metrics are already scaled, so never wrap those.
#pragma once

namespace mp::ui {

// HUD pixels per design unit: the OS display scale on desktop (1.0 at 100%),
// density / 1.5 on Android. Set by ImGuiLayer::SetUiScale.
inline float g_uiScale = 1.0f;

inline float Px(float v) { return v * g_uiScale; }

// Android's dp in HUD pixels. There g_uiScale = density / 1.5, so a design unit is 2/3 dp.
// Only touch code sizes things in dp (the browser, the touch player's chrome).
inline float Dp(float v) { return Px(v * 1.5f); }

// Top edge (HUD pixels) of the bottom transport this frame, or 0 when none is showing.
// Written by BuildTransportUI, which runs before Pro's UI, so the Pro status line can
// sit above the strip instead of under it.
inline float g_transportTop = 0.0f;

// Top edge of everything docked on the bottom edge this frame — the transport and the Pro
// status line sitting on it — or 0 when nothing is. Bottom-anchored panels (scopes, the
// rig history) end above it rather than covering the status line.
inline float g_bottomUiTop = 0.0f;

// HUD pixels the system's status and nav bars cover at the top and bottom (Android, #180),
// 0 elsewhere. The UI stays clear of them.
inline float g_systemBarTop = 0.0f;
inline float g_systemBarBottom = 0.0f;

}  // namespace mp::ui
