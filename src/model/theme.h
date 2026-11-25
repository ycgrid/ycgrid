// SPDX-License-Identifier: Apache-2.0
// theme.h - document themes (Page Layout > Themes): 12 colors and a heading / body font pair
//
// A style color can refer to a theme color instead of a fixed RGB: 0xFE in the top byte, the theme index in bits
// 16-23 and the tint (-1..1, times 10000) as an int16 in bits 0-15. Such colors change with the workbook's theme.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace yc::model {

// theme color indexes (Excel's clrScheme order; Excel's xlThemeColor constants are index + 1)
enum ThemeIndex : int { kDark1 = 0, kLight1, kDark2, kLight2, kAccent1, kAccent2, kAccent3, kAccent4, kAccent5, kAccent6, kHyperlink, kFollowed, kThemeColors };

struct Theme {
  std::string name = "Office";
  std::array<uint32_t, kThemeColors> colors{0x000000, 0xFFFFFF, 0x44546A, 0xE7E6E6, 0x4472C4, 0xED7D31,
                                             0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47, 0x0563C1, 0x954F72};  // 0xRRGGBB
  std::string colors_name = "Office";
  std::string major_font = "Calibri Light", minor_font = "Calibri";  // headings, body
  std::string fonts_name = "Office";
  bool operator==(const Theme&) const = default;
};

inline constexpr uint32_t kThemeTag = 0xFE000000u;
inline bool IsThemeColor(uint32_t c) { return (c >> 24) == 0xFE; }
uint32_t MakeThemeColor(int index, double tint = 0);
inline int ThemeColorIndex(uint32_t c) { return static_cast<int>((c >> 16) & 0xFF); }
inline double ThemeColorTint(uint32_t c) { return static_cast<int16_t>(c & 0xFFFF) / 10000.0; }

// Excel's tint: lighter towards white, darker towards black in HSL luminance
uint32_t ApplyTint(uint32_t rgb, double tint);
// a style color as 0xFFRRGGBB (theme colors resolved) or 0 for automatic
uint32_t ResolveColor(uint32_t color, const Theme& theme);

// the built-in themes, color sets and font pairs (the first of each is Office)
const std::vector<Theme>& BuiltinThemes();
struct ThemeColorSet {
  std::string name;
  std::array<uint32_t, kThemeColors> colors;
  bool operator==(const ThemeColorSet&) const = default;
};
const std::vector<ThemeColorSet>& BuiltinThemeColors();
struct ThemeFontPair {
  std::string name, major, minor;
  bool operator==(const ThemeFontPair&) const = default;
};
const std::vector<ThemeFontPair>& BuiltinThemeFonts();
Theme DefaultTheme();

// the theme used to draw colors on screen and in print (the app sets it from the workbook); any thread may read it
const Theme& CurrentTheme();
void SetCurrentTheme(const Theme& t);
inline uint32_t ResolveColor(uint32_t color) { return IsThemeColor(color) ? ResolveColor(color, CurrentTheme()) : color; }

}  // namespace yc::model
