// SPDX-License-Identifier: Apache-2.0
// Round 6 (calculation and editing): a last addition or subtraction that cancels shows 0, as Excel's (KB 78113);
// F4 toggles whole columns and rows
#include <catch2/catch_test_macros.hpp>

#include "formula/parser.h"
#include "test_helpers.h"

#include <cmath>
#include <string>

namespace formula = yc::formula;

TEST_CASE("Round6: a formula whose last step cancels out shows 0, as Excel's") {
  yctest::Doc t;
  t.Set("A1", "=(0.1+0.2)-0.3");
  t.Set("A2", "=0.5-0.4-0.1");
  t.Set("A3", "=(0.5-0.4-0.1)");    // parentheses are the last step: the residue stays, as in Excel
  t.Set("A4", "=1*(0.5-0.4-0.1)");  // a product last: it stays
  t.Set("A5", "=(4/3-1)*3-1");
  t.Set("A6", "=((4/3-1)*3-1)");
  t.Set("A7", "=(4/3-1)*3-1+0");    // the residue plus 0 is the residue
  t.Set("B1", "=0.1+0.2");
  t.Set("B2", "0.3");
  t.Set("B3", "=B1-B2");            // a difference of two cells: 0 (it showed 5.55E-17)
  t.Set("B4", "=5-3");
  t.Set("B5", "=0.3-0.1");
  t.Set("B6", "=1E-20-0");          // a tiny number is no cancellation
  t.Set("B7", "=1.333+1.225-1.333-1.225");
  CHECK(t.N("A1") == 0);
  CHECK(t.N("A2") == 0);
  CHECK(t.N("A3") != 0);
  CHECK(std::fabs(t.N("A3")) < 1e-16);
  CHECK(t.N("A4") != 0);
  CHECK(t.N("A5") == 0);
  CHECK(t.N("A6") != 0);
  CHECK(t.N("A7") != 0);
  CHECK(t.N("B3") == 0);
  CHECK(t.T("B3") == "0");
  CHECK(t.N("B4") == 2);
  CHECK(t.N("B5") == 0.3 - 0.1);
  CHECK(t.N("B6") == 1e-20);
  CHECK(t.N("B7") == 0);  // KB 78113's own example
  // what Python's Evaluate reads goes the same way
  std::string err;
  CHECK(t.d.EvaluateFormula(t.sheet(), "=0.1+0.2-0.3", &err).AsDouble() == 0);
}

TEST_CASE("Round6: F4 toggles whole columns and rows, as Excel's") {
  auto f4 = [](std::string t, int caret) {
    int ob = 0, oe = 0;
    if (!formula::CycleAbsoluteRef(t, caret, caret, &ob, &oe)) return std::string("none");
    return t;
  };
  CHECK(f4("=A:A", 4) == "=$A:$A");
  CHECK(f4("=$A:$A", 6) == "=A:A");
  CHECK(f4("=SUMIF(A:B,1,C:C)", 16) == "=SUMIF(A:B,1,$C:$C)");
  CHECK(f4("=SUMIF(A:B,1,C:C)", 10) == "=SUMIF($A:$B,1,C:C)");
  CHECK(f4("=SUM(1:3)", 8) == "=SUM($1:$3)");
  CHECK(f4("=SUM($1:$3)", 10) == "=SUM(1:3)");
  CHECK(f4("=Sheet2!B:B", 11) == "=Sheet2!$B:$B");
  // numbers, times in text and names are not references
  CHECK(f4("=10*2", 5) == "none");
  CHECK(f4("=\"10:30\"", 7) == "none");
  CHECK(f4("=2.5+1", 6) == "none");
  // a range of cells: the end at the caret (Excel 16, measured: =B2:C3, F4 is =B2:$C$3)
  CHECK(f4("=A1:B2", 6) == "=A1:$B$2");
}
