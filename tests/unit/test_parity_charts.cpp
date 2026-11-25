// SPDX-License-Identifier: Apache-2.0
// Charts working like Excel's: what is saved with a chart, histogram overflow / underflow bins,
// palettes, waterfall points and connectors, the chart type families, titles linked to cells in scenes
#include "core/i18n.h"
#include "engine/chart_layout.h"
#include "engine/document.h"
#include "io/ycg.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using yc::model::ChartType;
namespace eng = yc::engine;
namespace model = yc::model;

namespace {

model::ChartObj RoundTrip(const model::ChartObj& ch) {
  yc::engine::Document d(model::Workbook::NewDefault("Sheet"));
  d.AddChart(d.wb().sheets[0]->id, ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets[0]->charts->size() == 1);
  return (*res.wb.sheets[0]->charts).at(0);
}

}  // namespace

TEST_CASE("Charts: axis options, the secondary axis and each bound's Auto are saved") {
  model::ChartObj ch;
  ch.type = ChartType::LineStacked100;
  ch.val_axis.min_auto = true;
  ch.val_axis.max_auto = false;
  ch.val_axis.max = 2000;
  ch.val_axis.major = 250;
  ch.val_axis.minor = 50;
  ch.val_axis.log = true;
  ch.val_axis.log_base = 2;
  ch.val_axis.reverse = true;
  ch.val_axis.crosses = 1;
  ch.val_axis.crosses_at = 7.5;
  ch.val_axis.display_unit = 1000;
  ch.val_axis.major_tick = 0;
  ch.val_axis.minor_tick = 3;
  ch.val_axis.label_pos = 1;
  ch.val_axis.minor_gridlines = true;
  ch.cat_axis.between = false;
  ch.cat_axis.label_interval = 3;
  ch.cat_axis.reverse = true;
  ch.val_axis2.has_title = true;
  ch.val_axis2.title = "=Sheet!$C$1";
  ch.val_axis2.min_auto = false;
  ch.val_axis2.min = 100;
  ch.val_axis2.number_format = "0.0";
  const model::ChartObj c = RoundTrip(ch);
  CHECK(c.type == ChartType::LineStacked100);
  CHECK(c.val_axis == ch.val_axis);
  CHECK(c.cat_axis == ch.cat_axis);
  CHECK(c.val_axis2 == ch.val_axis2);
  CHECK(c.val_axis.min_auto);  // fixing the maximum leaves the minimum automatic
  CHECK_FALSE(c.val_axis.max_auto);
}

TEST_CASE("Charts: hidden and empty cells, overlays, colors, bins, connectors and the manual layout are saved") {
  model::ChartObj ch;
  ch.plot_visible_only = false;
  ch.display_blanks = 3;
  ch.na_as_blank = false;
  ch.title_overlay = true;
  ch.legend_overlay = true;
  ch.color_scheme = 5;
  ch.style = 2;
  ch.bins_overflow = true;
  ch.overflow_at = 20;
  ch.bins_underflow = true;
  ch.underflow_at = 2;
  ch.bins_by_category = true;
  ch.connectors = false;
  ch.lock_aspect = true;
  ch.plot_line = 0xFF112233;
  ch.legend_fill = 0xFF445566;
  ch.legend_line = 0xFF778899;
  ch.title_fill = 0xFFAABBCC;
  ch.title_line = 0xFFDDEEFF;
  ch.title_rect = {true, 0.1f, 0.05f, 0.3f, 0.1f};
  ch.legend_rect = {true, 0.7f, 0.2f, 0.2f, 0.3f};
  ch.plot_rect = {true, 0.05f, 0.15f, 0.6f, 0.7f};
  const model::ChartObj c = RoundTrip(ch);
  CHECK_FALSE(c.plot_visible_only);
  CHECK(c.display_blanks == 3);
  CHECK_FALSE(c.na_as_blank);
  CHECK(c.title_overlay);
  CHECK(c.legend_overlay);
  CHECK(c.color_scheme == 5);
  CHECK(c.style == 2);
  CHECK(c.bins_overflow);
  CHECK(c.overflow_at == 20);
  CHECK(c.bins_underflow);
  CHECK(c.underflow_at == 2);
  CHECK(c.bins_by_category);
  CHECK_FALSE(c.connectors);
  CHECK(c.lock_aspect);
  CHECK(c.plot_line == 0xFF112233);
  CHECK(c.legend_fill == 0xFF445566);
  CHECK(c.legend_line == 0xFF778899);
  CHECK(c.title_fill == 0xFFAABBCC);
  CHECK(c.title_line == 0xFFDDEEFF);
  CHECK(c.title_rect == ch.title_rect);
  CHECK(c.legend_rect == ch.legend_rect);
  CHECK(c.plot_rect == ch.plot_rect);
  // Excel's defaults: hidden cells left out, empty cells as gaps, #N/A as empty, connector lines shown
  const model::ChartObj fresh = RoundTrip(model::ChartObj{});
  CHECK(fresh.plot_visible_only);
  CHECK(fresh.display_blanks == 1);
  CHECK(fresh.na_as_blank);
  CHECK(fresh.connectors);
  CHECK(fresh.val_axis.min_auto);
  CHECK(fresh.val_axis.max_auto);
  CHECK_FALSE(fresh.title_rect.on);
}

TEST_CASE("Charts: points, data labels, explosion and fill & line of a series are saved") {
  model::ChartObj ch;
  ch.type = ChartType::Pie;
  model::ChartSeries s;
  s.values = "Sheet!$B$2:$B$5";
  s.points.push_back({2, 0x0000FF, 0x000000, 30, 1});
  s.points.push_back({0, 0, 0x00FF00, -1, 0});
  s.explosion = 10;
  s.vary_colors = false;
  s.label_pos = 3;
  s.label_show = 1 | 8 | 32;
  s.label_range = "Sheet!$C$2:$C$5";
  s.label_format = "0.0%";
  s.label_sep = "; ";
  s.bubble_sizes = "Sheet!$D$2:$D$5";
  s.line_color = 0x123456;
  s.no_fill = true;
  s.no_line = true;
  s.dash = 3;
  s.transparency = 40;
  s.invert_negative = true;
  s.invert_color = 0x00FFFF;
  s.marker_fill = 0x111111;
  s.marker_line = 0x222222;
  ch.series = {s};
  const model::ChartObj c = RoundTrip(ch);
  REQUIRE(c.series.size() == 1);
  const model::ChartSeries& r = c.series[0];
  CHECK(r.points == s.points);
  CHECK(r.explosion == 10);
  CHECK_FALSE(r.vary_colors);
  CHECK(r.label_pos == 3);
  CHECK(r.label_show == (1 | 8 | 32));
  CHECK(r.label_range == s.label_range);
  CHECK(r.label_format == "0.0%");
  CHECK(r.label_sep == "; ");
  CHECK(r.bubble_sizes == s.bubble_sizes);
  CHECK(r.line_color == 0x123456);
  CHECK(r.no_fill);
  CHECK(r.no_line);
  CHECK(r.dash == 3);
  CHECK(r.transparency == 40);
  CHECK(r.invert_negative);
  CHECK(r.invert_color == 0x00FFFF);
  CHECK(r.marker_fill == 0x111111);
  CHECK(r.marker_line == 0x222222);
  // a fresh series: labels show the value, slices vary their colors
  CHECK(model::ChartSeries{}.label_show == 4);
  CHECK(model::ChartSeries{}.vary_colors);
}

TEST_CASE("Histogram overflow and underflow bins take the values beyond them") {
  std::vector<double> v;
  for (int i = 1; i <= 19; ++i) v.push_back(i);
  v.push_back(1000);
  model::ChartObj ch;
  ch.bins_overflow = true;
  ch.overflow_at = 20;
  const auto h = eng::HistogramOf(v, ch);
  REQUIRE(!h.counts.empty());
  CHECK(h.labels.back() == ">20");
  CHECK(h.counts.back() == 1);
  double inside = 0;
  for (size_t i = 0; i + 1 < h.counts.size(); ++i) inside += h.counts[i];
  CHECK(inside == 19);
  CHECK(h.counts.size() > 3);  // the others spread over several bins, not one
  ch.bins_underflow = true;
  ch.underflow_at = 5;
  const auto u = eng::HistogramOf(v, ch);
  CHECK(u.labels.front() == "<=5");
  CHECK(u.counts.front() == 5);
  // without them: Excel's bins over every value
  CHECK(eng::HistogramOf(v, model::ChartObj{}).labels.back().find('>') == std::string::npos);
  // By Category: one bin per distinct value
  model::ChartObj bycat;
  bycat.bins_by_category = true;
  const auto c = eng::HistogramOf({3, 1, 3, 2, 3}, bycat);
  REQUIRE(c.counts.size() == 3);
  CHECK(c.labels[2] == "3");
  CHECK(c.counts[2] == 3);
}

TEST_CASE("Change Colors: palettes in other orders and shades of one accent") {
  const uint32_t c0 = eng::ChartPalette(1);
  uint32_t mono0 = 0, mono1 = 0, other = 0;
  {
    const eng::PaletteScope p(1);  // Colorful Palette 2: accent 1, 3, 5...
    other = eng::ChartPalette(1);
  }
  {
    const eng::PaletteScope p(4);  // Monochromatic Palette 1: shades of accent 1
    mono0 = eng::ChartPalette(0);
    mono1 = eng::ChartPalette(1);
  }
  CHECK(other != c0);
  CHECK(mono0 == eng::ChartPalette(0));  // the first shade is the accent itself
  CHECK(mono1 != mono0);
  CHECK(eng::ChartPalette(1) == c0);  // the scope ended: the theme's palette again
  // the names are UI text: read in English (the machine running the tests may show another language)
  const yc::i18n::Lang lang = yc::i18n::Current();
  yc::i18n::SetCurrent(yc::i18n::Lang::En);
  CHECK(std::string(eng::ChartColorSchemeName(4)).find("Monochromatic") != std::string::npos);
  yc::i18n::SetCurrent(lang);
}

TEST_CASE("Waterfall columns are points of their own; connector lines can be hidden") {
  model::ChartObj ch;
  ch.type = ChartType::Waterfall;
  ch.has_legend = false;
  model::ChartSeries s;
  s.values = "x";
  s.points.push_back({4, 0x0000FF, 0, -1, -1});  // the last column red
  ch.series = {s};
  eng::ChartValues v;
  eng::ChartSeriesValues b;
  b.y = {10, -3, 4, 2, 13};
  b.x = {1, 2, 3, 4, 5};
  v.series = {b};
  auto measure = [](const std::string& t, float sc) { return static_cast<float>(t.size()) * 6 * sc; };
  eng::ChartScene sc;
  eng::BuildChartScene(ch, v, 400, 300, 12, measure, &sc);
  int columns = 0, lines = 0;
  uint32_t last_fill = 0;
  for (const auto& p : sc.prims) {
    if (p.kind == eng::ChartPrim::Rect && p.point >= 0) {
      ++columns;
      if (p.point == 4) last_fill = p.fill;
    }
    if (p.kind == eng::ChartPrim::Line && p.elem == 7) ++lines;
  }
  CHECK(columns == 5);
  CHECK(last_fill == 0xFF0000FFu);  // ABGR of the point's red
  CHECK(lines == 4);
  ch.connectors = false;
  eng::BuildChartScene(ch, v, 400, 300, 12, measure, &sc);
  lines = 0;
  for (const auto& p : sc.prims) lines += p.kind == eng::ChartPrim::Line && p.elem == 7 ? 1 : 0;
  CHECK(lines == 0);
}

TEST_CASE("Chart type families: stacked, 100%, bars, lines, areas, scatter and stock") {
  CHECK(eng::IsBarType(ChartType::ColumnStacked100));
  CHECK(eng::IsBarType(ChartType::BarStacked));
  CHECK(eng::IsHorizontalBar(ChartType::BarStacked100));
  CHECK_FALSE(eng::IsHorizontalBar(ChartType::ColumnStacked));
  CHECK(eng::IsStackedType(ChartType::AreaStacked));
  CHECK(eng::IsPercentType(ChartType::LineMarkersStacked100));
  CHECK_FALSE(eng::IsPercentType(ChartType::LineStacked));
  CHECK(eng::IsLineType(ChartType::LineMarkersStacked));
  CHECK(eng::IsAreaType(ChartType::AreaStacked100));
  CHECK(eng::IsScatterType(ChartType::Bubble));
  CHECK(eng::IsScatterType(ChartType::ScatterSmoothNoMarkers));
  CHECK(eng::HasMarkers(ChartType::ScatterLines));
  CHECK_FALSE(eng::HasMarkers(ChartType::ScatterLinesNoMarkers));
  CHECK(eng::IsStockType(ChartType::StockHLC));
  CHECK_FALSE(eng::IsSceneChart(ChartType::Bubble));
  CHECK(model::kChartTypeCount == static_cast<int>(ChartType::StockVOHLC) + 1);
}

TEST_CASE("Scene charts show the title as resolved (a linked cell's text), and its overlay keeps the plot's room") {
  model::ChartObj ch;
  ch.type = ChartType::Funnel;
  ch.has_title = true;
  ch.title = "=Sheet!$B$1";
  ch.has_legend = false;
  eng::ChartValues v;
  eng::ChartSeriesValues b;
  b.y = {5, 3, 1};
  b.x = {1, 2, 3};
  v.series = {b};
  v.title = "Sales";
  auto measure = [](const std::string& t, float sc) { return static_cast<float>(t.size()) * 6 * sc; };
  eng::ChartScene sc;
  eng::BuildChartScene(ch, v, 400, 300, 12, measure, &sc);
  bool shown = false;
  for (const auto& p : sc.prims) shown = shown || (p.kind == eng::ChartPrim::Text && p.text == "Sales" && p.elem == 3);
  CHECK(shown);
}
