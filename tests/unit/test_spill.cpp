// SPDX-License-Identifier: Apache-2.0
// Dynamic arrays: spilling, #SPILL!, A1#, @ and the array functions
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/ycg.h"

using yc::core::Rect;

namespace {

std::string Txt(yctest::Doc& t, const char* a1) { return yc::engine::DisplayTextOf(t.V(a1), "General"); }

bool SpillRect(yctest::Doc& t, const char* a1, Rect* out) {
  const auto p = yctest::Doc::P(a1);
  uint32_t ar, ac;
  return t.d.SpillAt(t.sheet(), p.row, p.col, &ar, &ac, out);
}

}  // namespace

TEST_CASE("an array result spills from its cell") {
  yctest::Doc t;
  t.Set("B2", "=SEQUENCE(3,2)");
  CHECK(t.N("B2") == 1);
  CHECK(t.N("C2") == 2);
  CHECK(t.N("B4") == 5);
  CHECK(t.N("C4") == 6);
  Rect r;
  REQUIRE(SpillRect(t, "C3", &r));
  CHECK(r == Rect{1, 1, 3, 2});
  CHECK(t.F("C3") == "4");
  // dependents of member cells follow the anchor
  t.Set("E1", "=C4*10");
  CHECK(t.N("E1") == 60);
  t.Set("B2", "=SEQUENCE(3,2,10)");
  CHECK(t.N("E1") == 150);
  // a range spills too, blanks become 0
  t.Set("H1", "1");
  t.Set("H3", "3");
  t.Set("J1", "=H1:H3");
  CHECK(t.N("J1") == 1);
  CHECK(t.N("J2") == 0);
  CHECK(t.N("J3") == 3);
}

TEST_CASE("a blocked spill shows #SPILL! and recovers when cleared") {
  yctest::Doc t;
  t.Set("A3", "x");
  t.Set("A1", "=SEQUENCE(4)");
  CHECK(Txt(t, "A1") == "#SPILL!");
  CHECK(t.V("A2").IsEmpty());
  t.Set("A3", "");
  CHECK(t.N("A1") == 1);
  CHECK(t.N("A4") == 4);
  // typing into a member blocks it again
  t.Set("A2", "hello");
  CHECK(Txt(t, "A1") == "#SPILL!");
  CHECK(Txt(t, "A2") == "hello");
  CHECK(t.V("A3").IsEmpty());
  t.Set("A2", "");
  CHECK(t.N("A4") == 4);
  // removing the formula removes its values
  t.Set("A1", "5");
  CHECK(t.V("A2").IsEmpty());
  CHECK(t.V("A4").IsEmpty());
  Rect r;
  CHECK_FALSE(SpillRect(t, "A2", &r));
}

TEST_CASE("a spill range that grows and shrinks updates its dependents") {
  yctest::Doc t;
  t.Set("A1", "2");
  t.Set("B1", "=SEQUENCE(A1)");
  t.Set("D1", "=SUM(B1#)");
  t.Set("E1", "=COUNT(B1#)");
  t.Set("F1", "=B5");
  CHECK(t.N("D1") == 3);
  CHECK(t.N("F1") == 0);
  t.Set("A1", "5");
  CHECK(t.N("D1") == 15);
  CHECK(t.N("E1") == 5);
  CHECK(t.N("F1") == 5);
  t.Set("A1", "1");
  CHECK(t.N("D1") == 1);  // B1# is just B1 now
  CHECK(t.V("B2").IsEmpty());
  CHECK(t.N("F1") == 0);
  CHECK(Txt(t, "G1") == "");
  // A1# of a cell that doesn't spill is #REF!
  t.Set("G1", "=SUM(A1#)");
  CHECK(Txt(t, "G1") == "#REF!");
  CHECK(t.F("D1") == "=SUM(B1#)");
}

TEST_CASE("@ is implicit intersection") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  t.Set("B2", "=@A1:A3");
  CHECK(t.N("B2") == 2);
  Rect r;
  CHECK_FALSE(SpillRect(t, "B3", &r));
  t.Set("C1", "=@SEQUENCE(3)");
  CHECK(t.N("C1") == 1);
  CHECK(t.V("C2").IsEmpty());
  CHECK(t.F("B2") == "=@A1:A3");
}

TEST_CASE("@ of a reference is still a reference") {
  // Excel 16, measured: what tells a reference from a value sees the cell itself, an empty one too
  yctest::Doc t;
  t.Set("A1", "5");
  t.Set("A3", "7");  // A2 stays empty
  CHECK(t.Eval("ISREF(@A2)") == "TRUE");
  CHECK(t.Eval("COUNT(@A2)") == "0");
  CHECK(t.Eval("COUNTA(@A2)") == "0");
  CHECK(t.Eval("COUNTA(@A2:A2)") == "0");
  CHECK(t.Eval("COUNTBLANK(@A2:A2)") == "1");
  CHECK(t.Eval("ROW(@A3)") == "3");
  CHECK(t.Eval("CELL(\"address\",@A3)") == "$A$3");
  CHECK(t.Eval("ISREF(@INDEX(A1:B3,2,1))") == "TRUE");
  CHECK(t.Eval("ISREF(@OFFSET(A1,1,0))") == "TRUE");
  CHECK(t.Eval("ISREF(@TAKE(A1:A3,1))") == "TRUE");
  // its value is the cell's, as without the @
  CHECK(t.Eval("@A1+1") == "6");
  CHECK(t.Eval("@A2") == "0");
  CHECK(t.Eval("@A2&\"x\"") == "x");
  // a column that covers the formula's row meets it there, a row that covers its column likewise
  t.Set("C2", "=COUNTBLANK(@A1:A3)");
  t.Set("C3", "=ROW(@A1:A10)");
  t.Set("C4", "=ISREF(@A:A)");
  t.Set("C5", "=ROW(OFFSET(@A1:A10,1,0))");
  t.Set("C6", "=ISREF(@IF(TRUE,A1:A10))");
  t.Set("D7", "=COLUMN(@C1:F1)");
  t.Set("D8", "=COUNT(@A1:A10,@B1:B10)");
  CHECK(t.N("C2") == 1);
  CHECK(t.N("C3") == 3);
  CHECK(t.V("C4").boolean());
  CHECK(t.N("C5") == 6);
  CHECK(t.V("C6").boolean());
  CHECK(t.N("D7") == 4);
  CHECK(t.N("D8") == 0);
  // cells that don't meet the formula's row or column, and an array, give a value
  CHECK(t.Eval("ISREF(@A1:A3)") == "FALSE");
  CHECK(t.Eval("ISREF(@A1:B1)") == "FALSE");
  CHECK(t.Eval("ISREF(@A1:B3)") == "FALSE");
  CHECK(t.Eval("ISREF(@{1,2})") == "FALSE");
  CHECK(t.Eval("ISREF(@SORT(A1:A3))") == "FALSE");
  CHECK(t.Eval("COUNTA(@A1:A3)") == "1");
  CHECK(t.Eval("ROW(@A1:A3)") == "#VALUE!");
  CHECK(t.Eval("SUM(@A1:A3,1)") == "#VALUE!");
}

TEST_CASE("undo and insert rows keep spills consistent") {
  yctest::Doc t;
  t.d.Begin("in");
  t.Set("A1", "=SEQUENCE(3)");
  t.d.Commit();
  t.d.Begin("in");
  t.Set("A2", "x");
  t.d.Commit();
  CHECK(Txt(t, "A1") == "#SPILL!");
  t.d.Undo();
  t.d.Recalc();
  CHECK(t.N("A1") == 1);
  CHECK(t.N("A2") == 2);
  CHECK(t.N("A3") == 3);
  t.d.InsertRows(t.sheet(), 0, 2);
  t.d.Recalc();
  CHECK(t.N("A3") == 1);
  CHECK(t.N("A5") == 3);
  CHECK(t.V("A1").IsEmpty());
  t.d.InsertRows(t.sheet(), 3, 1);  // inside the range: it spills again below the anchor
  t.d.Recalc();
  CHECK(t.N("A3") == 1);
  CHECK(t.N("A4") == 2);
  CHECK(t.N("A5") == 3);
  CHECK(t.V("A6").IsEmpty());
}

TEST_CASE("spill ranges are saved") {
  yctest::Doc t;
  t.Set("B1", "=SEQUENCE(2,2)");
  const std::string bytes = yc::io::SaveYcgToBytes(t.d.wb(), {});
  auto res = yc::io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  yc::engine::Document d(res.wb);
  CHECK(d.GetValue(t.sheet(), 1, 2).AsDouble() == 4);
  uint32_t ar = 0, ac = 0;
  Rect r;
  REQUIRE(d.SpillAt(t.sheet(), 1, 2, &ar, &ac, &r));
  CHECK(ar == 0);
  CHECK(ac == 1);
  CHECK(r == Rect{0, 1, 1, 2});
}

TEST_CASE("FILTER SORT SORTBY UNIQUE") {
  yctest::Doc t;
  const char* names[] = {"b", "a", "c", "a", "b"};
  const char* nums[] = {"3", "1", "5", "1", "2"};
  for (int i = 0; i < 5; ++i) {
    t.Set(("A" + std::to_string(i + 1)).c_str(), names[i]);
    t.Set(("B" + std::to_string(i + 1)).c_str(), nums[i]);
  }
  t.Set("D1", "=FILTER(A1:B5,B1:B5>1)");
  CHECK(Txt(t, "D1") == "b");
  CHECK(Txt(t, "D3") == "b");
  CHECK(t.N("E3") == 2);
  CHECK(t.V("D4").IsEmpty());
  t.Set("G1", "=FILTER(A1:A5,B1:B5>9,\"none\")");
  CHECK(Txt(t, "G1") == "none");
  t.Set("H1", "=FILTER(A1:A5,B1:B5>9)");
  CHECK(Txt(t, "H1") == "#CALC!");

  t.Set("J1", "=SORT(B1:B5)");
  CHECK(t.N("J1") == 1);
  CHECK(t.N("J5") == 5);
  t.Set("K1", "=SORT(A1:B5,2,-1)");
  CHECK(Txt(t, "K1") == "c");
  CHECK(t.N("L1") == 5);
  t.Set("N1", "=SORTBY(A1:A5,B1:B5,1)");
  CHECK(Txt(t, "N1") == "a");
  CHECK(Txt(t, "N5") == "c");
  t.Set("P1", "=UNIQUE(A1:A5)");
  CHECK(Txt(t, "P1") == "b");
  CHECK(Txt(t, "P2") == "a");
  CHECK(Txt(t, "P3") == "c");
  CHECK(t.V("P4").IsEmpty());
  t.Set("Q1", "=UNIQUE(A1:A5,,TRUE)");
  CHECK(Txt(t, "Q1") == "c");
  CHECK(t.V("Q2").IsEmpty());
  t.Set("R1", "=ROWS(UNIQUE(A1:B5))");
  CHECK(t.N("R1") == 4);
}

TEST_CASE("SEQUENCE RANDARRAY stacking and reshaping") {
  yctest::Doc t;
  CHECK(t.Eval("SUM(SEQUENCE(10))") == "55");
  CHECK(t.Eval("SUM(SEQUENCE(2,3,0,5))") == "75");
  CHECK(t.Eval("SEQUENCE(0)") == "#CALC!");
  CHECK(t.Eval("COUNT(RANDARRAY(4,5))") == "20");
  CHECK(t.Eval("AND(RANDARRAY(50,1,3,4,TRUE)>=3,RANDARRAY(50,1,3,4,TRUE)<=4)") == "TRUE");
  CHECK(t.Eval("ROWS(VSTACK({1,2},{3,4;5,6}))") == "3");
  CHECK(t.Eval("INDEX(HSTACK({1;2},{3}),2,2)") == "#N/A");
  CHECK(t.Eval("SUM(TAKE(SEQUENCE(10),3))") == "6");
  CHECK(t.Eval("SUM(TAKE(SEQUENCE(10),-2))") == "19");
  CHECK(t.Eval("SUM(DROP(SEQUENCE(10),8))") == "19");
  CHECK(t.Eval("DROP(SEQUENCE(3),3)") == "#CALC!");
  CHECK(t.Eval("SUM(CHOOSEROWS(SEQUENCE(5),1,-1))") == "6");
  CHECK(t.Eval("SUM(CHOOSECOLS(SEQUENCE(2,3),2))") == "7");
  CHECK(t.Eval("CHOOSEROWS(SEQUENCE(5),6)") == "#VALUE!");
  CHECK(t.Eval("COLUMNS(TOROW(SEQUENCE(3,4)))") == "12");
  CHECK(t.Eval("INDEX(TOCOL(SEQUENCE(2,3),,TRUE),2)") == "4");
  CHECK(t.Eval("ROWS(EXPAND({1,2},3))") == "3");
  CHECK(t.Eval("INDEX(EXPAND({1,2},2,3,0),2,3)") == "0");
  CHECK(t.Eval("ROWS(WRAPROWS(SEQUENCE(7),3))") == "3");
  CHECK(t.Eval("INDEX(WRAPROWS(SEQUENCE(7),3),3,2)") == "#N/A");
  CHECK(t.Eval("INDEX(WRAPCOLS(SEQUENCE(7),3),1,3)") == "7");
}

TEST_CASE("TEXTSPLIT") {
  yctest::Doc t;
  t.Set("A1", "=TEXTSPLIT(\"a,b;c,d,e\",\",\",\";\")");
  CHECK(Txt(t, "A1") == "a");
  CHECK(Txt(t, "B1") == "b");
  CHECK(Txt(t, "C1") == "#N/A");
  CHECK(Txt(t, "A2") == "c");
  CHECK(Txt(t, "C2") == "e");
  CHECK(t.Eval("COLUMNS(TEXTSPLIT(\"a--b-c\",\"-\"))") == "4");
  CHECK(t.Eval("COLUMNS(TEXTSPLIT(\"a--b-c\",\"-\",,TRUE))") == "3");
  CHECK(t.Eval("COLUMNS(TEXTSPLIT(\"aXbxc\",\"x\",,,1))") == "3");
  CHECK(t.Eval("INDEX(TEXTSPLIT(\"one two\",{\" \",\",\"}),2)") == "two");
}

TEST_CASE("array lifting of single-value arguments") {
  yctest::Doc t;
  t.Set("A1", "ab");
  t.Set("A2", "cde");
  t.Set("A3", "f");
  t.Set("B1", "=LEN(A1:A3)");
  CHECK(t.N("B1") == 2);
  CHECK(t.N("B2") == 3);
  CHECK(t.N("B3") == 1);
  CHECK(t.Eval("SUMPRODUCT(LEN(A1:A3))") == "6");
  CHECK(t.Eval("SUM(IF(LEN(A1:A3)>1,1,0))") == "2");
  CHECK(t.Eval("SUM(IF({TRUE,FALSE,TRUE},{1,2,3},{10,20,30}))") == "24");
  CHECK(t.Eval("SUM(ROUND({1.26,2.71},1))") == "4");
  CHECK(t.Eval("SUM(IFERROR(1/{1,0,2},0))") == "1.5");
  t.Set("D1", "x");
  t.Set("D2", "y");
  t.Set("D3", "x");
  t.Set("E1", "1");
  t.Set("E2", "2");
  t.Set("E3", "3");
  t.Set("G1", "=SUMIF(D1:D3,{\"x\";\"y\"},E1:E3)");
  CHECK(t.N("G1") == 4);
  CHECK(t.N("G2") == 2);
  t.Set("H1", "=VLOOKUP({\"y\",\"x\"},D1:E3,2,FALSE)");
  CHECK(t.N("H1") == 2);
  CHECK(t.N("I1") == 1);
  // a lazy IF still skips the unused branch (no false circular reference)
  t.Set("K1", "=IF(TRUE,1,K1)");
  CHECK(t.N("K1") == 1);
  CHECK(t.d.circular_cells.empty());
  // scalars are unchanged
  CHECK(t.Eval("LEN(\"hello\")") == "5");
}

TEST_CASE("undo of clearing a blocking cell blocks the spill again") {
  yctest::Doc t;
  t.Set("A1", "=SEQUENCE(3)");
  t.d.Begin("type");
  t.Set("A2", "zzz");
  t.d.Commit();
  CHECK(Txt(t, "A1") == "#SPILL!");
  t.d.Begin("clear");
  t.d.ClearContents(t.sheet(), Rect::Cell(1, 0));
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.N("A2") == 2);
  t.d.Undo();
  t.d.Recalc();
  CHECK(Txt(t, "A2") == "zzz");
  CHECK(Txt(t, "A1") == "#SPILL!");
  t.d.Redo();
  t.d.Recalc();
  CHECK(t.N("A2") == 2);
  CHECK(t.N("A3") == 3);
}

TEST_CASE("array formulas (Ctrl+Shift+Enter)") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  std::string err;
  REQUIRE(t.d.SetArrayFormula(t.sheet(), Rect{0, 1, 3, 1}, "A1:A3*10", &err));
  t.d.Recalc();
  CHECK(t.N("B1") == 10);
  CHECK(t.N("B3") == 30);
  CHECK(Txt(t, "B4") == "#N/A");  // larger than the result
  Rect r;
  REQUIRE(SpillRect(t, "B2", &r));
  CHECK(r == Rect{0, 1, 3, 1});
  t.Set("A2", "5");
  CHECK(t.N("B2") == 50);
  // a single value fills the whole range; a smaller range shows part of the result
  REQUIRE(t.d.SetArrayFormula(t.sheet(), Rect{0, 3, 1, 4}, "7", &err));
  t.d.Recalc();
  CHECK(t.N("E2") == 7);
  REQUIRE(t.d.SetArrayFormula(t.sheet(), Rect{5, 0, 5, 0}, "SUM(A1:A3*2)", &err));
  t.d.Recalc();
  CHECK(t.N("A6") == 18);
  // cutting through an array is refused; replacing it as a whole works
  CHECK_FALSE(t.d.SetArrayFormula(t.sheet(), Rect{2, 1, 5, 1}, "1", &err));
  REQUIRE(t.d.SetArrayFormula(t.sheet(), Rect{0, 1, 4, 1}, "ROW(A1:A5)", &err));
  t.d.Recalc();
  CHECK(t.N("B5") == 5);
  // clearing the whole array removes it
  t.d.ClearContents(t.sheet(), Rect{0, 1, 4, 1});
  t.d.Recalc();
  CHECK_FALSE(SpillRect(t, "B2", &r));
  CHECK(t.V("B3").IsEmpty());
  // saved and loaded
  REQUIRE(t.d.SetArrayFormula(t.sheet(), Rect{0, 6, 1, 6}, "A1:A2+1", &err));
  t.d.Recalc();
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  yc::engine::Document d(res.wb);
  uint32_t ar = 0, ac = 0;
  REQUIRE(d.SpillAt(t.sheet(), 1, 6, &ar, &ac, &r));
  CHECK(d.wb().sheets[0]->SpillOf(0, 6)->cse);
  CHECK(d.GetValue(t.sheet(), 1, 6).AsDouble() == 6);
  // inserting a row through the array is refused, as in Excel ("You can't change part of an array"): the array
  // keeps its range
  const uint32_t blocks = t.d.protect_blocks();
  t.d.InsertRows(t.sheet(), 1, 1);
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == blocks + 1);
  REQUIRE(SpillRect(t, "G1", &r));
  CHECK(r == Rect{0, 6, 1, 6});
}

TEST_CASE("undoing a typed dynamic array leaves no spilled values behind") {
  // as the UI does it: the edit is one undo step, the array spills in the recalculation after it
  yctest::Doc t;
  t.Set("A1", "5");
  auto type = [&](uint32_t r, uint32_t c, const char* text) {
    t.d.Begin("type");
    t.d.SetInput(t.sheet(), r, c, text);
    t.d.Commit();
    t.d.Recalc();
  };
  type(6, 4, "=SEQUENCE(2,3)");  // E7
  REQUIRE(t.N("G8") == 6);
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(Txt(t, "E7").empty());
  CHECK(Txt(t, "F7").empty());  // the spilled values went with the formula, as they do when it is deleted
  CHECK(Txt(t, "G8").empty());
  t.d.RecalcAll();
  CHECK(Txt(t, "F7").empty());
  REQUIRE(t.d.Redo());
  t.d.Recalc();
  CHECK(t.N("G8") == 6);
  // a typed array replaced by a value: undo spills it again
  type(6, 4, "1");
  CHECK(Txt(t, "F7").empty());
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.N("G8") == 6);
}
