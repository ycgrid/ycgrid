// SPDX-License-Identifier: Apache-2.0
// fonts.h - font loading (ImGui 1.92+ dynamic fonts: any size, glyphs rasterized on demand)
//
// - System fonts are never redistributed: they are always loaded from C:\Windows\Fonts (PLAN §8.4).
// - The Fluent icon font (MIT) is embedded in the exe resources: portable, no external files.
#pragma once

#include "imgui.h"

#include <string>
#include <vector>

namespace yc::ui {

struct Fonts {
  ImFont* ui = nullptr;       // ribbon and dialogs (Segoe UI + Microsoft JhengHei UI)
  ImFont* ui_bold = nullptr;  // selected tab, ScreenTip title
  ImFont* grid = nullptr;     // cells (to be calibrated: the user's default Excel font)
  ImFont* icons = nullptr;    // Fluent UI System Icons

  // base size (px before DPI; ImGui scales it by style.FontScaleDpi)
  float ui_size = 12.0f;
  float ui_small = 11.0f;
  float grid_size = 15.0f;    // Calibri 11pt ~ 14.67 px (to be calibrated)
  float icon_small = 16.0f;
  float icon_large = 32.0f;
};

Fonts& F();

// icon_resource_id: number of the RCDATA resource in the exe (Fluent icon font).
bool LoadFonts(int icon_resource_id);

// enumerate system font families with DirectWrite (localized Chinese names first), on the first call only.
const std::vector<std::string>& SystemFontFamilies();

}  // namespace yc::ui
