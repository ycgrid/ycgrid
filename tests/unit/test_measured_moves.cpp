// SPDX-License-Identifier: Apache-2.0
// Cut and paste against Excel itself: what a range a formula reads becomes when cells that hold part of it, or cells
// next to it, are cut and pasted over or beside it (excel_measured_moves.inc: every cut a probe of Excel 16 measured,
// over one column, over B2:D5 and over random ranges). A change to how a move rewrites references that fixes one case
// and breaks another fails here: the measured table decides, not the cases someone picked.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

struct Cut {
  int r0, c0, r1, c1;  // the range =SUM(...) in Z40 reads, 1-based
  int s0, t0, s1, t1;  // the cells cut
  int dr, dc;          // where they go
  int w0, wc0, w1, wc1;  // what the formula reads after it (all 0: =SUM(#REF!))
};

const Cut kCuts[] = {
#include "excel_measured_moves.inc"
};

std::string A1(int r, int c) {
  std::string col;
  for (int n = c; n > 0; n = (n - 1) / 26) col.insert(col.begin(), static_cast<char>('A' + (n - 1) % 26));
  return col + std::to_string(r);
}

std::string Range(int r0, int c0, int r1, int c1) { return A1(r0, c0) + ":" + A1(r1, c1); }

// =SUM(B2) and =SUM(B2:B2) name the same cells
std::string Normal(std::string f) {
  const auto open = f.find('('), close = f.rfind(')');
  if (open == std::string::npos || close == std::string::npos) return f;
  const std::string inner = f.substr(open + 1, close - open - 1);
  if (inner.find(':') == std::string::npos && inner != "#REF!") return f.substr(0, open + 1) + inner + ":" + inner + ")";
  return f;
}

}  // namespace

TEST_CASE("Cut and paste: every cut measured in Excel 16 leaves the formulas reading what Excel's read") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  std::vector<std::string> wrong;
  for (const Cut& c : kCuts) {
    REQUIRE(t.Set("Z40", "=SUM(" + Range(c.r0, c.c0, c.r1, c.c1) + ")"));
    const yc::core::Rect src{static_cast<uint32_t>(c.s0 - 1), static_cast<uint32_t>(c.t0 - 1), static_cast<uint32_t>(c.s1 - 1),
                             static_cast<uint32_t>(c.t1 - 1)};
    t.d.Begin("Move");
    const bool ok = t.d.MoveCells(sh, src, sh, static_cast<uint32_t>(c.s0 - 1 + c.dr), static_cast<uint32_t>(c.t0 - 1 + c.dc));
    t.d.Commit();
    const std::string want = c.w0 == 0 ? "=SUM(#REF!)" : "=SUM(" + Range(c.w0, c.wc0, c.w1, c.wc1) + ")";
    const std::string got = t.F("Z40");
    if (!ok || Normal(got) != want) {
      wrong.push_back(Range(c.r0, c.c0, c.r1, c.c1) + " cut " + Range(c.s0, c.t0, c.s1, c.t1) + " by (" + std::to_string(c.dr) + "," +
                      std::to_string(c.dc) + "): Excel " + want + ", YCGrid " + (ok ? got : "refused"));
    }
  }
  for (size_t i = 0; i < wrong.size() && i < 40; ++i) UNSCOPED_INFO(wrong[i]);
  CHECK(wrong.empty());
}
