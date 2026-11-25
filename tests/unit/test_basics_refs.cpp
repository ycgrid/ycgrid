// SPDX-License-Identifier: Apache-2.0
// References at a range's edge, as Excel adjusts them (measured: tests/data/excel_basics_measured.json, the edge_ and
// delete_up / cut_ cases): cells deleted with a shift trim a range whose edge they hold whole, and a cut that moves a
// range's edge moves that edge; cell formulas, defined names and the rules' formulas follow alike.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

using yc::core::Rect;

namespace {

// A1:F10 hold numbers (10 * row + column), the formulas sit far from them
void Fill(yctest::Doc& t) {
  for (int r = 1; r <= 10; ++r) {
    for (int c = 0; c < 6; ++c) {
      const std::string a1 = std::string(1, static_cast<char>('A' + c)) + std::to_string(r);
      t.Set(a1.c_str(), std::to_string(10 * r + c + 1));
    }
  }
}

std::string Name(yctest::Doc& t, const char* name) {
  const auto* n = t.d.FindName(name, t.sheet());
  return n ? n->refers_to : "(none)";
}

// a rule on J1 whose formula reads a range, and a validation on K1 reading it too
void AddRules(yctest::Doc& t, const std::string& formula) {
  yc::model::CfRule cf;
  cf.type = yc::model::CfType::Expression;
  cf.formula1 = formula;
  cf.ranges = {Rect::Cell(0, 9)};
  t.d.AddCondFormat(t.sheet(), cf);
  yc::model::DvRule dv;
  dv.type = yc::model::DvType::Custom;
  dv.formula1 = formula;
  t.d.SetValidation(t.sheet(), Rect::Cell(0, 10), dv);
}
std::string CfFormula(yctest::Doc& t) { return t.d.S(t.sheet())->cond_formats->front().formula1; }
std::string DvFormula(yctest::Doc& t) { return t.d.S(t.sheet())->validations->front().formula1; }

bool Move(yctest::Doc& t, const Rect& src, uint32_t dst_sheet, uint32_t r, uint32_t c) {
  t.d.Begin("Move");
  const bool ok = t.d.MoveCells(t.sheet(), src, dst_sheet, r, c);
  t.d.Commit();
  t.d.Recalc();
  return ok;
}

}  // namespace

TEST_CASE("Basics: cells deleted with a shift up trim a range whose edge columns they hold whole") {
  yctest::Doc t;
  Fill(t);
  t.Set("H20", "=SUM(B2:C3)");     // its first column deleted whole: C2:C3
  t.Set("H21", "=SUM(A2:B3)");     // its last column: A2:A3
  t.Set("H22", "=SUM(B1:C3)");     // B1 stays: kept as it is
  t.Set("H23", "=SUM($B$2:$D$3)");  // absolute too: $C$2:$D$3
  t.Set("H24", "=SUM(2:2)");       // whole rows keep their text
  t.Set("H25", "=SUM(B3:B6)");     // inside the band: shifts as before (B2:B2)
  REQUIRE(t.d.AddName("NA", "=Sheet1!$B$2:$C$3"));
  AddRules(t, "SUM($B$2:$C$3)>0");
  t.d.DeleteCells(t.sheet(), Rect{1, 1, 4, 1}, true);  // B2:B5, shift up
  t.d.Recalc();
  CHECK(t.F("H20") == "=SUM(C2:C3)");
  CHECK(t.N("H20") == 23 + 33);
  CHECK(t.F("H21") == "=SUM(A2:A3)");
  CHECK(t.F("H22") == "=SUM(B1:C3)");
  CHECK(t.F("H23") == "=SUM($C$2:$D$3)");
  CHECK(t.F("H24") == "=SUM(2:2)");
  CHECK(t.F("H25") == "=SUM(B2:B2)");
  CHECK(Name(t, "NA") == "Sheet1!$C$2:$C$3");
  CHECK(CfFormula(t) == "SUM($C$2:$C$3)>0");
  CHECK(DvFormula(t) == "SUM($C$2:$C$3)>0");
  // the last columns of a range (D2:D3 of B2:D3)
  yctest::Doc u;
  Fill(u);
  u.Set("H20", "=SUM(B2:D3)");
  u.d.DeleteCells(u.sheet(), Rect{1, 3, 2, 3}, true);  // D2:D3, shift up
  u.d.Recalc();
  CHECK(u.F("H20") == "=SUM(B2:C3)");
  CHECK(u.N("H20") == 22 + 23 + 32 + 33);
}

TEST_CASE("Basics: cells deleted with a shift left trim a range whose edge rows they hold whole") {
  yctest::Doc t;
  Fill(t);
  t.Set("H20", "=SUM(B3:D4)");  // row 3 deleted whole within B:D: B4:D4
  t.Set("H21", "=SUM(B1:C3)");  // its last rows: B1:C1
  t.Set("H22", "=SUM(B1:D4)");  // only its middle: kept
  t.Set("H23", "=SUM(B:D)");    // whole columns keep their text
  REQUIRE(t.d.AddName("ND", "=Sheet1!$B$3:$D$4"));
  AddRules(t, "SUM($B$3:$D$4)>0");
  t.d.DeleteCells(t.sheet(), Rect{1, 1, 2, 3}, false);  // B2:D3, shift left
  t.d.Recalc();
  CHECK(t.F("H20") == "=SUM(B4:D4)");
  CHECK(t.N("H20") == 42 + 43 + 44);
  CHECK(t.F("H21") == "=SUM(B1:C1)");
  CHECK(t.F("H22") == "=SUM(B1:D4)");
  CHECK(t.F("H23") == "=SUM(B:D)");
  CHECK(Name(t, "ND") == "Sheet1!$B$4:$D$4");
  CHECK(CfFormula(t) == "SUM($B$4:$D$4)>0");
  CHECK(DvFormula(t) == "SUM($B$4:$D$4)>0");
  // undo puts the texts back
  yctest::Doc u;
  Fill(u);
  u.Set("H20", "=SUM(B2:C3)");
  const size_t depth = u.d.UndoDepth();
  u.d.Begin("Delete");
  u.d.DeleteCells(u.sheet(), Rect{1, 1, 4, 1}, true);
  u.d.Commit();
  REQUIRE(u.d.UndoDepth() == depth + 1);
  u.d.Recalc();
  CHECK(u.F("H20") == "=SUM(C2:C3)");
  REQUIRE(u.d.Undo());
  u.d.Recalc();
  CHECK(u.F("H20") == "=SUM(B2:C3)");
}

TEST_CASE("Basics: a cut that moves a range's whole edge along it moves that edge") {
  yctest::Doc t;
  Fill(t);
  t.Set("H20", "=SUM(B2:B5)");     // B2 to B3: B3:B5
  t.Set("H21", "=SUM($B$2:$B$5)");
  t.Set("H22", "=SUM(B2:C5)");     // B2 isn't a whole edge of it: kept
  t.Set("H23", "=SUM(B1:B3)");     // B2 in its middle, B3 written over inside it: kept
  REQUIRE(t.d.AddName("NB", "=Sheet1!$B$2:$B$5"));
  AddRules(t, "SUM($B$2:$B$5)>0");
  REQUIRE(Move(t, Rect::Cell(1, 1), t.sheet(), 2, 1));  // B2 -> B3
  CHECK(t.F("H20") == "=SUM(B3:B5)");
  CHECK(t.N("H20") == 22 + 42 + 52);  // B3 holds the 22 of B2 now
  CHECK(t.F("H21") == "=SUM($B$3:$B$5)");
  CHECK(t.F("H22") == "=SUM(B2:C5)");
  CHECK(t.F("H23") == "=SUM(B1:B3)");
  CHECK(Name(t, "NB") == "Sheet1!$B$3:$B$5");
  CHECK(CfFormula(t) == "SUM($B$3:$B$5)>0");
  CHECK(DvFormula(t) == "SUM($B$3:$B$5)>0");
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.F("H20") == "=SUM(B2:B5)");
  CHECK(Name(t, "NB") == "Sheet1!$B$2:$B$5");
}

TEST_CASE("Basics: a cut edge moved outward stretches the range; sideways or past its other edge it stays") {
  {
    yctest::Doc t;
    Fill(t);
    t.Set("H20", "=SUM(B2:B4)");  // its last row to B9: B2:B9
    t.Set("H21", "=SUM(B2:B5)");  // B4 in its middle: kept
    REQUIRE(Move(t, Rect::Cell(3, 1), t.sheet(), 8, 1));
    CHECK(t.F("H20") == "=SUM(B2:B9)");
    CHECK(t.F("H21") == "=SUM(B2:B5)");
  }
  {
    yctest::Doc t;
    Fill(t);
    t.Set("H20", "=SUM(B2:D3)");  // D2:D5 to H2: its last column goes along, B2:H3
    REQUIRE(t.d.AddName("NC", "=Sheet1!$B$2:$D$3"));
    REQUIRE(Move(t, Rect{1, 3, 4, 3}, t.sheet(), 1, 7));
    CHECK(t.F("H20") == "=SUM(B2:H3)");
    CHECK(Name(t, "NC") == "Sheet1!$B$2:$H$3");
  }
  {
    yctest::Doc t;
    Fill(t);
    t.Set("H20", "=SUM(B2:B5)");
    REQUIRE(Move(t, Rect::Cell(1, 1), t.sheet(), 1, 8));  // B2 -> I2: sideways
    CHECK(t.F("H20") == "=SUM(B2:B5)");
    REQUIRE(Move(t, Rect::Cell(1, 8), t.sheet(), 1, 1));  // back
    REQUIRE(Move(t, Rect::Cell(1, 1), t.sheet(), 6, 1));  // B2 -> B7: past B5
    CHECK(t.F("H20") == "=SUM(B2:B5)");
  }
  {
    yctest::Doc t;
    Fill(t);
    t.Set("H20", "=SUM(B2:B5)");  // B2 -> B5, onto its last cell: B3:B5
    REQUIRE(Move(t, Rect::Cell(1, 1), t.sheet(), 4, 1));
    CHECK(t.F("H20") == "=SUM(B3:B5)");
  }
}

TEST_CASE("Basics: cells written over at a range's edge by a block reaching past it leave the range") {
  yctest::Doc t;
  Fill(t);
  t.Set("H20", "=SUM(B5:B9)");  // B4:B5 written over: B6:B9
  t.Set("H21", "=SUM(B3:B6)");  // its first row moves to B5, B4 written over: B5:B6
  t.Set("H22", "=SUM(B2:B5)");  // its first rows move to B4:B5: B4:B5
  t.Set("H23", "=SUM(B1:B3)");  // its last rows move out to B4:B5: B1:B5
  t.Set("H24", "=SUM(B4:B5)");  // written over whole: #REF!
  REQUIRE(Move(t, Rect{1, 1, 2, 1}, t.sheet(), 3, 1));  // B2:B3 -> B4:B5
  CHECK(t.F("H20") == "=SUM(B6:B9)");
  CHECK(t.F("H21") == "=SUM(B5:B6)");
  CHECK(t.F("H22") == "=SUM(B4:B5)");
  CHECK(t.F("H23") == "=SUM(B1:B5)");
  CHECK(t.F("H24") == "=SUM(#REF!)");
  // written over inside it, by cells from outside: kept (B5 -> B4 leaves B2:B4)
  yctest::Doc u;
  Fill(u);
  u.Set("H20", "=SUM(B2:B4)");
  REQUIRE(Move(u, Rect::Cell(4, 1), u.sheet(), 3, 1));
  CHECK(u.F("H20") == "=SUM(B2:B4)");
}

TEST_CASE("Basics: a range's edge cut to another sheet leaves it; whole rows and columns move their edges") {
  {
    yctest::Doc t;
    Fill(t);
    const uint32_t other = t.d.AddSheet("Other");
    t.Set("H20", "=SUM(B2:B5)");
    t.Set("H21", "=SUM(B2:F2)");
    REQUIRE(t.d.AddName("NB", "=Sheet1!$B$2:$B$5"));
    REQUIRE(Move(t, Rect::Cell(1, 1), other, 9, 7));  // B2 -> Other!H10
    CHECK(t.F("H20") == "=SUM(B3:B5)");
    CHECK(t.F("H21") == "=SUM(C2:F2)");
    CHECK(Name(t, "NB") == "Sheet1!$B$3:$B$5");
  }
  {
    yctest::Doc t;
    Fill(t);
    t.Set("H20", "=SUM(2:2)");    // moved whole: 12:12
    t.Set("H21", "=SUM(B1:D2)");  // its last row: B1:D12
    t.Set("H22", "=SUM(2:3)");    // its first row past its last: kept
    const uint32_t last = yc::core::kMaxCols - 1;
    REQUIRE(Move(t, Rect{1, 0, 1, last}, t.sheet(), 11, 0));  // row 2 -> row 12
    CHECK(t.F("H20") == "=SUM(12:12)");
    CHECK(t.F("H21") == "=SUM(B1:D12)");
    CHECK(t.F("H22") == "=SUM(2:3)");
  }
  {
    yctest::Doc t;
    Fill(t);
    t.Set("H20", "=SUM(B:D)");    // its last column: B:F
    t.Set("H21", "=SUM(B2:D3)");  // B2:F3
    t.Set("H22", "=SUM(A:B)");    // kept
    REQUIRE(Move(t, Rect{0, 3, yc::core::kRowEnd, 3}, t.sheet(), 0, 5));  // D:D (open-ended, as Python sends it) -> F:F
    CHECK(t.N("F2") == 24);
    CHECK(t.V("D2").IsEmpty());
    CHECK(t.F("H20") == "=SUM(B:F)");
    CHECK(t.F("H21") == "=SUM(B2:F3)");
    CHECK(t.F("H22") == "=SUM(A:B)");
  }
}
