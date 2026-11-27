// SPDX-License-Identifier: Apache-2.0
// What-If Analysis and data tools: Goal Seek, Data Tables, Scenarios, Consolidate, Advanced Filter, Flash Fill, Solver
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/snapshot.h"
#include "io/ycg.h"
#include "test_helpers.h"

using Catch::Matchers::WithinAbs;
using yc::core::CellPos;
using yc::core::Rect;

TEST_CASE("Goal Seek finds the input value") {
  yctest::Doc t;
  t.Set("A1", "5");
  t.Set("B1", "=A1^2+3");
  const auto r = t.d.GoalSeek(t.sheet(), 0, 1, 52, t.sheet(), 0, 0);
  CHECK(r.found);
  REQUIRE_THAT(t.N("A1"), WithinAbs(7, 1e-3));
  REQUIRE_THAT(t.N("B1"), WithinAbs(52, 1e-3));
  // the set cell needs a formula, the changing cell a value
  CHECK_FALSE(t.d.GoalSeek(t.sheet(), 0, 0, 1, t.sheet(), 0, 1).found);
}

TEST_CASE("Data tables: one and two variables, recalculated, locked") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B1", "0.1");
  // one variable, values down C3:C5, formula in D2
  t.Set("C3", "1");
  t.Set("C4", "2");
  t.Set("C5", "3");
  t.Set("D2", "=B1*10");
  const CellPos col_in{0, 1};
  std::string err;
  REQUIRE(t.d.AddDataTable(sh, Rect{1, 2, 4, 3}, nullptr, &col_in, &err));
  CHECK(t.N("D3") == 10);
  CHECK(t.N("D4") == 20);
  CHECK(t.N("D5") == 30);
  CHECK(t.N("B1") == 0.1);  // the input cell is back
  CHECK(t.F("D4") == "{=TABLE(,B1)}");
  CHECK_FALSE(t.d.SetInput(sh, 3, 3, "5"));  // part of the table
  CHECK(t.d.block_table());
  // follows the formula
  t.Set("D2", "=B1*100");
  CHECK(t.N("D5") == 300);
  // two variables: F1 * G1 with row values H... and column values
  t.Set("F1", "1");
  t.Set("G1", "1");
  t.Set("H3", "=F1*G1");
  t.Set("I3", "2");
  t.Set("J3", "3");
  t.Set("H4", "5");
  t.Set("H5", "7");
  const CellPos row_in{0, 5}, col_in2{0, 6};
  REQUIRE(t.d.AddDataTable(sh, Rect{2, 7, 4, 9}, &row_in, &col_in2, &err));
  CHECK(t.N("I4") == 10);
  CHECK(t.N("J5") == 21);
  // saved and loaded
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets[0]->data_tables->size() == 2);
  CHECK(res.wb.sheets[0]->data_tables->at(1).has_row);
  // clearing the whole body removes the table
  t.d.ClearContents(sh, Rect{2, 3, 4, 3});
  CHECK(t.d.wb().sheets[0]->data_tables->size() == 1);
}

TEST_CASE("Scenarios: show and summary") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "100");
  t.Set("A2", "=A1*2");
  yc::model::Scenario hi{"High", "", {CellPos{0, 0}}, {yc::core::Value::Number(500)}};
  yc::model::Scenario lo{"Low", "", {CellPos{0, 0}}, {yc::core::Value::Number(10)}};
  std::string err;
  REQUIRE(t.d.AddScenario(sh, hi, &err));
  REQUIRE(t.d.AddScenario(sh, lo, &err));
  REQUIRE(t.d.ShowScenario(sh, "high"));
  CHECK(t.N("A2") == 1000);
  t.Set("A1", "100");
  const uint32_t rep = t.d.ScenarioSummary(sh, {CellPos{1, 0}}, &err);
  REQUIRE(rep);
  t.d.Recalc();
  CHECK(t.d.GetValue(rep, 4, 2).AsDouble() == 100);   // current value of $A$1
  CHECK(t.d.GetValue(rep, 4, 3).AsDouble() == 500);   // High
  CHECK(t.d.GetValue(rep, 6, 4).AsDouble() == 20);    // result $A$2 under Low
  CHECK(t.N("A1", sh) == 100);                        // left as it was
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& sc = *res.wb.SheetById(sh)->scenarios;
  REQUIRE(sc.size() == 2);
  CHECK(sc[1].name == "Low");
  CHECK(sc[1].values[0].AsDouble() == 10);
}

TEST_CASE("Scenario summaries take at most 32 result cells") {
  // Excel 16 (measured 2026-11-20): 32 result cells make a summary, 33 and more fail; the Scenario Manager typed
  // A:XFD as the result cells and collected 17 billion of them
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("B1", "=A1*2");
  std::string err;
  REQUIRE(t.d.AddScenario(sh, yc::model::Scenario{"s1", "", {CellPos{0, 0}}, {yc::core::Value::Number(5)}}, &err));
  std::vector<CellPos> res;
  for (uint32_t r = 0; r < 33; ++r) res.push_back(CellPos{r, 1});
  CHECK(t.d.ScenarioSummary(sh, res, &err) == 0);
  CHECK(err.find("at most 32 result cells") != std::string::npos);
  err.clear();
  CHECK(t.d.ScenarioPivotReport(sh, res, &err) == 0);
  CHECK(err.find("at most 32 result cells") != std::string::npos);
  res.pop_back();
  CHECK(t.d.ScenarioSummary(sh, res, &err) != 0);
}

TEST_CASE("Consolidate by labels") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* a[][2] = {{"", "Q1"}, {"East", "10"}, {"West", "20"}};
  const char* b[][2] = {{"", "Q1"}, {"West", "5"}, {"North", "7"}};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 2; ++c) {
      t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), a[r][c]);
      t.Set((std::string(1, static_cast<char>('D' + c)) + std::to_string(r + 1)).c_str(), b[r][c]);
    }
  }
  std::string err;
  REQUIRE(t.d.Consolidate(sh, 9, 0, {{sh, Rect{0, 0, 2, 1}}, {sh, Rect{0, 3, 2, 4}}}, 0, true, true, &err));
  t.d.Recalc();
  CHECK(t.T("B10") == "Q1");
  CHECK(t.T("A11") == "East");
  CHECK(t.N("B12") == 25);  // West
  CHECK(t.T("A13") == "North");
  CHECK(t.N("B13") == 7);
}

TEST_CASE("Consolidate with every function; a label's cells without numbers stay blank (Excel 16, measured)") {
  // D!A1:C4 and D!F1:H3, as measured; the result's rows a, b, c, d and columns q1, q2, q3
  const char* a[][3] = {{"", "q1", "q2"}, {"a", "1", "2"}, {"b", "3", ""}, {"c", "x", "5"}};
  const char* b[][3] = {{"", "q1", "q3"}, {"a", "10", "7"}, {"d", "4", "8"}};
  // per function: a|q1, b|q1, b|q2, c|q1, c|q2 as Excel showed them ("" = blank)
  const struct {
    int func;
    const char* want[5];
  } cases[] = {
      {0, {"11", "3", "", "", "5"}},     {1, {"2", "1", "", "1", "1"}},    {2, {"5.5", "3", "", "", "5"}},
      {3, {"10", "3", "", "", "5"}},     {4, {"1", "3", "", "", "5"}},     {5, {"10", "3", "", "", "5"}},
      {6, {"2", "1", "", "", "1"}},      {7, {"6.363961031", "#DIV/0!", "", "", "#DIV/0!"}},
      {8, {"4.5", "0", "", "", "0"}},    {9, {"40.5", "#DIV/0!", "", "", "#DIV/0!"}},
      {10, {"20.25", "0", "", "", "0"}},
  };
  for (const auto& k : cases) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 3; ++c) {
        if (*a[r][c]) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), a[r][c]);
        if (r < 3 && *b[r][c]) t.Set((std::string(1, static_cast<char>('F' + c)) + std::to_string(r + 1)).c_str(), b[r][c]);
      }
    }
    std::string err;
    REQUIRE(t.d.Consolidate(sh, 9, 0, {{sh, Rect{0, 0, 3, 2}}, {sh, Rect{0, 5, 2, 7}}}, k.func, true, true, &err));
    t.d.Recalc();
    CAPTURE(k.func);
    REQUIRE(t.T("A11") == "a");
    REQUIRE(t.T("C10") == "q2");
    const char* cells[5] = {"B11", "B12", "C12", "B13", "C13"};
    for (int i = 0; i < 5; ++i) {
      CAPTURE(cells[i]);
      CHECK(t.T(cells[i]) == k.want[i]);
    }
    CHECK(t.T("C14").empty());  // d has no q2 in any source
  }
}

TEST_CASE("Advanced Filter: copy unique matches, filter in place") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* rows[][2] = {{"Name", "Qty"}, {"Apple", "5"}, {"Pear", "12"}, {"Apple", "5"}, {"Plum", "20"}};
  for (int r = 0; r < 5; ++r) {
    t.Set(("A" + std::to_string(r + 1)).c_str(), rows[r][0]);
    t.Set(("B" + std::to_string(r + 1)).c_str(), rows[r][1]);
  }
  t.Set("D1", "Qty");
  t.Set("D2", "<15");
  std::string err;
  REQUIRE(t.d.AdvancedFilter(sh, Rect{0, 0, 4, 1}, sh, Rect{0, 3, 1, 3}, true, sh, Rect::Cell(0, 5), true, &err));
  t.d.Recalc();
  CHECK(t.T("F1") == "Name");
  CHECK(t.T("F2") == "Apple");
  CHECK(t.T("F3") == "Pear");
  CHECK(t.T("F4") == "");  // the second Apple 5 is a duplicate
  REQUIRE(t.d.AdvancedFilter(sh, Rect{0, 0, 4, 1}, sh, Rect{0, 3, 1, 3}, false, sh, Rect{}, false, &err));
  CHECK(t.d.wb().sheets[0]->rows->IsHidden(4));   // Plum 20
  CHECK_FALSE(t.d.wb().sheets[0]->rows->IsHidden(1));
  t.Set("H1", "=SUBTOTAL(9,B2:B5)");
  CHECK(t.N("H1") == 22);
  t.d.ShowAllData(sh);
  CHECK_FALSE(t.d.wb().sheets[0]->rows->IsHidden(4));
}

TEST_CASE("Flash Fill learns from examples") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "John Smith");
  t.Set("A2", "Mary Jones");
  t.Set("A3", "Bob Lee");
  t.Set("B1", "Smith");
  std::string err;
  CHECK(t.d.FlashFill(sh, 0, 1, &err) == 2);
  CHECK(t.T("B2") == "Jones");
  CHECK(t.T("B3") == "Lee");
  t.Set("C1", "J. SMITH");
  CHECK(t.d.FlashFill(sh, 0, 2, &err) == 2);
  CHECK(t.T("C2") == "M. JONES");
  CHECK(t.T("C3") == "B. LEE");
}

TEST_CASE("Flash Fill takes a token at either end over one counted to the middle (Excel 16, measured)") {
  // one example each, the sources of different lengths: Range.FlashFill in Excel 16 gave these
  struct Case {
    std::vector<std::string> src;
    std::string example;
    std::vector<std::string> want;
  };
  const Case cases[] = {
      {{"Taiwan Semi (2330)", "Hon Hai (2317)", "MediaTek (2454)", "Delta Electronics Inc (2308)"}, "2330", {"2317", "2454", "2308"}},
      {{"John Smith", "Mary Ann Lee", "Bob Jones", "Al B Cee Dee"}, "Smith", {"Lee", "Jones", "Dee"}},
      {{"John Smith", "Mary Ann Lee", "Bob Jones", "Al B Cee Dee"}, "John", {"Mary", "Bob", "Al"}},
      {{"a-b-c", "d-e", "f-g-h-i", "j"}, "c", {"e", "i", "j"}},
  };
  for (const Case& k : cases) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    for (size_t i = 0; i < k.src.size(); ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), k.src[i]);
    t.Set("B1", k.example);
    std::string err;
    CHECK(t.d.FlashFill(sh, 0, 1, &err) == k.want.size());
    for (size_t i = 0; i < k.want.size(); ++i) CHECK(t.T(("B" + std::to_string(i + 2)).c_str()) == k.want[i]);
  }
}

TEST_CASE("Solver: linear, nonlinear, integer, evolutionary") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B1", "0");
  t.Set("B2", "0");
  t.Set("B3", "=3*B1+2*B2");
  t.Set("B4", "=B1+B2");
  t.Set("B5", "=B1+3*B2");
  yc::model::SolverModel m;
  m.objective = "$B$3";
  m.goal = 1;
  m.changing = "$B$1:$B$2";
  m.constraints = {{"$B$4", 1, "4"}, {"$B$5", 1, "6"}};
  m.method = 2;
  auto r = t.d.Solve(sh, m);
  CHECK(r.status == 0);
  REQUIRE_THAT(t.N("B3"), WithinAbs(12, 1e-6));
  REQUIRE_THAT(t.N("B1"), WithinAbs(4, 1e-6));
  // the same model with GRG
  t.Set("B1", "0");
  t.Set("B2", "0");
  m.method = 1;
  r = t.d.Solve(sh, m);
  CHECK(r.status == 0);
  REQUIRE_THAT(t.N("B3"), WithinAbs(12, 1e-3));
  // a nonlinear model is refused by Simplex LP
  t.Set("C1", "1");
  t.Set("C2", "1");
  t.Set("C3", "=(C1-3)^2+(C2+1)^2");
  yc::model::SolverModel q;
  q.objective = "C3";
  q.goal = 2;
  q.changing = "C1:C2";
  q.method = 2;
  CHECK(t.d.Solve(sh, q).status == 7);
  q.method = 1;
  r = t.d.Solve(sh, q);
  CHECK(r.status == 0);
  REQUIRE_THAT(t.N("C1"), WithinAbs(3, 1e-3));
  REQUIRE_THAT(t.N("C2"), WithinAbs(0, 1e-3));  // non-negative
  // integer program: max 5x + 4y, 6x + 4y <= 24, x + 2y <= 6
  t.Set("D1", "0");
  t.Set("D2", "0");
  t.Set("D3", "=5*D1+4*D2");
  t.Set("D4", "=6*D1+4*D2");
  t.Set("D5", "=D1+2*D2");
  yc::model::SolverModel ip;
  ip.objective = "D3";
  ip.changing = "D1:D2";
  ip.constraints = {{"D4", 1, "24"}, {"D5", 1, "6"}, {"D1:D2", 4, ""}};
  ip.method = 2;
  r = t.d.Solve(sh, ip);
  CHECK(r.status == 14);
  REQUIRE_THAT(t.N("D3"), WithinAbs(20, 1e-6));
  // infeasible
  yc::model::SolverModel bad = m;
  bad.constraints.push_back({"$B$4", 3, "10"});
  bad.method = 2;
  CHECK(t.d.Solve(sh, bad).status == 5);
  // evolutionary needs bounds; with them it finds the optimum of a bumpy function
  t.Set("E1", "0");
  t.Set("E2", "=-(E1-2)^2+COS(E1*6)");
  yc::model::SolverModel ev;
  ev.objective = "E2";
  ev.changing = "E1";
  ev.method = 3;
  CHECK(t.d.Solve(sh, ev).status == 13);
  ev.constraints = {{"E1", 1, "4"}};
  ev.max_iterations = 200;
  r = t.d.Solve(sh, ev);
  CHECK(r.status == 1);
  CHECK(t.N("E2") > 0.9);
}
