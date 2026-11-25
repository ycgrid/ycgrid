// SPDX-License-Identifier: Apache-2.0
// Round 3 (data): the Data tab's tools working like Excel's - Subtotal, Sort Options and Sort On icon, Consolidate
// links and its kept references, Scenario Merge and PivotTable report, Solver Options and reports, Forecast Sheet,
// From Text/CSV and the Flash Fill preview
#include <catch2/catch_test_macros.hpp>

#include "core/i18n.h"
#include "io/ycg.h"
#include "test_helpers.h"

#include <cmath>

using yc::core::Rect;
using yc::core::Value;
namespace engine = yc::engine;
namespace model = yc::model;
namespace io = yc::io;

namespace {
Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}
std::string Str(yctest::Doc& t, const char* a1, uint32_t sh = 0) { return t.V(a1, sh).IsString() ? std::string(t.V(a1, sh).str()) : t.T(a1, sh); }

void Region(yctest::Doc& t) {
  const char* region[] = {"East", "East", "West", "West", "West"};
  const int sales[] = {10, 20, 5, 6, 7};
  t.Set("A1", "Region");
  t.Set("B1", "Sales");
  for (int i = 0; i < 5; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), region[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(sales[i]));
  }
}
}  // namespace

namespace {
// the labels Subtotal writes are UI text: read them in English (the machine may run in Chinese)
struct English {
  yc::i18n::Lang was = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(was); }
};
}  // namespace

TEST_CASE("Round3 data: Subtotal inserts a total row per group, a grand total and the outline") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  Region(t);
  std::string err;
  REQUIRE(t.d.Subtotal(sh, R("A1:B6"), 0, 9, {1}, true, false, true, &err));
  t.d.Recalc();
  CHECK(Str(t, "A4") == "East Total");
  CHECK(t.F("B4") == "=SUBTOTAL(9,B2:B3)");
  CHECK(t.N("B4") == 30);
  CHECK(Str(t, "A8") == "West Total");
  CHECK(t.N("B8") == 18);
  CHECK(Str(t, "A9") == "Grand Total");
  CHECK(t.N("B9") == 48);
  const auto* s = t.d.S(sh);
  CHECK(s->rows->Level(1) == 2);  // detail
  CHECK(s->rows->Level(3) == 1);  // East Total
  CHECK(s->rows->Level(8) == 0);  // Grand Total
  // Replace current subtotals: the old rows go, the new function's come
  REQUIRE(t.d.Subtotal(sh, R("A1:B9"), 0, 1, {1}, true, false, true, &err));
  t.d.Recalc();
  CHECK(Str(t, "A4") == "East Average");
  CHECK(t.N("B4") == 15);
  CHECK(Str(t, "A9") == "Grand Average");
  // Remove All
  CHECK(t.d.RemoveSubtotal(sh, R("A1:B9")) == 3);
  CHECK(Str(t, "A4") == "West");
  CHECK(t.V("A7").IsEmpty());
  CHECK(t.d.S(sh)->rows->Level(1) == 0);
}

TEST_CASE("Round3 data: Subtotal with summaries above, page breaks and nested groups") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  Region(t);
  std::string err;
  REQUIRE(t.d.Subtotal(sh, R("A1:B6"), 0, 9, {1}, true, true, false, &err));
  t.d.Recalc();
  CHECK(Str(t, "A2") == "Grand Total");
  CHECK(t.N("B2") == 48);
  CHECK(Str(t, "A3") == "East Total");
  CHECK(t.N("B3") == 30);
  CHECK(Str(t, "A6") == "West Total");
  CHECK(t.N("B6") == 18);
  CHECK_FALSE(t.d.S(sh)->outline_below);
  const auto& breaks = t.d.S(sh)->page_setup->row_breaks;
  REQUIRE(breaks.size() == 1);
  CHECK(breaks[0] == 5);  // the West group (its summary row 6) starts a page
  // a table can't take subtotals
  yctest::Doc t2;
  Region(t2);
  REQUIRE(t2.d.AddTable(t2.sheet(), R("A1:B6"), true, "TableStyleMedium2"));
  CHECK_FALSE(t2.d.Subtotal(t2.sheet(), R("A1:B6"), 0, 9, {1}, true, false, true, &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("Round3 data: Sort Options - case sensitive and left to right") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "B");
  t.Set("A2", "a");
  t.Set("A3", "A");
  t.Set("A4", "b");
  t.d.Sort(sh, R("A1:A4"), {engine::SortKey{0, false}}, false, true, true, false);
  CHECK(Str(t, "A1") == "a");
  CHECK(Str(t, "A2") == "A");
  CHECK(Str(t, "A3") == "b");
  CHECK(Str(t, "A4") == "B");
  // left to right: the columns move, ordered by row 1
  t.Set("C1", "3");
  t.Set("D1", "1");
  t.Set("E1", "2");
  t.Set("C2", "c");
  t.Set("D2", "a");
  t.Set("E2", "b");
  t.d.Sort(sh, R("C1:E2"), {engine::SortKey{0, false}}, false, true, false, true);
  CHECK(t.N("C1") == 1);
  CHECK(Str(t, "C2") == "a");
  CHECK(t.N("E1") == 3);
  CHECK(Str(t, "E2") == "c");
}

TEST_CASE("Round3 data: Sort On Conditional Formatting Icon puts that icon's rows on top") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const int v[] = {50, 90, 10, 95, 5};
  for (int i = 0; i < 5; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string(v[i]));
  model::CfRule rule;
  rule.type = model::CfType::IconSet;
  rule.ranges = {R("A1:A5")};
  rule.icon_style = 1;  // 3 arrows
  t.d.AddCondFormat(sh, rule);
  t.d.Recalc();
  engine::SortKey k{0, false};
  k.sort_on = 3;
  k.icon_set = 1;
  k.icon = 2;  // the highest icon (green up arrow)
  t.d.Sort(sh, R("A1:A5"), {k}, false);
  CHECK(t.N("A1") == 90);
  CHECK(t.N("A2") == 95);
  CHECK(t.N("A3") == 50);
}

TEST_CASE("Round3 data: Consolidate creates links to the source data and keeps its references") {
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  const uint32_t s2 = t.d.AddSheet("Two");
  const uint32_t out = t.d.AddSheet("Out");
  t.Set("A1", "x", s1);
  t.Set("B1", "10", s1);
  t.Set("A1", "x", s2);
  t.Set("B1", "5", s2);
  std::string err;
  // links can't point at the destination's own sheet
  CHECK_FALSE(t.d.Consolidate(s1, 5, 5, {{s1, R("A1:B1")}, {s2, R("A1:B1")}}, 0, false, true, &err, true));
  REQUIRE(t.d.Consolidate(out, 0, 0, {{s1, R("A1:B1")}, {s2, R("A1:B1")}}, 0, false, true, &err, true));
  t.d.Recalc();
  CHECK(t.F("C1", out) == "=Sheet1!$B$1");
  CHECK(t.F("C2", out) == "=Two!$B$1");
  CHECK(t.F("C3", out) == "=SUM(C1:C2)");
  CHECK(t.N("C3", out) == 15);
  CHECK(Str(t, "A3", out) == "x");
  CHECK(t.d.S(out)->rows->Level(0) == 1);
  CHECK(t.d.S(out)->rows->IsHidden(0));
  t.Set("B1", "20", s1);
  CHECK(t.N("C3", out) == 25);
  // the dialog's settings and references stay with the sheet (.ycg SHET 81)
  model::ConsolidateSettings co;
  co.func = 2;
  co.left_col = true;
  co.links = true;
  co.refs = {"Sheet1!$A$1:$B$1", "Two!$A$1:$B$1"};
  t.d.M(out)->consolidate.mut() = co;
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const model::Sheet& back = res.wb.sheets[2].get();
  CHECK(*back.consolidate == co);
}

TEST_CASE("Round3 data: Solver Options are saved with the model (.ycg SHET 80)") {
  yctest::Doc t;
  model::SolverModel m;
  m.defined = true;
  m.objective = "$A$1";
  m.changing = "$B$1";
  m.ignore_integer = true;
  m.integer_optimality = 5;
  m.max_subproblems = 300;
  m.convergence = 0.001;
  m.derivatives = 2;
  m.multistart = true;
  m.population = 50;
  m.random_seed = 7;
  m.mutation_rate = 0.2;
  m.max_time_no_improve = 12;
  m.require_bounds = false;
  m.auto_scaling = false;
  t.d.SetSolverModel(t.sheet(), m);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  CHECK(*res.wb.sheets[0]->solver == m);
}

TEST_CASE("Round3 data: Solver reports - Answer, Sensitivity and Limits") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  // max 3x + 2y, x + y <= 4, x <= 3
  t.Set("A1", "x");
  t.Set("A2", "y");
  t.Set("B1", "0");
  t.Set("B2", "0");
  t.Set("B3", "=3*B1+2*B2");
  t.Set("B4", "=B1+B2");
  model::SolverModel m;
  m.defined = true;
  m.objective = "$B$3";
  m.goal = 1;
  m.changing = "$B$1:$B$2";
  m.method = 2;
  m.constraints = {{"$B$4", 1, "4"}, {"$B$1", 1, "3"}};
  const auto r = t.d.Solve(sh, m);
  REQUIRE(r.status == 0);
  CHECK(std::fabs(t.N("B3") - 11) < 1e-6);
  std::string err;
  const auto made = t.d.SolverReports(sh, m, r, true, true, true, &err);
  REQUIRE(made.size() == 3);
  CHECK(t.d.S(made[0])->name == "Answer Report 1");
  CHECK(t.d.S(made[1])->name == "Sensitivity Report 1");
  CHECK(t.d.S(made[2])->name == "Limits Report 1");
  CHECK(std::fabs(t.N("B3", sh) - 11) < 1e-6);  // the solution stays (the reports go before the model's sheet)
  // Answer: the objective's original and final values
  bool found = false;
  const auto* a = t.d.S(made[0]);
  for (uint32_t row = 0; row < 40; ++row) {
    if (a->GetValue(row, 1).IsString() && a->GetValue(row, 1).str() == "$B$3") {
      CHECK(a->GetValue(row, 3).AsDouble() == 0);
      CHECK(std::fabs(a->GetValue(row, 4).AsDouble() - 11) < 1e-6);
      found = true;
    }
  }
  CHECK(found);
  // Sensitivity: B4's shadow price is 2 (one more unit of x + y adds 2), B1's is 1
  const auto* sr = t.d.S(made[1]);
  double price_b4 = -1, price_b1 = -1;
  bool constraints = false;
  for (uint32_t row = 0; row < 40; ++row) {
    const auto& c1 = sr->GetValue(row, 1);
    if (c1.IsString() && c1.str() == "Cell" && sr->GetValue(row, 4).IsString() && sr->GetValue(row, 4).str() == "Shadow Price") constraints = true;
    if (!constraints || !c1.IsString()) continue;
    if (c1.str() == "$B$4") price_b4 = sr->GetValue(row, 4).AsDouble();
    if (c1.str() == "$B$1") price_b1 = sr->GetValue(row, 4).AsDouble();
  }
  CHECK(std::fabs(price_b4 - 2) < 1e-3);
  CHECK(std::fabs(price_b1 - 1) < 1e-3);
}

TEST_CASE("Round3 data: Scenario Merge copies scenarios; the PivotTable report lists the results") {
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  const uint32_t s2 = t.d.AddSheet("Other");
  t.Set("A1", "1", s1);
  t.Set("B1", "=A1*10", s1);
  model::Scenario a;
  a.name = "Low";
  a.cells = {{0, 0}};
  a.values = {Value::Number(2)};
  model::Scenario b = a;
  b.name = "High";
  b.values = {Value::Number(9)};
  std::string err;
  REQUIRE(t.d.AddScenario(s2, a, &err));
  REQUIRE(t.d.AddScenario(s2, b, &err));
  REQUIRE(t.d.AddScenario(s1, a, &err));
  CHECK(t.d.MergeScenarios(s1, s2) == 2);
  REQUIRE(t.d.S(s1)->scenarios->size() == 3);
  CHECK((*t.d.S(s1)->scenarios)[1].name == "Low 2");
  const uint32_t rep = t.d.ScenarioPivotReport(s1, {{0, 1}}, &err);
  REQUIRE(rep);
  CHECK(t.d.S(rep)->name == "Scenario PivotTable");
  CHECK(t.d.S(rep)->pivots->size() == 1);
  CHECK(t.N("B1", s1) == 10);  // the sheet's values come back
}

TEST_CASE("Round3 data: Forecast Sheet builds the table of FORECAST.ETS formulas and a chart") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "Date");
  t.Set("B1", "Sales");
  for (int i = 0; i < 12; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), std::to_string(45000 + i));
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(100 + i * 3 + (i % 3)));
  }
  engine::Document::ForecastRequest rq;
  rq.sheet = sh;
  rq.timeline = R("A1:A13");
  rq.values = R("B1:B13");
  rq.end = 45016;
  rq.statistics = true;
  std::string err;
  const uint32_t out = t.d.ForecastSheet(rq, &err);
  REQUIRE(out);
  t.d.Recalc();
  CHECK(Str(t, "C1", out) == "Forecast(Sales)");
  CHECK(Str(t, "D1", out) == "Lower Confidence Bound(Sales)");
  CHECK(t.N("C13", out) == t.N("B13", out));  // the forecast starts at the last point
  CHECK(t.F("C14", out).rfind("=FORECAST.ETS(A14,$B$2:$B$13,$A$2:$A$13,1,1,1)", 0) == 0);
  CHECK(t.V("C14", out).IsNumber());
  CHECK(t.N("A18", out) == 45016);
  CHECK(t.d.S(out)->charts->size() == 1);
  CHECK(t.d.S(out)->tables->size() == 1);
  CHECK(Str(t, "G2", out) == "Alpha");
  CHECK(t.F("H2", out).rfind("=FORECAST.ETS.STAT(", 0) == 0);
  // Forecast End before the data's end is refused
  rq.end = 45005;
  CHECK_FALSE(t.d.ForecastSheet(rq, &err));
}

TEST_CASE("Round3 data: From Text/CSV rows keep Text columns as text") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const Rect r = t.d.ImportRows(sh, 1, 1, {{"Code", "Qty"}, {"00123", "5"}, {"=1+1", "7.5"}}, {1, 0});
  CHECK(r.r1 == 3);
  CHECK(r.c1 == 2);
  CHECK(Str(t, "B3") == "00123");
  CHECK(t.N("C3") == 5);
  CHECK(Str(t, "B4") == "=1+1");
  CHECK(t.N("C4") == 7.5);
}

TEST_CASE("Round3 data: Flash Fill's dry run gives the suggestions without writing them") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "John Smith");
  t.Set("A2", "Mary Jones");
  t.Set("A3", "Ann Lee");
  t.Set("B1", "John");
  std::vector<std::pair<uint32_t, std::string>> got;
  std::string err;
  CHECK(t.d.FlashFill(sh, 1, 1, &err, &got, true) == 2);
  REQUIRE(got.size() == 2);
  CHECK(got[0].second == "Mary");
  CHECK(got[1].second == "Ann");
  CHECK(t.V("B2").IsEmpty());
}

TEST_CASE("Round3 data: Solver Save Model writes the model into cells and Load Model reads it back") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  model::SolverModel m;
  m.defined = true;
  m.objective = "$B$3";
  m.goal = 2;
  m.changing = "$B$1:$B$2";
  m.method = 2;
  m.constraints = {{"$B$4", 1, "4"}, {"$B$1", 3, "$C$1"}, {"$B$2", 4, ""}};
  m.max_time = 50;
  m.population = 77;
  std::string err;
  const Rect at = t.d.SaveSolverModel(sh, m, 9, 4, &err);
  REQUIRE(at.r1 == 9 + 5);
  CHECK(t.F("E10") == "=MIN($B$3)");
  CHECK(t.F("E11") == "=COUNT($B$1:$B$2)");
  CHECK(t.F("E12") == "=$B$4<=4");
  model::SolverModel back;
  REQUIRE(t.d.LoadSolverModel(sh, at, &back, &err));
  CHECK(back.objective == "$B$3");
  CHECK(back.goal == 2);
  CHECK(back.changing == "$B$1:$B$2");
  REQUIRE(back.constraints.size() == 3);
  CHECK(back.constraints[1] == model::SolverConstraint{"$B$1", 3, "$C$1"});
  CHECK(back.constraints[2].rel == 4);
  CHECK(back.method == 2);
  CHECK(back.max_time == 50);
  CHECK(back.population == 77);
}
