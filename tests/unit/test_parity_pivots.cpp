// SPDX-License-Identifier: Apache-2.0
// PivotTables working like Excel's: fields by name, dates, expand / collapse, sorting by
// values, what a report may cover, Show Values As, filters, Field Settings, groups, options, styles, formats kept
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.h"

#include "core/i18n.h"
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

// Region, Product, Sales, Qty in A1:D7
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

const PivotTable& Pt(yctest::Doc& t, uint32_t id) { return *t.d.PivotById(t.sheet(), id); }

PivotTable Reload(yctest::Doc& t) {
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  return res.wb.sheets[0]->pivots->front();
}

}  // namespace

TEST_CASE("pivot parity H2: fields follow their names when source columns move") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t, 8);
  p.rows = {Axis(1)};  // Product
  p.data = {Data(2)};  // Sales
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  yc::model::Slicer sl;
  sl.pivot = "PivotTable1";
  sl.field = 3;  // Qty: not on the report
  REQUIRE(t.d.AddSlicer(t.sheet(), sl) != 0);
  CHECK(t.T("I2") == "A");
  CHECK(t.N("J2") == 45);
  // a column inserted at B (inside the source) with a heading
  t.d.InsertCols(t.sheet(), 1, 1);
  t.Set("B1", "Cost");
  for (int r = 2; r <= 7; ++r) t.Set(("B" + std::to_string(r)).c_str(), std::to_string(r * 100));
  const uint32_t id2 = Pt(t, id).id;
  std::string err;
  REQUIRE(t.d.RefreshPivot(t.sheet(), id2, &err));
  const auto& pt = Pt(t, id);
  CHECK(pt.fields[static_cast<size_t>(pt.rows[0].field)] == "Product");
  CHECK(pt.fields[static_cast<size_t>(pt.data[0].field)] == "Sales");
  CHECK(t.T("J2") == "A");  // the report moved one column right with the insert
  CHECK(t.N("K2") == 45);
  CHECK(t.d.wb().sheets[0]->slicers->front().field == 4);  // the slicer still filters Qty
}

TEST_CASE("pivot parity H3: dates go to Rows and keep their format; blanks make Count") {
  English en;
  yctest::Doc t;
  t.Set("A1", "Date");
  t.Set("B1", "Sales");
  t.Set("C1", "Qty");
  t.Set("D1", "Region");
  t.Set("A2", "2024-01-05");
  t.Set("A3", "2024-02-10");
  t.Set("B2", "10");
  t.Set("B3", "20");
  t.Set("C2", "1");
  t.Set("D2", "East");
  t.Set("D3", "West");
  REQUIRE(t.V("A2").IsNumeric());
  PivotTable p;
  p.src_sheet = t.sheet();
  p.src = Rect{0, 0, 2, 3};
  p.c0 = 6;
  yc::engine::PivotCache cache;
  std::string err;
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(yc::engine::PivotFieldDefaultArea(cache, 0) == 0);  // dates: Rows
  CHECK(yc::engine::PivotFieldDefaultArea(cache, 1) == 1);  // numbers: Values, Sum
  CHECK(yc::engine::PivotFieldDefaultArea(cache, 2) == 2);  // numbers and a blank: Values, Count
  CHECK(yc::engine::PivotFieldDefaultArea(cache, 3) == 0);  // text: Rows
  p.rows = {Axis(0)};
  p.data = {Data(1)};
  REQUIRE(t.d.AddPivot(t.sheet(), p) != 0);
  CHECK(t.V("G2").IsNumeric());  // the date stays a number
  CHECK(t.T("G2") == t.T("A2"));  // shown in the source's format
  const auto rep = yc::engine::ComputePivot(*t.d.wb().sheets[0]->pivots->begin(), cache);
  CHECK(rep.at(1, 0).numfmt == cache.numfmts[0]);
}

TEST_CASE("pivot parity H4: expand and collapse outer items") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0), Axis(1)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  // compact: East (35), A, B, North, B, West...
  CHECK(t.T("G2") == "East");
  CHECK(t.T("G3") == "A");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { yc::engine::SetPivotItemCollapsed(x.rows[0], "East", true); }));
  CHECK(t.T("G2") == "East");
  CHECK(t.N("H2") == 35);  // its totals on its one row
  CHECK(t.T("G3") == "North");
  // the report has +/- buttons: East collapsed, the others expanded
  yc::engine::PivotCache cache;
  std::string err;
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), Pt(t, id), &cache, &err));
  const auto rep = yc::engine::ComputePivot(Pt(t, id), cache);
  REQUIRE(rep.toggles.size() == 3);
  CHECK(rep.toggles[0].collapsed);
  CHECK(!rep.toggles[1].collapsed);
  // Collapse Entire Field, then expand one
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.rows[0].collapsed.clear();
    x.rows[0].collapse_all = true;
    yc::engine::SetPivotItemCollapsed(x.rows[0], "West", false);
  }));
  CHECK(t.T("G2") == "East");
  CHECK(t.T("G3") == "North");
  CHECK(t.T("G4") == "West");
  CHECK(t.T("G5") == "A");
  CHECK(t.T("G7") == "Grand Total");
  const PivotTable lp = Reload(t);
  CHECK(lp.rows[0].collapse_all);
  CHECK(lp.rows[0].collapsed == std::vector<std::string>{"West"});
}

TEST_CASE("pivot parity H5: sort by values") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  CHECK(t.T("G2") == "East");  // A to Z: East 35, North 7, West 70
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.rows[0].sort_by = 0;
    x.rows[0].descending = true;
  }));
  CHECK(t.T("G2") == "West");
  CHECK(t.T("G3") == "East");
  CHECK(t.T("G4") == "North");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows[0].descending = false; }));
  CHECK(t.T("G2") == "North");
  // by the values of one column item
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.cols = {Axis(1)};
    x.rows[0].sort_col = {"A"};
    x.rows[0].descending = true;
  }));
  CHECK(t.T("G3") == "West");  // A: West 30, East 15, North none
  CHECK(t.T("G4") == "East");
  CHECK(t.T("G5") == "North");
  CHECK(Reload(t).rows[0].sort_col == std::vector<std::string>{"A"});
}

TEST_CASE("pivot parity H1: a growing report never covers a table, and covers data only when allowed") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  t.Set("G4", "999");
  t.d.pivot_overwrite = 1;
  std::string err;
  CHECK(!t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows = {Axis(0)}; }, &err));
  CHECK(t.d.pivot_overwrite_hit);
  CHECK(err.find("There's already data in") != std::string::npos);
  CHECK(t.N("G4") == 999);
  CHECK(Pt(t, id).rows.empty());
  t.d.pivot_overwrite = 0;
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows = {Axis(0)}; }));
  CHECK(t.T("G4") == "West");
  // a table below: refused, whatever the setting
  REQUIRE(t.d.AddTable(t.sheet(), Rect{7, 6, 9, 7}, true, "TableStyleMedium2") != 0);
  err.clear();
  CHECK(!t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows = {Axis(0), Axis(1)}; }, &err));
  CHECK(err == "A PivotTable report cannot overlap a table.");
  CHECK(Pt(t, id).rows.size() == 1);
}

TEST_CASE("pivot parity M2: Show Details lists source rows only") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  p.calc_items = {yc::model::PivotCalcItem{0, "EW", "=East+West"}};
  const uint32_t sh = t.sheet();  // Show Details puts its sheet first
  const uint32_t id = t.d.AddPivot(sh, p);
  REQUIRE(id != 0);
  const Rect out = t.d.PivotById(sh, id)->output;
  const uint32_t ds = t.d.PivotShowDetails(sh, out.r1, out.c1);  // the grand total
  REQUIRE(ds != 0);
  CHECK(t.T("A7", ds) != "");
  CHECK(t.T("A8", ds) == "");  // 6 records and the header: no made-up "EW" rows
  // PivotTable Options > Enable show details off: refused
  REQUIRE(t.d.UpdatePivot(sh, id, [](PivotTable& x) { x.enable_drilldown = false; }));
  std::string err;
  const Rect out2 = t.d.PivotById(sh, id)->output;
  CHECK(t.d.PivotShowDetails(sh, out2.r1, out2.c1, &err) == 0);
  CHECK(!err.empty());
}

TEST_CASE("pivot parity M3: Show Values As with a base field and item") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0), Axis(1)};
  p.data = {Data(2)};
  p.layout = 2;      // tabular: one row per product, totals below
  p.subtotals = 2;
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  // East: A 15, B 20 | North: B 7 | West: A 30, B 40
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.data[0].show_as = yc::model::PivotShowAs::RunningTotal; }));
  CHECK(t.N("I2") == 15);
  CHECK(t.N("I3") == 35);  // restarts in each region
  CHECK(t.N("I5") == 7);
  CHECK(t.N("I7") == 30);
  CHECK(t.N("I8") == 70);
  // Difference From (previous) along Product: the first is blank
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.data[0].show_as = yc::model::PivotShowAs::DifferenceFrom;
    x.data[0].base_field = 1;
    x.data[0].base_item = "(previous)";
  }));
  CHECK(t.V("I2").IsEmpty());
  CHECK(t.N("I3") == 5);
  CHECK(t.N("I8") == 10);
  // % Of item A
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.data[0].show_as = yc::model::PivotShowAs::PercentOf;
    x.data[0].base_item = "A";
  }));
  CHECK(t.N("I2") == 1);
  REQUIRE_THAT(t.N("I8"), Catch::Matchers::WithinAbs(40.0 / 30.0, 1e-12));
  // rank, largest first, among the products of each region
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.data[0].show_as = yc::model::PivotShowAs::RankDescending; }));
  CHECK(t.N("I2") == 2);
  CHECK(t.N("I3") == 1);
  const PivotTable lp = Reload(t);
  CHECK(lp.data[0].show_as == yc::model::PivotShowAs::RankDescending);
  CHECK(lp.data[0].base_field == 1);
  CHECK(lp.data[0].base_item == "A");
}

TEST_CASE("pivot parity M4 M5: several page items, label and value filters") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.pages = {Axis(0)};
  p.rows = {Axis(1)};
  p.data = {Data(2)};
  p.r0 = 0;
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.pages[0].multi_page = true;
    x.pages[0].hidden = {"North"};
  }));
  CHECK(t.T("H1") == "(Multiple Items)");
  CHECK(t.N("H6") == 105);  // the grand total without North
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.pages.clear();
    x.rows = {Axis(0)};
    x.rows[0].value_filter = yc::model::PivotFilter{1, 0, "1", ""};  // Top 1 item by Sum of Sales
  }));
  CHECK(t.T("G4") == "West");
  CHECK(t.T("G5") == "Grand Total");
  CHECK(t.N("H5") == 70);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.rows[0].value_filter = {};
    x.rows[0].label_filter = yc::model::PivotFilter{17, 0, "e", ""};  // begins with "e"
  }));
  CHECK(t.T("G4") == "East");
  CHECK(t.T("G5") == "Grand Total");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.rows[0].label_filter = {};
    x.rows[0].value_filter = yc::model::PivotFilter{9, 0, "30", ""};  // greater than 30
  }));
  CHECK(t.T("G4") == "East");
  CHECK(t.T("G5") == "West");
  const PivotTable lp = Reload(t);
  CHECK(lp.rows[0].value_filter == yc::model::PivotFilter{9, 0, "30", ""});
}

TEST_CASE("pivot parity M6: a slicer filters the field's own items") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(t.d.SetSlicerFilter("PivotTable1", 0, {"North"}));
  CHECK(Pt(t, id).rows[0].hidden == std::vector<std::string>{"North"});
  CHECK(Pt(t, id).slicer_filters.empty());
  // a field off the report: the slicer's own filter
  REQUIRE(t.d.SetSlicerFilter("PivotTable1", 1, {"B"}));
  REQUIRE(Pt(t, id).slicer_filters.size() == 1);
  CHECK(t.N("H4") == 45);  // A only, North hidden
}

TEST_CASE("pivot parity M7: Field Settings: custom subtotals, repeated labels, blank lines, items with no data") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0), Axis(1)};
  p.data = {Data(2)};
  p.layout = 2;
  p.subtotals = 2;
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows[0].repeat_labels = true; }));
  CHECK(t.T("G2") == "East");
  CHECK(t.T("G3") == "East");  // repeated on B's row
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.rows[0].subtotal_funcs = (1u << 0) | (1u << 1);  // Sum and Count
    x.rows[0].caption = "Area";
  }));
  CHECK(t.T("G1") == "Area");
  CHECK(t.T("G4") == "East Sum");
  CHECK(t.N("I4") == 35);
  CHECK(t.T("G5") == "East Count");
  CHECK(t.N("I5") == 3);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.rows[0].subtotal_funcs = 0;
    x.rows[0].blank_line = true;
  }));
  CHECK(t.T("G4") == "East Total");
  CHECK(t.T("G5") == "");  // the blank line
  CHECK(t.T("G6") == "North");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.rows[0].blank_line = false;
    x.rows[1].show_all_items = true;
  }));
  CHECK(t.T("G5") == "North");
  CHECK(t.T("H5") == "A");  // North has no A, shown anyway
  CHECK(t.V("I5").IsEmpty());
  const PivotTable lp = Reload(t);
  CHECK(lp.rows[0].repeat_labels);
  CHECK(lp.rows[0].caption == "Area");
  CHECK(lp.rows[1].show_all_items);
}

TEST_CASE("pivot parity M8 M9: several date periods and groups of chosen items make fields") {
  English en;
  yctest::Doc t;
  t.Set("A1", "Date");
  t.Set("B1", "Sales");
  const char* dates[] = {"2024-01-05", "2024-02-10", "2025-01-03", "2025-03-02"};
  for (int i = 0; i < 4; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), dates[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string((i + 1) * 10));
  }
  PivotTable p;
  p.src_sheet = t.sheet();
  p.src = Rect{0, 0, 4, 1};
  p.c0 = 4;
  p.rows = {Axis(0)};
  p.data = {Data(1)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    yc::model::PivotGroup g;
    g.field = 0;
    g.by = yc::model::PivotGroupBy::YearMonths;
    g.periods = yc::model::kPeriodMonths | yc::model::kPeriodYears;
    g.auto_start = g.auto_end = true;
    x.groups = {g};
  }));
  const auto& pt = Pt(t, id);
  REQUIRE(pt.fields.size() == 3);
  CHECK(pt.fields[2] == "Years");
  REQUIRE(pt.rows.size() == 2);
  CHECK(pt.rows[0].field == 2);  // Years before Date
  CHECK(t.T("E2") == "2024");
  CHECK(t.T("E3") == "Jan");
  CHECK(t.T("E4") == "Feb");
  CHECK(t.T("E5") == "2025");
  // a group of chosen items: "Region2" with Group1 = East, West
  yctest::Doc u;
  Fill(u);
  auto q = Base(u);
  q.rows = {Axis(0)};
  q.data = {Data(2)};
  const uint32_t qid = u.d.AddPivot(u.sheet(), q);
  REQUIRE(u.d.UpdatePivot(u.sheet(), qid, [](PivotTable& x) {
    yc::model::PivotGroup g;
    g.field = 0;
    g.by = yc::model::PivotGroupBy::Items;
    g.named = {{"Group1", {"East", "West"}}};
    x.groups = {g};
  }));
  const auto& qt = *u.d.PivotById(u.sheet(), qid);
  CHECK(qt.fields[4] == "Region2");
  REQUIRE(qt.rows.size() == 2);
  CHECK(u.T("G2") == "Group1");
  CHECK(u.N("H2") == 105);
  CHECK(u.T("G3") == "East");
  CHECK(u.T("G5") == "North");
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(u.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& lg = res.wb.sheets[0]->pivots->front().groups.front();
  CHECK(lg.by == yc::model::PivotGroupBy::Items);
  CHECK(lg.named.front().second == std::vector<std::string>{"East", "West"});
  CHECK(lg.placed);
}

TEST_CASE("pivot parity M10 L2: options: empty cells, field captions, refresh on open, column header") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.cols = {Axis(1)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  // North has no A
  CHECK(t.T("H4") == "");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.show_null = true;
    x.null_string = "0";
  }));
  CHECK(t.V("H4").IsNumeric());
  CHECK(t.N("H4") == 0);
  CHECK(t.T("H1") == "Column Labels");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.layout = 1; }));
  CHECK(t.T("H1") == "Product");  // outline form: the field's name
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.layout = 0;
    x.field_captions = false;
    x.refresh_on_open = true;
  }));
  CHECK(t.T("H1") == "");
  CHECK(t.T("G2") == "");
  // refresh on open: a changed source shows in the reopened file
  t.Set("C2", "1000");
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  CHECK(res.wb.sheets[0]->pivots->front().refresh_on_open);
  yc::engine::Document d2(res.wb);
  const auto& pt = d2.wb().sheets[0]->pivots->front();
  CHECK(d2.GetValue(d2.wb().sheets[0]->id, pt.output.r0 + 2, pt.output.c0 + 1).AsDouble() == 1005);
}

TEST_CASE("pivot parity M11 M1: styles paint the report; the user's formats survive refreshes") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  auto fill = [&](const char* a1) {
    const auto pos = yctest::Doc::P(a1);
    return t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(pos.row, pos.col)).fill_color;
  };
  CHECK(fill("G1") == 0xFFDDEBF7u);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.style = "PivotStyleMedium9"; }));
  const uint32_t medium = fill("G1");
  CHECK(medium != 0xFFDDEBF7u);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) {
    x.style = "PivotStyleLight16";
    x.style_banded_rows = true;
  }));
  CHECK(fill("G3") != fill("G2"));  // every other item row
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.style_banded_rows = false; }));
  // the user fills West's value yellow; a refresh and a layout change keep it on West's row
  const auto w = yctest::Doc::P("H4");
  REQUIRE(t.T("G4") == "West");
  t.d.ModifyStyle(t.sheet(), Rect::Cell(w.row, w.col), [](yc::model::Style& st) { st.fill_color = 0xFFFFFF00; });
  REQUIRE(t.d.RefreshPivot(t.sheet(), id));
  CHECK(fill("H4") == 0xFFFFFF00u);
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.rows[0].descending = true; }));
  CHECK(t.T("G2") == "West");
  CHECK(fill("H2") == 0xFFFFFF00u);
  CHECK(fill("H4") != 0xFFFFFF00u);
  const PivotTable lp = Reload(t);
  CHECK(!lp.formats.empty());
}

TEST_CASE("pivot parity L4 L8: month names in calendar order; the same field twice gets a number") {
  English en;
  yctest::Doc t;
  t.Set("A1", "Month");
  t.Set("B1", "Sales");
  const char* months[] = {"Mar", "Jan", "Feb", "Apple"};
  for (int i = 0; i < 4; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), months[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), "1");
  }
  PivotTable p;
  p.src_sheet = t.sheet();
  p.src = Rect{0, 0, 4, 1};
  p.c0 = 4;
  p.rows = {Axis(0)};
  p.data = {Data(1), Data(1)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  CHECK(t.T("E2") == "Jan");
  CHECK(t.T("E3") == "Feb");
  CHECK(t.T("E4") == "Mar");
  CHECK(t.T("E5") == "Apple");
  CHECK(t.T("F1") == "Sum of Sales");
  CHECK(t.T("G1") == "Sum of Sales2");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](PivotTable& x) { x.custom_lists = false; }));
  CHECK(t.T("E2") == "Apple");
}

TEST_CASE("pivot parity: older files keep loading; new kinds degrade for older readers") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2), Data(2)};
  p.data[0].show_as = yc::model::PivotShowAs::RunningTotal;
  p.data[1].show_as = yc::model::PivotShowAs::Index;
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  const PivotTable lp = Reload(t);
  CHECK(lp.data[0].show_as == yc::model::PivotShowAs::RunningTotal);
  CHECK(lp.data[1].show_as == yc::model::PivotShowAs::Index);
  CHECK(lp.indent == 1);
  CHECK(lp.autofit);
  CHECK(lp.rows[0].sort_by == -1);
}
