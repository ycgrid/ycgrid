// SPDX-License-Identifier: Apache-2.0
// Round 3 (pivots): PivotTables, slicers and timelines working like Excel's:
// per-field layout forms, subtotals at the top, page breaks, new items left out of a manual filter, General values,
// calculated fields and items with worksheet functions, Report Connections and table slicers, and the file keys they use
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.h"

#include "core/i18n.h"
#include "engine/chart_layout.h"
#include "engine/pivot_engine.h"
#include "io/ycg.h"

using yc::core::Rect;
using yc::model::PivotTable;

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

// Region, Product, Sales, Qty in A1:D7 (East 35, North 7, West 70)
void Fill(yctest::Doc& t) {
  const char* rows[][4] = {{"Region", "Product", "Sales", "Qty"}, {"East", "A", "10", "1"}, {"East", "B", "20", "2"}, {"West", "A", "30", "3"},
                           {"West", "B", "40", "4"},          {"East", "A", "5", "1"},  {"North", "B", "7", "1"}};
  for (int r = 0; r < 7; ++r) {
    for (int c = 0; c < 4; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
  }
}

PivotTable Base(yctest::Doc& t, uint32_t c0 = 6) {
  PivotTable p;
  p.src_sheet = t.sheet();
  p.src = Rect{0, 0, 6, 3};
  p.c0 = c0;  // G1
  return p;
}

yc::model::PivotAxisField Axis(int f) {
  yc::model::PivotAxisField a;
  a.field = f;
  return a;
}

yc::model::PivotDataField Data(int f, yc::model::PivotFunc fn = yc::model::PivotFunc::Sum) {
  yc::model::PivotDataField d;
  d.field = f;
  d.func = fn;
  return d;
}

yc::engine::PivotReport Report(yctest::Doc& t, const PivotTable& p, yc::engine::PivotCache* cache) {
  std::string err;
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), p, cache, &err));
  return yc::engine::ComputePivot(p, *cache);
}

}  // namespace

TEST_CASE("round3 pivots M7: a field in tabular form beside compact ones, subtotals at the top per field") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0), Axis(1)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  CHECK(t.T("G2") == "East");  // compact: one column
  CHECK(t.T("G3") == "A");
  // Region in tabular form: its labels in their own column, on the first row of each group, the subtotal below
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows[0].form = 2; }));
  CHECK(t.T("G1") == "Region");
  CHECK(t.T("G2") == "East");
  CHECK(t.T("H2") == "A");
  CHECK(t.N("I2") == 15);
  CHECK(t.T("H3") == "B");
  CHECK(t.T("G3") == "");
  CHECK(t.T("G4") == "East Total");
  CHECK(t.N("I4") == 35);
  // outline form with its subtotal at the bottom while the report puts them at the top
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.layout = 1;
    x.subtotals = 1;
    x.rows[0].form = -1;
    x.rows[0].sub_top = 0;
  }));
  CHECK(t.T("G2") == "East");
  CHECK(t.V("I2").IsEmpty());  // no total on the group row
  CHECK(t.T("H3") == "A");
  CHECK(t.T("G5") == "East Total");
  CHECK(t.N("I5") == 35);
}

TEST_CASE("round3 pivots M7: Include new items in manual filter, page breaks after each item") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.src = Rect{0, 0, 7, 3};  // one empty row for a new item later
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows[0].hidden = {"West"}; }));
  const auto& known = t.d.PivotById(t.sheet(), id)->rows[0].known;
  CHECK(std::find(known.begin(), known.end(), "East") != known.end());  // the items when the filter was set
  // a new item (South) arrives: left out, as Excel's default does
  t.Set("A8", "South");
  t.Set("C8", "100");
  REQUIRE(t.d.RefreshPivot(t.sheet(), id));
  CHECK(t.N("H4") == 7 + 35);  // the grand total: East and North only
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows[0].include_new = true; }));
  CHECK(t.T("G4") == "South");
  CHECK(t.N("H5") == 142);
  // Insert page break after each item: the sheet's page setup gets the breaks, and loses them again
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows[0].page_break = true; }));
  const auto& rb = t.d.S(t.sheet())->page_setup->row_breaks;
  CHECK(std::find(rb.begin(), rb.end(), 2u) != rb.end());  // before North (row 3, 0-based 2)
  CHECK(std::find(rb.begin(), rb.end(), 3u) != rb.end());
  CHECK(std::find(rb.begin(), rb.end(), 4u) == rb.end());  // none after the last item
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows[0].page_break = false; }));
  CHECK(t.d.S(t.sheet())->page_setup->row_breaks.empty());
}

TEST_CASE("round3 pivots L9: values are General unless set; older files keep the source's format") {
  English en;
  yctest::Doc t;
  Fill(t);
  const uint32_t fmt = t.d.wb_mut().styles.mut().InternString("$#,##0.00");
  t.d.ModifyStyle(t.sheet(), Rect{1, 2, 6, 2}, [fmt](yc::model::Style& st) { st.numfmt = fmt; });
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  yc::engine::PivotCache cache;
  auto rep = Report(t, p, &cache);
  CHECK(rep.at(1, 1).numfmt.empty());  // General
  p.data[0].number_format = "0.0";
  rep = Report(t, p, &cache);
  CHECK(rep.at(1, 1).numfmt == "0.0");
  p.data[0].number_format.clear();
  p.general_values = false;  // a file saved before
  rep = Report(t, p, &cache);
  CHECK(rep.at(1, 1).numfmt == "$#,##0.00");
}

TEST_CASE("round3 pivots L7: calculated fields and items with worksheet functions") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.calc_fields = {{"Bonus", "=IF(Sales>20,Sales*0.1,0)"}, {"Avg Price", "=ROUND(Sales/Qty,2)"}};
  yc::engine::PivotCache cache;
  std::string err;
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  p.data = {Data(static_cast<int>(cache.source_fields)), Data(static_cast<int>(cache.source_fields) + 1)};
  auto rep = yc::engine::ComputePivot(p, cache);
  // East: 35 sales, 4 qty; North: 7, 1; West: 70, 7
  CHECK_THAT(rep.at(1, 1).v.AsDouble(), Catch::Matchers::WithinAbs(3.5, 1e-9));
  CHECK(rep.at(2, 1).v.AsDouble() == 0);
  CHECK_THAT(rep.at(3, 1).v.AsDouble(), Catch::Matchers::WithinAbs(7.0, 1e-9));
  CHECK_THAT(rep.at(1, 2).v.AsDouble(), Catch::Matchers::WithinAbs(8.75, 1e-9));
  CHECK(yc::engine::CheckPivotFormula(cache.names, "=MAX(Sales,Qty)*2", &err));
  CHECK_FALSE(yc::engine::CheckPivotFormula(cache.names, "=A1*2", &err));
  CHECK_FALSE(yc::engine::CheckPivotFormula(cache.names, "=Price*2", &err));
  // a calculated item with a function: worked out in each cell from its items' values; sums include it
  p.calc_fields.clear();
  p.data = {Data(2)};
  p.calc_items = {{0, "Top", "=MAX(East,West)"}};
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  rep = yc::engine::ComputePivot(p, cache);
  bool found = false;
  for (uint32_t r = 0; r < rep.rows; ++r) {
    if (rep.at(r, 0).v.IsString() && std::string(rep.at(r, 0).v.str()) == "Top") {
      found = true;
      CHECK(rep.at(r, 1).v.AsDouble() == 70);
    }
    if (rep.at(r, 0).v.IsString() && std::string(rep.at(r, 0).v.str()) == "Grand Total") CHECK(rep.at(r, 1).v.AsDouble() == 112 + 70);
  }
  CHECK(found);
}

TEST_CASE("round3 pivots L10: Report Connections and a table's slicer") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  const uint32_t a = t.d.AddPivot(t.sheet(), p);
  auto q = Base(t, 10);  // K1
  q.rows = {Axis(1)};
  q.data = {Data(2)};
  q.name = "Other";
  const uint32_t b = t.d.AddPivot(t.sheet(), q);
  REQUIRE((a && b));
  yc::model::Slicer sl;
  sl.pivot = "PivotTable1";
  sl.field = 0;  // Region
  const uint32_t sid = t.d.AddSlicer(t.sheet(), sl);
  REQUIRE(t.d.ConnectSlicer(t.sheet(), sid, {"PivotTable1", "Other"}));
  REQUIRE(t.d.SetSlicerSelection(t.sheet(), sid, {"West"}));
  CHECK(t.d.PivotById(t.sheet(), a)->rows[0].hidden == std::vector<std::string>{"West"});
  const auto& sf = t.d.PivotById(t.sheet(), b)->slicer_filters;
  REQUIRE(sf.size() == 1);
  CHECK(sf[0].hidden == std::vector<std::string>{"West"});
  CHECK(t.N("L2") == 15);  // Other: A without West
  // the slicer leaves Other: its filter goes with it
  REQUIRE(t.d.ConnectSlicer(t.sheet(), sid, {"PivotTable1"}));
  CHECK(t.d.PivotById(t.sheet(), b)->slicer_filters.empty());
  // a table's slicer filters the table's column
  yctest::Doc u;
  u.Set("A1", "Sym");
  u.Set("B1", "Qty");
  const char* syms[] = {"TXF", "MXF", "TXF", "TE"};
  for (int i = 0; i < 4; ++i) {
    u.Set(("A" + std::to_string(i + 2)).c_str(), syms[i]);
    u.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(i + 1));
  }
  REQUIRE(u.d.AddTable(u.sheet(), Rect{0, 0, 4, 1}, true, "TableStyleMedium2", nullptr) != 0);
  const std::string tname = u.d.S(u.sheet())->tables->front().name;
  yc::model::Slicer ts;
  ts.table = tname;
  ts.field = 0;
  const uint32_t tid = u.d.AddSlicer(u.sheet(), ts);
  REQUIRE(u.d.SetSlicerSelection(u.sheet(), tid, {"MXF"}));
  CHECK(u.d.S(u.sheet())->rows->IsHidden(2));
  CHECK_FALSE(u.d.S(u.sheet())->rows->IsHidden(1));
  std::vector<std::string> items, hidden;
  std::vector<char> has;
  REQUIRE(yc::engine::TableSlicerState(u.d.wb(), u.d.S(u.sheet())->slicers->front(), nullptr, &items, &hidden, &has));
  CHECK(items == std::vector<std::string>{"MXF", "TE", "TXF"});
  CHECK(hidden == std::vector<std::string>{"MXF"});
  REQUIRE(u.d.DeleteSlicer(u.sheet(), tid));
  CHECK_FALSE(u.d.S(u.sheet())->rows->IsHidden(2));  // the last slicer takes its filter with it
}

TEST_CASE("round3 pivots: the new keys and elements round-trip; older files keep the source's number format") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0), Axis(1)};
  p.data = {Data(2)};
  p.rows[0].include_new = true;
  p.rows[0].form = 2;
  p.rows[0].sub_top = 1;
  p.rows[0].page_break = true;
  p.rows[1].hidden = {"B"};
  p.enable_selection = false;
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  yc::model::Slicer sl;
  sl.pivot = "PivotTable1";
  sl.field = 0;
  sl.more_pivots = {"Elsewhere"};
  sl.tl_hide = 5;
  REQUIRE(t.d.AddSlicer(t.sheet(), sl) != 0);
  yc::model::Slicer ts;
  ts.table = "Table9";
  ts.field = 1;
  REQUIRE(t.d.AddSlicer(t.sheet(), ts) != 0);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const PivotTable& lp = res.wb.sheets[0]->pivots->front();
  CHECK(lp.rows[0].include_new);
  CHECK(lp.rows[0].form == 2);
  CHECK(lp.rows[0].sub_top == 1);
  CHECK(lp.rows[0].page_break);
  CHECK(lp.rows[1].known == t.d.PivotById(t.sheet(), id)->rows[1].known);
  CHECK_FALSE(lp.rows[1].known.empty());
  CHECK_FALSE(lp.enable_selection);
  CHECK(lp.general_values);
  CHECK(lp.breaks == t.d.PivotById(t.sheet(), id)->breaks);
  const auto& sls = *res.wb.sheets[0]->slicers;
  REQUIRE(sls.size() == 2);
  CHECK(sls[0].more_pivots == std::vector<std::string>{"Elsewhere"});
  CHECK(sls[0].tl_hide == 5);
  CHECK(sls[1].table == "Table9");
  CHECK(sls[1].field == 1);
  // a file from before (options key 22 absent): values keep the source's format
  auto old = t.d.wb();
  for (auto& sh : old.sheets) {
    for (auto& x : sh.mut().pivots.mut()) x.general_values = false;
  }
  auto res2 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(old, {}));
  REQUIRE(res2.ok);
  CHECK_FALSE(res2.wb.sheets[0]->pivots->front().general_values);
}

TEST_CASE("round3 pivots M14: a PivotChart's categories by level; Multi-level Category Labels") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0), Axis(1)};
  p.data = {Data(2)};
  p.subtotals = 0;
  p.row_grand = false;
  p.col_grand = false;
  REQUIRE(t.d.AddPivot(t.sheet(), p) != 0);
  yc::model::ChartObj ch;
  ch.type = yc::model::ChartType::ColumnClustered;
  ch.pivot = "PivotTable1";
  REQUIRE(yc::engine::PivotChartSeries(t.d.wb(), *t.d.PivotById(t.sheet(), t.d.wb().sheets[0]->pivots->front().id), &ch));
  REQUIRE(!ch.series.empty());
  const auto& paths = ch.series[0].data->cat_paths;
  REQUIRE(paths.size() == ch.series[0].data->cats.size());
  CHECK(paths.front() == std::vector<std::string>{"East", "A"});
  CHECK(paths.back() == std::vector<std::string>{"West", "B"});
  // the axis shows them in levels while Multi-level Category Labels is on
  yc::engine::ChartValues v;
  v.cats = ch.series[0].data->cats;
  v.paths = paths;
  CHECK(yc::engine::ChartCategoryLevels(ch, v, v.cats.size()).size() == v.cats.size());
  ch.cat_axis.multi_level = false;
  CHECK(yc::engine::ChartCategoryLevels(ch, v, v.cats.size()).empty());
  CHECK(yc::engine::ChartCategoryLabel(v, 0) == "East A");
  // the check box is saved (axis element 30); older files read it as on
  t.d.AddChart(t.sheet(), ch);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(!res.wb.sheets[0]->charts->empty());
  CHECK_FALSE(res.wb.sheets[0]->charts->front().cat_axis.multi_level);
  CHECK(res.wb.sheets[0]->charts->front().val_axis.multi_level);
}

TEST_CASE("Rows and columns inserted or deleted through a PivotTable report are refused; around it they move it (Excel 16, measured)") {
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.r0 = 9;  // G10, below the source: whole rows through the report don't touch the source
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  auto area = [&] {
    const PivotTable* q = t.d.PivotById(t.sheet(), id);
    return q ? yc::engine::PivotArea(*q) : Rect{};
  };
  REQUIRE(area() == Rect{9, 6, 13, 7});  // G10:H14: labels, East, North, West, Grand Total
  CHECK(t.T("G11") == "East");
  const uint32_t blocks = t.d.protect_blocks();
  t.d.InsertRows(t.sheet(), 11, 1);  // row 12, inside: Excel says it would affect a PivotTable
  CHECK(t.d.protect_blocks() == blocks + 1);
  CHECK(t.d.block_pivot_shift());
  CHECK(area() == Rect{9, 6, 13, 7});
  CHECK(t.T("G12") == "North");
  t.d.InsertRows(t.sheet(), 13, 1);  // before the last row
  t.d.DeleteRows(t.sheet(), 10, 1);  // part of it
  t.d.DeleteRows(t.sheet(), 13, 2);  // its last row and the one below
  t.d.InsertCols(t.sheet(), 7, 1);   // column H, inside
  t.d.DeleteCols(t.sheet(), 7, 1);
  t.d.InsertCells(t.sheet(), Rect{11, 5, 11, 5}, false);  // F12 shifted right: part of the report's rows would move
  CHECK(t.d.protect_blocks() == blocks + 7);
  CHECK(area() == Rect{9, 6, 13, 7});
  // before its first row or column, or beside it, the report moves with the cells
  t.d.InsertRows(t.sheet(), 9, 2);  // rows 10:11
  CHECK(t.d.protect_blocks() == blocks + 7);
  CHECK(area() == Rect{11, 6, 15, 7});
  CHECK(t.T("G13") == "East");
  t.d.InsertCols(t.sheet(), 6, 1);  // column G
  CHECK(area() == Rect{11, 7, 15, 8});
  t.d.InsertCells(t.sheet(), Rect{11, 5, 15, 5}, false);  // F12:F16, every row of the report, shifted right
  CHECK(area() == Rect{11, 8, 15, 9});
  CHECK(t.T("I13") == "East");
  t.d.InsertRows(t.sheet(), 16, 1);  // just below it
  CHECK(area() == Rect{11, 8, 15, 9});
  // deleting every row of it deletes the report with them
  t.d.DeleteRows(t.sheet(), 10, 7);
  CHECK(t.d.protect_blocks() == blocks + 7);
  CHECK(t.d.PivotById(t.sheet(), id) == nullptr);
  CHECK(t.d.S(t.sheet())->pivots->empty());
}
