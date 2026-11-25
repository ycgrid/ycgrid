// SPDX-License-Identifier: Apache-2.0
// plot_theme.cpp - see plot_theme.h
#include "plot_theme.h"

#include <cstring>

namespace yc::app {

namespace {

constexpr ImVec4 Hex(unsigned v, float a = 1.0f) {
  return ImVec4(static_cast<float>((v >> 16) & 0xFF) / 255.0f, static_cast<float>((v >> 8) & 0xFF) / 255.0f,
                static_cast<float>(v & 0xFF) / 255.0f, a);
}

// light: clear, saturated colors that read on white and stay apart from each other
const ImVec4 kLight[] = {Hex(0x2F6FED), Hex(0xF08C00), Hex(0x16A34A), Hex(0xE5383B), Hex(0x7C3AED),
                         Hex(0x0EA5E9), Hex(0xDB2777), Hex(0x65A30D), Hex(0x92400E), Hex(0x6B7280)};
// dark (trading): the bright line colors of trading terminals on black
const ImVec4 kDark[] = {Hex(0xFFD400), Hex(0xFF4FD8), Hex(0x00D8FF), Hex(0x39E07A), Hex(0xFF8A00),
                        Hex(0xFFFFFF), Hex(0x9D7BFF), Hex(0xFF5A5A)};

ImPlotColormap Map(PlotTheme t) {
  static ImPlotColormap light = -1, dark = -1;
  if (light < 0) {
    light = ImPlot::GetColormapIndex("YCGrid");
    if (light < 0) light = ImPlot::AddColormap("YCGrid", kLight, IM_ARRAYSIZE(kLight));
    dark = ImPlot::GetColormapIndex("YCGrid Dark");
    if (dark < 0) dark = ImPlot::AddColormap("YCGrid Dark", kDark, IM_ARRAYSIZE(kDark));
  }
  return t == PlotTheme::Dark ? dark : light;
}

constexpr int kImGuiColors = 20;
constexpr int kImPlotColors = 11;

}  // namespace

PlotTheme PlotThemeFromName(const char* name) {
  if (name && (std::strcmp(name, "dark") == 0 || std::strcmp(name, "trading") == 0)) return PlotTheme::Dark;
  return PlotTheme::Light;
}

void PushPlotTheme(PlotTheme t) {
  ImPlot::PushColormap(Map(t));
  if (t != PlotTheme::Dark) return;
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Hex(0x000000));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, Hex(0x000000));
  ImGui::PushStyleColor(ImGuiCol_Text, Hex(0xD8D8D8));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, Hex(0x161616));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, Hex(0x262626));
  ImGui::PushStyleColor(ImGuiCol_Border, Hex(0x3A3A3A));
  // popups of the figure (its context menu, ImPlot's own menus) in the same dark look
  ImGui::PushStyleColor(ImGuiCol_PopupBg, Hex(0x1A1A1A, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_Header, Hex(0x2E3A4A));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Hex(0x2E4A6A));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, Hex(0x3A5A80));
  ImGui::PushStyleColor(ImGuiCol_Separator, Hex(0x3A3A3A));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, Hex(0x707070));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, Hex(0x202020));
  // its tab when docked: dark too (light text on the light theme's tab was unreadable)
  ImGui::PushStyleColor(ImGuiCol_Tab, Hex(0x1C1C1C));
  ImGui::PushStyleColor(ImGuiCol_TabHovered, Hex(0x34465C));
  ImGui::PushStyleColor(ImGuiCol_TabSelected, Hex(0x2A2A2A));
  ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, Hex(0x4A90D9));
  ImGui::PushStyleColor(ImGuiCol_TabDimmed, Hex(0x161616));
  ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, Hex(0x242424));
  ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, Hex(0x3A3A3A));
  ImPlot::PushStyleColor(ImPlotCol_FrameBg, Hex(0x000000));
  ImPlot::PushStyleColor(ImPlotCol_PlotBg, Hex(0x000000));
  ImPlot::PushStyleColor(ImPlotCol_PlotBorder, Hex(0x3A3A3A));
  ImPlot::PushStyleColor(ImPlotCol_LegendBg, Hex(0x000000, 0.85f));
  ImPlot::PushStyleColor(ImPlotCol_LegendBorder, Hex(0x3A3A3A));
  ImPlot::PushStyleColor(ImPlotCol_LegendText, Hex(0xD8D8D8));
  ImPlot::PushStyleColor(ImPlotCol_TitleText, Hex(0xE8E8E8));
  ImPlot::PushStyleColor(ImPlotCol_InlayText, Hex(0xD8D8D8));
  ImPlot::PushStyleColor(ImPlotCol_AxisText, Hex(0xB4B4B4));
  ImPlot::PushStyleColor(ImPlotCol_AxisGrid, Hex(0x2A2A2A));
  ImPlot::PushStyleColor(ImPlotCol_AxisTick, Hex(0x4A4A4A));
}

void PopPlotTheme(PlotTheme t) {
  if (t == PlotTheme::Dark) {
    ImPlot::PopStyleColor(kImPlotColors);
    ImGui::PopStyleColor(kImGuiColors);
  }
  ImPlot::PopColormap();
}

ImVec4 PlotPaletteColor(PlotTheme t, int i) {
  if (t == PlotTheme::Dark) return kDark[static_cast<size_t>(i) % IM_ARRAYSIZE(kDark)];
  return kLight[static_cast<size_t>(i) % IM_ARRAYSIZE(kLight)];
}

int PlotColormapByName(const std::string& name) {
  Map(PlotTheme::Light);  // YCGrid's palettes first, so the indexes stay the same in a session
  const int i = ImPlot::GetColormapIndex(name.c_str());
  if (i >= 0) return i;
  static const ImVec4 pnl[] = {Hex(0x00D25B), Hex(0x00451E), Hex(0x000000), Hex(0x550000), Hex(0xFF3B3B)};
  static const ImVec4 pnl_light[] = {Hex(0x16A34A), Hex(0xBBF7D0), Hex(0xFFFFFF), Hex(0xFECACA), Hex(0xE5383B)};
  if (name == "pnl") return ImPlot::AddColormap("pnl", pnl, IM_ARRAYSIZE(pnl), false);
  if (name == "pnl_light") return ImPlot::AddColormap("pnl_light", pnl_light, IM_ARRAYSIZE(pnl_light), false);
  return ImPlotColormap_Viridis;
}

ImU32 CandleUpColor(PlotTheme t) { return ImGui::GetColorU32(t == PlotTheme::Dark ? Hex(0xFF3B3B) : Hex(0xE5383B)); }
ImU32 CandleDownColor(PlotTheme t) { return ImGui::GetColorU32(t == PlotTheme::Dark ? Hex(0x00D25B) : Hex(0x16A34A)); }

}  // namespace yc::app
