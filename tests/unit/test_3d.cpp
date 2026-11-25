// SPDX-License-Identifier: Apache-2.0
// 3D references: Sheet1:Sheet3!A1
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/snapshot.h"
#include "formula/parser.h"

#include <utility>

TEST_CASE("3D references") {
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  const uint32_t s2 = t.d.AddSheet("Sheet2");
  const uint32_t s3 = t.d.AddSheet("My Sheet");
  const uint32_t s4 = t.d.AddSheet("Sheet4");
  t.Set("A1", "1", s1);
  t.Set("A1", "10", s2);
  t.Set("A2", "5", s2);
  t.Set("A1", "100", s3);
  t.Set("A1", "1000", s4);
  t.d.RenameSheet(s1, "Sheet1");
  t.Set("C1", "=SUM(Sheet1:Sheet2!A1)", s4);
  CHECK(t.N("C1", s4) == 11);
  t.Set("C2", "=SUM('Sheet1:My Sheet'!A1:A2)", s4);
  CHECK(t.N("C2", s4) == 116);
  CHECK(t.F("C2", s4) == "=SUM('Sheet1:My Sheet'!A1:A2)");
  CHECK(t.F("C1", s4) == "=SUM(Sheet1:Sheet2!A1)");
  t.Set("C3", "=COUNT(Sheet1:'My Sheet'!A1:A5)", s4);
  CHECK(t.N("C3", s4) == 4);
  t.Set("C4", "=AVERAGE(Sheet1:Sheet2!A1)", s4);
  CHECK(t.N("C4", s4) == 5.5);
  t.Set("C5", "=MAX(Sheet2:Sheet1!A1)", s4);  // reversed order works too
  CHECK(t.N("C5", s4) == 10);
  t.Set("C6", "=Sheet1:Sheet2!A1", s4);
  CHECK(t.T("C6", s4) == "#VALUE!");
  // values change, sheets move into or out of the span
  t.Set("A1", "20", s2);
  CHECK(t.N("C1", s4) == 21);
  t.d.MoveSheet(s4, 1);  // Sheet4 now sits between Sheet1 and Sheet2
  t.d.Recalc();
  CHECK(t.N("C1", s4) == 1021);
  // deleting an end sheet ends the reference at the next sheet inside, as Excel's help says (Sheet4 sits between)
  t.d.DeleteSheet(s2);
  t.d.Recalc();
  CHECK(t.F("C1", s4) == "=SUM(Sheet1:Sheet4!A1)");
  CHECK(t.N("C1", s4) == 1001);
  CHECK(t.N("C5", s4) == 1000);  // the reversed one too: MAX(Sheet4:Sheet1!A1)
  t.Set("C7", "=SUM(Nope:Sheet1!A1)", s4);
  CHECK(t.T("C7", s4) == "#REF!");
}

namespace {
// parse R1C1 text at the origin and give it back in both styles
std::pair<std::string, std::string> R1C1Round(yctest::Doc& t, const char* text, uint32_t r, uint32_t c) {
  const auto pr = yc::formula::Parse(text, yc::core::CellPos{r, c}, &t.d, true);
  if (!pr.ok) return {"error: " + pr.error, ""};
  return {yc::formula::ToA1(pr.root, yc::core::CellPos{r, c}, &t.d), yc::formula::ToR1C1(pr.root, yc::core::CellPos{r, c}, &t.d)};
}
}  // namespace

TEST_CASE("R1C1 reference style") {
  yctest::Doc t;
  // origin C3 (row 2, column 2)
  CHECK(R1C1Round(t, "R[-1]C+RC[-2]", 2, 2) == std::make_pair(std::string("C2+A3"), std::string("R[-1]C+RC[-2]")));
  CHECK(R1C1Round(t, "SUM(R1C1:R2C3)", 2, 2) == std::make_pair(std::string("SUM($A$1:$C$2)"), std::string("SUM(R1C1:R2C3)")));
  CHECK(R1C1Round(t, "SUM(R2:R4)", 2, 2).first == "SUM($2:$4)");
  CHECK(R1C1Round(t, "SUM(C[-1])", 2, 2).first == "SUM(B:B)");
  CHECK(R1C1Round(t, "SUM(C[-1]:C)", 2, 2).second == "SUM(C[-1]:C)");
  CHECK(R1C1Round(t, "ROUND(R[1]C[1],2)+COUNT(C1)", 2, 2).first == "ROUND(D4,2)+COUNT($A:$A)");
  CHECK(R1C1Round(t, "R1C1#", 2, 2).first == "$A$1#");
  // A1 names aren't references in R1C1 style; R1C1 text isn't in A1 style (RC is a name, R1C1 is cell R1C1)
  CHECK(R1C1Round(t, "A1", 2, 2).first == "A1");
  // typing in R1C1 style stores the same formula as A1 style
  t.Set("A1", "5");
  REQUIRE(t.d.SetInput(t.sheet(), 1, 0, "=R[-1]C*2", nullptr, yc::engine::kSrcUser, true));
  t.d.Recalc();
  CHECK(t.F("A2") == "=A1*2");
  CHECK(t.N("A2") == 10);
  CHECK(yc::engine::FormulaTextOf(t.d.wb(), t.sheet(), 1, 0, true) == "=R[-1]C*2");
  CHECK(yc::formula::RangeR1C1(yc::core::Rect{0, 0, 1, 1}, nullptr) == "R1C1:R2C2");
  const yc::core::CellPos o{2, 2};
  CHECK(yc::formula::RangeR1C1(yc::core::Rect::Cell(2, 0), &o) == "RC[-2]");
}
