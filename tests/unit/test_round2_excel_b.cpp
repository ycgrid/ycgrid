// SPDX-License-Identifier: Apache-2.0
// Round 2 (excel-b): charts, slicers, notes and views working like Excel's. Engine and file parts;
// the UI parts are in python/tests/test_round2_excel_b.py.
#include "core/i18n.h"
#include "engine/chart_layout.h"
#include "engine/document.h"
#include "io/ycg.h"
#include "model/page_setup.h"

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

model::ChartObj TwoSeriesColumns() {
  model::ChartObj ch;
  ch.type = ChartType::ColumnClustered;
  ch.series.resize(2);
  ch.has_title = true;
  return ch;
}

}  // namespace

TEST_CASE("excel-b: Quick Layout sets the elements Excel's layouts show") {
  model::ChartObj ch = TwoSeriesColumns();
  eng::ApplyChartQuickLayout(ch, 2);  // title, legend at the top, data labels, no value axis, no gridlines
  CHECK(ch.has_title);
  CHECK(ch.has_legend);
  CHECK(ch.legend_pos == 2);
  CHECK_FALSE(ch.val_axis.visible);
  CHECK_FALSE(ch.val_axis.gridlines);
  for (const auto& se : ch.series) CHECK(se.data_labels);
  eng::ApplyChartQuickLayout(ch, 8);  // title, both axis titles, no legend, columns touching
  CHECK_FALSE(ch.has_legend);
  CHECK(ch.cat_axis.has_title);
  CHECK(ch.val_axis.has_title);
  CHECK(ch.cat_axis.title == TR("Axis Title"));
  CHECK(ch.gap_width == 0);
  for (const auto& se : ch.series) CHECK_FALSE(se.data_labels);
  eng::ApplyChartQuickLayout(ch, 11);  // no title, legend in the corner
  CHECK_FALSE(ch.has_title);
  CHECK(ch.legend_pos == 4);
  CHECK(ch.val_axis.visible);
  // a title the user typed survives a layout without and with axis titles
  ch.val_axis.title = "Price";
  eng::ApplyChartQuickLayout(ch, 9);
  CHECK(ch.val_axis.title == "Price");
}

TEST_CASE("excel-b: a Chart Style replaces the elements' own fills and is saved") {
  model::ChartObj ch = TwoSeriesColumns();
  ch.series[0].color = 0x0000FF;  // the user's red series
  ch.plot_fill = 0xFFFFFF00;
  eng::ApplyChartStyle(ch, 8);  // dark background, white text
  CHECK(ch.style == 8);
  CHECK(ch.chart_fill == 0xFF404040);
  CHECK(ch.title_font.color == 0xFFFFFFFF);
  CHECK(ch.axis_font.color == 0xFFFFFFFF);
  CHECK(ch.plot_fill == 0);
  CHECK(ch.series[0].color == 0);  // from the palette again, as Excel's styles do
  const model::ChartObj back = RoundTrip(ch);
  CHECK(back.style == 8);
  CHECK(back.chart_fill == 0xFF404040);
  eng::ApplyChartStyle(ch, 2);  // values inside wide columns, no value axis
  CHECK(ch.chart_fill == 0);
  CHECK(ch.title_font.color == 0);
  CHECK(ch.gap_width == 80);
  CHECK_FALSE(ch.val_axis.visible);
  CHECK(ch.series[1].data_labels);
  CHECK(ch.series[1].label_pos == 1);
  // on a line chart the labels take the automatic position (inside end is a column's)
  model::ChartObj line = TwoSeriesColumns();
  line.type = ChartType::Line;
  eng::ApplyChartStyle(line, 2);
  CHECK(line.series[0].label_pos == -1);
  // the out-of-range numbers are clamped, never out of the tables
  eng::ApplyChartStyle(ch, 99);
  CHECK(ch.style == eng::ChartStyleCount(ch.type));  // the last of the column set (14)
  eng::ApplyChartQuickLayout(ch, -3);
  CHECK(ch.legend_pos == 1);
}

TEST_CASE("excel-b: Slicer Settings are saved with the slicer; old files keep Excel's defaults") {
  yc::engine::Document d(model::Workbook::NewDefault("Sheet"));
  const uint32_t sid = d.wb().sheets[0]->id;
  model::Slicer sl;
  sl.pivot = "PivotTable1";
  sl.caption = "Region";
  d.AddSlicer(sid, sl);
  sl.caption = "Product";
  sl.show_header = false;
  sl.descending = true;
  sl.hide_no_data = true;
  sl.indicate_no_data = false;
  sl.no_data_last = false;
  d.AddSlicer(sid, sl);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& v = *res.wb.sheets[0]->slicers;
  REQUIRE(v.size() == 2);
  CHECK(v[0].show_header);
  CHECK_FALSE(v[0].descending);
  CHECK(v[0].indicate_no_data);
  CHECK(v[0].no_data_last);
  CHECK_FALSE(v[0].hide_no_data);
  CHECK(model::SlicerFlags(v[0]) == 0);  // Excel's defaults are the number an older file reads as nothing
  CHECK_FALSE(v[1].show_header);
  CHECK(v[1].descending);
  CHECK(v[1].hide_no_data);
  CHECK_FALSE(v[1].indicate_no_data);
  CHECK_FALSE(v[1].no_data_last);
  CHECK(v[1].caption == "Product");
}

TEST_CASE("excel-b: Show/Hide Note keeps a note on the sheet, through edits, undo and the file") {
  yc::engine::Document d(model::Workbook::NewDefault("Sheet"));
  const uint32_t sid = d.wb().sheets[0]->id;
  d.SetNote(sid, 1, 1, "Ann", "check the open");
  d.SetNote(sid, 4, 2, "Ann", "second");
  d.SetComment(sid, 6, 0, "Bob", "a thread", 1.0);
  auto note = [&](uint32_t r, uint32_t c) { return d.wb().sheets[0]->notes->at((uint64_t{r} << 16) | c); };
  CHECK_FALSE(note(1, 1).visible);
  d.Begin("Show/Hide Note");
  CHECK(d.SetNotesVisible(sid, yc::core::Rect::Cell(1, 1), true) == 1);
  d.Commit();
  CHECK(note(1, 1).visible);
  CHECK_FALSE(note(4, 2).visible);
  CHECK(d.Undo());
  CHECK_FALSE(note(1, 1).visible);
  CHECK(d.Redo());
  CHECK(note(1, 1).visible);
  d.SetNote(sid, 1, 1, "Ann", "check the open again");  // editing the text keeps it shown
  CHECK(note(1, 1).visible);
  // Show All Notes: every note, never the threads
  CHECK(d.SetNotesVisible(sid, yc::core::Rect{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1}, true) == 1);
  CHECK(note(4, 2).visible);
  CHECK_FALSE(note(6, 0).visible);
  CHECK(d.SetNotesVisible(sid, yc::core::Rect::Cell(4, 2), true) == 0);  // nothing to change
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(d.wb(), {}));
  REQUIRE(res.ok);
  const auto& notes = *res.wb.sheets[0]->notes;
  CHECK(notes.at((uint64_t{1} << 16) | 1).visible);
  CHECK(notes.at((uint64_t{4} << 16) | 2).visible);
  CHECK_FALSE(notes.at((uint64_t{6} << 16) | 0).visible);
  CHECK(notes.at((uint64_t{6} << 16) | 0).threaded);
}

TEST_CASE("excel-b: header fields typed on the page read as Excel shows them and are stored as codes") {
  CHECK(model::HeaderCodesToDisplay("Page &P of &N") == "Page &[Page] of &[Pages]");
  CHECK(model::HeaderCodesToDisplay("&Z&F - &A && co") == "&[Path]&[File] - &[Tab] && co");
  CHECK(model::HeaderCodesFromDisplay("Page &[Page] of &[Pages]") == "Page &P of &N");
  CHECK(model::HeaderCodesFromDisplay("&[Path]&[File] &[Date] &[Time] &[Tab]") == "&Z&F &D &T &A");
  CHECK(model::HeaderCodesFromDisplay("R&&D") == "R&&D");
  for (const char* t : {"", "Quotes &A", "&P/&N", "&\"Arial,Bold\"Title", "a & b", "&&[Page]"}) {
    CHECK(model::HeaderCodesFromDisplay(model::HeaderCodesToDisplay(t)) == t);
  }
}
