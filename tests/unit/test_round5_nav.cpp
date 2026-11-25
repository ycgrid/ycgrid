// SPDX-License-Identifier: Apache-2.0
// Round 5 (navigation): Ctrl+arrow and Range.End pass over hidden rows and columns, as in Excel
#include <catch2/catch_test_macros.hpp>

#include "engine/navigate.h"
#include "test_helpers.h"

namespace engine = yc::engine;

TEST_CASE("Round5: Ctrl+arrow passes over hidden rows and columns") {
  yctest::Doc t;
  for (int i = 1; i <= 10; ++i) t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
  t.d.HideRows(t.sheet(), 8, 9, true);  // rows 9:10
  const auto& s = *t.d.wb().sheets[0];
  CHECK(engine::CtrlJump(s, 0, 0, true, +1) == 7);  // A1 -> A8, the block's last shown cell
  t.d.HideRows(t.sheet(), 3, 4, true);  // rows 4:5
  t.Set("A5", "");
  const auto& s2 = *t.d.wb().sheets[0];
  CHECK(engine::CtrlJump(s2, 2, 0, true, +1) == 7);  // A3 -> A8: the hidden rows (one blank) are passed over
  CHECK(engine::CtrlJump(s2, 7, 0, true, -1) == 0);  // A8 -> A1
  // from a blank cell: the next shown value, never one in a hidden row
  t.Set("C9", "x");  // row 9 is hidden
  t.Set("C12", "y");
  const auto& s3 = *t.d.wb().sheets[0];
  CHECK(engine::CtrlJump(s3, 0, 2, true, +1) == 11);
  // the edge: the last shown row when nothing is found
  t.d.HideRows(t.sheet(), s3.row_limit - 3, s3.row_limit - 1, true);
  const auto& s4 = *t.d.wb().sheets[0];
  CHECK(engine::CtrlJump(s4, 11, 2, true, +1) == s4.row_limit - 4);
  // columns
  t.Set("E20", "1");
  t.Set("F20", "2");
  t.Set("G20", "3");
  t.d.HideCols(t.sheet(), 6, 6, true);  // column G
  const auto& s5 = *t.d.wb().sheets[0];
  CHECK(engine::CtrlJump(s5, 19, 4, false, +1) == 5);  // E20 -> F20
  // no hidden lines: as before
  CHECK(engine::CtrlJump(s5, 19, 0, false, +1) == 4);
}
