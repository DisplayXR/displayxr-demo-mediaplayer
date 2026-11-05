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

}  // namespace mp::ui
