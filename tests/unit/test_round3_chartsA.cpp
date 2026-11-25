// SPDX-License-Identifier: Apache-2.0
// Round 3 (chartsA): formatting chart elements as Excel's Format pane does. The file parts: each element's font
// (name, italic, underline), an axis' own font, line and label distance, a series' gradient / picture / pattern fill,
// compound line, caps, joins, arrows, its data labels' font, one label dragged away, leader lines, vary colors by
// point and a deleted legend entry. The UI parts are in python/tests/test_round3_chartsA.py.
#include "engine/document.h"
#include "io/ycg.h"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("chartsA: fonts keep their name, italic and underline; each axis its own font, line and label distance") {
  model::ChartObj ch;
  ch.type = model::ChartType::ColumnClustered;
  ch.title_font.name = "Georgia";
  ch.title_font.italic = true;
  ch.title_font.underline = true;
  ch.title_font.size = 16;
  ch.legend_font.italic = true;
  ch.val_axis.own_font = true;
  ch.val_axis.font.name = "Consolas";
  ch.val_axis.font.bold = true;
  ch.val_axis.font.size = 11;
  ch.val_axis.title_font.italic = true;
  ch.val_axis.line_color = 0xFF112233;
  ch.val_axis.line_weight = 2.25f;
  ch.cat_axis.no_line = true;
  ch.cat_axis.label_offset = 300;
  const model::ChartObj c = RoundTrip(ch);
  CHECK(c.title_font == ch.title_font);
  CHECK(c.legend_font == ch.legend_font);
  CHECK(c.val_axis == ch.val_axis);
  CHECK(c.cat_axis == ch.cat_axis);
  CHECK(c.val_axis.font.name == "Consolas");
  CHECK(c.cat_axis.label_offset == 300);
  // defaults: the chart's axis font, Excel's label distance
  CHECK_FALSE(c.val_axis2.own_font);
  CHECK(c.val_axis2.label_offset == 100);
}

TEST_CASE("chartsA: a series' fills, line ends, label font, dragged label and legend entry are saved") {
  model::ChartObj ch;
  ch.type = model::ChartType::ColumnClustered;
  model::ChartSeries a;
  a.values = "Sheet!$B$2:$B$5";
  a.fill_type = 1;
  a.grad_color2 = 0x00FFFF;
  a.grad_angle = 45;
  a.vary_points = true;
  a.legend_hidden = true;
  a.leader_lines = false;
  a.label_show = 4 | 16;
  a.label_font.name = "Arial";
  a.label_font.size = 12;
  a.label_font.bold = true;
  a.label_font.color = 0xFFC00000;
  model::ChartPointFormat pf;
  pf.index = 2;
  pf.label = 1;
  pf.label_dx = 0.125f;
  pf.label_dy = -0.25f;
  a.points.push_back(pf);
  model::ChartSeries b;
  b.values = "Sheet!$C$2:$C$5";
  b.fill_type = 3;
  b.pattern = 7;
  b.pattern_back = 0xEEEEEE;
  b.compound = 4;
  b.cap = 1;
  b.join = 1;
  b.begin_arrow = 4;
  b.end_arrow = 1;
  model::ChartSeries p;
  p.values = "Sheet!$D$2:$D$5";
  p.fill_type = 2;
  p.picture_tile = true;
  p.picture = std::string("\x89PNG\r\n\x1a\n\0\1\2", 11);  // bytes, NULs included
  ch.series = {a, b, p};
  const model::ChartObj c = RoundTrip(ch);
  REQUIRE(c.series.size() == 3);
  const auto& ra = c.series[0];
  CHECK(ra.fill_type == 1);
  CHECK(ra.grad_color2 == 0x00FFFFu);
  CHECK(ra.grad_angle == 45);
  CHECK(ra.vary_points);
  CHECK(ra.legend_hidden);
  CHECK_FALSE(ra.leader_lines);
  CHECK(ra.label_show == (4 | 16));
  CHECK(ra.label_font == a.label_font);
  REQUIRE(ra.points.size() == 1);
  CHECK(ra.points[0] == pf);
  const auto& rb = c.series[1];
  CHECK(rb.fill_type == 3);
  CHECK(rb.pattern == 7);
  CHECK(rb.pattern_back == 0xEEEEEEu);
  CHECK(rb.compound == 4);
  CHECK(rb.cap == 1);
  CHECK(rb.join == 1);
  CHECK(rb.begin_arrow == 4);
  CHECK(rb.end_arrow == 1);
  CHECK(rb.leader_lines);  // Excel's default
  CHECK_FALSE(rb.vary_points);
  const auto& rp = c.series[2];
  CHECK(rp.fill_type == 2);
  CHECK(rp.picture_tile);
  CHECK(rp.picture == p.picture);
}

TEST_CASE("chartsA: one label's own text, options and font, legend entry fonts, tiling and both label offsets are saved") {
  model::ChartObj ch;
  ch.type = model::ChartType::Pie;
  model::ChartSeries a;
  a.values = "Sheet!$B$2:$B$5";
  a.fill_type = 2;
  a.picture_tile = true;
  a.tile_ox = 3.5f;
  a.tile_oy = -2;
  a.tile_sx = 50;
  a.tile_sy = 200;
  a.tile_align = 4;
  a.tile_mirror = 3;
  a.legend_entry_font_set = true;
  a.legend_entry_font.italic = true;
  a.label_dx = 0.1f;  // the series' labels dragged together (chartsB)
  model::ChartPointFormat pf;
  pf.index = 1;
  pf.label = 1;
  pf.label_dx = 0.05f;  // ... then this one
  pf.label_dy = -0.02f;
  pf.label_text = "=Sheet!$E$1";
  pf.label_show = 1 | 4;
  pf.label_pos = 3;
  pf.label_font_set = true;
  pf.label_font.bold = true;
  pf.label_font.size = 12;
  pf.legend_font_set = true;
  pf.legend_font.name = "Georgia";
  pf.label_fill = 0xFFFFFF00;
  pf.label_line = 0xFF000000;
  pf.legend_hidden = true;
  a.label_fill = 0xFFEEEEEE;
  a.points.push_back(pf);
  ch.series = {a};
  const model::ChartObj c = RoundTrip(ch);
  const auto& r = c.series.at(0);
  CHECK(r.tile_ox == 3.5f);
  CHECK(r.tile_oy == -2);
  CHECK(r.tile_sx == 50);
  CHECK(r.tile_sy == 200);
  CHECK(r.tile_align == 4);
  CHECK(r.tile_mirror == 3);
  CHECK(r.legend_entry_font_set);
  CHECK(r.legend_entry_font == a.legend_entry_font);
  CHECK(r.label_fill == 0xFFEEEEEEu);
  CHECK(r.label_line == 0u);
  REQUIRE(r.points.size() == 1);
  CHECK(r.points[0] == pf);
  // each offset is kept on its own and counted once
  CHECK(r.label_dx == 0.1f);
  CHECK(r.points[0].label_dx == 0.05f);
  float dx = 0, dy = 0;
  model::DataLabelOffset(r, 1, &dx, &dy);
  CHECK(dx == 0.1f + 0.05f);
  CHECK(dy == -0.02f);
  model::DataLabelOffset(r, 0, &dx, &dy);  // a label without its own offset: the series' only
  CHECK(dx == 0.1f);
  CHECK(dy == 0.0f);
}
