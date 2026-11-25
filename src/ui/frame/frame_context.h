// SPDX-License-Identifier: Apache-2.0
// frame_context.h - small per-frame helpers: DPI conversion, redraw scheduling
#pragma once

#include "imgui.h"

namespace yc::ui {

// DPI scale of the monitor the current window is on (1.0 = 96 DPI). ImGui updates style.FontScaleDpi per monitor in Begin()
// (io.ConfigDpiScaleFonts), so floating windows on other monitors are correct too.
inline float Dpi() { return ImGui::GetStyle().FontScaleDpi; }
inline float S(float base_px) { return base_px * Dpi(); }
// font base size -> actual pixels (DPI and global scale included)
inline float FontPx(float base) {
  const ImGuiStyle& s = ImGui::GetStyle();
  return base * s.FontScaleMain * s.FontScaleDpi;
}

// event-driven redraw: a module asks for "another frame within seconds at the latest" (ScreenTip delay, auto-repeat, caret blinking).
void RequestRedrawIn(double seconds);
// for the main loop: take the earliest wake-up time (ImGui::GetTime() timeline, < 0 means none) and clear it.
double ConsumeRedrawDeadline();

}  // namespace yc::ui
