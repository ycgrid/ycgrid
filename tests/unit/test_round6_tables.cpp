// SPDX-License-Identifier: Apache-2.0
// Round 6 (tables): the fill handle across moves table column names along; the totals row names its own columns
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

using yc::core::Rect;

namespace {
// Sales: Region, Jan, Feb, Mar in A1:D3
uint32_t MonthTable(yctest::Doc& t) {
  t.Set("A1", "Region");
  t.Set("B1", "Jan");
  t.Set("C1", "Feb");
  t.Set("D1", "Mar");
  t.Set("A2", "North");
  t.Set("B2", "1");
  t.Set("C2", "10");
  t.Set("D2", "100");
  t.Set("A3", "South");
  t.Set("B3", "2");
  t.Set("C3", "20");
  t.Set("D3", "200");
  std::string err;
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 2, 3}, true, "TableStyleMedium2", &err);
  REQUIRE(t.d.RenameTable(t.sheet(), id, "Sales", &err));
  t.d.Recalc();
  return id;
}
}  // namespace

TEST_CASE("Round6: the fill handle across moves table column names along, as in Excel") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  MonthTable(t);
  t.Set("B6", "=SUM(Sales[Jan])");
  t.d.AutoFill(sh, Rect::Cell(5, 1), Rect{5, 1, 5, 4});  // B6 -> C6:E6
  t.d.Recalc();
  CHECK(t.F("C6") == "=SUM(Sales[Feb])");
  CHECK(t.F("D6") == "=SUM(Sales[Mar])");
  CHECK(t.N("D6") == 300);
  CHECK(t.F("E6") == "=SUM(#REF!)");  // past the last column
  // to the left too, and a range of columns moves as a whole
  t.Set("E8", "=SUM(Sales[[Feb]:[Mar]])");
  t.d.AutoFill(sh, Rect::Cell(7, 4), Rect{7, 3, 7, 4});  // E8 -> D8
  CHECK(t.F("D8") == "=SUM(Sales[[Jan]:[Feb]])");
  // copying, pasting and Fill Right (Ctrl+R) keep the names
  t.d.Paste(sh, 9, 2, t.d.Copy(sh, Rect::Cell(5, 1)));  // B6 -> C10
  CHECK(t.F("C10") == "=SUM(Sales[Jan])");
  t.Set("B11", "=SUM(Sales[Jan])");
  t.d.FillRight(sh, Rect{10, 1, 10, 2});
  CHECK(t.F("C11") == "=SUM(Sales[Jan])");
  // down keeps them: the same column on every row
  t.Set("G2", "=Sales[@Jan]*2");
  t.d.AutoFill(sh, Rect::Cell(1, 6), Rect{1, 6, 2, 6});
  t.d.Recalc();
  CHECK(t.F("G3") == "=Sales[@Jan]*2");
  CHECK(t.N("G3") == 4);
}

TEST_CASE("Round6: a this-row reference filled across a table's columns names the next column") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  MonthTable(t);
  // a column of totals per month next to the table, from its first data row
  t.Set("F2", "=Sales[@Jan]+0");
  t.d.AutoFill(sh, Rect::Cell(1, 5), Rect{1, 5, 1, 7});  // F2 -> G2:H2
  t.d.Recalc();
  CHECK(t.F("G2") == "=Sales[@Feb]+0");
  CHECK(t.F("H2") == "=Sales[@Mar]+0");
  CHECK(t.N("H2") == 100);
}

TEST_CASE("Round6: the totals row names its own table's columns without the table's name") {
  yctest::Doc t;
  const uint32_t id = MonthTable(t);
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.totals = true; }));
  t.d.Recalc();
  CHECK(t.F("D4") == "=SUBTOTAL(109,[Mar])");
  CHECK(t.N("D4") == 300);
  // still right after the table is renamed, and converted to a range
  std::string err;
  REQUIRE(t.d.RenameTable(t.sheet(), id, "Sales2", &err));
  t.d.Recalc();
  CHECK(t.F("D4") == "=SUBTOTAL(109,[Mar])");
  CHECK(t.N("D4") == 300);
}
