// SPDX-License-Identifier: Apache-2.0
// Chart types and their options as Excel's: pie of pie / bar of pie, the four stock
// charts' elements, 3-D floor and walls, the date axis, trendlines (several, a set intercept), X error bars, treemap /
// box and whisker / map options, the axis titles' places and Format Chart Area > Properties; all saved with the chart
#include "engine/chart_layout.h"
#include "engine/document.h"
#include "formula/datetime.h"
#include "io/ycg.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

using yc::model::ChartType;
namespace eng = yc::engine;
namespace model = yc::model;

namespace {

model::ChartObj RoundTrip(const model::ChartObj& ch) {
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  d.AddChart(d.wb().sheets[0]->id, ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets[0]->charts->size() == 1);
  return (*res.wb.sheets[0]->charts).at(0);
}

float Measure(const std::string& t, float sc) { return static_cast<float>(t.size()) * 6 * sc; }

eng::ChartValues ValuesOf(std::vector<double> ys) {
  eng::ChartValues v;
  eng::ChartSeriesValues b;
  b.y = std::move(ys);
  for (size_t i = 0; i < b.y.size(); ++i) b.x.push_back(static_cast<double>(i + 1));
  b.name = "S";
  v.series = {b};
  return v;
}

}  // namespace

TEST_CASE("Round 3 charts: the chart types' options are saved and read back") {
  model::ChartObj ch;
  ch.type = ChartType::BarOfPie;
  ch.split_type = 4;
  ch.split_value = 2.5;
  ch.second_size = 60;
  ch.pie_gap = 150;
  ch.second_points = {1, 4};
  ch.up_down_bars = false;
  ch.hi_lo_lines = false;
  ch.up_fill = 0xFF0000FF;
  ch.down_fill = 0xFF00FF00;
  ch.hilo_line = 0xFF123456;
  ch.updown_gap = 80;
  ch.floor_fill = 0xFFAABBCC;
  ch.wall_fill = 0xFFDDEEFF;
  ch.cat_type = 2;
  ch.base_unit = 1;
  ch.major_unit_scale = 2;
  ch.cat_title_rect = {true, 0.1f, 0.8f, 0.2f, 0.05f};
  ch.val_title_rect = {true, 0.01f, 0.3f, 0.05f, 0.3f};
  ch.val2_title_rect = {true, 0.9f, 0.3f, 0.05f, 0.3f};
  ch.print_object = false;
  ch.locked = false;
  model::ChartSeries s;
  s.values = "Sheet!$B$2:$B$6";
  s.trend = model::TrendKind::Linear;
  s.trend_name = "Fit";
  s.trend_icept_on = true;
  s.trend_icept = 3.5;
  model::ChartTrendline t;
  t.kind = model::TrendKind::MovingAverage;
  t.period = 3;
  t.name = "MA";
  t.forward = 1;
  s.more_trends = {t};
  s.err_cap = false;
  s.xerr = model::ErrorBarKind::Custom;
  s.xerr_amount = 2;
  s.xerr_plus = "{1,2}";
  s.xerr_minus = "Sheet!$E$2:$E$6";
  s.xerr_include = 2;
  s.parent_labels = 2;
  s.inclusive_median = true;
  s.geo_projection = 4;
  s.geo_area = 7;
  s.geo_labels = 2;
  s.geo_colors = 1;
  s.geo_min = 0x0000FF;
  s.geo_mid = 0x00FFFF;
  s.geo_max = 0x00FF00;
  s.label_dx = 0.25f;
  s.label_dy = -0.125f;
  ch.series = {s};
  const model::ChartObj c = RoundTrip(ch);
  CHECK(c.type == ChartType::BarOfPie);
  CHECK(c.split_type == 4);
  CHECK(c.split_value == 2.5);
  CHECK(c.second_size == 60);
  CHECK(c.pie_gap == 150);
  CHECK(c.second_points == std::vector<int>{1, 4});
  CHECK_FALSE(c.up_down_bars);
  CHECK_FALSE(c.hi_lo_lines);
  CHECK(c.up_fill == 0xFF0000FFu);
  CHECK(c.down_fill == 0xFF00FF00u);
  CHECK(c.hilo_line == 0xFF123456u);
  CHECK(c.updown_gap == 80);
  CHECK(c.floor_fill == 0xFFAABBCCu);
  CHECK(c.wall_fill == 0xFFDDEEFFu);
  CHECK(c.cat_type == 2);
  CHECK(c.base_unit == 1);
  CHECK(c.major_unit_scale == 2);
  CHECK(c.cat_title_rect == ch.cat_title_rect);
  CHECK(c.val_title_rect == ch.val_title_rect);
  CHECK(c.val2_title_rect == ch.val2_title_rect);
  CHECK_FALSE(c.print_object);
  CHECK_FALSE(c.locked);
  REQUIRE(c.series.size() == 1);
  const model::ChartSeries& r = c.series[0];
  CHECK(r.trend_name == "Fit");
  CHECK(r.trend_icept_on);
  CHECK(r.trend_icept == 3.5);
  REQUIRE(r.more_trends.size() == 1);
  CHECK(r.more_trends[0] == t);
  CHECK_FALSE(r.err_cap);
  CHECK(r.xerr == model::ErrorBarKind::Custom);
  CHECK(r.xerr_amount == 2);
  CHECK(r.xerr_plus == "{1,2}");
  CHECK(r.xerr_minus == "Sheet!$E$2:$E$6");
  CHECK(r.xerr_include == 2);
  CHECK(r.parent_labels == 2);
  CHECK(r.inclusive_median);
  CHECK(r.geo_projection == 4);
  CHECK(r.geo_area == 7);
  CHECK(r.geo_labels == 2);
  CHECK(r.geo_colors == 1);
  CHECK(r.geo_min == 0x0000FFu);
  CHECK(r.geo_mid == 0x00FFFFu);
  CHECK(r.geo_max == 0x00FF00u);
  CHECK(r.label_dx == 0.25f);
  CHECK(r.label_dy == -0.125f);
  // a chart saved without them reads Excel's defaults
  const model::ChartObj d = RoundTrip(model::ChartObj{});
  CHECK(d.split_type == 1);
  CHECK(d.second_size == 75);
  CHECK(d.pie_gap == 100);
  CHECK(d.up_down_bars);
  CHECK(d.hi_lo_lines);
  CHECK(d.updown_gap == 150);
  CHECK(d.cat_type == 0);
  CHECK(d.base_unit == -1);
  CHECK(d.print_object);
  CHECK(d.locked);
}

TEST_CASE("Round 3 charts: the new types are stock and scene types as Excel's") {
  CHECK(eng::IsStockType(ChartType::StockVHLC));
  CHECK(eng::IsStockType(ChartType::StockVOHLC));
  CHECK(eng::IsSceneChart(ChartType::PieOfPie));
  CHECK(eng::IsSceneChart(ChartType::BarOfPie));
  CHECK(model::kChartTypeCount == static_cast<int>(ChartType::StockVOHLC) + 1);
  CHECK(model::kChartElemLast >= static_cast<int>(model::ChartElem::Walls));
}

TEST_CASE("Pie of pie: the second plot takes the last points, the values under a limit, or the ones picked") {
  model::ChartObj ch;
  ch.type = ChartType::PieOfPie;
  const double ys[] = {40, 30, 10, 8, 7, 5};
  auto second = [&]() {
    const std::vector<char> v = eng::SecondPlotPoints(ch, ys, 6);
    std::vector<int> out;
    for (size_t k = 0; k < v.size(); ++k) {
      if (v[k]) out.push_back(static_cast<int>(k));
    }
    return out;
  };
  CHECK(second() == std::vector<int>{3, 4, 5});  // by position: the last 3 (Excel's default)
  ch.split_value = 2;
  CHECK(second() == std::vector<int>{4, 5});
  ch.split_type = 2;  // by value: less than 9
  ch.split_value = 9;
  CHECK(second() == std::vector<int>{3, 4, 5});
  ch.split_type = 3;  // by percentage: less than 10% of 100
  ch.split_value = 10;
  CHECK(second() == std::vector<int>{3, 4, 5});
  ch.split_type = 4;  // custom
  ch.second_points = {0, 2};
  CHECK(second() == std::vector<int>{0, 2});
  // the scene: a wedge per first-plot point, "Other" (point n), and the second plot's points, each its own point
  ch.split_type = 1;
  ch.split_value = 0;
  model::ChartSeries s;
  s.values = "x";
  ch.series = {s};
  eng::ChartValues v = ValuesOf({40, 30, 10, 8, 7, 5});
  eng::ChartScene sc;
  eng::BuildChartScene(ch, v, 500, 300, 12, Measure, &sc);
  std::set<int> pts;
  int bars = 0;
  for (const auto& p : sc.prims) {
    if (p.elem == static_cast<uint8_t>(model::ChartElem::Series) && p.point >= 0 && (p.kind == eng::ChartPrim::Poly || p.kind == eng::ChartPrim::Rect)) pts.insert(p.point);
  }
  CHECK(pts == std::set<int>{0, 1, 2, 3, 4, 5, 6});  // 6 = "Other"
  ch.type = ChartType::BarOfPie;
  eng::BuildChartScene(ch, v, 500, 300, 12, Measure, &sc);
  for (const auto& p : sc.prims) bars += p.kind == eng::ChartPrim::Rect && p.point >= 3 && p.point <= 5 ? 1 : 0;
  CHECK(bars == 3);  // the second plot is a stacked bar
}

TEST_CASE("Box and whisker: inclusive median quartiles and the mean line") {
  const eng::BoxStats ex = eng::BoxWhisker({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  const eng::BoxStats in = eng::BoxWhisker({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, true);
  CHECK(ex.q1 == Catch::Approx(2.75));  // QUARTILE.EXC
  CHECK(in.q1 == Catch::Approx(3.25));  // QUARTILE.INC
  CHECK(in.q3 == Catch::Approx(7.75));
  model::ChartObj ch;
  ch.type = ChartType::BoxWhisker;
  ch.has_legend = false;
  model::ChartSeries s;
  s.values = "x";
  s.box_flags = 6 | 8;
  ch.series = {s};
  eng::ChartValues v = ValuesOf({1, 2, 3, 10, 20, 30});
  v.cats = {"A", "A", "A", "B", "B", "B"};
  eng::ChartScene sc;
  auto mean_lines = [&]() {
    int n = 0;
    for (const auto& p : sc.prims) n += p.kind == eng::ChartPrim::Line && p.elem == 7 && p.width == 1.5f && p.pts.size() == 4 && p.pts[0] != p.pts[2] && std::fabs((p.pts[3] - p.pts[1])) > 1 ? 1 : 0;
    return n;
  };
  eng::BuildChartScene(ch, v, 400, 300, 12, Measure, &sc);
  const int with = mean_lines();
  ch.series[0].box_flags = 6;
  eng::BuildChartScene(ch, v, 400, 300, 12, Measure, &sc);
  CHECK(with == mean_lines() + 1);  // one line from box A's mean to box B's
}

TEST_CASE("Treemap Label Options: banners, overlapping names or none") {
  model::ChartObj ch;
  ch.type = ChartType::Treemap;
  ch.has_legend = false;
  model::ChartSeries s;
  s.values = "x";
  ch.series = {s};
  eng::ChartValues v = ValuesOf({5, 3, 4, 2});
  v.paths = {{"Fruit", "Apple"}, {"Fruit", "Pear"}, {"Veg", "Kale"}, {"Veg", "Leek"}};
  v.cats = {"Apple", "Pear", "Kale", "Leek"};
  auto texts = [&](const char* t) {
    eng::ChartScene sc;
    eng::BuildChartScene(ch, v, 400, 300, 12, Measure, &sc);
    int n = 0;
    float scale = 0;
    for (const auto& p : sc.prims) {
      if (p.kind == eng::ChartPrim::Text && p.text == t) ++n, scale = p.scale;
    }
    return std::make_pair(n, scale);
  };
  CHECK(texts("Fruit").first == 1);  // a banner (Excel's default)
  ch.series[0].parent_labels = 0;
  CHECK(texts("Fruit").first == 0);
  ch.series[0].parent_labels = 2;
  const auto ov = texts("Fruit");
  CHECK(ov.first == 1);
  CHECK(ov.second > 1.0f);  // over the rectangles, larger
}

TEST_CASE("Trendlines: Set Intercept fixes the constant; the equation drops a zero constant") {
  const double xs[] = {1, 2, 3, 4};
  const double ys[] = {3, 5, 7, 9};  // y = 2x + 1
  const eng::TrendFit free = eng::FitTrend(xs, ys, 4, model::TrendKind::Linear, 2);
  REQUIRE(free.ok);
  CHECK(free.c[0] == Catch::Approx(1));
  const double zero = 0;
  const eng::TrendFit fixed = eng::FitTrend(xs, ys, 4, model::TrendKind::Linear, 2, &zero);
  REQUIRE(fixed.ok);
  CHECK(fixed.c[0] == 0);
  CHECK(fixed.c[1] == Catch::Approx(2.3333333).epsilon(1e-6));  // through the origin: sum(xy) / sum(x^2) = 70 / 30
  CHECK(fixed.Equation().find('+') == std::string::npos);
  const double two = 2;
  const double ey[] = {2 * std::exp(0.5), 2 * std::exp(1.0), 2 * std::exp(1.5), 2 * std::exp(2.0)};
  const eng::TrendFit ex = eng::FitTrend(xs, ey, 4, model::TrendKind::Exponential, 2, &two);
  REQUIRE(ex.ok);
  CHECK(ex.c[0] == Catch::Approx(2));
  CHECK(ex.c[1] == Catch::Approx(0.5));
}

TEST_CASE("X error bars run along a scatter chart's X values") {
  model::ChartSeries d;
  d.xerr = model::ErrorBarKind::Fixed;
  d.xerr_amount = 0.5;
  d.xerr_include = 2;  // plus only
  eng::ChartSeriesValues b;
  b.x = {1, 2, 3};
  b.y = {10, 20, 30};
  b.numeric_x = true;
  std::vector<double> minus, plus;
  bool at_mean = false;
  double mean = 0;
  eng::ErrorExtentsX(d, b, 3, &minus, &plus, &at_mean, &mean);
  REQUIRE(plus.size() == 3);
  CHECK(plus[0] == 0.5);
  CHECK(minus[0] == 0);
  d.xerr = model::ErrorBarKind::Percent;
  d.xerr_amount = 10;
  d.xerr_include = 1;
  eng::ErrorExtentsX(d, b, 3, &minus, &plus, &at_mean, &mean);
  CHECK(plus[2] == Catch::Approx(0.3));  // 10% of x = 3, not of y = 30
}

TEST_CASE("3-D charts: the floor and the walls are elements with their own fills") {
  model::ChartObj ch;
  ch.type = ChartType::Column3D;
  ch.has_legend = false;
  model::ChartSeries s;
  s.values = "x";
  ch.series = {s};
  ch.floor_fill = 0xFFFF0000;  // ARGB red
  ch.wall_fill = 0xFF0000FF;   // ARGB blue
  eng::ChartValues v = ValuesOf({3, 5, 2});
  eng::ChartScene sc;
  eng::BuildChartScene(ch, v, 400, 300, 12, Measure, &sc);
  int floors = 0, walls = 0;
  for (const auto& p : sc.prims) {
    if (p.elem == static_cast<uint8_t>(model::ChartElem::Floor)) {
      ++floors;
      CHECK(p.fill == 0xFF0000FFu);  // ABGR of red
    }
    if (p.elem == static_cast<uint8_t>(model::ChartElem::Walls)) {
      ++walls;
      CHECK(p.fill == 0xFFFF0000u);  // ABGR of blue
    }
  }
  CHECK(floors == 1);
  CHECK(walls == 2);
}

TEST_CASE("Filled map: projections, the area shown and the diverging colors change the drawing") {
  model::ChartObj ch;
  ch.type = ChartType::FilledMap;
  ch.has_legend = false;
  model::ChartSeries s;
  s.values = "x";
  ch.series = {s};
  eng::ChartValues v = ValuesOf({10, 20, 30});
  v.cats = {"France", "Germany", "Spain"};
  auto bounds = [&]() {
    eng::ChartScene sc;
    eng::BuildChartScene(ch, v, 400, 300, 12, Measure, &sc);
    float x0 = 1e9f, x1 = -1e9f;
    std::set<uint32_t> fills;
    for (const auto& p : sc.prims) {
      if (p.kind != eng::ChartPrim::Poly || p.elem != 7) continue;
      fills.insert(p.fill);
      for (size_t i = 0; i + 1 < p.pts.size(); i += 2) x0 = std::min(x0, p.pts[i]), x1 = std::max(x1, p.pts[i]);
    }
    return std::make_tuple(x0, x1, fills);
  };
  const auto miller = bounds();
  ch.series[0].geo_projection = 4;  // Robinson
  const auto robinson = bounds();
  CHECK(std::get<0>(robinson) != std::get<0>(miller));
  ch.series[0].geo_projection = 0;
  ch.series[0].geo_area = 7;  // the world: the countries get much smaller
  const auto world = bounds();
  CHECK(std::get<1>(world) - std::get<0>(world) < (std::get<1>(miller) - std::get<0>(miller)) * 0.5f);
  ch.series[0].geo_area = 0;
  ch.series[0].geo_colors = 1;  // diverging: the middle value is yellow
  const auto div = bounds();
  CHECK(std::get<2>(div).count(0xFF84EBFFu) == 1);  // ABGR of Excel's yellow midpoint
}

TEST_CASE("Date axis: stock charts too (weekend gaps), the base unit from the dates, a text axis on request") {
  model::ChartObj ch;
  ch.type = ChartType::StockOHLC;
  eng::ChartValues v;
  v.cat_vals = {46000, 46001, 46004, 46005};  // Thursday, Friday, Monday, Tuesday
  v.cat_dates = true;
  eng::DatePlan p = eng::PlanDateAxis(ch, v, 4);
  REQUIRE(p.on);
  CHECK(p.unit == 0);
  CHECK(p.pos == std::vector<double>{1, 2, 5, 6});  // the weekend kept as a gap, as Excel's Automatic does
  CHECK(p.u1 - p.u0 + 1 == 6);
  ch.cat_type = 1;  // Text axis: one place per row
  CHECK_FALSE(eng::PlanDateAxis(ch, v, 4).on);
  ch.cat_type = 0;
  ch.type = ChartType::ColumnClustered;
  v.cat_vals = {yc::formula::DateSerial(2026, 1, 15), yc::formula::DateSerial(2026, 2, 15), yc::formula::DateSerial(2026, 5, 15)};
  p = eng::PlanDateAxis(ch, v, 3);
  REQUIRE(p.on);
  CHECK(p.unit == 1);  // a month or more apart: months
  CHECK(p.pos == std::vector<double>{1, 2, 5});
  v.cat_dates = false;  // numbers that aren't dates: a text axis
  CHECK_FALSE(eng::PlanDateAxis(ch, v, 3).on);
  ch.cat_type = 2;  // Date axis chosen: the numbers taken as dates
  CHECK(eng::PlanDateAxis(ch, v, 3).on);
  CHECK(eng::UnitSerial(eng::DateUnit(yc::formula::DateSerial(2026, 5, 15), 1), 1) == yc::formula::DateSerial(2026, 5, 1));
}

TEST_CASE("Chart Styles: a set per kind of chart, numbered as Excel's, each with its own looks") {
  CHECK(eng::ChartStyleCount(ChartType::ColumnClustered) == 14);
  CHECK(eng::ChartStyleCount(ChartType::BarClustered) == 13);
  CHECK(eng::ChartStyleCount(ChartType::Line) == 15);
  CHECK(eng::ChartStyleCount(ChartType::Pie) == 12);
  CHECK(eng::ChartStyleCount(ChartType::Doughnut) == 10);
  CHECK(eng::ChartStyleCount(ChartType::Area) == 11);
  CHECK(eng::ChartStyleCount(ChartType::Scatter) == 15);
  CHECK(eng::ChartStyleCount(ChartType::Waterfall) == 8);
  CHECK(eng::ChartStyleCount(ChartType::Line) <= eng::kChartStyles);
  CHECK(eng::ChartStyleNumber(ChartType::ColumnClustered) == 201);
  CHECK(eng::ChartStyleNumber(ChartType::Line) == 227);
  CHECK(eng::ChartStyleNumber(ChartType::Pie) == 251);
  model::ChartObj line;
  line.type = ChartType::Line;
  line.series.resize(2);
  eng::ApplyChartStyle(line, 3);  // thick lines
  CHECK(line.series[0].line_weight == 3.5f);
  eng::ApplyChartStyle(line, 7);  // triangle markers
  CHECK(line.series[1].marker == 4);
  CHECK(line.series[1].line_weight == 0);
  eng::ApplyChartStyle(line, 99);
  CHECK(line.style == 15);
  model::ChartObj pie;
  pie.type = ChartType::Pie;
  pie.series.resize(1);
  eng::ApplyChartStyle(pie, 3);  // slices pulled out
  CHECK(pie.series[0].explosion == 10);
  eng::ApplyChartStyle(pie, 2);  // best-fit labels, no legend
  CHECK(pie.series[0].label_pos == 8);
  CHECK_FALSE(pie.has_legend);
  model::ChartObj col;
  col.type = ChartType::ColumnClustered;
  col.series.resize(2);
  eng::ApplyChartStyle(col, 10);  // gradient fills
  CHECK(col.series[0].fill_type == 1);
  eng::ApplyChartStyle(col, 9);  // outlined
  CHECK(col.series[1].no_fill);
  CHECK(col.series[1].line_color != 0);
  CHECK(eng::ChartStyleTip(ChartType::ColumnClustered, 10) != eng::ChartStyleTip(ChartType::ColumnClustered, 9));
}

TEST_CASE("charts that draw a mark a category lay out a million categories in a moment") {
  // scale_sweep: over whole columns a 3D line took 22 s to insert and every frame after it seconds, a radar 16 s
  using ChartType = model::ChartType;
  std::vector<double> ys(1000000);
  for (size_t i = 0; i < ys.size(); ++i) ys[i] = static_cast<double>(i % 97);
  eng::ChartValues v = ValuesOf(ys);
  for (size_t i = 0; i < ys.size(); ++i) v.cats.push_back("c" + std::to_string(i));
  for (ChartType t : {ChartType::Column3D, ChartType::Line3D, ChartType::Area3D, ChartType::BarClustered3D, ChartType::Funnel,
                      ChartType::Radar, ChartType::RadarFilled, ChartType::Surface}) {
    model::ChartObj ch;
    ch.type = t;
    model::ChartSeries se;
    se.values = "x";
    ch.series = {se};
    eng::ChartScene sc;
    const auto t0 = std::chrono::steady_clock::now();
    eng::BuildChartScene(ch, v, 500, 300, 12, Measure, &sc);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO("type " << static_cast<int>(t) << ", " << secs << " s, " << sc.prims.size() << " primitives");
#ifdef NDEBUG
    CHECK(secs < 1.5);
#endif
    CHECK(sc.prims.size() < 100000);
  }
}

TEST_CASE("a 3D area over whole columns of mostly empty cells has a front of few vertices") {
  // scale_sweep: Insert 3D Area over B:D (five values, then empty cells to row 1048576) took 73 s, its Undo never
  // answered: the front was one concave polygon of thousands of vertices on one line, cut into triangles every frame
  std::vector<double> ys(1000000, std::nan(""));
  for (int i = 0; i < 5; ++i) ys[static_cast<size_t>(i)] = 10.0 * (i + 1);
  eng::ChartValues v = ValuesOf(ys);
  model::ChartObj ch;
  ch.type = model::ChartType::Area3D;
  model::ChartSeries se;
  se.values = "x";
  ch.series = {se};
  eng::ChartScene sc;
  eng::BuildChartScene(ch, v, 500, 300, 12, Measure, &sc);
  size_t most = 0;
  for (const auto& p : sc.prims) {
    if (p.kind == eng::ChartPrim::Poly && p.fill) most = std::max(most, p.pts.size() / 2);
  }
  CHECK(most < 400);
}
