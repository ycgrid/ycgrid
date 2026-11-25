// SPDX-License-Identifier: Apache-2.0
// theme.h - appearance tokens (PLAN §8.11)
//
// Every color is a provisional "to be calibrated" value: later sampled from screenshots of the user's Excel,
// kept in this header (no theme file is loaded). Code only reads colors through tokens, never hard-codes them.
#pragma once

#include "imgui.h"

namespace yc::ui {

struct Theme {
  // frame and ribbon
  ImU32 title_bg, title_text, title_btn_hover, title_btn_pressed, close_hover, close_pressed;
  ImU32 ribbon_bg, ribbon_border, tab_text, tab_text_selected, tab_hover_bg, tab_accent;
  ImU32 group_label, group_separator;
  ImU32 btn_hover, btn_pressed, btn_checked, btn_checked_border, btn_text, btn_text_disabled;
  ImU32 accent;  // accent color (active cell border, underline of the selected tab)
  // formula bar and status bar
  ImU32 formula_bg, formula_border, statusbar_bg, statusbar_text;
  // grid
  ImU32 cell_bg, gridline, cell_text;
  ImU32 header_bg, header_text, header_border, header_sel_bg, header_sel_text, header_sel_bar;
  ImU32 selection_fill, selection_border, fill_handle_border;
  ImU32 merge_hint;
  ImU32 spill_border;  // dynamic array range of the active cell
  // scroll bars and sheet tabs
  ImU32 scroll_track, scroll_thumb, scroll_thumb_hover, scroll_thumb_active, scroll_arrow;
  ImU32 sheettab_bg, sheettab_active_bg, sheettab_text, sheettab_active_text, sheettab_border;
  // Backstage
  ImU32 backstage_nav_bg, backstage_nav_text, backstage_nav_hover, backstage_bg;
};

const Theme& T();

// style applied to ImGui's built-in widgets (popups, tooltips, InputText, ImGui window title bars).
void ApplyImGuiStyle(float dpi_scale);

}  // namespace yc::ui
