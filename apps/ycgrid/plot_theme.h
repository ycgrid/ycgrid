// SPDX-License-Identifier: Apache-2.0
// plot_theme.h - the looks of yc.plot figures and the trading monitors: YCGrid's own palettes (not matplotlib's) and a
// black "trading" theme in the style of Taiwanese trading terminals (red up, green down, bright lines on black).
#pragma once

#include <string>

#include "imgui.h"
#include "implot.h"

namespace yc::app {

enum class PlotTheme { Light = 0, Dark = 1 };

// "light" (default) / "dark" or "trading" (the dark trading theme)
PlotTheme PlotThemeFromName(const char* name);

// push before ImGui::Begin of the window and pop after ImGui::End: window, text and ImPlot colors plus the palette
void PushPlotTheme(PlotTheme t);
void PopPlotTheme(PlotTheme t);

// series colors, in order (the palette the theme pushes)
ImVec4 PlotPaletteColor(PlotTheme t, int i);

// a colormap by name: ImPlot's own, or YCGrid's "pnl" (loss green, zero black, profit red: for the black theme) and
// "pnl_light" (the same through white); unknown names give viridis
int PlotColormapByName(const std::string& name);

// candles and volume: red up, green down (Taiwanese convention)
ImU32 CandleUpColor(PlotTheme t);
ImU32 CandleDownColor(PlotTheme t);

}  // namespace yc::app
