// SPDX-License-Identifier: Apache-2.0
// Arrays entered with Ctrl+Shift+Enter that read their own cells (Excel 16, measured 2026-11-23 through
// Worksheet.CircularReference): calculated, one value after another, when no value reads itself or waits for one that
// does; a circular reference showing 0 when one does (a value of the array times 1 or 0 too, a swap of two values, a
// SUM over its own cells).
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <string>

using yc::core::Rect;

namespace {

bool Array(yctest::Doc& t, const char* range, const char* formula) {
  std::string err;
  const bool ok = t.d.SetArrayFormula(t.sheet(), yctest::Doc::Range(range), formula, &err);
  t.d.Recalc();
  return ok;
}

}  // namespace

TEST_CASE("Arrays reading their own cells: values that read others of it are calculated in turn") {
  SECTION("each value reads a cell beside it") {  // {=B2:C4*2} over C2:C4
    yctest::Doc t;
    t.Set("B2", "10");
    t.Set("B3", "20");
    t.Set("B4", "30");
    t.Set("F1", "=SUM(C2:C4)");
    REQUIRE(Array(t, "C2:C4", "B2:C4*2"));
    CHECK(t.N("C2") == 20);
    CHECK(t.N("C3") == 40);
    CHECK(t.N("C4") == 60);
    CHECK(t.N("F1") == 120);  // what reads it is calculated after it
    CHECK(t.d.circular_cells.empty());
    t.Set("B2", "11");  // an edit calculates it again
    CHECK(t.N("C2") == 22);
    CHECK(t.N("F1") == 122);
    CHECK(t.d.circular_cells.empty());
  }
  SECTION("a value reads the one to its right") {  // {=D2:E3+1} over C2:D3
    yctest::Doc t;
    t.Set("E2", "1");
    t.Set("E3", "2");
    REQUIRE(Array(t, "C2:D3", "D2:E3+1"));
    CHECK(t.N("D2") == 2);
    CHECK(t.N("C2") == 3);
    CHECK(t.N("D3") == 3);
    CHECK(t.N("C3") == 4);
    CHECK(t.d.circular_cells.empty());
  }
  SECTION("a value reads the one to its left") {  // {=B2:C3+1} over C2:D3
    yctest::Doc t;
    t.Set("B2", "5");
    t.Set("B3", "6");
    REQUIRE(Array(t, "C2:D3", "B2:C3+1"));
    CHECK(t.N("C2") == 6);
    CHECK(t.N("D2") == 7);
    CHECK(t.N("C3") == 7);
    CHECK(t.N("D3") == 8);
    CHECK(t.d.circular_cells.empty());
  }
  SECTION("eight values each reading the next") {  // {=A2:A9+1} over A1:A8
    yctest::Doc t;
    t.Set("A9", "1");
    REQUIRE(Array(t, "A1:A8", "A2:A9+1"));
    for (int r = 1; r <= 8; ++r) CHECK(t.N(("A" + std::to_string(r)).c_str()) == 10 - r);
    CHECK(t.d.circular_cells.empty());
  }
  SECTION("a value copies the next") {  // {=B1:C1} over A1:B1
    yctest::Doc t;
    t.Set("C1", "5");
    REQUIRE(Array(t, "A1:B1", "B1:C1"));
    CHECK(t.N("A1") == 5);
    CHECK(t.N("B1") == 5);
    CHECK(t.d.circular_cells.empty());
  }
  SECTION("what a value reads is what it reads, not the range it names") {  // INDEX(B2:C3,1,1) reads B2 only
    yctest::Doc t;
    t.Set("B2", "7");
    REQUIRE(Array(t, "C2:C3", "INDEX(B2:C3,1,1)+ROW(C2:C3)"));
    CHECK(t.N("C2") == 9);
    CHECK(t.N("C3") == 10);
    CHECK(t.d.circular_cells.empty());
  }
}

TEST_CASE("Arrays reading their own cells: a value that reads itself is a circular reference") {
  const char* cases[][2] = {
      {"C2:D3", "C2:D3*1"},                // each value reads itself, though 0 would settle
      {"C2:D3", "C2:D3*0"},                //
      {"A1:B1", "CHOOSE({1,2},B1,A1)"},    // two values read each other
      {"A1:B1", "CHOOSE({1,2},B1,A1)+1"},  //
      {"A1:B2", "SUM(B2:B5)"},             // every value reads B2, a cell of it
      {"C2:D3", "TRANSPOSE(C2:D3)"},       // C2 and D3 read themselves, D2 and C3 each other
      {"A1:A3", "COUNT(A1:A3)"},           // counts its own cells: no value while they wait, three once they settle
  };
  for (const auto& c : cases) {
    INFO(std::string(c[0]) + " {=" + c[1] + "}");
    yctest::Doc t;
    t.Set("B3", "1");
    t.Set("B4", "2");
    REQUIRE(Array(t, c[0], c[1]));
    CHECK_FALSE(t.d.circular_cells.empty());
    const Rect r = yctest::Doc::Range(c[0]);
    for (uint32_t row = r.r0; row <= r.r1; ++row) {
      for (uint32_t col = r.c0; col <= r.c1; ++col) CHECK(t.d.GetValue(t.sheet(), row, col).AsDouble() == 0);
    }
  }
}

TEST_CASE("Arrays reading their own cells: Undo, Redo and a reopen give the same values") {
  yctest::Doc t;
  t.Set("A9", "1");
  const uint32_t sh = t.sheet();
  t.d.Begin("Array");
  std::string err;
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("A1:A8"), "A2:A9+1", &err));
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.N("A1") == 9);
  t.d.Undo();
  t.d.Recalc();
  CHECK(t.V("A1").IsEmpty());
  t.d.Redo();
  t.d.Recalc();
  CHECK(t.N("A1") == 9);
  CHECK(t.N("A8") == 2);
  CHECK(t.d.circular_cells.empty());
  t.d.RecalcAll();
  CHECK(t.N("A1") == 9);
  CHECK(t.d.circular_cells.empty());
}

TEST_CASE("Arrays reading their own cells: what reads one that is a circular reference keeps its value") {
  // rand3_112 (Excel 16, measured): {=SUM(B2:B5)} over A1:B2 reads B2, a cell of it; a formula reading it, on this
  // sheet or another, keeps the value it showed, and an array entered after it reads its cells as they show
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t other = t.d.AddSheet("Other");
  t.Set("B2", "10");
  t.Set("B3", "20");
  t.Set("C2", "1.5");
  t.Set("A3", "Pear");
  t.Set("D2", "=B2*C2");
  REQUIRE(t.Set("D1", "=SUM(Sheet1!B2:B4)", other));
  CHECK(t.N("D1", other) == 30);
  REQUIRE(Array(t, "A1:B2", "SUM(B2:B5)"));
  CHECK_FALSE(t.d.circular_cells.empty());
  CHECK(t.N("B2") == 0);
  CHECK(t.N("D2") == 15);
  CHECK(t.N("D1", other) == 30);
  t.Set("G2", "=C8&\"?\"");
  REQUIRE(Array(t, "C7:D9", "A2:A4"));
  CHECK(t.N("C7") == 0);  // reads A2, a cell of the cycle
  CHECK(t.V("C8").str() == "Pear");
  CHECK(t.V("D8").str() == "Pear");
  CHECK(t.N("C9") == 0);
  CHECK(t.V("G2").str() == "Pear?");  // what reads the array is calculated after it
  CHECK(t.N("D1", other) == 30);
  (void)sh;
}

TEST_CASE("Arrays reading a circular reference: each value that reads it shows 0, the others are calculated") {
  // Excel 16, measured: A1 =A1+1 is a circular reference; {=A1:A2+5} over C1:C2 shows 0 and 5
  yctest::Doc t;
  t.Set("A1", "=A1+1");
  CHECK_FALSE(t.d.circular_cells.empty());
  REQUIRE(Array(t, "C1:C2", "A1:A2+5"));
  CHECK(t.N("C1") == 0);
  CHECK(t.N("C2") == 5);
  t.Set("C3", "=A1+5");  // a formula typed that reads it shows 0
  CHECK(t.N("C3") == 0);
}
