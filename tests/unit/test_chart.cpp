// SPDX-License-Identifier: Apache-2.0
// Chart calculations (histogram bins, box & whisker, waterfall, trendlines, treemap / sunburst layout) and saving the chart options
#include "core/i18n.h"
#include "engine/chart_layout.h"
#include "engine/geo.h"
#include "engine/document.h"
#include "io/ycg.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using yc::model::ChartType;
using yc::model::ErrorBarKind;
using yc::model::TrendKind;
namespace eng = yc::engine;

TEST_CASE("Histogram bins: Excel's [first] and (later] intervals") {
  const auto h = eng::Histogram({1, 2, 2, 3, 4, 5, 6, 7, 8, 9}, 2.0, 0);
  REQUIRE(h.counts.size() == 4);  // [1,3] (3,5] (5,7] (7,9]
  CHECK(h.counts[0] == 4);        // 1 2 2 3
  CHECK(h.counts[1] == 2);        // 4 5
  CHECK(h.counts[2] == 2);        // 6 7
  CHECK(h.counts[3] == 2);        // 8 9
  CHECK(h.labels[0] == "[1, 3]");
  CHECK(h.labels[1] == "(3, 5]");
  const auto byc = eng::Histogram({0, 10}, 0, 5);
  REQUIRE(byc.counts.size() == 5);
  CHECK(byc.counts[0] == 1);
  CHECK(byc.counts[4] == 1);
  // automatic (Scott's rule): every value lands in some bin
  const auto a = eng::Histogram({3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5}, 0, 0);
  double total = 0;
  for (double c : a.counts) total += c;
  CHECK(total == 11);
  CHECK(eng::Histogram({}, 0, 0).counts.empty());
}

TEST_CASE("Box and whisker statistics use exclusive quartiles") {
  const auto b = eng::BoxWhisker({1, 2, 3, 4, 5, 6, 7, 8, 9, 100});
  REQUIRE(b.ok);
  CHECK(b.median == Approx(5.5));
  CHECK(b.q1 == Approx(2.75));  // QUARTILE.EXC(…, 1)
  CHECK(b.q3 == Approx(8.25));
  CHECK(b.mean == Approx(14.5));
  REQUIRE(b.outliers.size() == 1);
  CHECK(b.outliers[0] == 100);
  CHECK(b.hi == 9);
  CHECK(b.lo == 1);
  CHECK_FALSE(eng::BoxWhisker({}).ok);
}

TEST_CASE("Waterfall bars run from the previous total; totals start at zero") {
  const auto w = eng::Waterfall({100, 30, -50, 80}, {3});
  REQUIRE(w.size() == 4);
  CHECK(w[0].start == 0);
  CHECK(w[0].end == 100);
  CHECK(w[1].start == 100);
  CHECK(w[1].end == 130);
  CHECK(w[2].kind == 1);
  CHECK(w[2].end == 80);
  CHECK(w[3].kind == 2);
  CHECK(w[3].start == 0);
  CHECK(w[3].end == 80);
}

TEST_CASE("Trendline fits match Excel's LINEST / LOGEST") {
  const double x[] = {1, 2, 3, 4, 5};
  const double y[] = {2.1, 3.9, 6.2, 7.8, 10.1};
  const auto lin = eng::FitTrend(x, y, 5, TrendKind::Linear, 2);
  REQUIRE(lin.ok);
  CHECK(lin.c[1] == Approx(1.99));
  CHECK(lin.c[0] == Approx(0.05));
  CHECK(lin.r2 == Approx(0.997305).epsilon(1e-4));
  CHECK(lin.Equation() == "y = 1.99x + 0.05");
  CHECK(lin.Eval(6) == Approx(11.99));
  // y = 2 e^(0.5x)
  double ye[5];
  for (int i = 0; i < 5; ++i) ye[i] = 2 * std::exp(0.5 * x[i]);
  const auto ex = eng::FitTrend(x, ye, 5, TrendKind::Exponential, 2);
  REQUIRE(ex.ok);
  CHECK(ex.c[0] == Approx(2));
  CHECK(ex.c[1] == Approx(0.5));
  CHECK(ex.r2 == Approx(1));
  // y = x^2 - 3x + 1
  double yp[5];
  for (int i = 0; i < 5; ++i) yp[i] = x[i] * x[i] - 3 * x[i] + 1;
  const auto po = eng::FitTrend(x, yp, 5, TrendKind::Polynomial, 2);
  REQUIRE(po.ok);
  CHECK(po.c[2] == Approx(1));
  CHECK(po.c[1] == Approx(-3));
  CHECK(po.Equation() == "y = 1x^2 - 3x + 1");
  // y = 3 x^1.5
  double yw[5];
  for (int i = 0; i < 5; ++i) yw[i] = 3 * std::pow(x[i], 1.5);
  const auto pw = eng::FitTrend(x, yw, 5, TrendKind::Power, 2);
  REQUIRE(pw.ok);
  CHECK(pw.c[0] == Approx(3));
  CHECK(pw.c[1] == Approx(1.5));
  // logarithmic needs x > 0, exponential y > 0
  const double xn[] = {-1, 1, 2};
  const double yn[] = {1, 2, 3};
  CHECK_FALSE(eng::FitTrend(xn, yn, 3, TrendKind::Logarithmic, 2).ok);
  const auto ma = eng::MovingAverage(y, 5, 2);
  CHECK(std::isnan(ma[0]));
  CHECK(ma[1] == Approx(3.0));
}

TEST_CASE("Error bar amounts") {
  const double y[] = {2, 4, 4, 4, 5, 5, 7, 9};
  bool at_mean = false;
  double mean = 0;
  auto e = eng::ErrorAmounts(ErrorBarKind::Percent, 10, y, 8, &at_mean, &mean);
  CHECK(e[0] == Approx(0.2));
  CHECK_FALSE(at_mean);
  e = eng::ErrorAmounts(ErrorBarKind::StDev, 1, y, 8, &at_mean, &mean);
  CHECK(at_mean);
  CHECK(mean == Approx(5));
  CHECK(e[3] == Approx(2.13809).epsilon(1e-4));  // STDEV.S
  e = eng::ErrorAmounts(ErrorBarKind::Fixed, 1.5, y, 8, &at_mean, &mean);
  CHECK(e[7] == 1.5);
}

TEST_CASE("Treemap rectangles fill the area in proportion to the values") {
  const std::vector<std::vector<std::string>> paths = {{"A", "a1"}, {"A", "a2"}, {"B", "b1"}, {"C"}};
  const std::vector<double> vals = {30, 10, 40, 20};
  const auto r = eng::TreemapLayout(paths, vals, 0, 0, 100, 100, 0);
  double leaf_area = 0;
  int parents = 0;
  for (const auto& t : r) {
    if (t.leaf) leaf_area += (t.x1 - t.x0) * (t.y1 - t.y0);
    else ++parents;
    if (t.leaf && t.label == "b1") CHECK((t.x1 - t.x0) * (t.y1 - t.y0) == Approx(4000));
    CHECK(t.x0 >= -1e-9);
    CHECK(t.y1 <= 100 + 1e-9);
  }
  CHECK(leaf_area == Approx(10000));
  CHECK(parents == 2);  // A and B have children; C is a leaf
}

TEST_CASE("Sunburst segments split the circle by value") {
  int levels = 0;
  const auto s = eng::SunburstLayout({{"A", "x"}, {"A", "y"}, {"B"}}, {1, 1, 2}, &levels);
  CHECK(levels == 2);
  double outer = 0;
  for (const auto& g : s) {
    if (g.level == 0) outer += g.a1 - g.a0;
    if (g.label == "B") CHECK(g.a1 - g.a0 == Approx(3.14159265));
  }
  CHECK(outer == Approx(2 * 3.14159265));
}

TEST_CASE("Chart scenes draw something for every new chart type") {
  yc::model::ChartObj ch;
  ch.has_legend = true;
  ch.has_title = true;
  ch.title = "T";
  eng::ChartValues v;
  eng::ChartSeriesValues a;
  a.name = "S1";
  a.y = {5, 3, 8, 1, 4};
  a.x = {1, 2, 3, 4, 5};
  v.series.push_back(a);
  v.cats = {"p", "q", "r", "s", "t"};
  auto measure = [](const std::string& t, float sc) { return static_cast<float>(t.size()) * 6.0f * sc; };
  for (ChartType t : {ChartType::Radar, ChartType::RadarMarkers, ChartType::RadarFilled, ChartType::Waterfall, ChartType::Histogram, ChartType::Pareto, ChartType::BoxWhisker,
                      ChartType::Treemap, ChartType::Sunburst, ChartType::Funnel}) {
    ch.type = t;
    CHECK(eng::IsSceneChart(t));
    eng::ChartScene sc;
    eng::BuildChartScene(ch, v, 400, 300, 14, measure, &sc);
    CHECK(sc.prims.size() > 5);
  }
  CHECK_FALSE(eng::IsSceneChart(ChartType::Line));
}

TEST_CASE("Combo series types only combine line, column, area and scatter") {
  yc::model::ChartObj ch;
  ch.type = ChartType::ColumnClustered;
  ch.series.resize(2);
  ch.series[1].type = static_cast<int>(ChartType::Line);
  CHECK(eng::EffectiveSeriesType(ch, 0) == ChartType::ColumnClustered);
  CHECK(eng::EffectiveSeriesType(ch, 1) == ChartType::Line);
  ch.series[1].type = static_cast<int>(ChartType::Pie);
  CHECK(eng::EffectiveSeriesType(ch, 1) == ChartType::ColumnClustered);
  ch.type = ChartType::Treemap;
  ch.series[1].type = static_cast<int>(ChartType::Line);
  CHECK(eng::EffectiveSeriesType(ch, 1) == ChartType::Treemap);
}

TEST_CASE("Chart types, trendlines, labels, error bars and bins are saved") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  yc::model::ChartObj ch;
  ch.type = ChartType::Histogram;
  ch.bin_width = 2.5;
  ch.bin_count = 7;
  yc::model::ChartSeries se;
  se.values = "Sheet!$A$1:$A$9";
  se.type = static_cast<int>(ChartType::Line);
  se.data_labels = true;
  se.trend = TrendKind::Polynomial;
  se.trend_order = 3;
  se.trend_period = 4;
  se.trend_forward = 2;
  se.trend_backward = 1;
  se.trend_eq = true;
  se.trend_r2 = true;
  se.err = ErrorBarKind::Percent;
  se.err_amount = 5;
  se.totals = {0, 4};
  ch.series.push_back(se);
  ch.series.push_back(yc::model::ChartSeries{});
  d.AddChart(s1, ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& c = (*res.wb.sheets[0]->charts).at(0);
  CHECK(c.type == ChartType::Histogram);
  CHECK(c.bin_width == 2.5);
  CHECK(c.bin_count == 7);
  REQUIRE(c.series.size() == 2);
  const auto& r = c.series[0];
  CHECK(r.type == static_cast<int>(ChartType::Line));
  CHECK(r.data_labels);
  CHECK(r.trend == TrendKind::Polynomial);
  CHECK(r.trend_order == 3);
  CHECK(r.trend_period == 4);
  CHECK(r.trend_forward == 2);
  CHECK(r.trend_backward == 1);
  CHECK(r.trend_eq);
  CHECK(r.trend_r2);
  CHECK(r.err == ErrorBarKind::Percent);
  CHECK(r.err_amount == 5);
  CHECK(r.totals == std::vector<int>{0, 4});
  CHECK(c.series[1].type == -1);
  CHECK(c.series[1].trend == TrendKind::None);
}

TEST_CASE("Chart element formatting is saved") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  yc::model::ChartObj ch;
  ch.title_font = {18, 0xFFC00000, true};
  ch.legend_font = {9, 0, false};
  ch.axis_font = {8, 0xFF404040, false};
  ch.legend_pos = 1;
  ch.chart_fill = 0xFFF2F2F2;
  ch.chart_line = 0xFF000000;
  ch.chart_line_none = true;
  ch.plot_fill = 0xFFFFFFE0;
  ch.cat_axis = {true, true, "Month", "", true};
  ch.val_axis = {false, true, "Sales", "#,##0", false};
  yc::model::ChartSeries se;
  se.values = "Sheet!$A$1:$A$9";
  se.line_weight = 3.5f;
  se.marker = 2;
  se.marker_size = 7;
  ch.series.push_back(se);
  d.AddChart(s1, ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& c = (*res.wb.sheets[0]->charts).at(0);
  CHECK(c.title_font == ch.title_font);
  CHECK(c.legend_font == ch.legend_font);
  CHECK(c.axis_font == ch.axis_font);
  CHECK(c.legend_pos == 1);
  CHECK(c.chart_fill == 0xFFF2F2F2);
  CHECK(c.chart_line == 0xFF000000);
  CHECK(c.chart_line_none);
  CHECK(c.plot_fill == 0xFFFFFFE0);
  CHECK(c.cat_axis == ch.cat_axis);
  CHECK(c.val_axis == ch.val_axis);
  CHECK(c.series.at(0).line_weight == 3.5f);
  CHECK(c.series.at(0).marker == 2);
  CHECK(c.series.at(0).marker_size == 7);
  // defaults: Excel's value axis gridlines, no category gridlines
  yc::model::ChartObj fresh;
  CHECK(fresh.val_axis.gridlines);
  CHECK_FALSE(fresh.cat_axis.gridlines);
}

TEST_CASE("Chart Filters (hidden series and categories) are saved") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  yc::model::ChartObj ch;
  ch.hidden_cats = {0, 3};
  yc::model::ChartSeries a, b;
  a.values = "Sheet!$B$2:$B$9";
  b.values = "Sheet!$C$2:$C$9";
  b.filtered = true;
  ch.series = {a, b};
  d.AddChart(s1, ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& c = (*res.wb.sheets[0]->charts).at(0);
  CHECK(c.hidden_cats == std::vector<int>{0, 3});
  REQUIRE(c.series.size() == 2);
  CHECK_FALSE(c.series[0].filtered);
  CHECK(c.series[1].filtered);
  CHECK(c.series[1].values == "Sheet!$C$2:$C$9");
}

TEST_CASE("Format Data Series options (gap width, overlap, first slice, hole, smoothing, box options) are saved") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  yc::model::ChartObj ch;
  ch.gap_width = 80;
  ch.overlap = 25;
  ch.first_slice_angle = 90;
  ch.hole_size = 70;
  yc::model::ChartSeries a;
  a.values = "Sheet!$B$2:$B$9";
  a.smooth = true;
  a.box_flags = 1 | 4;
  ch.series = {a};
  d.AddChart(s1, ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& c = (*res.wb.sheets[0]->charts).at(0);
  CHECK(c.gap_width == 80);
  CHECK(c.overlap == 25);
  CHECK(c.first_slice_angle == 90);
  CHECK(c.hole_size == 70);
  REQUIRE(c.series.size() == 1);
  CHECK(c.series[0].smooth);
  CHECK(c.series[0].box_flags == 5);
  // Excel's defaults when a file has none of them
  yc::model::ChartObj fresh;
  CHECK(fresh.gap_width == 219);
  CHECK(fresh.overlap == -27);
  CHECK(yc::model::ChartSeries{}.box_flags == 6);
}

TEST_CASE("Chart sheets: moving a chart to its own sheet and back") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  const uint32_t s1 = d.wb().sheets[0]->id;
  yc::model::ChartObj ch;
  ch.type = ChartType::ColumnClustered;
  yc::model::ChartSeries se;
  se.name = "=B1";
  se.values = "B2:B5";  // without its sheet
  ch.series.push_back(se);
  const uint32_t id = d.AddChart(s1, ch);
  uint32_t nid = 0;
  const uint32_t cs = d.MoveChart(s1, id, 0, "", 0, 0, &nid);
  REQUIRE(cs != 0);
  CHECK(d.wb().sheets.size() == 2);
  const auto* sheet = d.S(cs);
  REQUIRE(sheet);
  CHECK(sheet->chart_sheet);
  CHECK(sheet->name == std::string(TR("Chart")) + "1");
  CHECK(d.wb().IndexOfId(cs) == 1);  // right after the chart's sheet
  CHECK(d.S(s1)->charts->empty());
  REQUIRE(sheet->charts->size() == 1);
  const std::string src = d.S(s1)->name;
  CHECK((*sheet->charts)[0].series[0].values == src + "!B2:B5");
  CHECK((*sheet->charts)[0].series[0].name == "=" + src + "!B1");
  // saved and loaded
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  CHECK(res.wb.sheets[1]->chart_sheet);
  CHECK_FALSE(res.wb.sheets[0]->chart_sheet);
  // back onto the worksheet: the empty chart sheet goes away
  const uint32_t back = d.MoveChart(cs, nid, s1, "", 72, 36, &nid);
  CHECK(back == s1);
  CHECK(d.wb().sheets.size() == 1);
  REQUIRE(d.S(s1)->charts->size() == 1);
  CHECK((*d.S(s1)->charts)[0].left == 72);
  CHECK((*d.S(s1)->charts)[0].top == 36);
  // a named chart sheet
  const uint32_t named = d.MoveChart(s1, nid, 0, "Sales Chart", 0, 0, &nid);
  CHECK(d.S(named)->name == "Sales Chart");
}

TEST_CASE("3-D charts of one series, one point or no numbers build a scene (no reads past the series)") {
  auto measure = [](const std::string& t, float sc) { return static_cast<float>(t.size()) * 6 * sc; };
  auto series = [](std::vector<double> ys) {
    eng::ChartSeriesValues se;
    se.name = "S";
    for (size_t i = 0; i < ys.size(); ++i) {
      se.x.push_back(static_cast<double>(i + 1));
      se.y.push_back(ys[i]);
    }
    return se;
  };
  const std::vector<std::vector<std::vector<double>>> shapes = {
      {{1, 2, 3}},                                       // one series: the surface grid is one cell deep
      {{5}},                                             // one point
      {{std::nan(""), std::nan("")}},                    // text only: no finite values
      {{1, 2}, {3}},                                     // series of different lengths
  };
  for (int t = static_cast<int>(ChartType::Column3D); t <= static_cast<int>(ChartType::SurfaceContour); ++t) {
    for (size_t sh = 0; sh < shapes.size(); ++sh) {
      eng::ChartValues v;
      for (const auto& ys : shapes[sh]) v.series.push_back(series(ys));
      yc::model::ChartObj ch;
      ch.type = static_cast<ChartType>(t);
      eng::ChartScene scene;
      INFO("type " << t << " shape " << sh);
      eng::BuildChartScene(ch, v, 480, 300, 12, measure, &scene);
      for (const auto& p : scene.prims)
        for (float f : p.pts) CHECK(std::isfinite(f));
    }
  }
}

TEST_CASE("3-D charts build a projected scene with every element tagged") {
  eng::ChartValues v;
  for (int k = 0; k < 3; ++k) {
    eng::ChartSeriesValues se;
    se.name = "S" + std::to_string(k + 1);
    for (int i = 0; i < 5; ++i) {
      se.x.push_back(i + 1);
      se.y.push_back(3 + ((i * 7 + k * 3) % 9));
    }
    v.series.push_back(se);
  }
  v.cats = {"Q1", "Q2", "Q3", "Q4", "Q5"};
  auto measure = [](const std::string& t, float sc) { return static_cast<float>(t.size()) * 6 * sc; };
  for (int t = static_cast<int>(ChartType::Column3D); t <= static_cast<int>(ChartType::SurfaceContour); ++t) {
    yc::model::ChartObj ch;
    ch.type = static_cast<ChartType>(t);
    ch.has_title = true;
    ch.title = "T";
    REQUIRE(eng::IsSceneChart(ch.type));
    eng::ChartScene scene;
    eng::BuildChartScene(ch, v, 480, 300, 12, measure, &scene);
    INFO("type " << t);
    size_t series = 0, legend = 0, title = 0, polys = 0;
    for (const auto& p : scene.prims) {
      series += p.elem == static_cast<int>(yc::model::ChartElem::Series);
      legend += p.elem == static_cast<int>(yc::model::ChartElem::Legend);
      title += p.elem == static_cast<int>(yc::model::ChartElem::Title);
      polys += p.kind == eng::ChartPrim::Poly || p.kind == eng::ChartPrim::Rect;
      for (float f : p.pts) CHECK(std::isfinite(f));
    }
    CHECK(series > 0);
    CHECK(legend > 0);
    CHECK(title == 1);
    CHECK(polys > 4);
  }
}

TEST_CASE("3-D column depth order: farther boxes are drawn first") {
  eng::ChartValues v;
  eng::ChartSeriesValues se;
  se.name = "A";
  se.x = {1, 2};
  se.y = {5, 10};
  v.series.push_back(se);
  yc::model::ChartObj ch;
  ch.type = ChartType::ColumnClustered3D;
  ch.has_legend = false;
  auto measure = [](const std::string& t, float sc) { return static_cast<float>(t.size()) * 6 * sc; };
  eng::ChartScene scene;
  eng::BuildChartScene(ch, v, 400, 300, 12, measure, &scene);
  // rotated 20 degrees: the right box is nearer, so its faces come after the left box's, although it is taller
  float first_left = 1e9f, last_x = -1e9f;
  std::vector<float> order;
  for (const auto& p : scene.prims) {
    if (p.elem != static_cast<int>(yc::model::ChartElem::Series) || p.kind != eng::ChartPrim::Poly) continue;
    float cx = 0;
    for (size_t i = 0; i < p.pts.size(); i += 2) cx += p.pts[i];
    order.push_back(cx / static_cast<float>(p.pts.size() / 2));
  }
  REQUIRE(order.size() == 6);  // two boxes, three visible faces each
  first_left = order[0];
  last_x = order.back();
  CHECK(first_left < last_x);
}

TEST_CASE("The 3-D view is saved") {
  yc::engine::Document d(yc::model::Workbook::NewDefault("Sheet"));
  yc::model::ChartObj ch;
  ch.type = ChartType::Pie3D;
  ch.view3d = true;
  ch.rotation = 45;
  ch.elevation = 60;
  ch.perspective = 0;
  ch.depth_percent = 150;
  d.AddChart(d.wb().sheets[0]->id, ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& c = (*res.wb.sheets[0]->charts).at(0);
  CHECK(c.type == ChartType::Pie3D);
  CHECK(c.view3d);
  CHECK(c.rotation == 45);
  CHECK(c.elevation == 60);
  CHECK(c.depth_percent == 150);
}

TEST_CASE("Custom error bars: amounts from arrays, one value for every point, directions") {
  CHECK(eng::ParseNumberList("{1,2.5,3}") == std::vector<double>{1, 2.5, 3});
  CHECK(eng::ParseNumberList("2") == std::vector<double>{2});
  CHECK(eng::ParseNumberList("Sheet1!A1:A3").empty());
  eng::ChartSeriesValues b;
  b.x = {1, 2, 3};
  b.y = {10, 20, 30};
  b.err_plus = {1, 2, 3};
  b.err_minus = {0.5};
  yc::model::ChartSeries d;
  d.err = ErrorBarKind::Custom;
  std::vector<double> minus, plus;
  bool at_mean = false;
  double mean = 0;
  eng::ErrorExtents(d, b, 3, &minus, &plus, &at_mean, &mean);
  CHECK(plus == std::vector<double>{1, 2, 3});
  CHECK(minus == std::vector<double>{0.5, 0.5, 0.5});
  d.err_include = 2;  // plus values only
  eng::ErrorExtents(d, b, 3, &minus, &plus, &at_mean, &mean);
  CHECK(minus == std::vector<double>{0, 0, 0});
  d.err = ErrorBarKind::Fixed;
  d.err_amount = 4;
  d.err_include = 3;  // minus values only
  eng::ErrorExtents(d, b, 3, &minus, &plus, &at_mean, &mean);
  CHECK(minus == std::vector<double>{4, 4, 4});
  CHECK(plus == std::vector<double>{0, 0, 0});
  // saved
  yc::engine::Document doc(yc::model::Workbook::NewDefault("Sheet"));
  yc::model::ChartObj ch;
  yc::model::ChartSeries se;
  se.err = ErrorBarKind::Custom;
  se.err_plus = "Sheet!$C$1:$C$3";
  se.err_minus = "{1,2,3}";
  se.err_include = 2;
  ch.series.push_back(se);
  doc.AddChart(doc.wb().sheets[0]->id, ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(doc.wb(), {}));
  REQUIRE(res.ok);
  const auto& r = (*res.wb.sheets[0]->charts).at(0).series.at(0);
  CHECK(r.err == ErrorBarKind::Custom);
  CHECK(r.err_plus == "Sheet!$C$1:$C$3");
  CHECK(r.err_minus == "{1,2,3}");
  CHECK(r.err_include == 2);
}

TEST_CASE("Filled maps: region names in several languages, ISO codes, and which level the names are") {
  const auto& regions = eng::GeoRegions();
  REQUIRE(regions.size() > 4000);
  auto iso = [&](std::string_view name, int level = -1, std::string_view country = "") {
    const int r = eng::FindGeoRegion(name, level, country);
    return r < 0 ? std::string("-") : regions[static_cast<size_t>(r)].iso;
  };
  CHECK(iso("Japan") == "JP");
  CHECK(iso("jp") == "JP");
  CHECK(iso("United States of America") == "US");
  CHECK(iso("\xE6\x97\xA5\xE6\x9C\xAC") == "JP");                           // 日本
  CHECK(iso("\xE8\x87\xBA\xE5\x8C\x97\xE5\xB8\x82", 1) == "TW-TPE");         // 臺北市
  CHECK(iso("\xE5\x8F\xB0\xE5\x8C\x97\xE5\xB8\x82", 1) == "TW-TPE");         // 台北市
  CHECK(iso("\xE5\x8F\xB0\xE5\x8C\x97", 1) == "TW-TPE");                     // 台北
  CHECK(iso("Taipei", 1) == "TW-TPE");
  CHECK(iso("California", 1) == "US-CA");
  CHECK(iso("TW-KHH") == "TW-KHH");
  CHECK(iso("Atlantis") == "-");
  // a column of states: subdivisions; a column of countries: countries
  eng::ChartValues v;
  v.cats = {"California", "Texas", "New York", "Ohio"};
  auto m = eng::MatchMapRegions(v, 4);
  CHECK(m.level == 1);
  CHECK(m.matched == 4);
  v.cats = {"France", "Germany", "Japan"};
  m = eng::MatchMapRegions(v, 3);
  CHECK(m.level == 0);
  CHECK(m.matched == 3);
  // a country column gives the subdivisions their country
  v.cats.clear();
  v.paths = {{"Australia", "Victoria"}, {"Australia", "Queensland"}};
  m = eng::MatchMapRegions(v, 2);
  REQUIRE(m.region[0] >= 0);
  CHECK(regions[static_cast<size_t>(m.region[0])].iso == "AU-VIC");
  // the scene: the regions with data are the series
  yc::model::ChartObj ch;
  ch.type = ChartType::FilledMap;
  eng::ChartValues sv;
  sv.cats = {"Japan", "France"};
  eng::ChartSeriesValues se;
  se.x = {1, 2};
  se.y = {5, 9};
  sv.series.push_back(se);
  eng::ChartScene scene;
  eng::BuildChartScene(ch, sv, 480, 300, 12, [](const std::string& t, float sc) { return static_cast<float>(t.size()) * 6 * sc; }, &scene);
  size_t series = 0, legend = 0;
  for (const auto& p : scene.prims) {
    series += p.elem == static_cast<int>(yc::model::ChartElem::Series);
    legend += p.elem == static_cast<int>(yc::model::ChartElem::Legend);
  }
  CHECK(series >= 2);
  CHECK(legend > 0);
}
