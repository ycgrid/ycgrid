// SPDX-License-Identifier: Apache-2.0
// Round 2, cells / formatting / formulas / Data tab: engine parts of the fixes
#include <catch2/catch_test_macros.hpp>

#include "engine/navigate.h"
#include "engine/rule_eval.h"
#include "formula/datetime.h"
#include "io/ycg.h"
#include "test_helpers.h"

#include <cmath>

using yc::core::Rect;
namespace engine = yc::engine;
namespace model = yc::model;
namespace io = yc::io;

namespace {
Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}
}  // namespace

TEST_CASE("Round 2 excel-a: Text to Columns reads a column as dates in the order chosen (Excel's MDY ... YDM)") {
  CHECK(engine::TextToColumnsDate("1/2/2026", 3) == "2026-01-02");    // MDY
  CHECK(engine::TextToColumnsDate("1/2/2026", 4) == "2026-02-01");    // DMY
  CHECK(engine::TextToColumnsDate("2026.12.31", 5) == "2026-12-31");  // YMD
  CHECK(engine::TextToColumnsDate("20260131", 5) == "2026-01-31");    // one run of digits
  CHECK(engine::TextToColumnsDate("310126", 4) == "2026-01-31");      // two-digit year
  CHECK(engine::TextToColumnsDate("12-2026-25", 6) == "2026-12-25");  // MYD
  CHECK(engine::TextToColumnsDate("25 2026 12", 7) == "2026-12-25");  // DYM
  CHECK(engine::TextToColumnsDate("2026/25/12", 8) == "2026-12-25");  // YDM
  CHECK(engine::TextToColumnsDate("13/13/2026", 3).empty());          // no 13th month
  CHECK(engine::TextToColumnsDate("2/29/2025", 3).empty());           // not a leap year
  CHECK(engine::TextToColumnsDate("abc", 3).empty());
  CHECK(engine::TextToColumnsDate("1/2/2026", 1).empty());  // General isn't a date order
}

TEST_CASE("Round 2 excel-a: Text to Columns writes Date columns as dates and keeps fields that aren't dates as text") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "31/01/2026,x");
  t.Set("A2", "nodate,y");
  engine::TextToColumnsOptions o;
  o.tab = false;
  o.comma = true;
  o.types = {4, 1};  // DMY, General
  t.d.TextToColumns(sh, R("A1:A2"), 0, 0, o);
  t.d.Recalc();
  REQUIRE(t.V("A1").IsNumber());
  CHECK(t.N("A1") == yc::formula::DateSerial(2026, 1, 31));
  CHECK(t.V("A2").IsString());
  CHECK(std::string(t.V("A2").str()) == "nodate");
  CHECK(std::string(t.V("B1").str()) == "x");
}

TEST_CASE("Round 2 excel-a: Text to Columns Advanced reads other separators and trailing minus signs") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "'1.234,5;7-;abc");
  engine::TextToColumnsOptions o;
  o.tab = false;
  o.semicolon = true;
  o.decimal = ',';
  o.thousands = '.';
  t.d.TextToColumns(sh, R("A1"), 0, 0, o);
  t.d.Recalc();
  REQUIRE(t.V("A1").IsNumber());
  CHECK(t.N("A1") == 1234.5);
  REQUIRE(t.V("B1").IsNumber());
  CHECK(t.N("B1") == -7.0);
  CHECK(std::string(t.V("C1").str()) == "abc");
  // trailing minus off: "7-" stays text
  t.Set("A2", "'7-;1");
  o.trailing_minus = false;
  o.decimal = o.thousands = 0;
  t.d.TextToColumns(sh, R("A2"), 1, 0, o);
  t.d.Recalc();
  CHECK(t.V("A2").IsString());
}

TEST_CASE("Round 2 excel-a: a conditional format's Red Border outlines the cells it meets and is saved in .ycg") {
  yctest::Doc t;
  t.Set("A1", "5");
  t.Set("A2", "1");
  model::CfRule r;
  r.type = model::CfType::CellValue;
  r.op = model::CmpOp::Greater;
  r.formula1 = "3";
  r.fmt.border_color = 0xFFFF0000;
  r.ranges = {R("A1:A2")};
  t.d.AddCondFormat(t.sheet(), r);
  engine::CondFormatEval ev(t.d.wb(), t.sheet());
  engine::CfVisual v1, v2;
  CHECK(ev.Eval(0, 0, &v1));
  CHECK(v1.border == 0xFFFF0000u);
  ev.Eval(1, 0, &v2);
  CHECK(v2.border == 0u);
  const std::string bytes = io::SaveYcgToBytes(t.d.wb(), {});
  auto res = io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets[0]->cond_formats->size() == 1);
  CHECK((*res.wb.sheets[0]->cond_formats)[0].fmt.border_color == 0xFFFF0000u);
  CHECK((*res.wb.sheets[0]->cond_formats)[0] == (*t.d.wb().sheets[0]->cond_formats)[0]);
}

TEST_CASE("Round 2 excel-a: Series > Trend fits a line (or a growth curve) through the starting values") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("A2", "3");
  t.Set("A3", "4");
  REQUIRE(t.d.FillTrend(sh, R("A1:A5"), true, false));
  // least squares through (1,1) (2,3) (3,4): y = 1.5x - 1/3
  CHECK(std::fabs(t.N("A1") - (1.5 - 1.0 / 3)) < 1e-9);
  CHECK(std::fabs(t.N("A5") - (7.5 - 1.0 / 3)) < 1e-9);
  t.Set("B1", "2");
  t.Set("B2", "4");
  REQUIRE(t.d.FillTrend(sh, R("B1:B4"), true, true));
  CHECK(std::fabs(t.N("B4") - 16.0) < 1e-9);
  t.Set("C1", "7");
  CHECK_FALSE(t.d.FillTrend(sh, R("C1:C3"), true, false));  // one value has no trend
}

TEST_CASE("Round 2 excel-a: Go To Special row / column differences") {
  yctest::Doc t;
  // A: 1 1 1 ; B: 1 2 1 ; C: =A1 copied down (same formula), D: =A1 then =B2 (different)
  t.Set("A1", "1"), t.Set("B1", "1"), t.Set("C1", "=A1"), t.Set("D1", "=A1");
  t.Set("A2", "1"), t.Set("B2", "2"), t.Set("C2", "=A2"), t.Set("D2", "=B2");
  const auto* sh = t.d.S(t.sheet());
  // rows against column A: B2 differs (2 vs 1); the formulas differ from the values in A
  auto rows = engine::DifferenceCells(*sh, R("A1:B2"), 0, 0, true);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0] == R("B2"));
  // columns against row 1: C2 is the same formula as C1 (relative), D2 isn't
  auto cols = engine::DifferenceCells(*sh, R("C1:D2"), 0, 2, false);
  REQUIRE(cols.size() == 1);
  CHECK(cols[0] == R("D2"));
}
