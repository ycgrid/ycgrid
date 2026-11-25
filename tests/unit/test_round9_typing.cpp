// SPDX-License-Identifier: Apache-2.0
// Round 9 (typing, measured against Excel 16 by posting the same keys to a hidden Excel): a percent cell's automatic
// percent entry and entries that start with a sign
#include <catch2/catch_test_macros.hpp>

#include "formula/numfmt.h"
#include "formula/parser.h"
#include "test_helpers.h"

#include <cmath>
#include <string>

namespace formula = yc::formula;

TEST_CASE("Round9: which formats make a typed number a percentage") {
  for (const char* code : {"0%", "0.00%", "0.0%;[Red]-0.0%", "#,##0.00%", "0 %", "[Red]0%", "%0", "0.0%;-0.0%;\"-\""}) {
    INFO(code);
    CHECK(formula::IsPercentFormat(code));
  }
  // a literal percent sign isn't a percent format (measured: 7 typed into 0"%" stays 7)
  for (const char* code : {"0\"%\"", "0\\%", "0.0\" pct\"", "General", "0.00", "m/d/yyyy", "@"}) {
    INFO(code);
    CHECK_FALSE(formula::IsPercentFormat(code));
  }
}

TEST_CASE("Round9: a number typed into a percent cell gets its percent sign") {
  CHECK(formula::PercentEntry("7", "0%") == "7%");
  CHECK(formula::PercentEntry(".5", "0%") == ".5%");
  CHECK(formula::PercentEntry("-5", "0%") == "-5%");
  CHECK(formula::PercentEntry("(5)", "0%") == "(5)%");
  CHECK(formula::PercentEntry("1,234.5", "0.00%") == "1,234.5%");
  CHECK(formula::PercentEntry("1e2", "0%") == "1e2%");
  // already a percentage, a formula, text, or not a percent cell: as typed
  CHECK(formula::PercentEntry("5%", "0%") == "5%");
  CHECK(formula::PercentEntry("=5", "0%") == "=5");
  CHECK(formula::PercentEntry("abc", "0%") == "abc");
  CHECK(formula::PercentEntry("5a%", "0%") == "5a%");
  CHECK(formula::PercentEntry("7", "General") == "7");
  CHECK(formula::PercentEntry("7", "0\"%\"") == "7");
  // the values the cell then holds
  yctest::Doc t;
  t.Set("A1", formula::PercentEntry("7", "0%"));
  CHECK(t.N("A1") == 0.07);
  t.Set("A2", formula::PercentEntry("12.345", "0%"));
  CHECK(std::fabs(t.N("A2") - 0.12345) < 1e-15);
}

TEST_CASE("Round9: an entry that starts with a sign is a formula unless it is an amount") {
  for (const char* text : {"+1+2", "-1-2", "+1/2", "-2^2", "+1&2", "+1=1", "+2024/1/5", "+1e3+1", "+1%+1", "+B2", "-A1"}) {
    INFO(text);
    CHECK(formula::LooksLikeFormula(text));
  }
  for (const char* text : {"-5", "+5", "+.5", "-.5", "-$5", "-5%", "- 5", "-1 1/2", "+1e3", "-1,234.5", "+", "-"}) {
    INFO(text);
    CHECK_FALSE(formula::LooksLikeFormula(text));
  }
  yctest::Doc t;
  t.Set("A1", "+1+2");
  CHECK(t.F("A1") == "=1+2");  // the + goes, as in Excel
  CHECK(t.N("A1") == 3);
  t.Set("A2", "-1-2");
  CHECK(t.F("A2") == "=-1-2");
  t.Set("A3", "-2^2");
  CHECK(t.N("A3") == 4);  // Excel negates first
  t.Set("B1", "5");
  t.Set("A4", "+B1*2");
  CHECK(t.F("A4") == "=+B1*2");  // a + before a reference stays (measured)
  CHECK(t.N("A4") == 10);
  t.Set("A5", "-5");
  CHECK(t.F("A5") == "-5");  // an amount stays a number
  CHECK(t.N("A5") == -5);
}
