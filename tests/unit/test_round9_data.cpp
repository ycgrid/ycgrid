// SPDX-License-Identifier: Apache-2.0
// Round 9 (data tools, measured against Excel 16 through its object model): Text to Columns fills its whole block
// and Subtotal's grand total sums the detail rows only
#include <catch2/catch_test_macros.hpp>

#include "core/i18n.h"
#include "test_helpers.h"

#include <string>

using yc::core::Rect;

TEST_CASE("Round9: Text to Columns empties the rest of its block in a row with fewer fields") {
  yctest::Doc t;
  t.Set("A1", "a,b,c,d");
  t.Set("A2", "1;2");
  t.Set("D2", "old");
  t.Set("E2", "keep");
  yc::engine::TextToColumnsOptions o;
  o.tab = false;
  o.comma = true;
  CHECK(t.d.TextToColumns(t.sheet(), Rect{0, 0, 1, 0}, 0, 0, o) == 4);
  t.d.Recalc();
  CHECK(t.T("D1") == "d");
  CHECK(t.T("A2") == "1;2");
  CHECK(t.V("B2").IsEmpty());
  CHECK(t.V("D2").IsEmpty());  // it kept "old" beside the new fields
  CHECK(t.T("E2") == "keep");  // past the block
  // empty fields at a row's end don't widen the block (Excel 16, measured: ,lead,,trail, with consecutive commas as
  // one left the old value in D)
  yctest::Doc u;
  u.Set("A1", "a,b,c");
  u.Set("A2", ",x,,y,");
  u.Set("D2", "old");
  o.consecutive = true;
  CHECK(u.d.TextToColumns(u.sheet(), Rect{0, 0, 1, 0}, 0, 0, o) == 3);
  u.d.Recalc();
  CHECK(u.T("D2") == "old");
}

TEST_CASE("Round9: Subtotal's grand total sums the detail rows, not the last group's total") {
  const yc::i18n::Lang was = yc::i18n::Current();
  yc::i18n::SetCurrent(yc::i18n::Lang::En);
  yctest::Doc t;
  t.Set("A1", "Region");
  t.Set("B1", "Qty");
  const char* const regions[] = {"East", "East", "West", "West"};
  for (int i = 0; i < 4; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), regions[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string((i + 1) * 10));
  }
  std::string err;
  REQUIRE(t.d.Subtotal(t.sheet(), Rect{0, 0, 4, 1}, 0, 9, {1}, true, false, true, &err));
  t.d.Recalc();
  CHECK(t.F("B4") == "=SUBTOTAL(9,B2:B3)");
  CHECK(t.F("B7") == "=SUBTOTAL(9,B5:B6)");
  CHECK(t.F("B8") == "=SUBTOTAL(9,B2:B6)");  // Excel's: =SUBTOTAL(9,C2:C10) over a group total in row 11 (measured)
  CHECK(t.N("B8") == 100);
  yc::i18n::SetCurrent(was);
}
