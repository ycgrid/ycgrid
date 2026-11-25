// SPDX-License-Identifier: Apache-2.0
// theme.cpp - built-in themes and theme color resolution
#include "model/theme.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace yc::model {

uint32_t MakeThemeColor(int index, double tint) {
  const int t = static_cast<int>(std::lround(std::clamp(tint, -1.0, 1.0) * 10000.0));
  return kThemeTag | (static_cast<uint32_t>(std::clamp(index, 0, kThemeColors - 1)) << 16) | static_cast<uint16_t>(static_cast<int16_t>(t));
}

uint32_t ApplyTint(uint32_t rgb, double tint) {
  if (tint == 0) return rgb & 0xFFFFFF;
  double r = ((rgb >> 16) & 0xFF) / 255.0, g = ((rgb >> 8) & 0xFF) / 255.0, b = (rgb & 0xFF) / 255.0;
  // RGB -> HSL
  const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  double h = 0, s = 0, l = (mx + mn) / 2;
  if (mx != mn) {
    const double d = mx - mn;
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
  }
  l = tint < 0 ? l * (1 + tint) : l * (1 - tint) + tint;
  // HSL -> RGB
  auto hue = [](double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
  };
  if (s == 0) {
    r = g = b = l;
  } else {
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue(p, q, h + 1.0 / 3);
    g = hue(p, q, h);
    b = hue(p, q, h - 1.0 / 3);
  }
  auto ch = [](double v) { return static_cast<uint32_t>(std::clamp(std::lround(v * 255), 0L, 255L)); };
  return (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

uint32_t ResolveColor(uint32_t color, const Theme& theme) {
  if (!IsThemeColor(color)) return color;
  const int i = ThemeColorIndex(color);
  if (i < 0 || i >= kThemeColors) return 0;
  return 0xFF000000u | ApplyTint(theme.colors[static_cast<size_t>(i)], ThemeColorTint(color));
}

const std::vector<ThemeColorSet>& BuiltinThemeColors() {
  static const std::vector<ThemeColorSet> v = {
      {"Office", {0x000000, 0xFFFFFF, 0x44546A, 0xE7E6E6, 0x4472C4, 0xED7D31, 0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47, 0x0563C1, 0x954F72}},
      {"Office 2007 - 2010", {0x000000, 0xFFFFFF, 0x1F497D, 0xEEECE1, 0x4F81BD, 0xC0504D, 0x9BBB59, 0x8064A2, 0x4BACC6, 0xF79646, 0x0000FF, 0x800080}},
      {"Office 2023", {0x000000, 0xFFFFFF, 0x0E2841, 0xE8E8E8, 0x156082, 0xE97132, 0x196B24, 0x0F9ED5, 0xA02B93, 0x4EA72E, 0x467886, 0x96607D}},
      {"Grayscale", {0x000000, 0xFFFFFF, 0x000000, 0xF8F8F8, 0xDDDDDD, 0xB2B2B2, 0x969696, 0x808080, 0x5F5F5F, 0x4D4D4D, 0x5F5F5F, 0x919191}},
      {"Blue", {0x000000, 0xFFFFFF, 0x17406D, 0xDBEFF9, 0x0F6FC6, 0x009DD9, 0x0BD0D9, 0x10CF9B, 0x7CCA62, 0xA5C249, 0xF49100, 0x85DFD0}},
      {"Green", {0x000000, 0xFFFFFF, 0x455F51, 0xE3DED1, 0x549E39, 0x8AB833, 0xC0CF3A, 0x029676, 0x4AB5C4, 0x0989B1, 0x6B9F25, 0xBA6906}},
      {"Red Orange", {0x000000, 0xFFFFFF, 0x505046, 0xEEECE1, 0xE84C22, 0xFFBD47, 0xB64926, 0xFF8427, 0xCC9900, 0xB22600, 0xCC9900, 0x666699}},
      {"Violet", {0x000000, 0xFFFFFF, 0x373545, 0xDCD8DC, 0xAD84C6, 0x8784C7, 0x5D739A, 0x6997AF, 0x84ACB6, 0x6F8183, 0x69A020, 0x8C8C8C}},
      {"Blue Green", {0x000000, 0xFFFFFF, 0x373545, 0xCEDBE6, 0x3494BA, 0x58B6C0, 0x75BDA7, 0x7A8C8E, 0x84ACB6, 0x2683C6, 0x6B9F25, 0x9F6715}},
      {"Yellow", {0x000000, 0xFFFFFF, 0x39302A, 0xE5DEDB, 0xFFCA08, 0xF8931D, 0xCE8D3E, 0xEC7016, 0xE64823, 0x9C6A6A, 0x2998E3, 0x7F723D}},
  };
  return v;
}

const std::vector<ThemeFontPair>& BuiltinThemeFonts() {
  static const std::vector<ThemeFontPair> v = {
      {"Office", "Calibri Light", "Calibri"},     {"Office 2007 - 2010", "Cambria", "Calibri"}, {"Office 2023", "Aptos Display", "Aptos"},
      {"Calibri", "Calibri", "Calibri"},           {"Arial", "Arial", "Arial"},                   {"Corbel", "Corbel", "Corbel"},
      {"Candara", "Candara", "Candara"},           {"Franklin Gothic", "Franklin Gothic Medium", "Franklin Gothic Book"},
      {"Century Gothic", "Century Gothic", "Century Gothic"}, {"Tw Cen MT", "Tw Cen MT", "Tw Cen MT"}, {"Cambria", "Cambria", "Cambria"},
      {"Georgia", "Georgia", "Georgia"},           {"Constantia", "Constantia", "Constantia"},   {"Times New Roman", "Times New Roman", "Times New Roman"},
      {"Segoe UI", "Segoe UI", "Segoe UI"},        {"Microsoft JhengHei", "Microsoft JhengHei", "Microsoft JhengHei"},
  };
  return v;
}

const std::vector<Theme>& BuiltinThemes() {
  static const std::vector<Theme> v = [] {
    std::vector<Theme> out;
    // a theme is a color set and a font pair; the classic combinations
    const std::pair<const char*, std::pair<int, int>> combos[] = {
        {"Office", {0, 0}},     {"Office 2007 - 2010", {1, 1}}, {"Office 2023", {2, 2}}, {"Facet", {5, 10}}, {"Ion", {4, 8}},
        {"Retrospect", {6, 13}}, {"Slice", {4, 9}},             {"Wisp", {9, 12}},       {"Integral", {8, 7}}, {"Gray", {3, 4}},
    };
    const auto& colors = BuiltinThemeColors();
    const auto& fonts = BuiltinThemeFonts();
    for (const auto& [name, cf] : combos) {
      Theme t;
      t.name = name;
      t.colors = colors[static_cast<size_t>(cf.first)].colors;
      t.colors_name = colors[static_cast<size_t>(cf.first)].name;
      t.major_font = fonts[static_cast<size_t>(cf.second)].major;
      t.minor_font = fonts[static_cast<size_t>(cf.second)].minor;
      t.fonts_name = fonts[static_cast<size_t>(cf.second)].name;
      out.push_back(std::move(t));
    }
    return out;
  }();
  return v;
}

Theme DefaultTheme() { return BuiltinThemes().front(); }

namespace {
std::atomic<const Theme*>& CurrentSlot() {
  static const Theme* first = new Theme(DefaultTheme());  // themes are never freed: other threads may still read one
  static std::atomic<const Theme*> slot{first};
  return slot;
}
}  // namespace

const Theme& CurrentTheme() { return *CurrentSlot().load(std::memory_order_acquire); }

void SetCurrentTheme(const Theme& t) {
  if (*CurrentSlot().load() == t) return;
  CurrentSlot().store(new Theme(t), std::memory_order_release);
}

}  // namespace yc::model
