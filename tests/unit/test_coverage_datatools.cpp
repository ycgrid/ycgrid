// SPDX-License-Identifier: Apache-2.0
// Coverage of the Data tab's tools (engine/document_datatools.cpp) beyond the other tests: Solver's reports for a
// nonlinear model, with integer constraints, with a right side on another sheet and with nothing to report; the
// Scenario PivotTable report's refusals; a Forecast Start too early; Subtotal with Varp; Consolidate's settings kept.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "core/i18n.h"

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

// is this text (or, with part, a cell holding it) anywhere in the first 60 x 12 cells of the sheet
bool SheetHas(yctest::Doc& t, uint32_t sheet, const std::string& text, bool part = false) {
  for (uint32_t r = 0; r < 60; ++r) {
    for (uint32_t c = 0; c < 12; ++c) {
      const auto& v = t.d.GetValue(sheet, r, c);
      if (v.IsString() && (part ? v.str().find(text) != std::string_view::npos : v.str() == text)) return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("Data tools coverage: Solver reports for a nonlinear model, integers, another sheet's right side") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t other = t.d.AddSheet("Limits Data");
  t.Set("A1", "4", other);
  t.Set("C1", "1");
  t.Set("C2", "1");
  t.Set("C3", "=-(C1-3)^2-(C2-1)^2");
  t.Set("C4", "=C1+C2");
  yc::model::SolverModel m;
  m.objective = "C3";
  m.goal = 1;
  m.changing = "C1:C2";
  m.method = 1;
  m.constraints = {{"C4", 1, "'Limits Data'!$A$1"}, {"C1", 3, "0.5"}};
  const auto r = t.d.Solve(sh, m);
  REQUIRE((r.status == 0 || r.status == 1));
  std::string err;
  const auto made = t.d.SolverReports(sh, m, r, true, true, true, &err);
  REQUIRE(made.size() == 3);
  CHECK(SheetHas(t, made[1], "Reduced Gradient"));  // GRG's sensitivity report
  CHECK(SheetHas(t, made[1], "Lagrange Multiplier"));
  CHECK(SheetHas(t, made[0], "'Limits Data'!$A$1", true));  // the right side on another sheet, named with its sheet
  // integer constraints: no sensitivity or limits report, with Excel's reason
  yc::model::SolverModel ip = m;
  ip.method = 2;
  ip.objective = "C4";
  ip.constraints = {{"C4", 1, "3"}, {"C1:C2", 4, ""}};
  const auto ri = t.d.Solve(sh, ip);
  REQUIRE(ri.status == 14);
  err.clear();
  const auto only = t.d.SolverReports(sh, ip, ri, true, true, true, &err);
  CHECK(only.size() == 1);
  CHECK(err == "Sensitivity and Limits Reports are not meaningful for problems with integer constraints.");
  // nothing solved: nothing to report
  yc::engine::Document::SolverResult bad;
  bad.status = 5;
  err.clear();
  CHECK(t.d.SolverReports(sh, m, bad, true, false, false, &err).empty());
  CHECK(err == "Solver didn't find a solution: there is nothing to report.");
}

TEST_CASE("Data tools coverage: Scenario PivotTable report refusals, an early Forecast Start, Subtotal with Varp") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  std::string err;
  CHECK(t.d.ScenarioPivotReport(sh, {{0, 1}}, &err) == 0);
  CHECK(err == "The sheet has no scenarios.");
  yc::model::Scenario sc;
  sc.name = "Hi";
  sc.cells = {{0, 0}};
  sc.values = {yc::core::Value::Number(5)};
  REQUIRE(t.d.AddScenario(sh, sc, &err));
  CHECK(t.d.ScenarioPivotReport(sh, {}, &err) == 0);
  CHECK(err.rfind("Enter the result cells", 0) == 0);
  // a forecast needs three points before its start
  t.Set("E1", "Day");
  t.Set("F1", "Sales");
  for (int i = 1; i <= 6; ++i) {
    t.Set(("E" + std::to_string(i + 1)).c_str(), std::to_string(i));
    t.Set(("F" + std::to_string(i + 1)).c_str(), std::to_string(i * 10));
  }
  yc::engine::Document::ForecastRequest rq;
  rq.sheet = sh;
  rq.timeline = yc::core::Rect{0, 4, 6, 4};
  rq.values = yc::core::Rect{0, 5, 6, 5};
  rq.end = 9;
  rq.start = 2;
  CHECK(t.d.ForecastSheet(rq, &err) == 0);
  CHECK(err == "Forecast Start must leave at least 3 points of data before it.");
  CHECK(t.d.wb().sheets.size() == 1);  // a refused forecast leaves no empty sheet before the data's
  CHECK(t.d.wb().sheets[0]->id == sh);
  // Subtotal by VarP: its labels
  yctest::Doc u;
  u.Set("A1", "k");
  u.Set("B1", "v");
  const char* keys[] = {"a", "a", "b", "b"};
  for (int i = 0; i < 4; ++i) {
    u.Set(("A" + std::to_string(i + 2)).c_str(), keys[i]);
    u.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(i + 1));
  }
  err.clear();
  INFO(err);
  REQUIRE(u.d.Subtotal(u.sheet(), yc::core::Rect{0, 0, 4, 1}, 0, 11, {1}, true, false, true, &err));
  CHECK(SheetHas(u, u.sheet(), "Grand Varp"));
  CHECK(SheetHas(u, u.sheet(), "a Varp"));
}

TEST_CASE("Data tools coverage: Consolidate's settings are kept with the sheet") {
  yctest::Doc t;
  yc::model::ConsolidateSettings co;
  co.func = 3;
  co.top_row = true;
  co.refs = {"Sheet1!$A$1:$B$4"};
  t.d.SetConsolidateSettings(t.sheet(), co);
  CHECK(*t.d.S(t.sheet())->consolidate == co);
  t.d.SetConsolidateSettings(t.sheet(), co);  // the same: nothing changes
  t.d.SetConsolidateSettings(999, co);
  CHECK(t.d.S(t.sheet())->consolidate->func == 3);
}
