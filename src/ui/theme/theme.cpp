// SPDX-License-Identifier: Apache-2.0
#include "theme/theme.h"

namespace yc::ui {
namespace {

constexpr ImU32 Rgb(unsigned rgb, unsigned a = 255) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

// YCGrid's own colors: a cool blue-gray chrome with a deep steel blue accent (about 7:1 on white, 6:1 on the chrome).
constexpr unsigned kAccent = 0x1D5D8F;
constexpr unsigned kAccentDark = 0x164A72;
constexpr unsigned kChrome = 0xE9F0F7;      // title bar, ribbon, status bar, sheet tabs (1.13:1 against the white grid)
constexpr unsigned kChromeHover = 0xDCE7F2;
constexpr unsigned kChromePressed = 0xC8D8E8;
constexpr unsigned kChromeChecked = 0xD0DEEC;
constexpr unsigned kChromeBorder = 0xCBD9E7;

Theme MakeLight() {
  Theme t{};
  t.accent = Rgb(kAccent);
  t.title_bg = Rgb(kChrome);
  t.title_text = Rgb(0x242424);
  t.title_btn_hover = Rgb(kChromeHover);
  t.title_btn_pressed = Rgb(kChromePressed);
  t.close_hover = Rgb(0xC42B1C);
  t.close_pressed = Rgb(0xC83C30);

  t.ribbon_bg = Rgb(kChrome);
  t.ribbon_border = Rgb(kChromeBorder);
  t.tab_text = Rgb(0x424242);
  t.tab_text_selected = Rgb(kAccent);
  t.tab_hover_bg = Rgb(kChromeHover);
  t.tab_accent = Rgb(kAccent);
  t.group_label = Rgb(0x616161);
  t.group_separator = Rgb(kChromeBorder);

  t.btn_hover = Rgb(kChromeHover);
  t.btn_pressed = Rgb(kChromePressed);
  t.btn_checked = Rgb(kChromeChecked);
  t.btn_checked_border = Rgb(0xA3BDD6);
  t.btn_text = Rgb(0x242424);
  t.btn_text_disabled = Rgb(0xA6A6A6);

  t.formula_bg = Rgb(0xFFFFFF);
  t.formula_border = Rgb(0xD4D4D4);
  t.statusbar_bg = Rgb(kChrome);
  t.statusbar_text = Rgb(0x424242);

  t.cell_bg = Rgb(0xFFFFFF);
  t.gridline = Rgb(0xE1E1E1);
  t.cell_text = Rgb(0x000000);
  t.header_bg = Rgb(0xF2F6FA);
  t.header_text = Rgb(0x424242);
  t.header_border = Rgb(0xD4D4D4);
  t.header_sel_bg = Rgb(kChromeChecked);
  t.header_sel_text = Rgb(kAccent);
  t.header_sel_bar = Rgb(kAccent);
  t.selection_fill = Rgb(0x202020, 26);
  t.selection_border = Rgb(kAccent);
  t.fill_handle_border = Rgb(0xFFFFFF);
  t.merge_hint = Rgb(0xFFFFFF);
  t.spill_border = Rgb(0x4A89DC);

  t.scroll_track = Rgb(kChrome);
  t.scroll_thumb = Rgb(0xC8C8C8);
  t.scroll_thumb_hover = Rgb(0xA6A6A6);
  t.scroll_thumb_active = Rgb(0x8A8A8A);
  t.scroll_arrow = Rgb(0x606060);

  t.sheettab_bg = Rgb(kChrome);
  t.sheettab_active_bg = Rgb(0xFFFFFF);
  t.sheettab_text = Rgb(0x424242);
  t.sheettab_active_text = Rgb(kAccent);
  t.sheettab_border = Rgb(0xD4D4D4);

  t.backstage_nav_bg = Rgb(kAccent);
  t.backstage_nav_text = Rgb(0xFFFFFF);
  t.backstage_nav_hover = Rgb(kAccentDark);
  t.backstage_bg = Rgb(0xFFFFFF);
  return t;
}

}  // namespace

const Theme& T() {
  static const Theme theme = MakeLight();
  return theme;
}

void ApplyImGuiStyle(float dpi_scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsLight(&s);
  s.WindowRounding = 0.0f;  // multi-viewport: platform windows look the same as ImGui windows
  s.ChildRounding = 0.0f;
  s.FrameRounding = 3.0f;
  s.PopupRounding = 4.0f;
  s.GrabRounding = 3.0f;
  s.TabRounding = 3.0f;
  s.WindowBorderSize = 1.0f;
  s.PopupBorderSize = 1.0f;
  s.FrameBorderSize = 1.0f;
  s.WindowPadding = ImVec2(8, 8);
  s.FramePadding = ImVec2(6, 3);
  s.ItemSpacing = ImVec2(6, 4);
  s.ScrollbarSize = 14.0f;

  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_PopupBg] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0xC8, 0xC8, 0xC8, 255));
  c[ImGuiCol_TitleBg] = ImGui::ColorConvertU32ToFloat4(Rgb(kChrome));
  c[ImGuiCol_TitleBgActive] = ImGui::ColorConvertU32ToFloat4(Rgb(kChromeHover));
  c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
  c[ImGuiCol_Header] = ImGui::ColorConvertU32ToFloat4(Rgb(0xD3E1EE));
  c[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(Rgb(kChromeHover));
  c[ImGuiCol_HeaderActive] = ImGui::ColorConvertU32ToFloat4(Rgb(kChromePressed));
  c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_Tab] = c[ImGuiCol_TitleBg];
  c[ImGuiCol_TabSelected] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_TabHovered] = c[ImGuiCol_HeaderHovered];
  c[ImGuiCol_TabSelectedOverline] = ImGui::ColorConvertU32ToFloat4(T().accent);
  c[ImGuiCol_DockingPreview] = ImGui::ColorConvertU32ToFloat4(Rgb(kAccent, 120));
  c[ImGuiCol_DockingEmptyBg] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0xEB, 0xEB, 0xEB, 255));
  c[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(T().accent);
  c[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(T().accent);
  c[ImGuiCol_Button] = ImGui::ColorConvertU32ToFloat4(Rgb(kChrome));
  c[ImGuiCol_ButtonHovered] = ImGui::ColorConvertU32ToFloat4(T().btn_hover);
  c[ImGuiCol_ButtonActive] = ImGui::ColorConvertU32ToFloat4(T().btn_pressed);
  c[ImGuiCol_NavCursor] = ImGui::ColorConvertU32ToFloat4(T().accent);
  s.ScaleAllSizes(dpi_scale);
}

}  // namespace yc::ui
