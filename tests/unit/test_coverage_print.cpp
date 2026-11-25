// SPDX-License-Identifier: Apache-2.0
// Coverage of the printed page (engine/page_layout.cpp) beyond the other print tests: what shapes, charts and
// sparklines add to the printed range, several print areas fitted to one page at one scale, print title columns, and
// every field of a header split into runs.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/page_layout.h"

using yc::core::Rect;

TEST_CASE("Print coverage: shapes, charts and sparklines widen what prints; hidden and floating ones don't") {
  yctest::Doc t;
  t.Set("A1", "x");
  yc::model::Sheet sh = *t.d.S(t.sheet());
  bool empty = true;
  CHECK(yc::engine::PrintableRange(sh, &empty) == Rect::Cell(0, 0));
  CHECK_FALSE(empty);
  yc::model::ShapeObj hidden;
  hidden.left = 5000;
  hidden.top = 5000;
  hidden.visible = false;
  yc::model::ShapeObj shape;
  shape.left = 400;
  shape.top = 600;  // rows of 15 points: from row 41
  sh.shapes.mut() = {hidden, shape};
  const Rect with_shape = yc::engine::PrintableRange(sh, &empty);
  CHECK(with_shape.r1 >= 40);
  CHECK(with_shape.c1 >= 8);
  CHECK(with_shape.r1 < 100);  // the hidden one adds nothing
  yc::model::ChartObj chart;
  chart.left = 0;
  chart.top = 1500;  // from row 101
  yc::model::ChartObj floating = chart;
  floating.floating = true;
  floating.top = 30000;
  sh.charts.mut() = {chart, floating};
  const Rect with_chart = yc::engine::PrintableRange(sh, &empty);
  CHECK(with_chart.r1 >= 100);
  CHECK(with_chart.r1 < 1000);
  // a sheet with a sparkline alone prints its cell
  yctest::Doc u;
  yc::model::Sheet sp = *u.d.S(u.sheet());
  CHECK(yc::engine::PrintableRange(sp, &empty) == Rect{});
  CHECK(empty);
  yc::model::SparklineGroup g;
  yc::model::Sparkline item;
  item.r = 7;
  item.c = 3;
  g.items = {item};
  sp.sparklines.mut() = {g};
  CHECK(yc::engine::PrintableRange(sp, &empty) == Rect::Cell(7, 3));
  CHECK_FALSE(empty);
}

TEST_CASE("Print coverage: several print areas fitted at one scale, print title columns") {
  yctest::Doc t;
  for (int r = 1; r <= 60; ++r) t.Set(("A" + std::to_string(r)).c_str(), std::to_string(r));
  for (int c = 0; c < 30; ++c) t.Set((std::string(1, static_cast<char>('A' + (c % 26))) + "70").c_str(), "w");
  const auto& sh = *t.d.S(t.sheet());
  yc::model::PageSetup ps;
  ps.has_area = true;
  ps.area = Rect{0, 0, 59, 0};          // a long column
  ps.more_areas = {Rect{69, 0, 69, 25}};  // a wide row
  ps.zoom = 0;                           // fit each to one page, all at the smallest scale
  const auto L = yc::engine::LayoutPages(sh, ps);
  REQUIRE(L.areas.size() == 2);
  REQUIRE(L.pages.size() == 2);
  CHECK(L.pages[0].area == 0);
  CHECK(L.pages[1].area == 1);
  CHECK(L.scale <= 1);
  ps.more_areas.clear();
  ps.area = Rect{0, 0, 59, 25};
  ps.zoom = 100;
  ps.has_title_cols = true;
  ps.title_c0 = 0;
  ps.title_c1 = 1;
  const auto T = yc::engine::LayoutPages(sh, ps);
  CHECK(T.title_w > 0);
  bool repeated = false;
  for (const auto& p : T.pages) repeated = repeated || (p.title_cols && p.c0 > 1);
  CHECK(repeated);
}

TEST_CASE("Print coverage: every field of a header in runs") {
  const auto runs = yc::engine::HeaderFooterRuns("&N|&D|&T|&F|&Z|&A", 2, 5, "Book.ycg", "C:\\Data\\", "Prices", "2026/11/14", "09:30");
  std::string all;
  for (const auto& r : runs) all += r.text;
  CHECK(all == "5|2026/11/14|09:30|Book.ycg|C:\\Data\\|Prices");
}
