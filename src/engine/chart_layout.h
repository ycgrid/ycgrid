// SPDX-License-Identifier: Apache-2.0
// Chart calculations and a renderer-independent scene for the chart types ImPlot does not draw
// (radar, waterfall, histogram, Pareto, box & whisker, treemap, sunburst, funnel, and the 3-D charts).
// The screen (ImGui draw list) and the printed page (GDI) draw the same scene.
#pragma once

#include "model/objects.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace yc::engine {

// values of one chart series, resolved from its ranges or taken from Python data
struct ChartSeriesValues {
  std::string name;
  std::vector<double> x, y;
  std::shared_ptr<const model::SeriesData> ext;  // Python data (not copied)
  std::string fmt;                               // number format of the first value cell
  uint32_t color = 0;                            // Excel BGR, 0 = automatic
  int axis = 0;
  bool numeric_x = false;  // x holds the scatter X values (otherwise 1..N)
  std::vector<double> err_plus, err_minus;  // custom error amounts per point (resolved from their ranges or arrays)
  std::vector<double> xerr_plus, xerr_minus;  // ... of the X error bars (scatter charts)
  std::vector<std::string> texts;           // filled maps: each point's text when it isn't a number (category maps)
  std::vector<double> sizes;                // bubble charts: each point's size
  std::vector<std::string> label_texts;     // data labels' Value From Cells
  std::vector<int> joined;                  // #N/A points a line joins over (Show #N/A as an empty cell off)
  const double* X() const { return ext && !ext->x.empty() ? ext->x.data() : x.data(); }
  const double* Y() const { return ext ? ext->y.data() : y.data(); }
  int N() const {
    if (ext) return static_cast<int>(ext->x.empty() ? std::min(ext->y.size(), x.size()) : std::min(ext->x.size(), ext->y.size()));
    return static_cast<int>(std::min(x.size(), y.size()));
  }
  std::vector<double> Finite() const;  // the finite Y values
};

struct ChartValues {
  std::vector<ChartSeriesValues> series;
  std::vector<std::string> cats;                // category labels
  std::vector<std::vector<std::string>> paths;  // hierarchical categories (several category columns), outermost first
  // the categories' numbers (NaN for text) and whether they are dates: a date axis places the points by date
  std::vector<double> cat_vals;
  bool cat_dates = false;
  std::string cat_fmt;  // the first category cell's number format
  size_t points = 0;
  // the titles as drawn: a title linked to a cell (=Sheet1!$B$1) shows the cell's text; "" = none
  std::string title, cat_title, val_title, val2_title;
  // single data labels with text of their own (Format Data Label), as drawn: (series << 32 | point) -> text
  std::map<uint64_t, std::string> point_labels;
};

// ---- calculations ----

struct HistBins {
  std::vector<double> edges;  // bins + 1 edges
  std::vector<double> counts;
  std::vector<std::string> labels;  // "[a, b]", "(b, c]", ...
};
// Excel's automatic bins use Scott's rule; bin_width wins over bin_count
HistBins Histogram(const std::vector<double>& values, double bin_width, int bin_count);
// the chart's bins: Format Axis' overflow ("> x") and underflow ("<= x") bins around the others
HistBins HistogramOf(const std::vector<double>& values, const model::ChartObj& ch);

struct BoxStats {
  bool ok = false;
  double lo = 0, q1 = 0, median = 0, q3 = 0, hi = 0, mean = 0;  // lo / hi: whisker ends (within 1.5 IQR)
  std::vector<double> outliers;
};
// quartiles as QUARTILE.EXC (Excel's default "exclusive median"), or QUARTILE.INC (inclusive median)
BoxStats BoxWhisker(std::vector<double> values, bool inclusive = false);

struct WaterfallBar {
  double start = 0, end = 0;
  int kind = 0;  // 0 increase, 1 decrease, 2 total
};
std::vector<WaterfallBar> Waterfall(const std::vector<double>& values, const std::vector<int>& totals);

struct TrendFit {
  bool ok = false;
  model::TrendKind kind = model::TrendKind::None;
  std::vector<double> c;  // coefficients (see Equation)
  double r2 = 0;
  double Eval(double x) const;
  std::string Equation() const;  // "y = 2.5x + 1", "y = 3e^(0.2x)", ...
};
// least-squares fit over the finite points (moving averages are drawn from MovingAverage instead)
// intercept: Set Intercept (linear, polynomial, exponential), nullptr = fitted
TrendFit FitTrend(const double* xs, const double* ys, int n, model::TrendKind kind, int order, const double* intercept = nullptr);
std::vector<double> MovingAverage(const double* ys, int n, int period);  // NaN for the first period - 1 points

// error bar half-lengths per point; for standard deviation bars *center is the series mean (Excel draws them there)
std::vector<double> ErrorAmounts(model::ErrorBarKind kind, double amount, const double* ys, int n, bool* at_mean, double* mean);
// the lengths below and above each point: custom amounts, and the directions drawn (Include)
void ErrorExtents(const model::ChartSeries& d, const ChartSeriesValues& b, int n, std::vector<double>* minus, std::vector<double>* plus, bool* at_mean, double* mean);
// ... of a scatter chart's X error bars (along the X values)
void ErrorExtentsX(const model::ChartSeries& d, const ChartSeriesValues& b, int n, std::vector<double>* minus, std::vector<double>* plus, bool* at_mean, double* mean);
// filled maps: the region each point names (-1 = not found); level 0 countries, 1 subdivisions
struct MapMatch {
  std::vector<int> region;
  int level = 0;
  size_t matched = 0;
};
MapMatch MatchMapRegions(const ChartValues& v, size_t n);

// a date axis (Format Axis > Axis Type: Automatic with dates as categories, or Date axis): each category placed at its
// date counted in days, months or years (Base Unit), the dates between them kept as gaps
struct DatePlan {
  bool on = false;
  int unit = 0;             // 0 days, 1 months, 2 years
  int64_t u0 = 0, u1 = 0;   // the first and last units shown
  std::vector<double> pos;  // each point's place, 1-based (the first unit is 1)
};
// the date axis of a chart (Axis Type Automatic with dates as categories, or Date axis); n: the points drawn
DatePlan PlanDateAxis(const model::ChartObj& ch, const ChartValues& v, size_t n);
// a date serial counted in days (unit 0), months (1) or years (2), and a unit's first day
int64_t DateUnit(double serial, int unit);
double UnitSerial(int64_t u, int unit);

// pie of pie / bar of pie: which points go to the second plot (ChartGroup.SplitType / SplitValue, custom points)
std::vector<char> SecondPlotPoints(const model::ChartObj& ch, const double* ys, int n);

// "{1,2,3}" or "2.5": the numbers of an array constant (empty when it is neither)
std::vector<double> ParseNumberList(const std::string& text);

struct TreeRect {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int level = 0, group = 0;  // group: index of the top-level category
  bool leaf = false;
  std::string label;
  double value = 0;
};
// squarified treemap; parents get a banner of banner_h at the top that holds their label
std::vector<TreeRect> TreemapLayout(const std::vector<std::vector<std::string>>& paths, const std::vector<double>& values, double x0, double y0, double x1, double y1,
                                    double banner_h);

struct SunSegment {
  int level = 0, group = 0;
  double a0 = 0, a1 = 0;  // radians, clockwise from 12 o'clock
  std::string label;
  double value = 0;
  bool leaf = false;
};
std::vector<SunSegment> SunburstLayout(const std::vector<std::vector<std::string>>& paths, const std::vector<double>& values, int* levels);

double NiceStep(double range);  // about 5 steps of 1, 2 or 5 x 10^n

// ---- scene ----

struct ChartPrim {
  enum Kind : uint8_t { Rect, Poly, Line, Text, Circle } kind = Rect;
  std::vector<float> pts;  // x, y pairs (Rect: x0 y0 x1 y1, Circle: cx cy r)
  uint32_t fill = 0;       // ABGR (IM_COL32 / COLORREF layout with alpha on top); 0 = none
  uint32_t stroke = 0;
  float width = 1;
  std::string text;
  int8_t halign = 0, valign = 0;  // 0 left / top, 1 center, 2 right / bottom
  float scale = 1;                // text size relative to the base font
  bool bold = false;
  int16_t rotate = 0;   // text: degrees counterclockwise around its anchor (90 = reading upwards)
  uint8_t elem = 2;     // the chart element it belongs to (model::ChartElem; 2 = plot area)
  int16_t series = -1;  // a series' index (elem = Series)
  int32_t point = -1;   // a data point's index (waterfall columns), -1 = the whole series
};

struct ChartScene {
  std::vector<ChartPrim> prims;
};

// width of a text at base font size * scale, in scene units
using MeasureText = std::function<float(const std::string& text, float scale)>;

bool IsSceneChart(model::ChartType t);
// a series' own type in a combo chart (line, column, area and scatter types combine); otherwise the chart's type
model::ChartType EffectiveSeriesType(const model::ChartObj& ch, size_t i);
bool IsBarType(model::ChartType t);  // clustered / stacked column, clustered bar
// the families of the chart types ImPlot draws
bool IsHorizontalBar(model::ChartType t);  // clustered, stacked and 100% stacked bars
bool IsStackedType(model::ChartType t);    // stacked and 100% stacked columns, bars, lines and areas
bool IsPercentType(model::ChartType t);    // 100% stacked
bool IsLineType(model::ChartType t);       // lines (with or without markers, stacked or not)
bool IsAreaType(model::ChartType t);       // area, stacked area, 100% stacked area
bool IsScatterType(model::ChartType t);    // XY scatter in every variant, and bubble
bool HasMarkers(model::ChartType t);       // drawn with markers by default
bool IsStockType(model::ChartType t);
// w, h: chart size; font: base line height (scene units)
void BuildChartScene(const model::ChartObj& ch, const ChartValues& v, float w, float h, float font, const MeasureText& measure, ChartScene* out);

uint32_t ChartPalette(size_t i);  // Excel's Office palette as ABGR (the palette of the chart being drawn, see PaletteScope)
// Change Colors: Excel's colorful palettes (0-3: the theme's accents in other orders) and monochromatic ones (4-9: shades
// of one accent)
constexpr int kChartColorSchemes = 10;
const char* ChartColorSchemeName(int scheme);
// while it lives, ChartPalette draws from a chart's palette (Chart.ChartColor); the previous one comes back after
struct PaletteScope {
  explicit PaletteScope(int scheme);
  ~PaletteScope();
  PaletteScope(const PaletteScope&) = delete;
  PaletteScope& operator=(const PaletteScope&) = delete;
  int prev = 0;
};
uint32_t SeriesColor(const model::ChartObj& ch, const ChartValues& v, size_t i);
std::string FormatChartNumber(double v, const std::string& fmt);

// Chart Design > Quick Layout (Excel's Layout 1-11: which elements show and where) and Chart Styles (Style 1-8: fills,
// text colors, gridlines, labels and column widths together). Both are 1-based; a style replaces the elements' own
// fill and text colors, as Excel's does. ChartObj::style remembers the style (0 = Style 1).
// kChartStyles: the most a set has (Excel: 10-15 per kind of chart, see ChartStyleCount)
constexpr int kChartQuickLayouts = 11, kChartStyles = 15;
void ApplyChartQuickLayout(model::ChartObj& ch, int layout);
void ApplyChartStyle(model::ChartObj& ch, int style);
const char* ChartQuickLayoutTip(int layout);  // what the layout shows ("Chart title, legend at the right")
const char* ChartStyleTip(int style);
// Excel's Chart Styles gallery has a set per kind of chart (columns 14, bars 13, lines 15, pies 12, doughnuts 10,
// areas 11, scatter 15, the others 8); Chart.ChartStyle reads ChartStyleNumber + style - 1 (201 for columns...)
int ChartStyleCount(model::ChartType t);
int ChartStyleNumber(model::ChartType t);
std::string ChartStyleTip(model::ChartType t, int style);  // what the style looks like ("Gradient fills, no gridlines")

// Multi-level Category Labels: the first n categories' labels by level, outermost first, when the category axis shows
// them in levels (several category columns, a PivotChart's row fields; Format Axis' check box on); empty: one level
std::vector<std::vector<std::string>> ChartCategoryLevels(const model::ChartObj& ch, const ChartValues& v, size_t n);
// the label of category i on a one-level axis: several levels joined with spaces
std::string ChartCategoryLabel(const ChartValues& v, size_t i);

}  // namespace yc::engine
