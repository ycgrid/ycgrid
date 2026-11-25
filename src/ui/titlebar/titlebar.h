// SPDX-License-Identifier: Apache-2.0
// titlebar.h - custom-drawn title bar (PLAN §11.1)
//
// How: WM_NCCALCSIZE turns the whole window into client area (keeping the DWM shadow), WM_NCHITTEST returns
// HTCAPTION / HTMAXBUTTON / border codes so dragging, double-click to maximize and Windows 11 snap layouts keep working.
// The maximize button returns HTMAXBUTTON (needed for the snap layout flyout); its hover and click are handled by us;
// the other buttons (QAT, minimize, close) are client area handled by ImGui.
#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <windows.h>

#include <functional>
#include <vector>
#include <string>

namespace yc::ui::titlebar {

struct State {
  // areas drawn in the last frame (client coordinates, physical pixels), used by WM_NCHITTEST
  RECT caption{};        // the whole title bar
  RECT max_button{};
  // interactive client areas on the title bar (QAT buttons and arrows, search, minimize, close): no fixed number, or
  // the last ones (minimize, close) fell off a full list and the X became part of the caption (a click dragged)
  std::vector<RECT> interactive;
  bool max_hover = false;
  bool max_pressed = false;
  std::string title;      // e.g. "test.ycg - YCGrid"
  // drawn by the app: the Customize Quick Access Toolbar drop-down, the search box's results (Alt+Q)
  std::function<void()> qat_menu;
  // the arrow beside Undo / Redo: the actions (newest first); choosing the n-th undoes (redoes) n of them, as in Excel
  std::function<std::vector<std::string>(bool redo)> history;
  std::function<void(bool redo, int count)> history_run;
  ImRect search_box;  // where the search box is (screen coordinates)
  bool search_open_request = false;
};

State& St();

float Height();  // DPI included

// called from the window procedure; returns true when handled (*result is the return value).
bool HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result);

// call once after the window is created: extends the DWM frame and triggers WM_NCCALCSIZE.
void Install(HWND hwnd);

// draws inside the main window's ImGui window; qat_cmds: command ids of the Quick Access Toolbar.
void Draw(HWND hwnd, const ImRect& r, const char* const* qat_cmds, int qat_count);

}  // namespace yc::ui::titlebar
