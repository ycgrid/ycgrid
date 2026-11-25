// SPDX-License-Identifier: Apache-2.0
// Round 8 (calculation, measured against Excel 16 itself): circular references without iteration keep their values
// (a formula just entered shows 0, what reads the cycle isn't calculated), iterative calculation goes on at every
// recalculation and counts its rounds as Excel does, turning iteration off keeps the values, Worksheet.Calculate and
// Range.Calculate leave what read a waiting cell waiting, Range.Calculate calculates the range only, and part of an
// array formula (Ctrl+Shift+Enter) can't be changed
#include <catch2/catch_test_macros.hpp>

#include "core/geometry.h"
#include "engine/document.h"
#include "test_helpers.h"

#include <string>

using yc::core::Rect;

namespace {
// what the engine does with manual calculation after an edit: only the formulas just entered are calculated
void Manual(yctest::Doc& t, const char* a1, const std::string& text, uint32_t sh = 0) {
  const auto p = yctest::Doc::P(a1);
  t.d.SetInput(sh ? sh : t.sheet(), p.row, p.col, text);
  t.d.CalcEntered();
}
}  // namespace

// ---- C8: without iteration ----

TEST_CASE("Round8 calc: without iteration the formula that closes a cycle shows 0 and the others keep their values") {
  yctest::Doc t;
  t.Set("A1", "=B1+1");
  CHECK(t.N("A1") == 1);
  t.Set("B1", "=A1");  // Excel: A1 1, B1 0 (it showed A1 0, B1 1)
  CHECK(t.N("A1") == 1);
  CHECK(t.N("B1") == 0);
  CHECK(!t.d.circular_cells.empty());
  t.Set("B1", "5");  // broken
  CHECK(t.N("A1") == 6);
  t.Set("B1", "=A1*2");  // B1 showed 5 before: the formula just entered shows 0, A1 keeps 6
  CHECK(t.N("A1") == 6);
  CHECK(t.N("B1") == 0);
  t.Set("B1", "");
  CHECK(t.N("A1") == 1);
  CHECK(t.d.circular_cells.empty());
}

TEST_CASE("Round8 calc: a cycle of three cells, and a formula that reads it, as Excel shows them") {
  yctest::Doc t;
  t.Set("A1", "=B1+1");
  t.Set("B1", "=C1+1");
  t.Set("C1", "=A1+1");  // Excel 2, 1, 0 (it showed 0, 4, 3)
  CHECK(t.N("A1") == 2);
  CHECK(t.N("B1") == 1);
  CHECK(t.N("C1") == 0);
  t.Set("D1", "=A1*10");  // reads the cycle: not calculated, 0 as just entered (Excel)
  CHECK(t.N("D1") == 0);
  t.Set("C1", "1");  // broken: everything is calculated again
  CHECK(t.N("A1") == 3);
  CHECK(t.N("B1") == 2);
  CHECK(t.N("D1") == 30);
}

TEST_CASE("Round8 calc: a cycle across sheets keeps the first cell's value") {
  yctest::Doc t;
  std::string err;
  REQUIRE(t.d.RenameSheet(t.sheet(), "S1", &err));
  const uint32_t s2 = t.d.AddSheet("S2");
  t.Set("A1", "=S2!A1+1");
  t.Set("A1", "=S1!A1*2", s2);  // Excel 1, 0 (it showed 0, 2)
  CHECK(t.N("A1") == 1);
  CHECK(t.N("A1", s2) == 0);
  t.Set("A1", "3", s2);
  CHECK(t.N("A1") == 4);
}

TEST_CASE("Round8 calc: what reads a circular reference isn't calculated while the cycle exists") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  t.Set("A4", "=SUM(A1:A4)");
  CHECK(t.N("A4") == 0);
  t.Set("B1", "=A4+1");  // Excel 0 (it showed 1)
  CHECK(t.N("B1") == 0);
  t.Set("A1", "10");  // Excel: A4 and B1 stay 0 (B1 showed 16)
  CHECK(t.N("A4") == 0);
  CHECK(t.N("B1") == 0);
  t.Set("A4", "5");  // no cycle any more
  CHECK(t.N("B1") == 6);
}

TEST_CASE("Round8 calc: what reads a cell of a circular array keeps its value, as what reads any cycle") {
  // Excel 16 (measured, cyc_array_self): C2:D3 {=C2:D3+1} shows 0, and =SUM(D2:D5) and =D2*2 over its cells keep
  // the values they showed; they were calculated from the array's zeros
  yctest::Doc t;
  const uint32_t other = t.d.AddSheet("Other");
  t.d.RenameSheet(t.sheet(), "T");
  t.Set("D2", "4");
  t.Set("D3", "5");
  t.Set("D5", "=SUM(D2:D4)");
  t.Set("E2", "=D3*2");
  t.Set("A1", "=SUM(T!D2:D4)", other);
  t.Set("A2", "=T!D3", other);
  CHECK(t.N("D5") == 9);
  CHECK(t.N("E2") == 10);
  std::string err;
  REQUIRE(t.d.SetArrayFormula(t.sheet(), Rect{1, 2, 2, 3}, "C2:D3+1", &err));
  t.d.Recalc();
  CHECK(t.N("C2") == 0);
  CHECK(t.N("D3") == 0);
  CHECK(t.N("D5") == 9);   // reads the array's D2:D3 through a range
  CHECK(t.N("E2") == 10);  // reads its D3, not its anchor
  CHECK(t.N("A1", other) == 9);  // from another sheet too
  CHECK(t.N("A2", other) == 5);
}

TEST_CASE("Round8 calc: what reads a cycle from another sheet keeps its value") {
  // rand3_049 (Excel 16, measured): C3 =SUM(B2:B5*C2:C5) reads itself; Other!D2 =T!C3 keeps the 2.25 it showed
  for (const bool other_first : {true, false}) {
    yctest::Doc t;
    const uint32_t first = t.sheet(), second = t.d.AddSheet("Second");
    const uint32_t other = other_first ? first : second, ts = other_first ? second : first;
    t.d.RenameSheet(other, "Other");
    t.d.RenameSheet(ts, "T");
    for (int r = 2; r <= 5; ++r) {
      t.Set(("B" + std::to_string(r)).c_str(), std::to_string(r * 10).c_str(), ts);
      t.Set(("C" + std::to_string(r)).c_str(), std::to_string(r).c_str(), ts);
    }
    t.Set("C3", "2.25", ts);
    t.Set("D3", "=B3*C3", ts);
    t.Set("D2", "=T!C3", other);
    t.d.Recalc();
    REQUIRE(t.N("D2", other) == 2.25);
    t.Set("C3", "=SUM(B2:B5*C2:C5)", ts);
    t.d.Recalc();
    CHECK(t.N("C3", ts) == 0);
    CHECK(t.N("D3", ts) == 67.5);      // the same sheet's reader keeps its value
    CHECK(t.N("D2", other) == 2.25);   // and another sheet's, before it or after it
  }
}

TEST_CASE("Round8 calc: an edit a cycle depends on leaves its values as they are") {
  yctest::Doc t;
  t.Set("C1", "5");
  t.Set("A1", "=B1+C1");
  t.Set("B1", "=A1");
  CHECK(t.N("A1") == 5);
  CHECK(t.N("B1") == 0);
  t.Set("C1", "6");  // Excel: 5 and 0 (it showed 0 and 5, then 0 and 0)
  CHECK(t.N("A1") == 5);
  CHECK(t.N("B1") == 0);
  t.Set("D1", "=C1*2");
  CHECK(t.N("D1") == 12);
}

// ---- C5, C6: iterative calculation ----

TEST_CASE("Round8 calc: with iteration every recalculation goes on with the cycle") {
  yctest::Doc t;
  t.d.iterate = true;
  t.d.max_iterations = 1;
  t.Set("A1", "=A1+1");
  CHECK(t.N("A1") == 1);
  t.d.CalculateNow();  // Excel 2, 3, then 4 after an edit elsewhere (it stayed 1)
  CHECK(t.N("A1") == 2);
  t.d.CalculateNow();
  CHECK(t.N("A1") == 3);
  t.Set("Z1", "1");
  CHECK(t.N("A1") == 4);
  t.d.Recalc();  // nothing changed: nothing to calculate
  CHECK(t.N("A1") == 4);

  t.Set("C1", "1");
  t.Set("D1", "=IF(C1=0,0,D1+1)");  // a counter: 1, 2 at F9, 3 when C1 changes, 4 at a full calculation
  CHECK(t.N("D1") == 1);
  t.d.CalculateNow();
  CHECK(t.N("D1") == 2);
  t.Set("C1", "2");
  CHECK(t.N("D1") == 3);
  t.d.RecalcAll();
  CHECK(t.N("D1") == 4);
}

TEST_CASE("Round8 calc: MaxIterations rounds at each calculation, MaxIterations in all") {
  yctest::Doc t;
  t.Set("B1", "1");
  t.Set("A1", "=A1+B1");
  CHECK(t.N("A1") == 0);
  t.d.iterate = true;
  t.d.max_iterations = 100;
  t.d.RecalcAll();  // Excel adds 100 per calculation
  CHECK(t.N("A1") == 100);
  t.d.CalculateNow();
  CHECK(t.N("A1") == 200);
  t.Set("B1", "2");
  CHECK(t.N("A1") == 400);
}

TEST_CASE("Round8 calc: iteration rounds calculate each cell once in order, as Excel's values show") {
  {
    // (a) two rounds: Excel B1 15, C1 7.5 (it gave B1 17.5)
    yctest::Doc t;
    t.d.iterate = true;
    t.d.max_iterations = 2;
    t.Set("A1", "100");
    t.Set("B1", "=A1*0.1+C1");
    t.Set("C1", "=B1*0.5");
    CHECK(t.N("B1") == 15);
    CHECK(t.N("C1") == 7.5);
  }
  {
    // three rounds, then A1 changes: Excel 17.5 / 8.75, then 37.1875 / 18.59375
    yctest::Doc t;
    t.d.iterate = true;
    t.d.max_iterations = 3;
    t.Set("A1", "100");
    t.Set("B1", "=A1*0.1+C1");
    t.Set("C1", "=B1*0.5");
    CHECK(t.N("B1") == 17.5);
    CHECK(t.N("C1") == 8.75);
    t.Set("A1", "200");
    CHECK(t.N("B1") == 37.1875);
    CHECK(t.N("C1") == 18.59375);
  }
  {
    // MaxChange: the rounds stop after the first whose changes are all below it (Excel 19.6875 / 9.84375)
    yctest::Doc t;
    t.d.iterate = true;
    t.d.max_iterations = 1000;
    t.d.max_change = 0.5;
    t.Set("A1", "100");
    t.Set("B1", "=A1*0.1+C1");
    t.Set("C1", "=B1*0.5");
    CHECK(t.N("B1") == 19.6875);
    CHECK(t.N("C1") == 9.84375);
  }
  {
    // (b) one round from the value B1 shows: Excel A1 11, B1 22 (B1 stayed 10)
    yctest::Doc t;
    t.Set("A1", "5");
    t.Set("B1", "=A1*2");
    t.d.iterate = true;
    t.d.max_iterations = 1;
    t.Set("A1", "=B1+1");
    CHECK(t.N("A1") == 11);
    CHECK(t.N("B1") == 22);
  }
  {
    // (c) converged by MaxChange: C1 is B1 * 0.05 of the same round (it was a round behind), and the formulas that
    // read the cycle are calculated in its rounds (Excel's values to the last bit)
    yctest::Doc t;
    t.d.iterate = true;
    t.Set("A1", "1000");
    t.Set("B1", "=A1+C1");
    t.Set("C1", "=B1*0.05");
    CHECK(t.N("B1") == 1052.6315625);
    CHECK(t.N("C1") == 1052.6315625 * 0.05);
    t.Set("D1", "=B1*2");
    t.Set("E1", "=C1+1");
    CHECK(t.N("B1") == 1052.6315789472656);
    CHECK(t.N("C1") == 52.63157894736328);
    CHECK(t.N("D1") == 2105.263157894531);
    CHECK(t.N("E1") == 53.63157894736328);
    CHECK(t.d.circular_cells.empty());  // with iteration on, circular references are allowed (no warning)
  }
}

// ---- C7: iteration turned off ----

TEST_CASE("Round8 calc: turning iteration off keeps the values it gave") {
  yctest::Doc t;
  t.d.iterate = true;
  t.Set("A1", "1000");
  t.Set("B1", "=A1+C1");
  t.Set("C1", "=B1*0.05");
  const double b = t.N("B1"), c = t.N("C1");
  REQUIRE(b > 1052);
  t.d.iterate = false;
  t.d.RecalcAll();  // Excel keeps B1 and C1 (they became 0 and 50)
  CHECK(t.N("B1") == b);
  CHECK(t.N("C1") == c);
  CHECK(!t.d.circular_cells.empty());  // reported now
  t.d.CalculateNow();
  t.Set("Z9", "1");
  CHECK(t.N("B1") == b);
  CHECK(t.N("C1") == c);

  yctest::Doc u;
  u.d.iterate = true;
  u.Set("A1", "=A1+1");
  CHECK(u.N("A1") == 100);
  u.d.iterate = false;
  u.d.Recalc();
  u.Set("B5", "1");
  u.d.CalculateNow();
  CHECK(u.N("A1") == 100);
}

// ---- C3, C11: calculating a sheet or a range ----

TEST_CASE("Round8 calc: a formula a sheet's calculation took from a waiting cell is calculated again by F9") {
  yctest::Doc t;
  std::string err;
  REQUIRE(t.d.RenameSheet(t.sheet(), "S1", &err));
  const uint32_t s2 = t.d.AddSheet("S2");
  t.Set("A1", "1");
  t.Set("B1", "=A1*2");
  t.Set("A1", "=S1!B1+1", s2);
  t.d.auto_calc = false;
  Manual(t, "A1", "5");
  t.d.RecalcSheet(s2);  // reads S1!B1 as it shows: 3 (Excel)
  CHECK(t.N("A1", s2) == 3);
  t.d.CalculateNow();  // Excel: B1 10, S2!A1 11 (it stayed 3 until a full calculation)
  CHECK(t.N("B1") == 10);
  CHECK(t.N("A1", s2) == 11);
  Manual(t, "Z9", "1");
  t.d.CalculateNow();
  CHECK(t.N("A1", s2) == 11);
}

TEST_CASE("Round8 calc: Range.Calculate calculates only the formulas of the range") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("B1", "=A1*2");
  t.Set("C1", "=A1*3");
  t.Set("D1", "=B1+1");
  t.d.auto_calc = false;
  Manual(t, "A1", "5");
  t.d.RecalcRange(t.sheet(), Rect::Cell(0, 1));  // Excel: B1 10, C1 and D1 stay 3 (they became 15 and 11)
  CHECK(t.N("B1") == 10);
  CHECK(t.N("C1") == 3);
  CHECK(t.N("D1") == 3);
  t.d.CalculateNow();
  CHECK(t.N("C1") == 15);
  CHECK(t.N("D1") == 11);
}

TEST_CASE("Round8 calc: Range.Calculate reads the cells outside as they show and leaves the formula waiting") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("B1", "=A1+1");
  t.Set("C1", "=B1+1");
  t.d.auto_calc = false;
  Manual(t, "A1", "10");
  t.d.RecalcRange(t.sheet(), Rect::Cell(0, 2));  // Excel: B1 2, C1 3 (both became 11 and 12)
  CHECK(t.N("B1") == 2);
  CHECK(t.N("C1") == 3);
  t.d.RecalcRange(t.sheet(), Rect{0, 1, 0, 2});  // both in the range: in order
  CHECK(t.N("B1") == 11);
  CHECK(t.N("C1") == 12);

  yctest::Doc u;
  u.Set("A1", "1");
  u.Set("B1", "=A1*2");
  u.Set("C1", "=B1+1");
  u.d.auto_calc = false;
  Manual(u, "A1", "5");
  u.d.RecalcRange(u.sheet(), Rect::Cell(0, 2));
  CHECK(u.N("C1") == 3);
  u.d.CalculateNow();  // C1 waited: 11 (it stayed 3)
  CHECK(u.N("B1") == 10);
  CHECK(u.N("C1") == 11);

  // a volatile formula of the range is calculated too
  yctest::Doc v;
  v.d.auto_calc = false;
  Manual(v, "A1", "1");
  Manual(v, "B1", "=OFFSET(A1,0,0)");
  Manual(v, "A1", "2");
  CHECK(v.N("B1") == 1);
  v.d.RecalcRange(v.sheet(), Rect::Cell(0, 1));
  CHECK(v.N("B1") == 2);
}

// ---- C9: part of an array formula ----

TEST_CASE("Round8 calc: part of an array formula can't be changed, cleared, inserted into or deleted") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  std::string err;
  REQUIRE(t.d.SetArrayFormula(t.sheet(), Rect{0, 2, 2, 2}, "A1:A3*10", &err));  // C1:C3
  t.d.Recalc();
  REQUIRE(t.N("C2") == 20);
  const uint32_t s = t.sheet();
  // the array is C1:C3 (or one row lower), still one formula reading `refs`, its second cell 20
  auto intact = [&](uint32_t r0 = 0, const std::string& refs = "A1:A3") {
    Rect r;
    uint32_t ar = 0, ac = 0;
    const std::string first = "C" + std::to_string(r0 + 1), second = "C" + std::to_string(r0 + 2);
    return t.d.SpillAt(s, r0 + 1, 2, &ar, &ac, &r) && r == Rect{r0, 2, r0 + 2, 2} && t.N(second.c_str()) == 20 &&
           t.F(first.c_str()) == "=" + refs + "*10";
  };
  uint32_t blocks = t.d.protect_blocks();
  auto refused = [&] {
    const bool r = t.d.protect_blocks() == blocks + 1 && t.d.block_array();
    blocks = t.d.protect_blocks();
    return r;
  };
  t.d.ClearContents(s, Rect::Cell(1, 2));
  t.d.Recalc();
  CHECK(refused());
  CHECK(intact());
  t.d.SetValue(s, 1, 2, yc::core::Value::Number(7));
  CHECK(refused());
  CHECK_FALSE(t.d.SetInput(s, 1, 2, "7"));
  CHECK(refused());
  CHECK_FALSE(t.d.SetInput(s, 1, 2, "=5"));
  CHECK(refused());
  t.d.ClearContents(s, Rect{0, 0, 1, 5});  // rows 1:2 of a wider range cut it too
  CHECK(refused());
  t.d.Recalc();
  CHECK(intact());
  t.d.DeleteRows(s, 1, 1);  // it became =A1:A2*10
  CHECK(refused());
  t.d.InsertRows(s, 1, 1);  // it grew to C1:C4
  CHECK(refused());
  t.d.InsertCells(s, Rect::Cell(1, 2), true);
  CHECK(refused());
  t.d.DeleteCells(s, Rect::Cell(1, 2), true);
  CHECK(refused());
  t.d.InsertCells(s, Rect::Cell(0, 2), true);  // a cell inserted at C1, shifting down: the whole array moves
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == blocks);
  REQUIRE(intact(1));
  t.d.DeleteCells(s, Rect::Cell(0, 2), true);  // back up as a whole
  t.d.Recalc();
  REQUIRE(intact());
  t.d.InsertCells(s, Rect{0, 1, 0, 1}, false);  // B1 shifted right: only C1 would move
  CHECK(refused());
  t.d.DeleteCols(s, 1, 1);  // a whole column left of it: it moves as a whole
  t.d.InsertCols(s, 1, 1);
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == blocks);
  t.d.Recalc();
  CHECK(intact());
  t.d.InsertRows(s, 0, 1);  // above it: it moves as a whole
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == blocks);
  REQUIRE(intact(1, "A2:A4"));
  t.d.DeleteRows(s, 0, 1);
  t.d.Recalc();
  REQUIRE(intact());
  t.d.ClearContents(s, Rect{0, 2, 2, 2});  // the whole array
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == blocks);
  CHECK(t.V("C2").IsEmpty());
}

TEST_CASE("Round8 calc: an end of a range operator that is an error gives that error") {
  // Excel 16 (measured 2026-11-21): a name nobody defined at either end is #NAME?, a reference INDEX can't give is
  // #REF!; it was #VALUE! (doc_fuzz --xlsx seed 34112022: Replace made Data!A:A3 of Data!A1:A3)
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  CHECK(t.Eval("SUM(Foo:A3)") == "#NAME?");
  CHECK(t.Eval("SUM(A3:Foo)") == "#NAME?");
  CHECK(t.Eval("SUM(A1:A2:Foo)") == "#NAME?");
  CHECK(t.Eval("SUM(INDEX(A1:A3,9):A3)") == "#REF!");
  CHECK(t.Eval("SUM(A1:INDEX(A1:A3,9))") == "#REF!");
  CHECK(t.Eval("SUM(#REF!:A3)") == "#REF!");
  CHECK(t.Eval("SUM(A1:INDEX(A1:A3,2))") == "3");  // two references still make a range
}
