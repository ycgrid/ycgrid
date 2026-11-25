// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/geometry.h"

using namespace yc::core;

TEST_CASE("欄名與 A1 位址") {
  CHECK(ColumnName(0) == "A");
  CHECK(ColumnName(25) == "Z");
  CHECK(ColumnName(26) == "AA");
  CHECK(ColumnName(16383) == "XFD");
  uint32_t c;
  REQUIRE(ParseColumnName("XFD", &c));
  CHECK(c == 16383);
  CHECK_FALSE(ParseColumnName("XFE", &c));
  CellPos p;
  REQUIRE(ParseCellA1("$B$7", &p));
  CHECK(p.row == 6);
  CHECK(p.col == 1);
  REQUIRE(ParseCellA1("A2000000", &p));  // dynamic row count: past 1,048,576 still parses
  CHECK(p.row == 1999999);
  Rect r;
  REQUIRE(ParseRectA1("B2:A1", &r));
  CHECK(RectA1(r) == "A1:B2");
  REQUIRE(ParseRectA1("C:A", &r));
  CHECK(r.c0 == 0);
  CHECK(r.c1 == 2);
  CHECK(r.r1 == kRowEnd);
  CHECK(RectA1(r) == "A:C");
  REQUIRE(ParseRectA1("3:5", &r));
  CHECK(RectA1(r) == "3:5");
  std::string sheet;
  std::string_view addr;
  REQUIRE(SplitSheetRef("'My Sheet'!A1:B2", &sheet, &addr));
  CHECK(sheet == "My Sheet");
  CHECK(addr == "A1:B2");
  CHECK(QuoteSheetName("工作表1") == "工作表1");
  CHECK(QuoteSheetName("My Sheet") == "'My Sheet'");
  CHECK(QuoteSheetName("A1") == "'A1'");
}

TEST_CASE("Sheet names that read as R1C1 references are quoted") {
  using yc::core::QuoteSheetName;
  CHECK(QuoteSheetName("R1C1") == "'R1C1'");
  CHECK(QuoteSheetName("RC") == "'RC'");
  CHECK(QuoteSheetName("R") == "'R'");
  CHECK(QuoteSheetName("c12") == "'c12'");
  CHECK(QuoteSheetName("Rate") == "Rate");
  CHECK(QuoteSheetName("Cost") == "Cost");
  // a name that starts with an R1C1 part ending in a digit is quoted too (measured in Excel 16: R2D2, C3PO, R1x)
  CHECK(QuoteSheetName("R2D2") == "'R2D2'");
  CHECK(QuoteSheetName("C3PO") == "'C3PO'");
  CHECK(QuoteSheetName("RC1x") == "'RC1x'");
  CHECK(QuoteSheetName("A1x") == "A1x");
}

TEST_CASE("A change event's areas read as one Excel range: no repeats, neighbours joined") {
  using yc::core::Rect;
  auto a1 = [](std::vector<Rect> v) {
    yc::core::TidyAreas(&v);
    std::string out;
    for (const Rect& r : v) out += (out.empty() ? "" : ",") + yc::core::RectA1(r);
    return out;
  };
  CHECK(a1({Rect{0, 5, 0, 5}, Rect{0, 5, 1, 5}}) == "F1:F2");  // Ctrl+Enter: the active cell, then the selection
  CHECK(a1({Rect{0, 9, 0, 9}, Rect{1, 9, 1, 9}, Rect{2, 9, 2, 9}}) == "J1:J3");  // a sort, cell by cell
  CHECK(a1({Rect{0, 0, 0, 0}, Rect{0, 1, 0, 1}, Rect{1, 0, 1, 1}}) == "A1:B2");
  CHECK(a1({Rect{0, 0, 0, 0}, Rect{4, 4, 4, 4}}) == "A1,E5");  // apart: two areas
  CHECK(a1({Rect{2, 2, 2, 2}, Rect{2, 2, 2, 2}}) == "C3");
  CHECK(a1({}) == "");
}
