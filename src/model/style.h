// SPDX-License-Identifier: Apache-2.0
// style.h - cell styles and the style table (PLAN §5.4)
//
// Styles are interned by their full structure into ids (uint32); id 0 = the default style. The table only grows; immutable entries can be shared across threads.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace yc::model {

// colors: 0xAARRGGBB; A = 0 means "automatic" (black text, no fill)
inline constexpr uint32_t kColorAuto = 0;
inline uint32_t Rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | (r << 16) | (g << 8) | b; }

enum class HAlign : uint8_t { General = 0, Left, Center, Right, Fill, Justify, CenterAcross, Distributed };
enum class VAlign : uint8_t { Bottom = 0, Center, Top, Justify, Distributed };
enum class Underline : uint8_t { None = 0, Single, Double, SingleAccounting, DoubleAccounting };

// 13 line styles + none (like Excel)
enum class LineStyle : uint8_t {
  None = 0, Thin, Medium, Dashed, Dotted, Thick, Double, Hair,
  MediumDashed, DashDot, MediumDashDot, DashDotDot, MediumDashDotDot, SlantDashDot,
};

struct BorderSide {
  LineStyle style = LineStyle::None;
  uint32_t color = kColorAuto;
  friend bool operator==(const BorderSide& a, const BorderSide& b) { return a.style == b.style && a.color == b.color; }
};

enum BorderEdge : int { kLeft = 0, kTop, kRight, kBottom, kDiagDown, kDiagUp, kEdgeCount };

// fill patterns of Format Cells -> Fill (Excel's 18 pattern styles; 0 = solid)
enum class FillPattern : uint8_t {
  Solid = 0, Gray75, Gray50, Gray25, Gray125, Gray0625, Horizontal, Vertical, Down, Up, Checker, SemiGray75,
  LightHorizontal, LightVertical, LightDown, LightUp, Grid, CrissCross, Count
};
// two-color gradient fills (Fill Effects): linear at an angle, or from the center outwards
// FromCorner: gradient_angle is the corner the first color starts in (0 upper left, 1 upper right, 2 lower left, 3 lower right)
enum class FillGradient : uint8_t { None = 0, Linear, FromCenter, FromCorner };

struct Style {
  uint32_t font_name = 0;  // StyleTable string id; 0 = default font
  float font_size = 0.0f;  // 0 = default size
  uint32_t font_color = kColorAuto;
  bool bold = false, italic = false, strike = false;
  Underline underline = Underline::None;
  uint8_t script = 0;  // 1 superscript, 2 subscript (Format Cells -> Font -> Effects)
  uint32_t fill_color = kColorAuto;  // solid fill; the background of a pattern; the first color of a gradient
  FillPattern pattern = FillPattern::Solid;
  uint32_t pattern_color = kColorAuto;  // automatic = black
  FillGradient gradient = FillGradient::None;
  int16_t gradient_angle = 0;  // linear: 0 left to right, 90 top to bottom, 45 / 135 diagonal
  uint32_t gradient_color2 = kColorAuto;
  BorderSide border[kEdgeCount]{};
  HAlign halign = HAlign::General;
  VAlign valign = VAlign::Bottom;
  uint8_t indent = 0;
  bool wrap = false, shrink = false;
  int16_t rotation = 0;
  uint32_t numfmt = 0;  // StyleTable string id; 0 = General
  bool locked = true, hidden = false;
  uint32_t cell_style = 0;  // StyleTable string id of the named cell style (Home > Cell Styles); 0 = Normal
  bool justify_last_line = false;  // Alignment > Justify distributed (Distributed text spreads its last line too)
  uint8_t reading_order = 0;       // Alignment > Text direction: 0 Context, 1 Left-to-Right, 2 Right-to-Left

  bool operator==(const Style& o) const;
  size_t Hash() const;
  // the cell has a fill of its own (covers the gridlines)
  bool HasFill() const { return (fill_color >> 24) != 0 || pattern != FillPattern::Solid || gradient != FillGradient::None; }
};

// a named cell style (Home > Cell Styles; Workbook.Styles): cells with it follow when it is modified
enum StyleIncludes : uint8_t { kIncNumber = 1, kIncAlignment = 2, kIncFont = 4, kIncBorder = 8, kIncFill = 16, kIncProtection = 32, kIncAll = 63 };
struct NamedStyle {
  std::string name;     // English name for built-in styles ("Good", "Heading 1"...)
  Style style;          // its formats (cell_style is ignored)
  uint8_t includes = kIncAll;  // the format categories it sets (Style Includes)
  bool builtin = false;
  std::string section;  // gallery section of a built-in style
  std::string number_format;  // built-in number styles: the format code (interned when applied)
  bool operator==(const NamedStyle& o) const { return name == o.name && style == o.style && includes == o.includes && builtin == o.builtin; }
};
// the cell's formats with the style's included categories; cell_style becomes name_id
void ApplyNamedStyle(Style& cell, const NamedStyle& ns, uint32_t name_id);
// Excel's built-in cell styles (colors refer to the theme)
const std::vector<NamedStyle>& BuiltinCellStyles();

// a cell's own Normal style: the default formats, set on a cell under a row's or column's own formats, which it
// doesn't show (Excel's cell record with the Normal style: Clear Formats on C5 under a bold column C). The table
// holds the default style at 0 and again at 1; 0 means "no style of its own".
constexpr uint32_t kStyleOwnNormal = 1;

class StyleTable {
 public:
  StyleTable();
  uint32_t Intern(const Style& s);
  const Style& Get(uint32_t id) const { return id < styles_.size() ? styles_[id] : styles_[0]; }
  size_t size() const { return styles_.size(); }

  uint32_t InternString(const std::string& s);   // font names, number format codes
  uint32_t FindString(const std::string& s) const;  // the id of a string already interned; 0 = not there
  const std::string& Str(uint32_t id) const { return id < strings_.size() ? strings_[id] : strings_[0]; }

  const std::string& FontName(const Style& s) const;  // 0 -> default font name
  float FontSize(const Style& s) const { return s.font_size > 0 ? s.font_size : default_font_size; }
  const std::string& NumberFormat(const Style& s) const;  // 0 -> "General"

  std::string default_font = "Calibri";   // to be calibrated
  float default_font_size = 11.0f;

 private:
  struct Hasher {
    size_t operator()(const Style& s) const { return s.Hash(); }
  };
  std::vector<Style> styles_;
  std::unordered_map<Style, uint32_t, Hasher> index_;
  std::vector<std::string> strings_;
  std::unordered_map<std::string, uint32_t> string_index_;
  std::string general_ = "General";
};

}  // namespace yc::model
