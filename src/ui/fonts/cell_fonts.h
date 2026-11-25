// SPDX-License-Identifier: Apache-2.0
// cell_fonts.h - cell fonts: loaded on demand by font family + bold + italic (PLAN §8.4)
//
// Finds the system font file with DirectWrite, loads it into the ImGui atlas (1.92+ dynamic fonts, any size) and merges the CJK fallback font.
// ImGui can only add fonts between frames: the first time a combination is seen the default font is returned and it is queued,
// then actually loaded before the next frame (LoadPendingCellFonts), followed by one more redraw.
#pragma once

#include "imgui.h"

#include <string>

namespace yc::ui {

ImFont* CellFont(const std::string& family, bool bold, bool italic);
// called by the main loop before NewFrame; returns true when new fonts were loaded
bool LoadPendingCellFonts();

}  // namespace yc::ui
