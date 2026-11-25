// SPDX-License-Identifier: Apache-2.0
// Coverage of conditional formatting rules that had no test (rule_eval.cpp): every comparison of Cell Value, Text that
// Ends With, Errors / No Errors, every A Date Occurring period, and color scales, data bars and icon sets whose points
// are percentiles or formulas.
#include <catch2/catch_test_macros.hpp>

#include "engine/rule_eval.h"
#include "test_helpers.h"

using yc::core::Rect;
using yc::model::CfRule;
using yc::model::CfType;
using yc::model::CmpOp;

namespace {

constexpr uint32_t kRed = 0xFFFF0000;

CfRule Fill(CfType type, Rect where) {
  CfRule r;
  r.type = type;
  r.ranges = {where};
  r.fmt.has_fill = true;
  r.fmt.fill_color = kRed;
  return r;
}

// the cells of column `col` (rows 0..n-1) the sheet's rules fill
std::vector<bool> Filled(yctest::Doc& t, uint32_t col, uint32_t n) {
  yc::engine::CondFormatEval ev(t.d.wb(), t.sheet());
  std::vector<bool> out;
  for (uint32_t r = 0; r < n; ++r) {
    yc::engine::CfVisual v;
    ev.Eval(r, col, &v);
    out.push_back(v.has_fill && v.fill == kRed);
  }
  return out;
}

void Numbers(yctest::Doc& t, const char* col) {
  for (int i = 1; i <= 10; ++i) t.Set((std::string(col) + std::to_string(i)).c_str(), std::to_string(i));
}

}  // namespace

TEST_CASE("Cell Value rules: equal, not equal, less, greater or equal, less or equal, not between") {
  const struct {
    CmpOp op;
    const char* f;
    std::vector<bool> want;
  } cases[] = {
      {CmpOp::Equal, "5", {false, false, false, false, true, false, false, false, false, false}},
      {CmpOp::NotEqual, "5", {true, true, true, true, false, true, true, true, true, true}},
      {CmpOp::Less, "3", {true, true, false, false, false, false, false, false, false, false}},
      {CmpOp::GreaterEqual, "9", {false, false, false, false, false, false, false, false, true, true}},
      {CmpOp::LessEqual, "2", {true, true, false, false, false, false, false, false, false, false}},
      {CmpOp::NotBetween, "3", {true, true, false, false, false, false, false, true, true, true}},
  };
  for (const auto& c : cases) {
    yctest::Doc t;
    Numbers(t, "A");
    CfRule r = Fill(CfType::CellValue, Rect{0, 0, 9, 0});
    r.op = c.op;
    r.formula1 = c.f;
    r.formula2 = "7";
    t.d.AddCondFormat(t.sheet(), r);
    CAPTURE(static_cast<int>(c.op));
    CHECK(Filled(t, 0, 10) == c.want);
  }
}

TEST_CASE("Text that ends with (no case), errors and no errors") {
  yctest::Doc t;
  t.Set("B1", "Testing");
  t.Set("B2", "ING");
  t.Set("B3", "ingx");
  t.Set("B5", "=1/0");
  CfRule r = Fill(CfType::TextContains, Rect{0, 1, 4, 1});
  r.text_op = 3;
  r.text = "ing";
  t.d.AddCondFormat(t.sheet(), r);
  CHECK(Filled(t, 1, 5) == std::vector<bool>{true, true, false, false, false});  // blank and error cells: no
  yctest::Doc e;
  e.Set("C1", "=1/0");
  e.Set("C2", "5");
  e.Set("C3", "=NA()");
  e.d.AddCondFormat(e.sheet(), Fill(CfType::Errors, Rect{0, 2, 3, 2}));
  CHECK(Filled(e, 2, 4) == std::vector<bool>{true, false, true, false});
  yctest::Doc n;
  n.Set("C1", "=1/0");
  n.Set("C2", "5");
  n.d.AddCondFormat(n.sheet(), Fill(CfType::NoErrors, Rect{0, 2, 3, 2}));
  CHECK(Filled(n, 2, 4) == std::vector<bool>{false, true, true, true});  // blank cells have no error
}

TEST_CASE("A Date Occurring: every period against today") {
  // one date per period, made from TODAY() so the test holds any day
  const char* dates[] = {"=TODAY()",                     // 0 today
                         "=TODAY()-1",                   // 1 yesterday
                         "=TODAY()-6",                   // 2 last 7 days
                         "=TODAY()-WEEKDAY(TODAY())+1",  // 3 this week (its Sunday)
                         "=TODAY()-WEEKDAY(TODAY())-6",  // 4 last week (its Sunday)
                         "=EOMONTH(TODAY(),-2)+1",       // 5 last month (its first day)
                         "=TODAY()+1",                   // 6 tomorrow
                         "=TODAY()-WEEKDAY(TODAY())+8",  // 7 next week (its Sunday)
                         "=EOMONTH(TODAY(),0)+1",        // 8 next month (its first day)
                         "=EOMONTH(TODAY(),-1)+1"};      // 9 this month (its first day)
  for (int period = 0; period < 10; ++period) {
    yctest::Doc t;
    for (int i = 0; i < 10; ++i) t.Set(("D" + std::to_string(i + 1)).c_str(), dates[i]);
    t.Set("D11", "x");  // text: never a date
    t.d.Recalc();
    CfRule r = Fill(CfType::TimePeriod, Rect{0, 3, 10, 3});
    r.time_period = period;
    t.d.AddCondFormat(t.sheet(), r);
    const std::vector<bool> got = Filled(t, 3, 11);
    CAPTURE(period);
    CHECK(got[static_cast<size_t>(period)]);
    CHECK_FALSE(got[10]);
  }
  // and two that must not match yesterday
  yctest::Doc t;
  for (int i = 0; i < 10; ++i) t.Set(("D" + std::to_string(i + 1)).c_str(), dates[i]);
  t.d.Recalc();
  CfRule y = Fill(CfType::TimePeriod, Rect{0, 3, 9, 3});
  y.time_period = 1;
  t.d.AddCondFormat(t.sheet(), y);
  const std::vector<bool> got = Filled(t, 3, 10);
  CHECK_FALSE(got[0]);  // today
  CHECK_FALSE(got[6]);  // tomorrow
}

TEST_CASE("A color scale whose points are percentiles, percents or formulas") {
  yctest::Doc t;
  Numbers(t, "A");
  CfRule r;
  r.type = CfType::ColorScale;
  r.ranges = {Rect{0, 0, 9, 0}};
  r.points = 3;
  r.point_type[0] = 5, r.point_value[0] = 20;  // 20th percentile: 2.8
  r.point_type[1] = 5, r.point_value[1] = 50;
  r.point_type[2] = 4, r.point_formula[2] = "=8";  // a formula
  t.d.AddCondFormat(t.sheet(), r);
  yc::engine::CondFormatEval ev(t.d.wb(), t.sheet());
  auto fill = [&](uint32_t row) {
    yc::engine::CfVisual v;
    REQUIRE(ev.Eval(row, 0, &v));
    return v.fill;
  };
  CHECK(fill(0) == r.colors[0]);  // 1 and 2 are at or below the 20th percentile
  CHECK(fill(1) == r.colors[0]);
  CHECK(fill(8) == r.colors[2]);  // 9 and 10 are past the formula's 8
  CHECK(fill(9) == r.colors[2]);
  CHECK(fill(4) != r.colors[0]);
  CHECK(fill(4) != r.colors[2]);
  // a 2-point scale from a percent to a formula that isn't a number (its point is the highest value)
  yctest::Doc u;
  Numbers(u, "A");
  CfRule two = r;
  two.points = 2;
  two.point_type[0] = 3, two.point_value[0] = 50;  // 50 percent of 1..10: 5.5
  two.point_type[1] = 4, two.point_formula[1] = "=\"x\"";
  u.d.AddCondFormat(u.sheet(), two);
  yc::engine::CondFormatEval ev2(u.d.wb(), u.sheet());
  yc::engine::CfVisual v1, v5, v10;
  REQUIRE(ev2.Eval(0, 0, &v1));
  REQUIRE(ev2.Eval(4, 0, &v5));
  REQUIRE(ev2.Eval(9, 0, &v10));
  CHECK(v1.fill == v5.fill);  // both at or below 5.5
  CHECK(v10.fill != v1.fill);
}

TEST_CASE("Data bars whose shortest and longest bars are percentiles, percents or formulas") {
  for (int kind = 0; kind < 3; ++kind) {
    yctest::Doc t;
    Numbers(t, "A");
    CfRule r;
    r.type = CfType::DataBar;
    r.ranges = {Rect{0, 0, 9, 0}};
    if (kind == 0) r.bar_min_type = 5, r.bar_min_value = 20, r.bar_max_type = 5, r.bar_max_value = 80;  // 2.8, 8.2
    if (kind == 1) r.bar_min_type = 3, r.bar_min_value = 20, r.bar_max_type = 3, r.bar_max_value = 80;
    if (kind == 2) r.bar_min_type = 4, r.bar_min_formula = "=3", r.bar_max_type = 4, r.bar_max_formula = "=8";
    t.d.AddCondFormat(t.sheet(), r);
    yc::engine::CondFormatEval ev(t.d.wb(), t.sheet());
    std::vector<float> bars;
    for (uint32_t row = 0; row < 10; ++row) {
      yc::engine::CfVisual v;
      REQUIRE(ev.Eval(row, 0, &v));
      bars.push_back(v.bar);
    }
    CAPTURE(kind);
    for (size_t i = 1; i < bars.size(); ++i) CHECK(bars[i] >= bars[i - 1]);
    CHECK(bars[0] == bars[1]);  // at or below the shortest bar's value: the shortest bar
    CHECK(bars[9] == bars[8]);  // at or past the longest one's: the longest
    CHECK(bars[9] > bars[0]);
  }
}

TEST_CASE("Icon sets whose thresholds are percentiles or formulas") {
  yctest::Doc t;
  Numbers(t, "A");
  Numbers(t, "B");
  CfRule p;
  p.type = CfType::IconSet;
  p.icon_style = yc::model::kIcon3Arrows;
  p.ranges = {Rect{0, 0, 9, 0}};
  p.icon_thresholds = {{2, 50, "", true}, {2, 90, "", true}};  // percentiles: 5.5 and 9.1
  t.d.AddCondFormat(t.sheet(), p);
  CfRule f = p;
  f.ranges = {Rect{0, 1, 9, 1}};
  f.icon_thresholds = {{3, 0, "4", true}, {3, 0, "8", false}};  // formulas: >= 4, > 8
  t.d.AddCondFormat(t.sheet(), f);
  yc::engine::CondFormatEval ev(t.d.wb(), t.sheet());
  auto icon = [&](uint32_t row, uint32_t col) {
    yc::engine::CfVisual v;
    REQUIRE(ev.Eval(row, col, &v));
    return static_cast<int>(v.icon);
  };
  CHECK(icon(0, 0) == 0);
  CHECK(icon(4, 0) == 0);  // 5 < 5.5
  CHECK(icon(5, 0) == 1);  // 6
  CHECK(icon(9, 0) == 2);  // 10 >= 9.1
  CHECK(icon(2, 1) == 0);  // 3
  CHECK(icon(3, 1) == 1);  // 4 >= 4
  CHECK(icon(7, 1) == 1);  // 8 is not > 8
  CHECK(icon(8, 1) == 2);  // 9
}
