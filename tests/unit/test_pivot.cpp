// SPDX-License-Identifier: Apache-2.0
// PivotTables: grouping, aggregation, layouts, filters, GETPIVOTDATA, saving
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.h"

#include "core/i18n.h"
#include "engine/pivot_engine.h"
#include "io/ycg.h"

using yc::core::Rect;

namespace {
// Region, Product, Sales, Qty in A1:D6
void FillData(yctest::Doc& t) {
  const char* rows[][4] = {{"Region", "Product", "Sales", "Qty"}, {"East", "A", "10", "1"}, {"East", "B", "20", "2"},
                           {"West", "A", "30", "3"},          {"West", "B", "40", "4"}, {"East", "A", "5", "1"}};
  for (int r = 0; r < 6; ++r) {
    for (int c = 0; c < 4; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
  }
}

// the report's labels are localized like Excel's: the tests read them in English
struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

yc::model::PivotTable Base(yctest::Doc& t) {
  yc::model::PivotTable p;
  p.src_sheet = t.sheet();
  p.src = Rect{0, 0, 5, 3};
  p.r0 = 0;
  p.c0 = 5;  // F1
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
}  // namespace

TEST_CASE("pivot: rows x columns with grand totals and GETPIVOTDATA") {
  English en;
  yctest::Doc t;
  FillData(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.cols = {Axis(1)};
  p.data = {Data(2)};
  std::string err;
  const uint32_t id = t.d.AddPivot(t.sheet(), p, &err);
  REQUIRE(id != 0);
  t.d.Recalc();
  const auto* pt = t.d.PivotById(t.sheet(), id);
  REQUIRE(pt != nullptr);
  CHECK(pt->name == "PivotTable1");
  CHECK(pt->output == Rect{0, 5, 4, 8});
  CHECK(t.T("F1") == "Sum of Sales");
  CHECK(t.T("G1") == "Column Labels");
  CHECK(t.T("F2") == "Row Labels");
  CHECK(t.T("G2") == "A");
  CHECK(t.T("I2") == "Grand Total");
  CHECK(t.T("F3") == "East");
  CHECK(t.N("G3") == 15);
  CHECK(t.N("H3") == 20);
  CHECK(t.N("I3") == 35);
  CHECK(t.N("I4") == 70);
  CHECK(t.T("F5") == "Grand Total");
  CHECK(t.N("I5") == 105);
  CHECK(t.Eval("GETPIVOTDATA(\"Sum of Sales\",F1,\"Region\",\"East\",\"Product\",\"A\")") == "15");
  CHECK(t.Eval("GETPIVOTDATA(\"Sales\",$F$1)") == "105");
  CHECK(t.Eval("GETPIVOTDATA(\"Sales\",F1,\"Region\",\"North\")") == "#REF!");
  // a changed source shows after a refresh
  t.Set("C2", "100");
  REQUIRE(t.d.RefreshPivot(t.sheet(), id));
  CHECK(t.N("G3") == 105);
  // filters: a page field, then a hidden item
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) {
    x.pages = {Axis(0)};
    x.pages[0].page_item = "West";
    x.rows.clear();
  }));
  CHECK(t.T("F1") == "Region");
  CHECK(t.T("G1") == "West");
  CHECK(t.N("H5") == 70);  // B, then grand total: West only
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) {
    x.pages.clear();
    x.rows = {Axis(0)};
    x.cols[0].hidden = {"B"};
  }));
  // the body stays where it was when the page field goes (Excel, measured): two rows lower than at F1
  CHECK(t.T("H4") == "Grand Total");
  CHECK(t.N("H7") == 135);
  CHECK(t.d.PivotShowDetails(t.sheet(), 3, 5) == 0);  // a label: no details
  // Show Details: West / A -> one record on a new sheet
  const uint32_t ds = t.d.PivotShowDetails(t.sheet(), 5, 6);
  REQUIRE(ds != 0);
  CHECK(t.T("A1", ds) == "Region");
  CHECK(t.T("A2", ds) == "West");
  CHECK(t.N("C2", ds) == 30);
  CHECK(t.T("A3", ds) == "");
  CHECK(t.d.wb().sheets[0]->id == ds);  // placed before the PivotTable's sheet
}

TEST_CASE("pivot: nested rows, several data fields, layouts and Show Values As") {
  English en;
  yctest::Doc t;
  FillData(t);
  auto p = Base(t);
  p.rows = {Axis(0), Axis(1)};
  p.data = {Data(2), Data(3, yc::model::PivotFunc::Count)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  // compact: one label column, subtotals on the group rows
  CHECK(t.T("F1") == "Row Labels");
  CHECK(t.T("G1") == "Sum of Sales");
  CHECK(t.T("H1") == "Count of Qty");
  CHECK(t.T("F2") == "East");
  CHECK(t.N("G2") == 35);
  CHECK(t.N("H2") == 3);
  CHECK(t.T("F3") == "A");
  CHECK(t.N("G3") == 15);
  CHECK(t.T("F5") == "West");
  CHECK(t.T("F8") == "Grand Total");
  CHECK(t.N("G8") == 105);
  CHECK(t.N("H8") == 5);
  // tabular: a column per row field, subtotals at the bottom
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) {
    x.layout = 2;
    x.subtotals = 2;
    x.data = {Data(2)};
  }));
  CHECK(t.T("F1") == "Region");
  CHECK(t.T("G1") == "Product");
  CHECK(t.T("H1") == "Sum of Sales");
  CHECK(t.T("F2") == "East");
  CHECK(t.T("G2") == "A");
  CHECK(t.N("H2") == 15);
  CHECK(t.T("F3") == "");
  CHECK(t.T("G3") == "B");
  CHECK(t.T("F4") == "East Total");
  CHECK(t.N("H4") == 35);
  CHECK(t.T("F8") == "Grand Total");
  // % of the grand total
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) {
    x.layout = 0;
    x.subtotals = 1;
    x.rows = {Axis(0)};
    x.data[0].show_as = yc::model::PivotShowAs::PercentOfGrandTotal;
  }));
  REQUIRE_THAT(t.N("G2"), Catch::Matchers::WithinAbs(35.0 / 105.0, 1e-12));
  CHECK(t.N("G4") == 1);
}

TEST_CASE("pivot: saved, moved with inserted rows, removed") {
  English en;
  yctest::Doc t;
  FillData(t);
  auto p = Base(t);
  p.r0 = 8;
  p.c0 = 0;  // A9
  p.rows = {Axis(1)};
  p.data = {Data(2, yc::model::PivotFunc::Average)};
  p.data[0].caption = "Avg sales";
  p.data[0].number_format = "0.0";
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  CHECK(t.T("B9") == "Avg sales");
  REQUIRE_THAT(t.N("B10"), Catch::Matchers::WithinAbs(15.0, 1e-12));  // A: (10 + 30 + 5) / 3
  t.d.InsertRows(t.sheet(), 7, 2);
  const auto* pt = t.d.PivotById(t.sheet(), id);
  REQUIRE(pt != nullptr);
  CHECK(pt->r0 == 10);
  CHECK(pt->output.r0 == 10);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets[0]->pivots->size() == 1);
  const auto& lp = res.wb.sheets[0]->pivots->front();
  CHECK(lp.name == "PivotTable1");
  CHECK(lp.r0 == 10);
  CHECK(lp.rows.size() == 1);
  CHECK(lp.data[0].func == yc::model::PivotFunc::Average);
  CHECK(lp.data[0].caption == "Avg sales");
  CHECK(lp.data[0].number_format == "0.0");
  CHECK(lp.fields == std::vector<std::string>{"Region", "Product", "Sales", "Qty"});
  // the report's cells can't be typed over
  CHECK(!t.d.SetInput(t.sheet(), 11, 1, "999"));
  CHECK(t.d.block_pivot());
  REQUIRE_THAT(t.N("B12"), Catch::Matchers::WithinAbs(15.0, 1e-12));
  // clearing part of the report is refused; clearing all of it deletes the PivotTable (pt.TableRange2.Clear())
  const yc::core::Rect area = pt->output;
  t.d.ClearAll(t.sheet(), yc::core::Rect{area.r0, area.c0, area.r0, area.c1});
  CHECK(t.d.wb().sheets[0]->pivots->size() == 1);
  REQUIRE_THAT(t.N("B12"), Catch::Matchers::WithinAbs(15.0, 1e-12));
  t.d.ClearAll(t.sheet(), area);
  CHECK(t.d.wb().sheets[0]->pivots->empty());
  CHECK(t.T("B11") == "");
  CHECK(t.d.SetInput(t.sheet(), 11, 1, "999"));
  CHECK(t.d.wb().sheets[0]->pivots->size() == 0);
  (void)id;
}

TEST_CASE("pivot: grouping dates and numbers, calculated fields") {
  English en;
  yctest::Doc t;
  FillData(t);
  // dates in E: 2026-01-15, 2026-02-03, 2026-02-20, 2027-01-10, 2027-03-31
  t.Set("E1", "Date");
  const char* dates[] = {"2026-01-15", "2026-02-03", "2026-02-20", "2027-01-10", "2027-03-31"};
  for (int i = 0; i < 5; ++i) t.Set(("E" + std::to_string(i + 2)).c_str(), dates[i]);
  REQUIRE(t.V("E2").IsNumeric());
  auto p = Base(t);
  p.src = Rect{0, 0, 5, 4};
  p.c0 = 7;  // H1
  p.rows = {Axis(4)};
  p.data = {Data(2)};
  yc::model::PivotGroup g;
  g.field = 4;
  g.by = yc::model::PivotGroupBy::YearMonths;
  p.groups = {g};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  CHECK(t.T("H2") == "2026-01");
  CHECK(t.N("I2") == 10);
  CHECK(t.T("H3") == "2026-02");
  CHECK(t.N("I3") == 50);
  CHECK(t.T("H5") == "2027-03");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) { x.groups[0].by = yc::model::PivotGroupBy::Years; }));
  CHECK(t.T("H2") == "2026");
  CHECK(t.N("I2") == 60);
  CHECK(t.T("H3") == "2027");
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) { x.groups[0].by = yc::model::PivotGroupBy::Quarters; }));
  CHECK(t.T("H2") == "Qtr1");
  CHECK(t.N("I2") == 105);
  // numbers by ranges of 20
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) {
    x.rows = {Axis(2)};
    x.data = {Data(3, yc::model::PivotFunc::Sum)};
    x.groups = {yc::model::PivotGroup{2, yc::model::PivotGroupBy::Numbers, 0, 100, 20}};
  }));
  CHECK(t.T("H2") == "0-19");
  CHECK(t.N("I2") == 2);  // 10 and 5: Qty 1 + 1
  CHECK(t.T("H3") == "20-39");
  CHECK(t.N("I3") == 5);
  CHECK(t.T("H4") == "40-59");
  // a calculated field: Sales / Qty on the sums
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) {
    x.groups.clear();
    x.rows = {Axis(0)};
    x.calc_fields = {{"Unit", "=Sales/Qty"}};
    x.data = {Data(5)};  // the first field after the source's five
  }));
  CHECK(t.T("I1") == "Sum of Unit");
  REQUIRE_THAT(t.N("I2"), Catch::Matchers::WithinAbs(35.0 / 4.0, 1e-12));
  REQUIRE_THAT(t.N("I4"), Catch::Matchers::WithinAbs(105.0 / 11.0, 1e-12));
  CHECK(t.Eval("GETPIVOTDATA(\"Unit\",H1,\"Region\",\"West\")") == "10");
  std::string err;
  CHECK(yc::engine::CheckPivotFormula({"Sales", "Qty"}, "=Sales*2+'Qty'", &err));
  CHECK(!yc::engine::CheckPivotFormula({"Sales", "Qty"}, "=Sales/Price", &err));
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& lp = res.wb.sheets[0]->pivots->front();
  REQUIRE(lp.calc_fields.size() == 1);
  CHECK(lp.calc_fields[0].formula == "=Sales/Qty");
  // Show Details lists the source's columns only (not the calculated field)
  const uint32_t ds = t.d.PivotShowDetails(t.sheet(), 1, 8);
  REQUIRE(ds != 0);
  CHECK(t.T("E1", ds) == "Date");
  CHECK(t.T("F1", ds) == "");
}

TEST_CASE("pivot: PivotCharts and slicers follow the report") {
  English en;
  yctest::Doc t;
  FillData(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.cols = {Axis(1)};
  p.data = {Data(2)};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  // a PivotChart: one series per column item, the row items as categories
  yc::model::ChartObj ch;
  ch.pivot = "PivotTable1";
  const uint32_t cid = t.d.AddChart(t.sheet(), ch);
  t.d.UpdatePivotCharts();
  auto chart = [&]() -> const yc::model::ChartObj& {
    const auto& charts = *t.d.wb().sheets[0]->charts;
    auto it = std::find_if(charts.begin(), charts.end(), [&](const auto& c) { return c.id == cid; });
    REQUIRE(it != charts.end());
    return *it;
  };
  REQUIRE(chart().series.size() == 2);
  CHECK(chart().series[0].name == "A");
  REQUIRE(chart().series[0].data);
  CHECK(chart().series[0].data->cats == std::vector<std::string>{"East", "West"});
  CHECK(chart().series[1].data->y == std::vector<double>{20, 40});
  // a slicer on Region: only West
  yc::model::Slicer sl;
  sl.pivot = "PivotTable1";
  sl.field = 0;
  sl.caption = "Region";
  const uint32_t sid = t.d.AddSlicer(t.sheet(), sl);
  REQUIRE(sid != 0);
  REQUIRE(t.d.SetSlicerFilter("PivotTable1", 0, {"East"}));
  CHECK(t.T("F3") == "West");
  CHECK(t.N("I4") == 70);  // the grand total: West only
  CHECK(chart().series[0].data->cats == std::vector<std::string>{"West"});
  // renaming the PivotTable keeps the chart and the slicer linked
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [](yc::model::PivotTable& x) { x.name = "Sales"; }));
  CHECK(chart().pivot == "Sales");
  CHECK(t.d.wb().sheets[0]->slicers->front().pivot == "Sales");
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets[0]->slicers->size() == 1);
  CHECK(res.wb.sheets[0]->slicers->front().caption == "Region");
  // the slicer filters the field on the report: one filter, the field's own
  CHECK(res.wb.sheets[0]->pivots->front().rows.front().hidden == std::vector<std::string>{"East"});
  CHECK(res.wb.sheets[0]->pivots->front().slicer_filters.empty());
  // a loaded workbook rebuilds its PivotCharts
  yc::engine::Document d2(res.wb);
  const auto& c2 = d2.wb().sheets[0]->charts->front();
  CHECK(c2.pivot == "Sales");
  REQUIRE(!c2.series.empty());
  REQUIRE(c2.series[0].data);
  CHECK(c2.series[0].data->cats == std::vector<std::string>{"West"});
  // deleting the slicer keeps the field's filter (as Excel does); Clear Filter brings East back
  REQUIRE(t.d.DeleteSlicer(t.sheet(), sid));
  CHECK(t.T("F3") == "West");
  REQUIRE(t.d.SetSlicerFilter("Sales", 0, {}));
  CHECK(t.N("I5") == 105);
}

TEST_CASE("pivot: calculated items add their items' values; totals include them") {
  English en;
  yctest::Doc t;
  FillData(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  p.calc_items.push_back(yc::model::PivotCalcItem{0, "Both", "=East+West*0.5"});
  std::string err;
  const uint32_t id = t.d.AddPivot(t.sheet(), p, &err);
  REQUIRE(id != 0);
  // East 35, West 70, Both = 35 + 35 = 70 (after the field's own items), grand total 175
  CHECK(t.T("F2") == "East");
  CHECK(t.T("F3") == "West");
  CHECK(t.T("F4") == "Both");
  CHECK(t.N("G4") == 70);
  CHECK(t.N("G5") == 175);
  std::vector<std::pair<std::string, double>> terms;
  CHECK(yc::engine::ParseCalcItem({"East", "West", "North East"}, "='North East'*2 - West/4", &terms, &err));
  REQUIRE(terms.size() == 2);
  CHECK(terms[0].second == 2);
  CHECK(terms[1].second == -0.25);
  CHECK_FALSE(yc::engine::ParseCalcItem({"East", "West"}, "=East*West", &terms, &err));
  CHECK_FALSE(yc::engine::ParseCalcItem({"East", "West"}, "=South", &terms, &err));
  // saved and loaded
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& lp = (*res.wb.sheets[0]->pivots)[0];
  REQUIRE(lp.calc_items.size() == 1);
  CHECK(lp.calc_items[0].formula == "=East+West*0.5");
}

TEST_CASE("pivot: a timeline keeps the records of its date range") {
  English en;
  yctest::Doc t;
  t.Set("A1", "Date");
  t.Set("B1", "Sales");
  // 2024-01-15, 2024-02-10, 2024-03-05 as serials
  t.Set("A2", "45306");
  t.Set("B2", "10");
  t.Set("A3", "45332");
  t.Set("B3", "20");
  t.Set("A4", "45356");
  t.Set("B4", "40");
  yc::model::PivotTable p;
  p.src_sheet = t.sheet();
  p.src = Rect{0, 0, 3, 1};
  p.r0 = 0;
  p.c0 = 4;  // E1
  p.data = {Data(1)};
  std::string err;
  const uint32_t id = t.d.AddPivot(t.sheet(), p, &err);
  REQUIRE(id != 0);
  const std::string name = t.d.PivotById(t.sheet(), id)->name;
  yc::model::Slicer tl;
  tl.pivot = name;
  tl.field = 0;
  tl.timeline = true;
  tl.level = 1;
  const uint32_t sid = t.d.AddSlicer(t.sheet(), tl);
  REQUIRE(sid != 0);
  CHECK(t.N("E2") == 70);
  REQUIRE(t.d.SetTimelineFilter(name, 0, 45323, 45351));  // February 2024
  CHECK(t.N("E2") == 20);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& sh = *res.wb.sheets[0];
  REQUIRE(sh.slicers->size() == 1);
  CHECK((*sh.slicers)[0].timeline);
  CHECK((*sh.slicers)[0].level == 1);
  REQUIRE((*sh.pivots)[0].date_filters.size() == 1);
  CHECK((*sh.pivots)[0].date_filters[0].to == 45351);
  // removing the timeline clears its filter
  REQUIRE(t.d.DeleteSlicer(t.sheet(), sid));
  CHECK(t.N("E2") == 70);
}
