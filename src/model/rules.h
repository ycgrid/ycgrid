// SPDX-License-Identifier: Apache-2.0
// rules.h - conditional formatting, data validation, AutoFilter (PLAN §8.x, §9, the Excel parity list)
//
// Rule formulas are stored as text (without '=') with references relative to the range's upper-left cell, like Excel:
// "B2>100" applied to B2:B100 is "B3>100" for B3.
#pragma once

#include "core/geometry.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace yc::model {

// comparison operators (values match Excel's XlFormatConditionOperator)
enum class CmpOp : uint8_t { Between = 1, NotBetween = 2, Equal = 3, NotEqual = 4, Greater = 5, Less = 6, GreaterEqual = 7, LessEqual = 8 };

// ---- Conditional formatting ----
enum class CfType : uint8_t {
  CellValue = 1,     // xlCellValue: cell value <operator> Formula1 [Formula2]
  Expression = 2,    // xlExpression: the formula is TRUE
  ColorScale = 3,    // xlColorScale
  DataBar = 4,       // xlDatabar
  Top10 = 5,         // xlTop10 (rank, bottom, percent)
  IconSet = 6,       // xlIconSets
  Unique = 8,        // xlUniqueValues (duplicate values when duplicate=true)
  TextContains = 9,  // xlTextString (text_op: contains, does not contain, begins with, ends with)
  Blanks = 10,
  TimePeriod = 11,   // xlTimePeriod: a date in time_period (A Date Occurring)
  AboveAverage = 12,
  NoBlanks = 13,
  Errors = 16,
  NoErrors = 17,
};

struct CfFormat {
  bool has_fill = false, has_font_color = false;
  uint32_t fill_color = 0, font_color = 0;  // ARGB (as in Style)
  int8_t bold = -1, italic = -1, strike = -1;  // -1 = unchanged
  uint32_t border_color = 0;  // the cell's outline (ARGB, alpha 0 = none): the Border tab of the rule's format, "Red Border"
  // Format... (Format Cells' Number, Font, Border and Fill tabs): a number format code ("" = the cell's own), the
  // underline (-1 = the cell's own, else model::Underline), the fill pattern (-1 = none, else model::FillPattern) and
  // its color, and a line per side (left, top, right, bottom: model::LineStyle, 0 = none, and ARGB color)
  std::string numfmt;
  int8_t underline = -1, pattern = -1;
  uint32_t pattern_color = 0;
  uint8_t side_style[4] = {0, 0, 0, 0};
  uint32_t side_color[4] = {0, 0, 0, 0};
  bool operator==(const CfFormat&) const = default;
};

// icon sets: the values of Excel's XlIconSet
enum IconSetStyle : int {
  kIcon3Arrows = 1, kIcon3ArrowsGray = 2, kIcon3Flags = 3, kIcon3TrafficLights1 = 4, kIcon3TrafficLights2 = 5, kIcon3Signs = 6,
  kIcon3Symbols = 7, kIcon3Symbols2 = 8, kIcon4Arrows = 9, kIcon4ArrowsGray = 10, kIcon4RedToBlack = 11, kIcon4Ratings = 12,
  kIcon4TrafficLights = 13, kIcon5Arrows = 14, kIcon5ArrowsGray = 15, kIcon5Ratings = 16, kIcon5Quarters = 17, kIcon3Stars = 18,
  kIcon3Triangles = 19, kIcon5Boxes = 20,
};
// one threshold of an icon set: the icon applies from this value up
struct IconThreshold {
  int type = 0;      // 0 percent (of min..max), 1 number, 2 percentile, 3 formula
  double value = 0;
  std::string formula;  // type 3 (without '=')
  bool gte = true;   // >= (otherwise >)
  bool operator==(const IconThreshold&) const = default;
};

inline int IconSetCount(int style) {
  switch (style) {
    case kIcon4Arrows: case kIcon4ArrowsGray: case kIcon4RedToBlack: case kIcon4Ratings: case kIcon4TrafficLights: return 4;
    case kIcon5Arrows: case kIcon5ArrowsGray: case kIcon5Ratings: case kIcon5Quarters: case kIcon5Boxes: return 5;
    default: return 3;
  }
}
// the default thresholds: equal percent steps (33 / 67, 25 / 50 / 75, 20 / 40 / 60 / 80)
inline std::vector<IconThreshold> DefaultIconThresholds(int style) {
  const int n = IconSetCount(style);
  std::vector<IconThreshold> t;
  for (int i = 1; i < n; ++i) {
    IconThreshold x;
    x.value = n == 3 ? (i == 1 ? 33 : 67) : 100.0 * i / n;
    t.push_back(x);
  }
  return t;
}

struct CfRule {
  uint32_t id = 0;
  std::vector<core::Rect> ranges;
  CfType type = CfType::CellValue;
  CmpOp op = CmpOp::Greater;
  std::string formula1, formula2;  // without '='
  std::string text;                // TextContains
  CfFormat fmt;
  int rank = 10;
  bool bottom = false, percent = false, below = false, duplicate = true;
  // color scale: 2 or 3 points; values are min / median (50th percentile) / max
  int points = 3;
  uint32_t colors[3] = {0xFFF8696B, 0xFFFFEB84, 0xFF63BE7B};  // red - yellow - green (Excel's default)
  uint32_t bar_color = 0xFF638EC6;
  // icon set: style, n - 1 ascending thresholds for the n icons (icon 0 is the lowest), reverse order, hide the value
  int icon_style = kIcon3TrafficLights1;
  std::vector<IconThreshold> icon_thresholds;
  bool icon_reverse = false, icon_only = false;
  int priority = 0;
  bool stop_if_true = false;
  // TextContains: Excel's XlContainsOperator (0 contains, 1 does not contain, 2 begins with, 3 ends with)
  int text_op = 0;
  // TimePeriod: Excel's XlTimePeriods (0 today, 1 yesterday, 2 last 7 days, 3 this week, 4 last week, 5 last month,
  // 6 tomorrow, 7 next week, 8 next month, 9 this month)
  int time_period = 0;
  // AboveAverage: equal or above / below (Excel's xlEqualAboveAverage...), or std_dev standard deviations away (1-3)
  bool equal_average = false;
  int std_dev = 0;
  // color scale points (Excel's ColorScaleCriteria): type per point, Excel's XlConditionValueTypes (0 number,
  // 1 lowest value, 2 highest value, 3 percent, 4 formula, 5 percentile; -1 the default: lowest, 50th percentile,
  // highest), its value, and its formula (type 4, without '=')
  int point_type[3] = {-1, -1, -1};
  double point_value[3] = {0, 50, 0};
  std::string point_formula[3];
  // data bar (Excel's Databar): its shortest and longest bar (MinPoint / MaxPoint: XlConditionValueTypes as above,
  // -1 = automatic), gradient or solid fill, a border (ARGB, 0 = none), the direction (0 context, 1 left-to-right,
  // 2 right-to-left) and Show Bar Only (show_value false)
  int bar_min_type = -1, bar_max_type = -1;
  double bar_min_value = 0, bar_max_value = 0;
  std::string bar_min_formula, bar_max_formula;
  bool bar_gradient = true, bar_show_value = true;
  uint32_t bar_border = 0;
  int bar_direction = 0;
  bool operator==(const CfRule&) const = default;
};
// the same rule apart from where it applies (id, ranges, priority): Paste and Format Painter grow such a rule instead
// of adding another one, as Excel does
inline bool SameRuleLook(CfRule a, const CfRule& b) {
  a.id = b.id;
  a.ranges = b.ranges;
  a.priority = b.priority;
  return a == b;
}

// ---- Data validation ----
enum class DvType : uint8_t { Any = 0, WholeNumber = 1, Decimal = 2, List = 3, Date = 4, Time = 5, TextLength = 6, Custom = 7 };
enum class DvAlert : uint8_t { Stop = 1, Warning = 2, Information = 3 };

struct DvRule {
  std::vector<core::Rect> ranges;
  DvType type = DvType::Any;
  CmpOp op = CmpOp::Between;
  std::string formula1, formula2;  // list: comma-separated values, or =range
  bool ignore_blank = true, in_cell_dropdown = true;
  bool show_input = true, show_error = true;
  DvAlert alert = DvAlert::Stop;
  std::string input_title, input_message, error_title, error_message;
  bool operator==(const DvRule&) const = default;
};

// ---- AutoFilter ----
struct FilterColumn {
  uint32_t field = 0;  // column within the range (0-based)
  // 1: by value (values are the allowed displayed texts; blanks: whether blanks are included); 2: custom (op1 crit1 [and/or op2 crit2])
  // 3: top N / bottom N (top, count, percent); 4: cell (fill) color; 5: font color; 6: dynamic (XlDynamicFilterCriteria:
  // 1 today ... 16 year to date, 17-20 quarters, 21-32 months, 33 above average, 34 below average)
  int kind = 0;
  std::vector<std::string> values;
  bool blanks = true;
  std::string crit1, crit2;  // e.g. ">100", "=T*"
  bool or_op = false;
  bool top = true, percent = false;
  int count = 10;
  uint32_t color = 0;  // kinds 4 and 5: ARGB (0 = no fill / automatic)
  int dynamic = 0;     // kind 6
  // kind 7, Filter by Cell Icon: the conditional formatting icon shown (icon_set an XlIconSet, icon 0 = the lowest
  // values' icon); icon_set 0 = the cells without an icon (No Cell Icon)
  int icon_set = 0, icon = -1;
  bool operator==(const FilterColumn&) const = default;
};

struct SortLevel {
  uint32_t col = 0;
  bool desc = false;
  bool operator==(const SortLevel& o) const { return col == o.col && desc == o.desc; }
};

struct AutoFilter {
  bool on = false;
  core::Rect range{};  // includes the header row
  std::vector<FilterColumn> columns;
  std::map<uint32_t, int32_t> hidden;  // rows hidden by the filter -> their height before hiding (for restoring; -1 = default)
  core::Rect adv_range{};              // the list an Advanced Filter (in place) hid rows of
  // the last sort of the list (AutoFilter.Sort): the buttons show it and Reapply sorts again; col within the range
  std::vector<SortLevel> sort_levels;
  bool operator==(const AutoFilter&) const = default;
};

}  // namespace yc::model
