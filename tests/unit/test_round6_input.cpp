// SPDX-License-Identifier: Apache-2.0
// Round 6 (input): a cell formatted as Text keeps what is typed as text, as in Excel
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

using yc::core::Rect;

TEST_CASE("Round6: a cell formatted as Text keeps what is typed as text, formulas too") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.d.ModifyStyle(sh, Rect{0, 0, 9, 0}, [&](yc::model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString("@"); });
  REQUIRE(t.d.IsTextFormatted(sh, 0, 0));
  CHECK_FALSE(t.d.IsTextFormatted(sh, 0, 1));
  t.Set("A1", "00123");  // a code keeps its zeros
  t.Set("A2", "30");
  t.Set("A3", "=1+1");   // a formula typed there is text, shown as typed
  t.Set("A4", "1/2");    // no date
  t.Set("A5", "TRUE");
  t.Set("A6", "'abc");   // the apostrophe still marks text, and isn't kept
  t.Set("A7", "=1+");    // nothing to complain about: it's text
  REQUIRE(t.V("A1").IsString());
  CHECK(t.V("A1").str() == "00123");
  REQUIRE(t.V("A2").IsString());
  CHECK(t.V("A2").str() == "30");
  REQUIRE(t.V("A3").IsString());
  CHECK(t.V("A3").str() == "=1+1");
  CHECK(t.d.S(sh)->GetFormula(2, 0) == 0);
  CHECK(t.V("A4").str() == "1/2");
  CHECK(t.V("A5").str() == "TRUE");
  CHECK(t.V("A6").str() == "abc");
  CHECK(t.V("A7").str() == "=1+");
  CHECK(t.d.wb().styles->NumberFormat(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(3, 0))) == "@");  // no date format put over it
  // the same typing elsewhere is a number, a formula and a date
  t.Set("B1", "00123");
  t.Set("B3", "=1+1");
  CHECK(t.N("B1") == 123);
  CHECK(t.N("B3") == 2);
  // text in a Text cell that a formula reads: as Excel, text (SUM skips it, + converts it)
  t.Set("C1", "=SUM(A2)");
  t.Set("C2", "=A2+1");
  CHECK(t.N("C1") == 0);
  CHECK(t.N("C2") == 31);
}
