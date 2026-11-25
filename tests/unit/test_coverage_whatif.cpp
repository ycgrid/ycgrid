// SPDX-License-Identifier: Apache-2.0
// Coverage of What-If Analysis and Solver (engine/document_whatif.cpp) beyond test_whatif.cpp: every model error Solver
// reports, the right side of a constraint as a cell, a formula or a range, bin / dif / int constraints with Ignore
// Integer, a "Value of" goal, an unbounded LP, references through names and quoted sheet names, scenarios' limits and
// the data table lookup.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.h"

#include "core/i18n.h"

#include <algorithm>

using Catch::Matchers::WithinAbs;
using yc::model::SolverModel;

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

SolverModel Model(const char* objective, const char* changing, int method = 2, int goal = 1) {
  SolverModel m;
  m.objective = objective;
  m.changing = changing;
  m.method = method;
  m.goal = goal;
  return m;
}

}  // namespace

TEST_CASE("Solver coverage: every model error is reported with Excel's code 13 and changes nothing") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("A2", "=A1*2");
  t.Set("B1", "5");
  auto msg = [&](const SolverModel& m) {
    const auto r = t.d.Solve(sh, m);
    CHECK(r.status == 13);
    CHECK(t.N("A1") == 1);
    return r.message;
  };
  CHECK(msg(Model("A2", "Nowhere!A1")) == "The variable cells are not valid references.");
  CHECK(msg(Model("A2", "NoSuchName")) == "The variable cells are not valid references.");
  CHECK(msg(Model("A2", "")) == "The variable cells are not valid references.");
  CHECK(msg(Model("A2", "C1:C201")) == "There must be 1 to 200 variable cells.");
  CHECK(msg(Model("A2", "A1:A2")) == "Variable cells must contain values, not formulas.");
  CHECK(msg(Model("A1:A2", "A1")) == "The objective must be a single cell.");
  CHECK(msg(Model("Nowhere!A9", "A1")) == "The objective must be a single cell.");
  auto with = [&](yc::model::SolverConstraint c) {
    SolverModel m = Model("A2", "A1");
    m.constraints = {c};
    return msg(m);
  };
  CHECK(with({"Nowhere!B1", 1, "3"}) == "A constraint has an invalid reference: Nowhere!B1");
  CHECK(with({"B1", 4, ""}) == "int, bin and dif constraints can only be used on variable cells.");
  CHECK(with({"A1", 1, ""}) == "A constraint has no right side.");
  CHECK(with({"A1", 1, "B1:B3"}) == "The right side of a constraint must be one cell or the same size as the left side.");
  // a protected sheet keeps its locked variable cells
  yctest::Doc p;
  p.Set("A1", "1");
  p.Set("A2", "=A1*2");
  REQUIRE(p.d.ProtectSheet(p.sheet(), yc::model::SheetProtection{}, ""));
  const auto r = p.d.Solve(p.sheet(), Model("A2", "A1"));
  CHECK(r.status == 13);
  CHECK(r.message == "A variable cell is protected.");
}

TEST_CASE("Solver coverage: right sides as a cell, a range and a formula; names and quoted sheet names") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.RenameSheet(sh, "My Data"));
  // max 3x + 2y with x <= C1, y <= C2 (one range against the other), x + y <= =C3*2
  t.Set("A1", "0");
  t.Set("A2", "0");
  t.Set("A3", "=3*A1+2*A2");
  t.Set("A4", "=A1+A2");
  t.Set("C1", "4");
  t.Set("C2", "5");
  t.Set("C3", "3");
  REQUIRE(t.d.AddName("Vars", "='My Data'!$A$1:$A$2"));
  SolverModel m = Model("'My Data'!$A$3", "Vars");
  m.constraints = {{"A1:A2", 1, "C1:C2"}, {"A4", 1, "=C3*2"}};
  auto r = t.d.Solve(sh, m);
  CHECK(r.status == 0);
  CHECK_THAT(t.N("A1"), WithinAbs(4, 1e-6));
  CHECK_THAT(t.N("A2"), WithinAbs(2, 1e-6));
  CHECK_THAT(r.objective, WithinAbs(16, 1e-6));
  REQUIRE(r.original.size() == 2);
  CHECK(r.original[0] == 0);
  // one cell on the right of a range: both variables at most C2; a lower bound and an equality
  t.Set("A1", "0");
  t.Set("A2", "0");
  m.changing = "$A$1,$A$2";
  m.constraints = {{"A1:A2", 1, "C3"}, {"A2", 3, "1"}, {"A1", 2, "2"}};
  r = t.d.Solve(sh, m);
  CHECK(r.status == 0);
  CHECK_THAT(t.N("A1"), WithinAbs(2, 1e-6));
  CHECK_THAT(t.N("A2"), WithinAbs(3, 1e-6));
}

TEST_CASE("Solver coverage: Value Of, minimize, unbounded, binary and all-different variables") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  // Value Of: 2x + 1 = 11
  t.Set("A1", "0");
  t.Set("A2", "=2*A1+1");
  SolverModel v = Model("A2", "A1", 1, 3);
  v.value = 11;
  auto r = t.d.Solve(sh, v);
  CHECK((r.status == 0 || r.status == 1));
  CHECK_THAT(t.N("A1"), WithinAbs(5, 1e-3));
  // an LP that grows without bound: Excel's "do not converge", the variables put back
  t.Set("A1", "7");
  r = t.d.Solve(sh, Model("A2", "A1", 2, 1));
  CHECK(r.status == 4);
  CHECK(r.message == "The Objective Cell values do not converge.");
  CHECK(t.N("A1") == 7);
  // minimize with a lower bound
  SolverModel lo = Model("A2", "A1", 2, 2);
  lo.constraints = {{"A1", 3, "1.5"}};
  r = t.d.Solve(sh, lo);
  CHECK(r.status == 0);
  CHECK_THAT(t.N("A1"), WithinAbs(1.5, 1e-6));
  // binary: max 3a + 2b + 4c with a + b + c <= 2
  for (const char* c : {"B1", "B2", "B3"}) t.Set(c, "0");
  t.Set("B4", "=3*B1+2*B2+4*B3");
  t.Set("B5", "=B1+B2+B3");
  SolverModel bin = Model("B4", "B1:B3");
  bin.constraints = {{"B5", 1, "2"}, {"B1:B3", 5, ""}};
  r = t.d.Solve(sh, bin);
  CHECK(r.status == 14);
  CHECK(t.N("B4") == 7);
  CHECK(t.N("B1") == 1);
  CHECK(t.N("B2") == 0);
  CHECK(t.N("B3") == 1);
  // Ignore Integer Constraints: the LP's own optimum (a fractional one here)
  t.Set("B1", "0");
  t.Set("B2", "0");
  t.Set("B3", "0");
  bin.constraints = {{"B5", 1, "2.5"}, {"B1:B3", 1, "1"}, {"B1:B3", 4, ""}};
  bin.ignore_integer = true;
  r = t.d.Solve(sh, bin);
  CHECK(r.status == 0);
  CHECK_THAT(t.N("B4"), WithinAbs(8, 1e-6));  // B1 and B3 at 1, B2 at 0.5
  // all different: three cells take 1, 2 and 3; the largest weight gets 3
  for (const char* c : {"C1", "C2", "C3"}) t.Set(c, "1");
  t.Set("C4", "=C1+2*C2+5*C3");
  SolverModel dif = Model("C4", "C1:C3", 3);
  dif.constraints = {{"C1:C3", 6, ""}, {"C1:C3", 1, "3"}, {"C1:C3", 3, "1"}};
  dif.max_iterations = 300;
  r = t.d.Solve(sh, dif);
  CHECK(r.status != 13);
  std::vector<double> got = {t.N("C1"), t.N("C2"), t.N("C3")};
  std::sort(got.begin(), got.end());
  CHECK(got == std::vector<double>{1, 2, 3});
  CHECK(t.N("C3") == 3);
}

TEST_CASE("Scenarios coverage: names, the number of cells, replacing, deleting, unknown names") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  std::string err;
  yc::model::Scenario sc;
  sc.cells = {{0, 0}};
  sc.values = {yc::core::Value::Number(5)};
  CHECK_FALSE(t.d.AddScenario(sh, sc, &err));
  CHECK(err == "Enter a scenario name.");
  sc.name = "Best";
  sc.cells.clear();
  CHECK_FALSE(t.d.AddScenario(sh, sc, &err));
  CHECK(err == "A scenario has 1 to 32 changing cells.");
  for (uint32_t r = 0; r < 33; ++r) sc.cells.push_back({r, 1});
  CHECK_FALSE(t.d.AddScenario(sh, sc, &err));
  sc.cells = {{0, 0}};
  REQUIRE(t.d.AddScenario(sh, sc, &err));
  sc.name = "BEST";  // the same name, any case: replaces it
  sc.values = {yc::core::Value::Number(9)};
  REQUIRE(t.d.AddScenario(sh, sc, &err));
  CHECK(t.d.wb().sheets[0]->scenarios->size() == 1);
  CHECK_FALSE(t.d.ShowScenario(sh, "Worst"));
  REQUIRE(t.d.ShowScenario(sh, "best"));
  CHECK(t.N("A1") == 9);
  CHECK_FALSE(t.d.DeleteScenario(sh, "Worst"));
  CHECK(t.d.DeleteScenario(sh, "Best"));
  CHECK(t.d.wb().sheets[0]->scenarios->empty());
  CHECK(t.d.ScenarioSummary(sh, {}, &err) == 0);
  CHECK_FALSE(t.d.AddScenario(999, sc, &err));
  CHECK_FALSE(t.d.DeleteScenario(999, "Best"));
}

TEST_CASE("Data tables coverage: which data table holds a cell") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "2");
  t.Set("B1", "=A1*10");
  t.Set("D2", "1");
  t.Set("D3", "2");
  t.Set("E1", "=B1");
  std::string err;
  // a one-variable table in D1:E3, its column input cell A1
  const yc::core::CellPos col_in{0, 0};
  REQUIRE(t.d.AddDataTable(sh, yc::core::Rect{0, 3, 2, 4}, nullptr, &col_in, &err));
  CHECK(t.N("E2") == 10);
  CHECK(t.N("E3") == 20);
  yc::model::DataTable dt;
  CHECK(t.d.DataTableAt(sh, 1, 4, &dt));
  CHECK(t.d.DataTableAt(sh, 2, 4, nullptr));
  CHECK_FALSE(t.d.DataTableAt(sh, 5, 5, &dt));
  CHECK_FALSE(t.d.DataTableAt(999, 1, 4, &dt));
}

TEST_CASE("Goal Seek coverage: its refusals, a target already met, no solution and errors on the way") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "3");
  t.Set("A2", "=A1^2+1");
  t.Set("A3", "=A1");
  t.Set("A4", "=SQRT(A1)");
  auto r = t.d.GoalSeek(sh, 0, 0, 5, sh, 0, 0);  // A1 has no formula
  CHECK(r.error == "The Set cell must contain a formula.");
  r = t.d.GoalSeek(sh, 1, 0, 5, sh, 2, 0);  // by changing A3, a formula
  CHECK(r.error == "The By changing cell must contain a value, not a formula.");
  r = t.d.GoalSeek(sh, 1, 0, 5, 999, 0, 0);
  CHECK_FALSE(r.found);
  r = t.d.GoalSeek(sh, 1, 0, 10, sh, 0, 0);  // already there
  CHECK(r.found);
  CHECK(r.iterations == 0);
  CHECK(r.value == 3);
  // x^2 + 1 never reaches 0: the closest value is left, with Excel's message
  r = t.d.GoalSeek(sh, 1, 0, 0, sh, 0, 0);
  CHECK_FALSE(r.found);
  CHECK(r.error == "Goal Seek may not have found a solution.");
  CHECK(t.N("A2") >= 1);
  CHECK_THAT(r.result, WithinAbs(t.N("A2"), 1e-9));
  // from a negative start, SQRT is an error until the search comes back to positive numbers
  t.Set("A1", "-4");
  r = t.d.GoalSeek(sh, 3, 0, 2, sh, 0, 0);
  if (r.found) CHECK_THAT(t.N("A1"), WithinAbs(4, 0.01));
  // a protected changing cell
  yctest::Doc p;
  p.Set("A1", "1");
  p.Set("A2", "=A1*2");
  REQUIRE(p.d.ProtectSheet(p.sheet(), yc::model::SheetProtection{}, ""));
  CHECK(p.d.GoalSeek(p.sheet(), 1, 0, 8, p.sheet(), 0, 0).error == "The changing cell is protected.");
}
