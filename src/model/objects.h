// SPDX-License-Identifier: Apache-2.0
// objects.h - objects over the sheet and workbook-level objects (PLAN §8.13, §9, §10, §12.9, §15.7)
#pragma once

#include "core/geometry.h"
#include "model/shape_effects.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace yc::model {

enum class Placement : uint8_t { MoveAndSize = 1, Move = 2, FreeFloating = 3, FloatWindow = 100 };

// ---- Controls (PLAN §12.9) ----
enum class ControlKind : uint8_t {
  Button = 0,
  ComboBox = 1,
  CheckBox = 2,
  OptionButton = 3,
  ToggleButton = 4,
  TextBox = 5,
  Label = 6,
  ListBox = 7,
  SpinButton = 8,
  ScrollBar = 9,
  Image = 10,
  GroupBox = 11,  // form controls: a frame that groups option buttons
};
inline constexpr int kControlKinds = 12;
// "CommandButton", "ComboBox", "CheckBox"... (the default name prefix and the Forms.<Name>.1 ProgID)
inline const char* ControlTypeName(ControlKind k) {
  static const char* kNames[] = {"CommandButton", "ComboBox", "CheckBox", "OptionButton", "ToggleButton", "TextBox", "Label", "ListBox", "SpinButton", "ScrollBar", "Image", "GroupBox"};
  const int i = static_cast<int>(k);
  return i >= 0 && i < kControlKinds ? kNames[i] : "CommandButton";
}

// a form control's name as Excel gives it, before its number ("Drop Down 1", "Spinner 2")
inline const char* FormControlTypeName(ControlKind k) {
  static const char* kNames[] = {"Button", "Drop Down", "Check Box", "Option Button", "Toggle Button", "Edit Box", "Label", "List Box", "Spinner", "Scroll Bar", "Image", "Group Box"};
  const int i = static_cast<int>(k);
  return i >= 0 && i < kControlKinds ? kNames[i] : "Button";
}

struct Control {
  uint32_t id = 0;
  ControlKind kind = ControlKind::Button;
  std::string name;
  std::string caption;
  // position (points, sheet coordinates; relative to the window for FloatWindow)
  double left = 0, top = 0, width = 72, height = 24;
  Placement placement = Placement::MoveAndSize;
  bool enabled = true, visible = true, locked = true, word_wrap = false;
  uint32_t back_color = 0, fore_color = 0;  // 0 = default
  std::string font_name;
  float font_size = 0;
  bool bold = false, italic = false;
  // combo box
  std::vector<std::string> items;
  std::string list_fill_range;  // e.g. "Settings!A2:A20"
  std::string linked_cell;
  int style = 0;  // 0 = editable, 1 = select only
  int list_rows = 8;
  std::string value;  // check box, option and toggle buttons: "True" / "False"; spin buttons and scroll bars: the number
  int list_index = -1;
  int z = 0;
  bool created_by_python = false;
  // option buttons: one choice per group (empty = the sheet's default group)
  std::string group_name;
  // spin buttons and scroll bars
  int min = 0, max = 100, small_change = 1, large_change = 10;
  int orientation = -1;  // -1 automatic (by the shape), 0 vertical, 1 horizontal
  // list box: 0 single, 1 multi (click toggles), 2 extended (Ctrl / Shift); selected items when not single
  int multi_select = 0;
  std::vector<int> selected;
  // text box
  bool multi_line = false;
  std::string password_char;
  // image: picture bytes (PNG, JPEG, BMP, GIF); 0 clip, 1 stretch, 3 zoom (fmPictureSizeMode)
  std::string picture;
  int picture_size_mode = 1;
  // a form control (Developer -> Insert -> Form Controls): runs its OnAction macro instead of sheet-module events, and
  // list boxes, combo boxes and option buttons put the 1-based number of the choice in the linked cell
  bool form = false;
  std::string on_action;
  // Format Control > Properties > Print object, Alt Text (control map keys 43, 44)
  bool print_object = true;
  std::string alt_text;
  // a form button's Alignment tab (key 45): 0 left / top, 1 center, 2 right / bottom; orientation 0 horizontal,
  // 1 vertical (stacked), 2 upward, 3 downward; Automatic size
  int text_halign = 1, text_valign = 1, text_orientation = 0;
  bool auto_size = false;
  // its Margins tab (key 46): Automatic, else left / top / right / bottom (points)
  bool auto_margins = true;
  double margin_l = 7.2, margin_t = 3.6, margin_r = 7.2, margin_b = 3.6;
  // 3-D shading (key 47): check boxes, option buttons, list / combo boxes, group boxes, scroll bars, spinners
  bool shading_3d = false;
  bool operator==(const Control&) const = default;
};

// the texts of a control that refer to cells (its linked cell and its list's range, "$A$1" or "Sheet1!$A$1"): code that
// moves, renames or breaks references goes through this list, as for charts (ForEachChartRefText)
template <class Fn>
void ForEachControlRefText(Control& c, Fn&& fn) {
  if (!c.linked_cell.empty()) fn(c.linked_cell);
  if (!c.list_fill_range.empty()) fn(c.list_fill_range);
}

inline bool ControlIsOn(const Control& c) { return c.value == "True" || c.value == "TRUE" || c.value == "true" || c.value == "1" || c.value == "-1"; }

// ---- Charts (PLAN §9) ----
enum class ChartType : uint8_t {
  Line = 0,
  LineMarkers,
  ColumnClustered,
  ColumnStacked,
  BarClustered,
  Scatter,
  Area,
  Pie,
  Doughnut,
  StockOHLC,
  Radar,
  RadarMarkers,
  RadarFilled,
  Waterfall,
  Histogram,
  Pareto,
  BoxWhisker,
  Treemap,
  Sunburst,
  Funnel,
  // 3-D charts (projected and drawn back to front)
  Column3D,           // 3-D Column: categories across, series in rows along the depth
  ColumnClustered3D,  // 3-D Clustered Column
  ColumnStacked3D,    // 3-D Stacked Column
  BarClustered3D,     // 3-D Clustered Bar
  Line3D,             // 3-D Line: a ribbon per series
  Area3D,             // 3-D Area
  Pie3D,              // 3-D Pie
  Surface,            // 3-D Surface (colored by value bands)
  SurfaceWireframe,   // Wireframe 3-D Surface
  SurfaceContour,     // Contour (a surface seen from the top)
  FilledMap,          // Filled map: countries or their states / provinces / counties colored by value (or by category)
  // appended (numbers in files keep their meaning)
  ColumnStacked100,        // 100% Stacked Column
  BarStacked,              // Stacked Bar
  BarStacked100,           // 100% Stacked Bar
  LineStacked,             // Stacked Line
  LineStacked100,          // 100% Stacked Line
  LineMarkersStacked,      // Stacked Line with Markers
  LineMarkersStacked100,   // 100% Stacked Line with Markers
  AreaStacked,             // Stacked Area
  AreaStacked100,          // 100% Stacked Area
  ScatterLines,            // Scatter with Straight Lines and Markers
  ScatterLinesNoMarkers,   // Scatter with Straight Lines
  ScatterSmooth,           // Scatter with Smooth Lines and Markers
  ScatterSmoothNoMarkers,  // Scatter with Smooth Lines
  Bubble,                  // Bubble: X, Y and a size per point
  StockHLC,                // Stock: High-Low-Close
  PieOfPie,                // Pie of Pie: the last points pulled out into a second, smaller pie
  BarOfPie,                // Bar of Pie: ... into a stacked bar
  StockVHLC,               // Stock: Volume-High-Low-Close (volume columns, prices on the secondary axis)
  StockVOHLC,              // Stock: Volume-Open-High-Low-Close
};
constexpr int kChartTypeCount = static_cast<int>(ChartType::StockVOHLC) + 1;
inline bool Is3DChart(ChartType t) { return t >= ChartType::Column3D && t <= ChartType::SurfaceContour; }

// trendline kinds (Series.Trendlines.Add)
enum class TrendKind : uint8_t { None = 0, Linear, Exponential, Logarithmic, Polynomial, Power, MovingAverage };
// a series' second and later trendlines (Series.Trendlines(2...)); the first one lives in the series' trend fields
struct ChartTrendline {
  TrendKind kind = TrendKind::Linear;
  int order = 2, period = 2;
  double forward = 0, backward = 0;
  bool eq = false, r2 = false;
  std::string name;           // "" = automatic ("Linear (Sales)")
  bool intercept_on = false;  // Set Intercept
  double intercept = 0;
  bool operator==(const ChartTrendline&) const = default;
};
// error bar kinds (Series.ErrorBar)
enum class ErrorBarKind : uint8_t { None = 0, Fixed, Percent, StDev, StError, Custom };

struct SeriesData {  // SeriesCache built by the engine (immutable)
  std::vector<double> x, y;
  std::vector<std::string> cats;  // category labels (PivotCharts)
  std::vector<std::vector<std::string>> cat_paths;  // PivotCharts: each category's labels by level, outermost first (several row fields)
  std::vector<double> open, high, low, close;
  uint64_t version = 0;
  bool operator==(const SeriesData&) const = default;
};

// a chart element's text (title, legend, axes): 0 = automatic
struct ChartFont {
  float size = 0;      // points
  uint32_t color = 0;  // ARGB
  bool bold = false;
  std::string name;    // "" = the chart's font
  bool italic = false, underline = false;
  bool operator==(const ChartFont&) const = default;
};

// one data point's own formatting (Series.Points(i))
struct ChartPointFormat {
  int index = 0;            // 0-based point
  uint32_t fill = 0;        // Excel BGR; 0 = the series'
  uint32_t line = 0;        // border
  int explosion = -1;       // pie slices: % of the radius (-1 = the series')
  int label = -1;           // its data label: -1 the series', 0 hidden, 1 shown
  float label_dx = 0, label_dy = 0;  // its data label dragged away from its place (fractions of the chart's size)
  // its data label's own Label Options and text (Format Data Label): "" the series' contents, "=Sheet1!$C$2" a cell's
  std::string label_text;
  int label_show = -1, label_pos = -1;  // -1: the series'
  bool label_font_set = false;
  ChartFont label_font;
  bool legend_font_set = false;  // a pie's (or a vary-colors chart's) category legend entry: its own font
  ChartFont legend_font;
  uint32_t label_fill = 0, label_line = 0;  // its data label's own fill and border (ARGB; 0 = the series')
  bool legend_hidden = false;               // its category legend entry deleted (a pie's)
  bool operator==(const ChartPointFormat&) const = default;
};


struct ChartSeries {
  std::string name;
  std::string values;    // A1 with the sheet, e.g. "Quotes!C2:C500"
  std::string x_values;  // may be empty: 1..N is used
  int axis_group = 0;    // 0 primary, 1 secondary
  uint32_t color = 0;    // 0 = automatic
  std::string topic;     // stream (BindTopic)
  uint32_t capacity = 100000;
  bool python_data = false;  // supplied by SetData / AppendData
  std::shared_ptr<const SeriesData> data;
  int type = -1;             // combo charts: this series' own ChartType (-1 = the chart's type)
  bool smooth = false;       // line / scatter: a smoothed line through the points (Series.Smooth)
  // box and whisker: 1 inner points, 2 outlier points, 4 mean markers, 8 mean line (Excel shows outliers and mean markers)
  int box_flags = 6;
  bool data_labels = false;  // show each point's value
  TrendKind trend = TrendKind::None;
  int trend_order = 2;   // polynomial order (2..6)
  int trend_period = 2;  // moving average period
  double trend_forward = 0, trend_backward = 0;  // forecast periods
  bool trend_eq = false, trend_r2 = false;        // show the equation / R-squared
  ErrorBarKind err = ErrorBarKind::None;
  double err_amount = 1;       // fixed value, percent or number of standard deviations
  std::string err_plus, err_minus;  // custom amounts: a range ("Sheet1!E2:E9"), an array ("{1,2,3}") or one number
  int err_include = 1;              // xlErrorBarIncludeBoth (1), PlusValues (2), MinusValues (3)
  std::vector<int> totals;     // waterfall: point indexes (0-based) drawn as totals
  float line_weight = 0;       // points (0 = automatic)
  int marker = -1;             // -1 automatic, 0 none, 1 circle, 2 square, 3 diamond, 4 triangle, 5 cross, 6 plus
  float marker_size = 0;       // points (0 = automatic)
  bool filtered = false;       // Chart Filters: left out of the chart, keeping its place and formatting (Series.IsFiltered)
  // single points (Format Data Point): their own fill and border, a pie slice pulled out, their own data label
  std::vector<ChartPointFormat> points;
  int explosion = 0;         // pie, doughnut: every slice pulled out (% of the radius)
  bool vary_colors = true;   // pie, doughnut: a color per slice (off: the series color)
  // data labels (Format Data Labels)
  int label_pos = -1;        // -1 automatic; 0 center, 1 inside end, 2 inside base, 3 outside end, 4 left, 5 right, 6 above, 7 below, 8 best fit
  int label_show = 4;        // what a label holds: 1 series name, 2 category name, 4 value, 8 percentage, 32 value from cells
  std::string label_range;   // Value From Cells ("Sheet1!$C$2:$C$5")
  std::string label_format;  // number format ("" = linked to the source)
  std::string label_sep = ", ";
  std::string bubble_sizes;  // bubble charts: the sizes' range
  // Fill & Line (Excel BGR colors; 0 = automatic)
  uint32_t line_color = 0;   // a line's color, a column's / an area's / a slice's border
  bool no_fill = false, no_line = false;
  int dash = 0;              // 0 solid, 1 round dot, 2 square dot, 3 dash, 4 dash dot, 5 long dash
  int transparency = 0;      // fill transparency (%)
  bool invert_negative = false;  // columns and bars: negative values in invert_color (white when automatic)
  uint32_t invert_color = 0;
  uint32_t marker_fill = 0, marker_line = 0;
  // Fill & Line, as Excel's Format Data Series (series keys 49-53)
  int fill_type = 0;          // 0 solid (or automatic), 1 gradient, 2 picture or texture, 3 pattern
  uint32_t grad_color2 = 0;   // gradient: the second stop (Excel BGR; 0 = a lighter shade of the color)
  int grad_angle = 90;        // gradient: degrees (90: top to bottom)
  int pattern = 0;            // pattern fill: 0 5%, 1 10%... (kChartPatternCount kinds)
  uint32_t pattern_back = 0;  // pattern fill: the background color (0 = white)
  bool picture_tile = false;  // picture fill: tiled instead of stretched
  // Tile picture as texture: offsets (points), scale (%), alignment (0 top left .. 8 bottom right, row by row), mirror
  // type (0 none, 1 horizontal, 2 vertical, 3 both)
  float tile_ox = 0, tile_oy = 0, tile_sx = 100, tile_sy = 100;
  int tile_align = 0, tile_mirror = 0;
  std::string picture;        // picture fill: the image file's bytes (PNG, JPEG, BMP, GIF)
  int compound = 0;           // line compound type: 0 single, 1 double, 2 thick-thin, 3 thin-thick, 4 triple
  int cap = 0;                // line cap: 0 flat, 1 round, 2 square
  int join = 0;               // line join: 0 round, 1 bevel, 2 miter
  int begin_arrow = 0, end_arrow = 0;  // lines: 0 none, 1 arrow, 2 open arrow, 3 stealth, 4 diamond, 5 oval
  bool leader_lines = true;   // data labels moved away from a pie: Show Leader Lines
  bool vary_points = false;   // a single series of columns, bars or lines: Vary colors by point (pies use vary_colors)
  bool legend_hidden = false; // its legend entry deleted (LegendEntry.Delete)
  ChartFont label_font;       // its data labels' text (Text Options)
  bool legend_entry_font_set = false;  // its legend entry's own font (Format Legend Entry), else the legend's
  ChartFont legend_entry_font;
  uint32_t label_fill = 0, label_line = 0;  // its data labels' fill and border (Format Data Labels > Fill & Line; ARGB, 0 = none)
  // ---- trendline name / intercept, more trendlines, error bar end style and X error bars, type options (series
  // keys 56-58) ----
  std::string trend_name;       // the first trendline's custom name ("" = automatic)
  bool trend_icept_on = false;  // Set Intercept (linear, polynomial: the constant; exponential: the factor)
  double trend_icept = 0;
  std::vector<ChartTrendline> more_trends;
  bool err_cap = true;  // error bars' End Style: Cap (false: No Cap)
  // X error bars (scatter and bubble charts): the same options as the Y ones
  ErrorBarKind xerr = ErrorBarKind::None;
  double xerr_amount = 1;
  std::string xerr_plus, xerr_minus;
  int xerr_include = 1;
  int parent_labels = 1;          // treemap: Label Options 0 None, 1 Banner, 2 Overlapping (Series.ParentDataLabelOption)
  bool inclusive_median = false;  // box and whisker: Quartile Calculation (false: exclusive median)
  // filled map: projection (0 automatic, 1 Mercator, 2 Miller, 3 Albers, 4 Robinson), area (Series.GeoMappingLevel: 0
  // automatic, 1 data only, 5 country / region, 6 countries / regions, 7 world), labels (0 none, 1 best fit, 2 all)
  int geo_projection = 0, geo_area = 0, geo_labels = 0;
  int geo_colors = 0;  // Series Color: 0 sequential (2-color), 1 diverging (3-color)
  uint32_t geo_min = 0, geo_mid = 0, geo_max = 0;  // Excel BGR, 0 = automatic
  float label_dx = 0, label_dy = 0;  // data labels dragged inside the chart: offset (fractions of the chart's size)
  bool operator==(const ChartSeries&) const = default;
};
constexpr int kChartPatternCount = 12;

inline const ChartPointFormat* PointFormatOf(const ChartSeries& s, int point) {
  for (const auto& p : s.points) {
    if (p.index == point) return &p;
  }
  return nullptr;
}
// a data label's offset from its place (fractions of the chart's size), the same on the screen and on paper: the series'
// labels dragged together, then this label dragged on its own
inline void DataLabelOffset(const ChartSeries& s, int point, float* dx, float* dy) {
  const ChartPointFormat* p = point >= 0 ? PointFormatOf(s, point) : nullptr;
  *dx = s.label_dx + (p ? p->label_dx : 0.0f);
  *dy = s.label_dy + (p ? p->label_dy : 0.0f);
}


// the chart's elements (Format pane, Chart Design > Add Chart Element); new ones are appended so stored numbers keep
// their meaning
enum class ChartElem : int {
  None = 0,
  ChartArea,
  PlotArea,
  Title,
  Legend,
  CategoryAxis,
  ValueAxis,
  Series,
  ValueAxis2,      // the secondary vertical axis
  Point,           // one data point of a series
  CatAxisTitle,    // the category axis' title
  ValAxisTitle,    // the value axis' title
  MajorGridlines,  // the value axis' major gridlines
  DataLabels,      // a series' data labels
  Trendline,       // a series' trendline
  ErrorBars,       // a series' error bars
  ValAxis2Title,   // the secondary vertical axis' title
  UpBars,          // a stock chart's rising candles (Up Bars)
  DownBars,        // ... falling candles (Down Bars)
  HiLoLines,       // ... high-low lines
  Floor,           // a 3-D chart's floor
  Walls,           // ... back and side walls
  LegendEntry,     // one legend entry (series: its series)
  DataLabel,       // one data label (series, point)
  MinorGridlines,  // the value axis' minor gridlines
};
constexpr int kChartElemLast = static_cast<int>(ChartElem::MinorGridlines);

// a category or value axis
struct ChartAxisFormat {
  bool visible = true;
  bool has_title = false;
  std::string title;          // "=Sheet1!$B$1": linked to a cell
  std::string number_format;  // tick labels ("" = from the data)
  bool gridlines = false;     // major gridlines
  // Format Axis > Axis Options (value axes; a scatter chart's X axis)
  bool min_auto = true, max_auto = true;  // Bounds, each automatic or fixed
  double min = 0, max = 1;
  double major = 0, minor = 0;  // Units (0 = automatic)
  bool log = false;             // Logarithmic scale
  double log_base = 10;
  bool reverse = false;         // Values (categories) in reverse order
  int crosses = 0;              // the other axis crosses at: 0 automatic, 1 crosses_at, 2 the maximum
  double crosses_at = 0;
  double display_unit = 1;      // Display units: the labels divided by it (1000 Thousands...)
  int major_tick = -1;          // Tick Marks: -1 automatic, 0 none, 1 inside, 2 outside, 3 cross
  int minor_tick = 0;
  int label_pos = 0;            // Labels: 0 next to axis, 1 high, 2 low, 3 none
  bool minor_gridlines = false;
  bool between = true;          // category axis: columns between tick marks (false: on tick marks)
  int label_interval = 0;       // category axis: a label every n categories (0 automatic)
  int mark_interval = 0;        // category axis: a tick mark every n categories (0: with the labels; Axis.TickMarkSpacing)
  bool unit_label = true;       // value axis: its display unit's label ("Thousands") beside it (HasDisplayUnitLabel)
  // the axis' own text (Text Options) and line (Fill & Line), Label distance from axis (axis array elements 23-28)
  bool own_font = false;        // false: the chart's axis_font
  ChartFont font;
  ChartFont title_font;         // its title's text (size 0: 10 pt)
  uint32_t line_color = 0;      // ARGB; 0 = automatic
  bool no_line = false;
  float line_weight = 0;        // points (0 = automatic)
  int label_offset = 100;       // Label distance from axis (Excel's 0..1000, 100 by default)
  // category axis: Multi-level Category Labels (several category columns / a PivotTable's row fields: a row of labels
  // per level, the outer ones grouped, lines between the groups); axis array element 30
  bool multi_level = true;
  bool operator==(const ChartAxisFormat&) const = default;
};

struct ChartObj {
  uint32_t id = 0;
  std::string name;
  ChartType type = ChartType::Line;
  double left = 0, top = 0, width = 360, height = 216;  // Excel's default 5 x 3 inches
  Placement placement = Placement::MoveAndSize;
  std::string title;
  bool has_title = false, has_legend = true, interactive = false, floating = false;
  // (the axes' bounds live in cat_axis / val_axis / val_axis2: min_auto, max_auto, min, max)
  std::vector<ChartSeries> series;
  int z = 0;
  bool created_by_python = false;
  std::string pivot;  // a PivotChart: the PivotTable whose report it shows (its series follow every refresh)
  double bin_width = 0;  // histogram: bin width (0 = automatic)
  int bin_count = 0;     // histogram: number of bins (0 = automatic; used when bin_width is 0)
  // Format Data Series options, as Excel's
  int gap_width = 219;        // columns and bars: the space between categories (% of a bar's width)
  int overlap = -27;          // clustered columns and bars: how much a category's bars overlap (%; negative: a gap)
  int first_slice_angle = 0;  // pie and doughnut: where the first slice starts (degrees clockwise from 12 o'clock)
  int hole_size = 50;         // doughnut: the hole (% of the diameter)
  std::vector<int> hidden_cats;  // Chart Filters: categories (0-based points) left out (FullCategoryCollection.IsFiltered)
  std::string source;    // the range the series came from (SetSourceData); a new chart type lays it out again
  bool source_by_rows = false;
  // element formatting
  ChartFont title_font, legend_font, axis_font;
  int legend_pos = 0;  // 0 bottom, 1 right, 2 top, 3 left, 4 top right corner
  uint32_t chart_fill = 0, chart_line = 0, plot_fill = 0;  // ARGB; 0 = automatic (white, light gray border, no fill)
  bool chart_line_none = false;
  ChartAxisFormat cat_axis, val_axis{true, false, {}, {}, true};
  ChartAxisFormat val_axis2{true, false, {}, {}, false};  // the secondary vertical axis (series on AxisGroup 2)
  // 3-D view (Format Chart Area > 3-D Rotation); view3d false: the chart type's defaults
  bool view3d = false;
  double rotation = 20, elevation = 15, perspective = 15, depth_percent = 100;
  // Hidden and Empty Cells (Select Data): hidden rows / columns left out; empty cells as gaps (1), zero (2) or joined (3)
  bool plot_visible_only = true;
  int display_blanks = 1;
  bool na_as_blank = true;  // #N/A shown as an empty cell
  bool title_overlay = false, legend_overlay = false;  // Centered Overlay title; a legend over the plot area
  int color_scheme = 0;  // Change Colors: 0 the theme's colors, 1-3 other colorful palettes, 4-9 one hue (monochromatic)
  int style = 0;         // Chart Styles: 0 default, 1-8 presets
  // histogram bins (Format Axis > Bins): overflow / underflow bins and categories counted as they are
  bool bins_overflow = false, bins_underflow = false, bins_by_category = false;
  double overflow_at = 0, underflow_at = 0;
  bool connectors = true;  // waterfall: Show connector lines
  bool lock_aspect = false;
  // borders and fills of the other elements (ARGB; 0 = automatic: none)
  uint32_t plot_line = 0, legend_fill = 0, legend_line = 0, title_fill = 0, title_line = 0;
  // manual layout (fractions of the chart): the title, the legend and the plot area dragged inside the chart
  struct ManualRect {
    bool on = false;
    float x = 0, y = 0, w = 0, h = 0;
    bool operator==(const ManualRect&) const = default;
  };
  ManualRect title_rect, legend_rect, plot_rect;
  // ---- pie of pie / bar of pie (ChartGroup.SplitType, SplitValue, SecondPlotSize, GapWidth) ----
  int split_type = 1;        // 1 position, 2 value, 3 percent value, 4 custom (XlChartSplitType)
  double split_value = 0;    // position: the last n points; value / percent: the points below it (0 = automatic)
  int second_size = 75;      // the second plot's size (% of the first)
  int pie_gap = 100;         // the gap between the two plots (%)
  std::vector<int> second_points;  // custom split: the points (0-based) in the second plot
  // ---- stock charts: up / down bars and high-low lines (ChartGroup.HasUpDownBars, HasHiLoLines) ----
  bool up_down_bars = true, hi_lo_lines = true;
  uint32_t up_fill = 0, down_fill = 0, hilo_line = 0;  // ARGB, 0 = automatic (YCGrid's red up, green down)
  int updown_gap = 150;  // Up / Down Bars' gap width (%)
  // ---- 3-D charts: floor and walls (Chart.Floor, Chart.Walls); ARGB, 0 = automatic ----
  uint32_t floor_fill = 0, wall_fill = 0;
  // ---- date axis (Axis.CategoryType: 0 automatic, 1 text, 2 date; Axis.BaseUnit: -1 automatic, 0 days, 1 months,
  // 2 years; Axis.MajorUnitScale: the major unit's unit on a date axis, -1 = the base unit) ----
  int cat_type = 0, base_unit = -1, major_unit_scale = -1;
  // the axis titles dragged inside the chart (manual layout, position only)
  ManualRect cat_title_rect, val_title_rect, val2_title_rect;
  // Format Chart Area > Properties: Print object, Locked
  bool print_object = true, locked = true;
  bool operator==(const ChartObj&) const = default;
};

// every text of a chart that refers to cells: the source and the series' ranges, and the texts linked to a cell
// ("=Sheet1!$B$1": a series' name, a title, a data label). Code that moves, renames or breaks references goes through
// this list, so a linked name follows its cell as the values do.
template <class Fn>
void ForEachChartRefText(ChartObj& c, Fn&& fn) {
  auto linked = [&](std::string& t) {
    if (!t.empty() && t[0] == '=') fn(t);
  };
  auto ranges = [&](std::string& t) {
    if (!t.empty()) fn(t);
  };
  ranges(c.source);
  linked(c.title);
  linked(c.cat_axis.title);
  linked(c.val_axis.title);
  linked(c.val_axis2.title);
  for (auto& se : c.series) {
    ranges(se.values);
    ranges(se.x_values);
    linked(se.name);
    ranges(se.err_plus);
    ranges(se.err_minus);
    ranges(se.label_range);   // data labels' Value From Cells
    ranges(se.bubble_sizes);
    ranges(se.xerr_plus);     // a scatter chart's custom X error amounts
    ranges(se.xerr_minus);
    for (auto& pf : se.points) linked(pf.label_text);
  }
}

// ---- Pictures, shapes and text boxes (Insert -> Illustrations / Text Box) ----
enum class ShapeKind : uint8_t { Picture = 0, Rectangle = 1, RoundedRectangle = 2, Oval = 3, Triangle = 4, Line = 5, Arrow = 6, RightArrow = 7, TextBox = 8 };

struct ShapeObj {
  uint32_t id = 0;
  ShapeKind kind = ShapeKind::Rectangle;
  std::string name;
  double left = 0, top = 0, width = 144, height = 72;  // points
  Placement placement = Placement::MoveAndSize;
  bool fill_visible = true, line_visible = true;
  uint32_t fill_color = 0xFF4472C4, line_color = 0xFF2F528F;  // ARGB
  float line_weight = 0.75f;                                  // points
  bool flip_h = false, flip_v = false;                        // lines and arrows: which diagonal
  // text (shapes and text boxes)
  std::string text;
  std::string font_name;
  float font_size = 11;
  uint32_t font_color = 0xFFFFFFFF;
  bool bold = false, italic = false;
  int halign = 1, valign = 1;  // 0 left / top, 1 center / middle, 2 right / bottom
  // picture: the image file's bytes (PNG, JPEG, BMP, GIF)
  std::string image;
  std::string on_action;  // macro run when clicked
  bool visible = true;
  int z = 0;
  // Format Shape / Shape Format (saved in the shape record's element #12, an integer-keyed map)
  int line_dash = 0;                 // 0 solid, 1 dash, 2 round dot, 3 square dot, 4 dash dot, 5 long dash
  int begin_arrow = 0, end_arrow = 0;  // 0 none, 1 triangle (msoArrowheadNone / msoArrowheadTriangle)
  float fill_transparency = 0;       // 0 opaque .. 1 clear
  double margin_l = 7.2, margin_t = 3.6, margin_r = 7.2, margin_b = 3.6;  // text box margins (points)
  bool wrap = true, autofit = false;
  std::string alt_text;
  double crop_l = 0, crop_t = 0, crop_r = 0, crop_b = 0;  // pictures: points cut from each side of the original
  bool locked = true;          // Format > Properties > Locked: a protected sheet (without "Edit objects") can't change it
  bool lock_aspect = false;    // corner drags keep the proportions (pictures: on by default)
  double rotation = 0;         // degrees clockwise
  uint32_t group = 0;          // group id (0 = not grouped): members move and size together
  uint32_t on_chart = 0;       // Chart.Shapes: the chart (its id) the shape is drawn on: it moves and goes with it
  bool print_object = true;
  // the base size the scale (ScaleHeight / ScaleWidth, Format Shape > Size > Scale) is measured from
  double orig_w = 0, orig_h = 0;
  // a preset shape of Excel's Shapes gallery: its msoAutoShapeType (0 = drawn by kind); kind stays a plain one
  // (Rectangle, Line) for older versions. Saved in the shape record's element #12, keys 23-26.
  int auto_type = 0;
  int connector = 0;          // lines: 0 / 1 straight, 2 elbow, 3 curved (msoConnectorType)
  std::vector<double> path;   // a freeform (Curve, Freeform: Shape / Scribble): x0, y0, x1, y1... as fractions of its box
  bool path_closed = false;   // ... a closed (filled) shape
  bool path_smooth = false;   // ... a curve through the points
  // WordArt (Insert > WordArt): the letters' outline (ARGB, 0 none) and shadow; keys 27-28
  uint32_t text_outline = 0;
  bool text_shadow = false;
  // Format Shape > Effects (Shape Format > Shape Effects); saved in element #12, key 29 (a map of its own)
  ShapeEffects fx;
  // a preset shape's adjustments (Shape.Adjustments, its yellow handles; empty: Excel's defaults); key 29
  std::vector<double> adj;
  bool operator==(const ShapeObj&) const = default;
};

// ---- Slicers (PivotTable Analyze -> Insert Slicer): item buttons that filter a PivotTable by one field ----
struct Slicer {
  uint32_t id = 0;
  std::string name, caption;
  std::string pivot;  // the PivotTable's name
  int field = 0;      // its source field
  double left = 0, top = 0, width = 144, height = 190;  // points
  int columns = 1;
  int z = 0;
  bool timeline = false;  // a timeline (Insert -> Timeline): a date field filtered by periods
  int level = 2;          // timeline periods: 0 years, 1 quarters, 2 months, 3 days
  // Slicer Settings (saved together as element 11 of the slicer record, SlicerFlags)
  bool show_header = true;       // Display header
  bool descending = false;       // Item Sorting and Filtering: Descending (Z to A)
  bool hide_no_data = false;     // Hide items with no data
  bool indicate_no_data = true;  // Visually indicate items with no data
  bool no_data_last = true;      // Show items with no data last
  // Report Connections: more PivotTables (of the same source) the slicer filters by the field of the same name
  std::vector<std::string> more_pivots;
  std::string table;  // a table's slicer (Insert > Slicer with a table selected): it filters the table's column `field`
  // timelines (Timeline tab > Show): bits 1 no selection label, 2 no scroll bar, 4 no time level
  int tl_hide = 0;
  bool operator==(const Slicer&) const = default;
};
// the Slicer Settings as one number (0 = Excel's defaults) and back
inline int SlicerFlags(const Slicer& s) {
  return (s.show_header ? 0 : 1) | (s.descending ? 2 : 0) | (s.hide_no_data ? 4 : 0) | (s.indicate_no_data ? 0 : 8) | (s.no_data_last ? 0 : 16);
}
inline void SetSlicerFlags(Slicer& s, int f) {
  s.show_header = !(f & 1);
  s.descending = (f & 2) != 0;
  s.hide_no_data = (f & 4) != 0;
  s.indicate_no_data = !(f & 8);
  s.no_data_last = !(f & 16);
}

// ---- Sparklines (Insert -> Sparklines) ----
struct Sparkline {
  uint32_t r = 0, c = 0;   // the cell it is drawn in
  uint32_t src_sheet = 0;  // its data
  core::Rect src{};
  bool operator==(const Sparkline&) const = default;
};

struct SparklineGroup {
  uint32_t id = 0;
  int type = 0;  // 0 line, 1 column, 2 win / loss
  std::vector<Sparkline> items;
  uint32_t color = 0xFF376092, negative_color = 0xFFD00000, marker_color = 0xFFD00000;
  uint32_t high_color = 0xFFD00000, low_color = 0xFFD00000, first_color = 0xFFD00000, last_color = 0xFFD00000;
  bool high = false, low = false, first = false, last = false, negative = false, markers = false, axis = false;
  float line_weight = 0.75f;
  // Hidden & Empty Cells: empty cells as gaps (1, xlNotPlotted), zero (2) or joined by the line (3, xlInterpolated);
  // the data of hidden rows and columns shown (DisplayHidden)
  int display_blanks = 1;
  bool show_hidden = false;
  // the vertical axis' minimum / maximum: each sparkline's own (2, xlSparkScaleSingle), the same for all the group's
  // (1, xlSparkScaleGroup) or a custom value (3, xlSparkScaleCustom)
  int min_scale = 2, max_scale = 2;
  double min_value = 0, max_value = 0;
  bool right_to_left = false;  // Plot Data Right-to-Left
  std::string date_range;      // Date Axis Type: the range of dates that places the points ("" = General Axis Type)
  uint32_t axis_color = 0xFF000000;
  // (saved as the group record's element #7, a map)
  bool operator==(const SparklineGroup&) const = default;
};

// the texts of a sparkline group that refer to cells (its date axis' range; its data and cells are rectangles that
// move by themselves)
template <class Fn>
void ForEachSparklineRefText(SparklineGroup& g, Fn&& fn) {
  if (!g.date_range.empty()) fn(g.date_range);
}

// ---- Custom panels (PLAN §10.3) ----
struct PanelControl {
  std::string type;  // label, button, textbox, combobox, checkbox, slider, chart, separator, group, listbox, option, spin
  std::string name, caption, linked_cell, on_action, list_fill_range, chart;
  std::vector<std::string> items;
  std::string value;
  double min = 0, max = 100;
  bool enabled = true, visible = true;
  // UserForms: the position and size (points; left < 0 = flow layout) and the option button group
  double left = -1, top = 0, width = 0, height = 0;
  std::string group;
  // styling (panels): colors as Excel's RGB numbers (-1 = the theme's), font size in points (0 = default), bold,
  // the number format of a label's linked value ("[Red]#,##0;[Green]-#,##0" colors it by sign)
  int64_t back_color = -1, fore_color = -1;
  double font_size = 0;
  bool bold = false;
  std::string number_format;
  double small_change = 1;  // spin / slider: the step of one click (a strike width of 100, lots of 1)
  bool operator==(const PanelControl&) const = default;
};

struct Panel {
  std::string name, caption;
  std::string dock = "right";  // left, right, top, bottom, float
  double width = 320, height = 240;
  bool visible = true;
  std::vector<PanelControl> controls;
  // [YC] the panel's own colors (Excel RGB numbers; -1 = the theme's): a dark trading panel beside a dark chart
  int64_t back_color = -1, fore_color = -1;
  // a UserForm shown from Python: a dialog with controls at their own positions; modal blocks the rest of the window.
  // Forms live while Python shows them and aren't saved with the workbook.
  bool form = false, modal = false;
  bool operator==(const Panel&) const = default;
};

// ---- Defined names ----
struct NameDef {
  std::string name;
  std::string refers_to;  // formula text (without =), e.g. "Quotes!$B$2:$B$100"
  int32_t sheet_scope = -1;  // -1 = workbook
  std::string comment;       // comment (Name Manager)
  bool hidden = false;       // Name.Visible = False
  bool operator==(const NameDef&) const = default;
};

// ---- Notes ----
// a reply in a threaded comment
struct NoteReply {
  std::string author, text;
  double date = 0;  // seconds since 1970 (UTC)
  bool operator==(const NoteReply&) const = default;
};

// a note (Excel's legacy comment) or, with threaded set, a threaded comment: the first post plus replies
struct CellNote {
  std::string author, text;
  bool threaded = false;
  double date = 0;  // threaded: when the first post was written (seconds since 1970, UTC)
  bool resolved = false;
  std::vector<NoteReply> replies;
  bool visible = false;  // a note shown all the time (Review > Show/Hide Note, Comment.Visible); saved in sheet key 68
  // the note's box (its handles and Format Comment): size and place in points (w 0: Excel's default box beside the
  // cell; dx / dy move it from there), font, alignment, fill and line (ARGB, 0: Excel's black text, light yellow fill,
  // gray line); saved as element 8 of the notes record
  struct Look {
    double w = 0, h = 0, dx = 0, dy = 0;
    std::string font_name;
    float font_size = 0;
    bool bold = false, italic = false;
    uint32_t font_color = 0, fill_color = 0, line_color = 0;
    int halign = 0, valign = 0;  // 0 left / top, 1 center, 2 right / bottom
    bool operator==(const Look&) const = default;
  } look;
  bool operator==(const CellNote&) const = default;
};

// ---- Hyperlinks (the cell's displayed text is the cell value) ----
struct Hyperlink {
  std::string address;      // web address, file path, mailto:; empty means a place in the workbook
  std::string sub_address;  // place in the workbook, e.g. "Sheet2!A1" or a defined name
  std::string tip;          // ScreenTip
  bool operator==(const Hyperlink&) const = default;
};

}  // namespace yc::model
