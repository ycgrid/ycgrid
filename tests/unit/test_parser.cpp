// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "formula/parser.h"

using namespace yc;

namespace {
struct FakeSheets : formula::SheetResolver {
  int32_t FindSheet(std::string_view n) const override { return n == "資料" ? 7 : -1; }
  std::string SheetName(int32_t id) const override { return id == 7 ? "資料" : ""; }
};

std::string RoundTrip(const std::string& f, core::CellPos o = {0, 0}) {
  FakeSheets ns;
  auto r = formula::Parse(f, o, &ns);
  if (!r.ok) return "ERR:" + r.error;
  return formula::ToA1(r.root, o, &ns);
}
}  // namespace

TEST_CASE("公式解析與 A1 還原") {
  CHECK(RoundTrip("1+2*3") == "1+2*3");
  CHECK(RoundTrip("SUM(A1:B2,$C$3)") == "SUM(A1:B2,$C$3)");
  CHECK(RoundTrip("sum(a1)") == "SUM(A1)");
  CHECK(RoundTrip("A:A") == "A:A");
  CHECK(RoundTrip("2:3") == "2:3");
  CHECK(RoundTrip("資料!B2") == "資料!B2");
  CHECK(RoundTrip("\"a\"\"b\"&C1") == "\"a\"\"b\"&C1");
  CHECK(RoundTrip("-2^2") == "-2^2");
  CHECK(RoundTrip("IF(A1>=1,\"y\",)") == "IF(A1>=1,\"y\",)");
  CHECK(RoundTrip("{1,2;3,4}") == "{1,2;3,4}");
  CHECK(RoundTrip("50%") == "50%");
  CHECK(RoundTrip("_xlfn.XLOOKUP(1,A:A,B:B)") == "XLOOKUP(1,A:A,B:B)");
  CHECK(RoundTrip("SUM(1,").rfind("ERR:", 0) == 0);
  CHECK(RoundTrip("VLOOKUP(1)").rfind("ERR:", 0) == 0);  // too few arguments
}

TEST_CASE("共用公式：向下填滿的公式有相同 R1C1 鍵") {
  FakeSheets ns;
  auto a = formula::Parse("A1+B1", {0, 2}, &ns);
  auto b = formula::Parse("A2+B2", {1, 2}, &ns);
  REQUIRE(a.ok);
  REQUIRE(b.ok);
  CHECK(formula::SerializeKey(a.root) == formula::SerializeKey(b.root));
  auto c = formula::Parse("$A$1+B2", {1, 2}, &ns);
  CHECK(formula::SerializeKey(a.root) != formula::SerializeKey(c.root));
  CHECK(formula::ToA1(a.root, {9, 2}, &ns) == "A10+B10");
}

TEST_CASE("F4 cycles the reference at the caret", "[parser]") {
  auto f4 = [](std::string t, int b, int e = -1) {
    int ob = 0, oe = 0;
    if (!formula::CycleAbsoluteRef(t, b, e < 0 ? b : e, &ob, &oe)) return std::string("none");
    return t + "|" + std::to_string(ob) + "," + std::to_string(oe);
  };
  CHECK(f4("=A1", 3) == "=$A$1|1,5");
  CHECK(f4("=$A$1", 5) == "=A$1|1,4");
  CHECK(f4("=A$1", 4) == "=$A1|1,4");
  CHECK(f4("=$A1", 4) == "=A1|1,3");
  // only the end of a range the caret is at, past closing parentheses (Excel 16, measured: =SUM(B2:B4), F4 is
  // =SUM(B2:$B$4)); nothing when the caret is at no reference (=B2+5), as Excel
  CHECK(f4("=SUM(A1:B3)", 7) == "=SUM($A$1:B3)|5,9");
  CHECK(f4("=SUM(A1:B3)", 10) == "=SUM(A1:$B$3)|8,12");
  CHECK(f4("=SUM(A1:B3)", 11) == "=SUM(A1:$B$3)|8,12");
  CHECK(f4("=SUM(A1:B3)", 8) == "=SUM(A1:$B$3)|8,12");
  CHECK(f4("=A1+B2*2", 8) == "none");
  CHECK(f4("=A1+B2 ", 7) == "none");
  CHECK(f4("=A1+B2,", 7) == "none");
  CHECK(f4("=A1+B2", 2) == "=$A$1+B2|1,5");
  CHECK(f4("=A1+B2", 4) == "=A1+$B$2|4,8");
  CHECK(f4("=A1+B2", 0) == "none");
  // a selection changes the ends it touches (=B2:C3 with C3 selected: =B2:$C$3)
  CHECK(f4("=B2:C3", 4, 6) == "=B2:$C$3|4,8");
  CHECK(f4("=B2:C3", 2, 6) == "=$B$2:$C$3|1,10");
  // a selection changes every reference in it
  CHECK(f4("=A1+B2", 0, 6) == "=$A$1+$B$2|1,10");
  // sheet prefixes stay; strings, function names and long words are not references
  CHECK(f4("=Sheet2!C5", 10) == "=Sheet2!$C$5|8,12");
  CHECK(f4("=\"A1\"", 5) == "none");
  CHECK(f4("=LOG10(100)", 6) == "none");
  CHECK(f4("=TOTAL", 6) == "none");
  CHECK(f4("=1+2", 4) == "none");
}

TEST_CASE("A formula nested too deeply is an error, not a stack overflow", "[parser]") {
  FakeSheets ns;
  const std::string deep = std::string(20000, '(') + "1" + std::string(20000, ')');
  auto r = formula::Parse(deep, {0, 0}, &ns);
  CHECK_FALSE(r.ok);
  CHECK_FALSE(r.error.empty());  // "The formula is nested too deeply" (translated in the Chinese UI)
  auto minus = formula::Parse(std::string(20000, '-') + "1", {0, 0}, &ns);
  CHECK_FALSE(minus.ok);
  CHECK(formula::Parse(std::string(100, '(') + "1" + std::string(100, ')'), {0, 0}, &ns).ok);  // a reasonable depth still parses
}

TEST_CASE("Typed text that is a signed amount is not a formula", "[parser]") {
  CHECK(formula::LooksLikeFormula("=1"));
  CHECK(formula::LooksLikeFormula("-A1"));
  CHECK(formula::LooksLikeFormula("+SUM(A1:A3)"));
  CHECK_FALSE(formula::LooksLikeFormula("-5"));
  CHECK_FALSE(formula::LooksLikeFormula("+.5"));
  CHECK_FALSE(formula::LooksLikeFormula("-$5"));   // a currency amount, not the name $5
  CHECK_FALSE(formula::LooksLikeFormula("+$1,000"));
  CHECK_FALSE(formula::LooksLikeFormula("-"));
}
