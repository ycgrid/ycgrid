// SPDX-License-Identifier: Apache-2.0
// Typed input measured against Excel 16 (Range.Formula of each text, then Value2 and NumberFormat; 2026-11-09): what
// stays text, what becomes a number and the format it gets, for spaces around booleans, errors, times and dates,
// times with a trailing colon or spaces around their colons, minutes past 59, mixed fractions with the marks of a
// number, thousands separators and Excel's largest and smallest numbers. The same reading serves =text+0 and VALUE.
#include <catch2/catch_test_macros.hpp>

#include "formula/numfmt.h"
#include "test_helpers.h"

#include <cmath>
#include <string>
#include <utility>

namespace formula = yc::formula;

namespace {

struct Typed {
  const char* text;
  double value;
  const char* format;
};

void CheckNumber(const Typed& c) {
  INFO(c.text);
  const auto p = formula::ParseInput(c.text);
  REQUIRE(p.value.IsNumeric());
  CHECK(std::fabs(p.value.AsDouble() - c.value) <= 1e-12 * std::fmax(1.0, std::fabs(c.value)));
  CHECK(p.format == c.format);
}

void CheckText(const char* text) {
  INFO(text);
  const auto p = formula::ParseInput(text);
  CHECK(p.value.IsString());
  CHECK(p.format.empty());
}

constexpr double kMin = 1.0 / 1440;  // a minute
const char* const kDollars = "$#,##0_);[Red]($#,##0)";

}  // namespace

TEST_CASE("Typed: booleans and error values take no spaces, and the newer errors stay text (measured)") {
  for (const char* t : {" TRUE", "TRUE ", "TRUE  ", " FALSE", "FALSE ", "true ", " True", " #N/A", "#N/A ", "#CALC!", "#SPILL!",
                        "#BLOCKED!", "#CONNECT!", "#FIELD!", "#BUSY!"})
    CheckText(t);
  CHECK(formula::ParseInput("TRUE").value.IsBool());
  CHECK(formula::ParseInput("True").value.IsBool());
  CHECK(formula::ParseInput("FALSE").value.IsBool());
  for (const char* t : {"#n/a", "#div/0!", "#GETTING_DATA", "#NULL!", "#VALUE!", "#REF!", "#NAME?", "#NUM!"}) {
    INFO(t);
    CHECK(formula::ParseInput(t).value.IsError());
  }
}

TEST_CASE("Typed: a date or time after a space is text, a number after one is a number (measured)") {
  for (const char* t : {" 9:30", " 1:30 pm", " 9:30 PM", " 12:00", " 2024/1/5", " 1/2", " 3:45.0"}) CheckText(t);
  CheckNumber({" 5", 5, ""});
  CheckNumber({" 1,234", 1234, "#,##0"});
  CheckNumber({" 1 1/2", 1.5, "# ?/?"});
  CheckNumber({"9:30 ", 9.5 / 24, "h:mm"});
  CheckNumber({"2024/1/5 ", 45296, "yyyy/m/d"});
}

TEST_CASE("Typed: AM and PM come after a space (measured: 1:30PM is text)") {
  for (const char* t : {"1:30PM", "1:30AM", "1:30am", "1:30p", "1:30a", "12:30a", "1PM", "1pm", "9:30:15PM", "9:30PM",
                        "1:30 P.M.", "1:30 A.M", "5-Jan-2024 9:30PM", "1/5 9:30PM", "9:30PM 1/5"})
    CheckText(t);
  CheckNumber({"1:30 P", 13.5 / 24, "h:mm AM/PM"});
  CheckNumber({"1 pm", 13.0 / 24, "h:mm AM/PM"});
  CheckNumber({"9:30 pm", 21.5 / 24, "h:mm AM/PM"});
}

TEST_CASE("Typed: spaces around a time's colons, a trailing colon and a trailing decimal point (measured)") {
  CheckNumber({"9:30:", 9.5 / 24, "h:mm"});
  CheckNumber({"9:30: ", 9.5 / 24, "h:mm"});
  CheckNumber({"9:30:00:", 9.5 / 24, "h:mm:ss"});
  CheckNumber({"9:30 PM:", 21.5 / 24, "h:mm AM/PM"});
  CheckNumber({"9 :30", 9.5 / 24, "h:mm"});
  CheckNumber({"9: 30", 9.5 / 24, "h:mm"});
  CheckNumber({"9 : 30 PM", 21.5 / 24, "h:mm AM/PM"});
  CheckNumber({"9 :30 AM", 9.5 / 24, "h:mm AM/PM"});
  CheckNumber({"9:30 :00", 9.5 / 24, "h:mm:ss"});
  CheckNumber({"9:30 :00 PM", 21.5 / 24, "h:mm:ss AM/PM"});
  CheckNumber({"9:30.", 9.5 / 24, "h:mm"});
  CheckNumber({"9:30 .", 9.5 / 24, "h:mm"});
  CheckNumber({"1:2.", 62 * kMin, "h:mm"});
  CheckNumber({"1:2:3.", 62 * kMin + 3.0 / 86400, "h:mm:ss"});
  CheckNumber({"9:", 9.0 / 24, "h:mm"});
  CheckNumber({"1:2.5", (62.5) / 86400, "mm:ss.0"});
  for (const char* t : {"9::30", "9:30::", "9:30:PM", "9:30: PM", "9: PM", "9:30.:", "1:2:3.4.", "1:.5", ":5", "1.:5"}) CheckText(t);
}

TEST_CASE("Typed: minutes or seconds past 59 are a number in General, within limits (measured)") {
  CheckNumber({"9:60", 10.0 / 24, ""});
  CheckNumber({"9:61", 9.0 / 24 + 61 * kMin, ""});
  CheckNumber({"9:99", 9.0 / 24 + 99 * kMin, ""});
  CheckNumber({"9:100", 9.0 / 24 + 100 * kMin, ""});
  CheckNumber({"9:9999", 9.0 / 24 + 9999 * kMin, ""});
  CheckNumber({"0:1440", 1, ""});
  CheckNumber({"0:60", 1.0 / 24, ""});
  CheckNumber({"12:60", 13.0 / 24, ""});
  CheckNumber({"23:60", 1, ""});
  CheckNumber({"23:120", 25.0 / 24, ""});
  CheckNumber({"9:30:60", 9.5 / 24 + 60.0 / 86400, ""});
  CheckNumber({"9:30:999", 9.5 / 24 + 999.0 / 86400, ""});
  CheckNumber({"9:60:00", 10.0 / 24, ""});
  CheckNumber({"23:59:99", 1 + 39.0 / 86400, ""});
  CheckNumber({"9:30:60.5", 9.5 / 24 + 60.5 / 86400, "mm:ss.0"});
  CheckNumber({"9999:00", 9999.0 / 24, "[h]:mm:ss"});
  CheckNumber({"00:30 PM", 12.5 / 24, "h:mm AM/PM"});
  CheckNumber({"09:30", 9.5 / 24, "h:mm"});
  for (const char* t : {"24:60", "25:60", "9:60:60", "12:60 PM", "11:60 PM", "1:60 AM", "9:60 AM", "9:30:60 PM", "012:30", "0009:30",
                        "0099:00", "099:00", "9:0099", "9:00099", "9:30:0099", "99999:00"})
    CheckText(t);
  // with a date too: the number in General
  const auto day = formula::ParseInput("1/5");
  REQUIRE(day.value.IsNumber());
  for (const char* t : {"1/5 9:60", "9:60 1/5"}) {
    INFO(t);
    const auto p = formula::ParseInput(t);
    REQUIRE(p.value.IsNumber());
    CHECK(std::fabs(p.value.AsDouble() - (day.value.AsDouble() + 10.0 / 24)) < 1e-9);
    CHECK(p.format.empty());
  }
}

TEST_CASE("Typed: the time before the date (measured: 9:30 1/5 is January 5 at 9:30)") {
  const double jan5 = formula::ParseInput("1/5").value.AsDouble();
  CheckNumber({"9:30 1/5", jan5 + 9.5 / 24, "yyyy/m/d h:mm"});
  CheckNumber({"9:30 AM 1/5 ", jan5 + 9.5 / 24, "yyyy/m/d h:mm"});
  CheckNumber({"9:30 PM 1/5", jan5 + 21.5 / 24, "yyyy/m/d h:mm"});
  CheckNumber({"9:30 2024/1/5", 45296 + 9.5 / 24, "yyyy/m/d h:mm"});
  CheckNumber({"9:30 PM 2024/1/5", 45296 + 21.5 / 24, "yyyy/m/d h:mm"});
  CheckNumber({"9:30 Jan 5", jan5 + 9.5 / 24, "yyyy/m/d h:mm"});
  CheckNumber({"9:30 5 Jan", jan5 + 9.5 / 24, "yyyy/m/d h:mm"});
  CheckText("1 1/2 9:30");
  CheckText("2024/1/5 2024/1/6");
}

TEST_CASE("Typed: spaces beside a date's separators don't count (measured)") {
  const auto jan2 = formula::ParseInput("1/2");
  REQUIRE(jan2.value.IsNumber());
  for (const char* t : {"1 /2", "1/ 2", "1 / 2", "1 -2", "1- 2", "1 - 2"}) CheckNumber({t, jan2.value.AsDouble(), jan2.format.c_str()});
  CheckNumber({"2024 /1/5", 45296, "yyyy/m/d"});
  CheckNumber({"2024/ 1/ 5", 45296, "yyyy/m/d"});
  CheckNumber({"1 /2/2024", 45293, "yyyy/m/d"});
}

TEST_CASE("Typed: a mixed fraction takes the marks of a number (measured)") {
  CheckNumber({"+1 1/2", 1.5, "# ?/?"});
  CheckNumber({"+ 1 1/2", 1.5, "# ?/?"});
  CheckNumber({"-1 1/2", -1.5, "# ?/?"});
  CheckNumber({"- 1 1/2", -1.5, "# ?/?"});
  CheckNumber({"(1 1/2)", -1.5, "# ?/?"});
  CheckNumber({"$1 1/2", 1.5, "# ?/?"});
  CheckNumber({"$ 1 1/2", 1.5, "# ?/?"});
  CheckNumber({"-$1 1/2", -1.5, "# ?/?"});
  CheckNumber({"$(1 1/2)", -1.5, "# ?/?"});
  CheckNumber({"1 1/2%", 0.015, "0.00%"});
  CheckNumber({"1 1/2 %", 0.015, "0.00%"});
  CheckNumber({"% 1 1/2", 0.015, "0.00%"});
  CheckNumber({" 1 1/2%", 0.015, "0.00%"});
  CheckNumber({"-1 1/2%", -0.015, "0.00%"});
  CheckNumber({"0 1/2%", 0.005, "0.00%"});
  CheckNumber({"1 3/16%", 0.011875, "0.00%"});
  CheckNumber({"1,000 1/2", 1000.5, "# ?/?"});
  CheckNumber({"1,234 1/2", 1234.5, "# ?/?"});
  CheckNumber({"1,2345 1/2", 12345.5, "# ?/?"});
  CheckNumber({"12345678901 1/2", 12345678901.5, "# ?/?"});
  CheckNumber({"123456789012345 1/2", 123456789012345.5, "# ?/?"});
  CheckNumber({"1234567890123456 1/2", 1234567890123450.5, "# ?/?"});  // the 16th digit becomes 0
  CheckNumber({"1 01/02", 1.5, "# ?/?"});
  CheckNumber({"1 1/10", 1.1, "# ??/??"});
  CheckNumber({"1 1/12345", 1 + 1.0 / 12345, "# ??/??"});
  CheckNumber({"1 12345/2", 6173.5, "# ?/?"});
  CheckNumber({"1 0/2", 1, "# ?/?"});
  CheckNumber({"1 1/2  ", 1.5, "# ?/?"});
  for (const char* t : {"1  1/2", "1 1/ 2", "1 1 /2", "1 1/123456", "1 123456/2", "1 1/123456789", "1.5 1/2", "1 1/2e3", "1 1/0", "1 1/2 $",
                        "1 1/2%%", "1,23 1/2", "1 1/2 1/2", "1 1/2.5", "1 1/2 PM"})
    CheckText(t);
}

TEST_CASE("Typed: thousands separators after a number that isn't 0, before three digits or more (measured)") {
  CheckNumber({"1,2345", 12345, "#,##0"});
  CheckNumber({"1,23456", 123456, "#,##0"});
  CheckNumber({"12,3456", 123456, "#,##0"});
  CheckNumber({"1234,567", 1234567, "#,##0"});
  CheckNumber({"12345,678", 12345678, "#,##0"});
  CheckNumber({"1,234,5678", 12345678, "#,##0"});
  CheckNumber({"1,2345,678", 12345678, "#,##0"});
  CheckNumber({"01,234", 1234, "#,##0"});
  CheckNumber({"1,2345.5", 12345.5, "#,##0.00"});
  CheckNumber({"1,234.", 1234, "#,##0.00"});
  CheckNumber({"1,2345e3", 12345000, "0.00E+00"});
  CheckNumber({"$1,2345", 12345, kDollars});
  CheckNumber({"1,2345%", 123.45, "0%"});
  CheckNumber({"-1,2345", -12345, "#,##0"});
  CheckNumber({"(1,2345)", -12345, "#,##0"});
  for (const char* t : {"0,123", "0,1234", "00,1234", "0,000", "00,000", "1,2", "1,23", "12,34", "123,45", "1234,5", "1,23.5", ",123",
                        "1,", "1,000,", "1,2,3", "1,23,456", "1,234,56", "1,234,5", "1,234.5,6"})
    CheckText(t);
}

TEST_CASE("Typed: Excel's largest and smallest numbers (measured)") {
  CheckNumber({"9.99999999999999e307", 9.99999999999999e307, "0.00E+00"});
  CheckNumber({"1e307", 1e307, "0.00E+00"});
  CheckNumber({"1E308%", 1e306, "0.00E+00"});
  CheckNumber({"1e-307", 1e-307, "0.00E+00"});
  for (const char* t : {"1e-308", "2.2e-308", "2.2250738585072014e-308", "5e-309", "1e-309"}) CheckNumber({t, 0, "0.00E+00"});
  for (const char* t : {"1e308", "1e+308", "-1e308", "1.7976931348623157e308", "1e309", "1e-310", "9e-310", "1e-315", "1e-320",
                        "1e-324", "1e-400", "1e-308%"})
    CheckText(t);
}

TEST_CASE("Typed: text read as a number in a formula follows the same rules (measured)") {
  yctest::Doc d;
  d.Set("A1", "=\"1,2345\"+0");
  d.Set("A2", "=VALUE(\"1 1/2%\")");
  d.Set("A3", "=TIMEVALUE(\"9:30:\")");
  d.Set("A4", "=\"9:60\"+0");
  d.Set("A5", "=\" TRUE\"+0");
  d.Set("A6", "=\"1:30PM\"+0");
  d.Set("A7", "=\"0,123\"+0");
  d.Set("A8", "=DATEVALUE(\"9:30 1/5\")=DATEVALUE(\"1/5\")");
  d.Set("A9", "=\"1e308\"+0");
  d.Set("A10", "=VALUE(\" 9:30\")");
  CHECK(d.V("A1").AsDouble() == 12345);
  CHECK(std::fabs(d.V("A2").AsDouble() - 0.015) < 1e-15);
  CHECK(std::fabs(d.V("A3").AsDouble() - 9.5 / 24) < 1e-12);
  CHECK(std::fabs(d.V("A4").AsDouble() - 10.0 / 24) < 1e-12);
  CHECK(d.V("A5").IsError());
  CHECK(d.V("A6").IsError());
  CHECK(d.V("A7").IsError());
  CHECK(d.V("A8").IsBool());
  CHECK(d.V("A8").boolean());
  CHECK(d.V("A9").IsError());
  CHECK(std::fabs(d.V("A10").AsDouble() - 9.5 / 24) < 1e-12);  // VALUE trims the space first
}

TEST_CASE("Typed: a month and year in numbers is a date typed, text to a formula (measured)") {
  CheckNumber({"12/2024", 45627, "mmm-yy"});
  CheckNumber({"2/30", 10990, "mmm-yy"});  // February 1930
  CHECK(formula::ParseInput("12/2024").typed_only);
  CHECK_FALSE(formula::ParseInput("Jan-2026").typed_only);
  yctest::Doc d;
  d.Set("A1", "=\"12/2024\"+0");
  d.Set("A2", "=VALUE(\"1/0\")");
  d.Set("A3", "=DATEVALUE(\"2/30\")");
  d.Set("A4", "=DATEVALUE(\"Jan 32\")");  // a month's name is read in a formula too
  d.Set("A5", "=YEAR(\"1/2026\")");
  CHECK(d.V("A1").IsError());
  CHECK(d.V("A2").IsError());
  CHECK(d.V("A3").IsError());
  CHECK(d.V("A4").AsDouble() == 11689);
  CHECK(d.V("A5").IsError());
}

TEST_CASE("COUNTIF criteria: TRUE / FALSE and errors compared by order, ~ itself without a wildcard (measured)") {
  // the data and the counts measured in Excel 16 (COUNTIF($A$1:$A$52, criterion))
  const char* data[] = {"1", "2", "5", "10", "-5", "0", "1.5", "1000", "'5", "'10", "' 5", "'5 ", "a", "A", "ab", "abc", "b", "a*", "a?", "~", "a~b", "*", "?", "", "", "=\"\"", "TRUE", "FALSE", "'TRUE", "=NA()", "=1/0", "2024/1/5", "9:30", "50%", "'50%", "'1,000", "1,000", "'=", "'<>", "'>5", "' ", "x y", "xy", "'abc ", "ABC", "co-op", "coop", "'$5", "$5", "'1/5", "e", "\xc3\xa9"""};
  const std::pair<const char*, int> cases[] = {{"<#DIV/0!", 0}, {"<1/0", 11}, {"<=#N/A", 2}, {"<=1/0", 11}, {"<=TRUE", 2}, {"<=true", 2}, {"<>~~", 52}, {"<FALSE", 0}, {"==~", 0}, {"=~~", 0}, {">#N/A", 0}, {">1/0", 22}, {">=#DIV/0!", 2}, {">=1/0", 22}, {">=FALSE", 2}, {">TRUE", 0}, {"~~", 0}, {"~", 1}, {"~*", 1}, {"~?", 1}, {"=~?*", 1}, {"~??", 0}, {"=~~*", 1}, {"a~*", 1}, {"#N/A", 1}, {"=#N/A", 1}, {"<>#N/A", 51}, {"TRUE", 1}, {"<>TRUE", 51}, {"1/5", 1}, {">9:30", 10}, {"<=~~", 7}, {">~~", 26}, {"*~**", 2}, {"=", 2}, {"<>", 50}, {">=a~*", 13}, {"1/0", 0}, {"=1/0", 0}};
  yctest::Doc d;
  int row = 0;
  for (const char* v : data) {
    ++row;
    if (*v) d.Set(("A" + std::to_string(row)).c_str(), v);
  }
  row = 0;
  for (const auto& c : cases) {
    ++row;
    const std::string at = "C" + std::to_string(row);
    d.Set(at.c_str(), std::string("'") + c.first);  // as text
    d.Set(("D" + std::to_string(row)).c_str(), ("=COUNTIF($A$1:$A$52," + at + ")").c_str());
  }
  row = 0;
  for (const auto& c : cases) {
    ++row;
    INFO(c.first);
    CHECK(d.V(("D" + std::to_string(row)).c_str()).AsDouble() == c.second);
  }
}

TEST_CASE("Formulas: a range typed the other way round is kept upper-left first, and rows meet in a space (measured)") {
  yctest::Doc d;
  for (int r = 1; r <= 10; ++r) {
    for (int c = 0; c < 7; ++c) d.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r)).c_str(), std::to_string(r * 10 + c + 1));
  }
  struct Case {
    const char* typed;
    const char* kept;  // Excel 16's Formula read back
    double value;
  };
  const Case cases[] = {{"=SUM(E7:B7)", "=SUM(B7:E7)", 294}, {"=SUM($C5:F$3)", "=SUM($C$3:F5)", 534}, {"=SUM(G:C)", "=SUM(C:G)", 3000},
                        {"=SUM(B6:F8 2:8)", "=SUM(B6:F8 2:8)", 1110}, {"=SUM(2:3 C:D)", "=SUM(2:3 C:D)", 114},
                        {"=SUM(E7:B7 C3:C9)", "=SUM(B7:E7 C3:C9)", 73}, {"=ROWS(9:2)", "=ROWS(2:9)", 8}, {"=ROWS($9:2)", "=ROWS(2:$9)", 8}};
  int row = 0;
  for (const Case& c : cases) {
    INFO(c.typed);
    const std::string at = "L" + std::to_string(++row);
    REQUIRE(d.Set(at.c_str(), c.typed));
    CHECK(d.F(at.c_str()) == c.kept);
    CHECK(d.V(at.c_str()).AsDouble() == c.value);
  }
}

TEST_CASE("Functions at their edges as Excel 16 (measured): whole parts, limits, and which error comes first") {
  yctest::Doc d;
  const char* a[] = {"1", "2", "3", "", "-1", "0.5", "TRUE", "abc", "10", "=NA()"};
  const char* c[] = {"3", "1", "2", "2", "5", "4", "7", "6", "9", "8"};
  for (int i = 0; i < 10; ++i) {
    if (*a[i]) d.Set(("A" + std::to_string(i + 1)).c_str(), a[i]);
    d.Set(("C" + std::to_string(i + 1)).c_str(), c[i]);
  }
  struct Case {
    const char* f;
    const char* want;  // a number, or an error's text
  };
  const Case cases[] = {
      {"=DATE(2024,1,-0.4)", "45290"}, {"=DATE(2024,-0.4,1)", "45231"}, {"=DATE(-0.4,1,1)", "#NUM!"}, {"=DATE(0,0,45296)", "#NUM!"},
      {"=DATE(1900,0,45296)", "#NUM!"}, {"=DATE(0,1,45296)", "32767"}, {"=DATE(1900,1,2958466)", "32767"}, {"=DATE(2024,1,1.9)", "45292"},
      {"=TIME(0,0,-0.4)", "#NUM!"}, {"=TIME(1,0,-0.4)", "0.041655092592592591"}, {"=MID(\"abc\",2147483648,1)", "#VALUE!"},
      {"=MID(\"abc\",1,2147483648)", "#VALUE!"}, {"=REPLACE(\"a b c\",1E+20,15,\"abc\")", "#VALUE!"}, {"=REPLACE(\"12\",1,32768,\"x\")", "#VALUE!"}, {"=N(REPLACE(\"12\",32767,1,\"x\")=\"12x\")", "1"},
      {"=RANK(1,A1:A10)", "#N/A"},
      {"=RANK.AVG(1,A1:A10)", "#N/A"}, {"=RANK.EQ(3,A1:A9)", "2"}, {"=SUMPRODUCT(A10:A11,C1:C5)", "#N/A"}, {"=SUMPRODUCT(A1:A3,C1:C5)", "#VALUE!"},
      {"=VLOOKUP(99,C1:C10,2,FALSE)", "#N/A"}, {"=VLOOKUP(3,C1:C10,2,FALSE)", "#REF!"}, {"=VLOOKUP(0,C1:C10,2,TRUE)", "#N/A"},
      {"=HLOOKUP(99,C1:C10,20,FALSE)", "#N/A"}, {"=HLOOKUP(3,C1:C10,20,FALSE)", "#REF!"}, {"=FACT(1E+20)", "#NUM!"},
      {"=LCM(2^52,3)", "#NUM!"}, {"=LCM(2^40,3)", "3298534883328"}, {"=GCD(1E+20)", "#NUM!"}, {"=GCD(2^53,2)", "2"},
      {"=DAYS(2958466,1)", "#NUM!"}, {"=NETWORKDAYS(2958466,1)", "#NUM!"}, {"=DAYS(2958465,1)", "2958464"},
      {"=ROUNDDOWN(2.5,400)", "2.5"}, {"=ROUNDDOWN(2.5,308)", "2.5"}, {"=ROUNDDOWN(2.5,-400)", "0"}, {"=ROUNDUP(15,1E+20)", "15"},
      {"=ROUNDUP(1E-20,-1)", "10"}, {"=ROUNDUP(1E-20,1)", "0.1"}, {"=ROUNDUP(-1E-20,1)", "-0.1"}, {"=ROUNDUP(0.1+0.2,1)", "0.3"},
      {"=ROUNDUP(2E-16,15)", "1E-15"}, {"=ROUNDDOWN(-1E-20,1)", "0"}};
  int row = 0;
  for (const Case& k : cases) {
    INFO(k.f);
    const std::string at = "H" + std::to_string(++row);
    REQUIRE(d.Set(at.c_str(), k.f));
    const auto& v = d.V(at.c_str());
    if (k.want[0] == '#') {
      REQUIRE(v.IsError());
      CHECK(std::string(yc::core::ErrorText(v.error())) == k.want);
    } else {
      REQUIRE(v.IsNumeric());
      const double w = std::strtod(k.want, nullptr);
      CHECK(std::fabs(v.AsDouble() - w) <= 1e-12 * std::fmax(1.0, std::fabs(w)));
    }
  }
}

TEST_CASE("Formulas: a copy whose range corners pass each other is written upper-left first (measured)") {
  using yc::core::Rect;
  yctest::Doc d;
  const uint32_t sh = d.sheet();
  auto copy = [&](uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
    const auto clip = d.d.Copy(sh, Rect{r0, c0, r0, c0});
    d.d.Paste(sh, r1, c1, clip, yc::engine::PasteMode::All);
  };
  // rows: each part keeps its $ when the corners trade places ($C5:F$3 is $C$3:F5, as typed in Excel)
  REQUIRE(d.Set("B2", "=SUM($E$5:F3)"));
  CHECK(d.F("B2") == "=SUM($E3:F$5)");
  copy(1, 1, 8, 1);  // to B9: the relative 3 moves to 10, past the $5
  CHECK(d.F("B9") == "=SUM($E$5:F10)");
  REQUIRE(d.Set("D8", "=INDEX($C$2:F7,1,1)"));
  copy(7, 3, 1, 3);  // to D2: the relative 7 moves to 1, above the $2
  CHECK(d.F("D2") == "=INDEX($C1:F$2,1,1)");
  // columns: the relative J moves to C, left of the $F
  REQUIRE(d.Set("H1", "=SUM($F$1:J2)"));
  copy(0, 7, 0, 0);
  CHECK(d.F("A1") == "=SUM(C$1:$F2)");
}

TEST_CASE("Date functions of the Analysis ToolPak refuse TRUE / FALSE; WORKDAY of no days; DATEDIF yd (measured)") {
  yctest::Doc d;
  d.Set("A1", "45001");
  d.Set("A4", "TRUE");
  d.Set("A5", "x");
  struct Case {
    const char* f;
    const char* want;  // a number, or an error's text
  };
  const Case cases[] = {
      {"=EDATE(TRUE,1)", "#VALUE!"}, {"=EDATE(45000,A4)", "#VALUE!"}, {"=EOMONTH(45000,FALSE)", "#VALUE!"},
      {"=WORKDAY(A4,3)", "#VALUE!"}, {"=WORKDAY(45000,TRUE)", "#VALUE!"}, {"=NETWORKDAYS(45000,A4)", "#VALUE!"},
      {"=WEEKNUM(A4,2)", "#VALUE!"}, {"=WEEKNUM(45000,TRUE)", "#VALUE!"}, {"=YEARFRAC(45000,45100,TRUE)", "#VALUE!"},
      {"=YEARFRAC(A4,45100,1)", "#VALUE!"}, {"=WORKDAY.INTL(45000,3,TRUE,45001)", "45006"},
      {"=WORKDAY.INTL(45000,3,1,TRUE)", "#VALUE!"}, {"=WORKDAY.INTL(45000,3,1,-1)", "#NUM!"},
      {"=NETWORKDAYS.INTL(45000,45010,A4,45001)", "7"}, {"=NETWORKDAYS.INTL(45000,45010,1,-1)", "#NUM!"},
      {"=NETWORKDAYS.INTL(1,10,1.5)", "7"}, {"=NETWORKDAYS.INTL(1,10,17.9)", "9"},
      {"=NETWORKDAYS.INTL(1,10,FALSE)", "#NUM!"}, {"=NETWORKDAYS.INTL(1,10,A6)", "#NUM!"},
      {"=NETWORKDAYS.INTL(1,10,\"1111111\")", "0"}, {"=NETWORKDAYS.INTL(-1,10,\"abc\")", "#VALUE!"},
      {"=NETWORKDAYS.INTL(1,10,TRUE,\"x\")", "#VALUE!"}, {"=WORKDAY.INTL(45000,0,99)", "45000"},
      {"=WORKDAY.INTL(45000,0,\"abc\",\"x\")", "45000"}, {"=WORKDAY.INTL(45000,0.9,\"abc\")", "45000"},
      {"=WORKDAY.INTL(45000,-0.9,TRUE)", "44999"}, {"=WORKDAY.INTL(-1,0,\"abc\")", "#NUM!"},
      {"=WORKDAY(45000,0,\"x\")", "45000"}, {"=WORKDAY(45000,5,{TRUE})", "#VALUE!"},
      {"=WORKDAY(45000,5,2958466)", "#NUM!"}, {"=NETWORKDAYS(45000,45010,-1)", "#NUM!"},
      {"=WORKDAY(-1,5,\"x\")", "#NUM!"}, {"=YEARFRAC(-1,1,\"x\")", "#VALUE!"}, {"=YEARFRAC(\"x\",2,9)", "#NUM!"},
      {"=YEARFRAC(-1,\"x\")", "#NUM!"}, {"=DATEDIF(-1,\"x\",\"Y\")", "#VALUE!"},
      {"=DATEDIF(TRUE,45100,\"D\")", "45099"}, {"=WEEKDAY(TRUE,2)", "7"}, {"=DAYS360(TRUE,45100,FALSE)", "44452"},
      {"=DATEDIF(60,1000000,\"YD\")", "271"}, {"=DATEDIF(1,1000000,\"YD\")", "330"},
      {"=YEARFRAC(45000,45100,-0.5)", "#NUM!"}, {"=YEARFRAC(45000,45100,4.9)", "0.272222222222222"},
      {"=DATEDIF(43889,44265,\"YD\")", "10"}, {"=DATEDIF(43889,44286,\"YD\")", "32"}, {"=DATEDIF(43889,44287,\"YD\")", "33"},
      {"=DATEDIF(45350,46445,\"YD\")", "365"}, {"=DATEDIF(43845,44256,\"YD\")", "45"}, {"=DATEDIF(60,61,\"YD\")", "1"},
      {"=DATEDIF(31,90,\"YD\")", "59"}, {"=DATEDIF(31,62,\"YD\")", "31"}};
  int row = 0;
  for (const Case& k : cases) {
    INFO(k.f);
    const std::string at = "H" + std::to_string(++row);
    REQUIRE(d.Set(at.c_str(), k.f));
    const auto& v = d.V(at.c_str());
    if (k.want[0] == '#') {
      REQUIRE(v.IsError());
      CHECK(std::string(yc::core::ErrorText(v.error())) == k.want);
    } else {
      REQUIRE(v.IsNumeric());
      const double w = std::strtod(k.want, nullptr);
      CHECK(std::fabs(v.AsDouble() - w) <= 1e-12 * std::fmax(1.0, std::fabs(w)));
    }
  }
}

TEST_CASE("Lookups, TEXTBEFORE / TEXTSPLIT and WRAPROWS at their edges as Excel 16 (measured)") {
  yctest::Doc d;
  const char* a[] = {"3", "b", "1", "A", "3", "", "TRUE", "2.5"};
  const char* t[] = {"apple pie", "a,b,,c", "x-y-z", "Hello World", "", "  sp  ", "a-B-a-b", "a;b|c"};
  const char* e[] = {"1", "2", "2", "3", "5", "8", "13", "21"};
  for (int i = 0; i < 8; ++i) {
    const std::string r = std::to_string(i + 1);
    if (*a[i]) d.Set(("A" + r).c_str(), a[i]);
    d.Set(("B" + r).c_str(), std::to_string((i + 1) * 10).c_str());
    if (*t[i]) d.Set(("D" + r).c_str(), t[i]);
    d.Set(("E" + r).c_str(), e[i]);
  }
  // an array result as one text: its values joined, then its rows and columns
  auto J = [](const std::string& x) { return "=TEXTJOIN(\"|\",FALSE," + x + ")&\"#\"&ROWS(" + x + ")&\"x\"&COLUMNS(" + x + ")"; };
  struct Case {
    std::string f;
    const char* want;  // an error's text, a number, or text after a '='
  };
  const Case cases[] = {
      // XMATCH / XLOOKUP next smaller / larger across types (numbers, text, TRUE / FALSE, empty), an empty key
      {"=XMATCH(13,A1:A8,1,1)", "4"}, {"=XMATCH(\"c\",A1:A8,1,1)", "7"}, {"=XMATCH(\"c\",E1:E8,-1,1)", "8"},
      {"=XMATCH(FALSE,D1:D8,1,1)", "5"}, {"=XMATCH(13,D1:D8,1,-1)", "6"}, {"=XMATCH(A6,A1:A8,0,1)", "6"},
      {"=XMATCH(A6,D1:D8,-1,1)", "5"}, {"=XMATCH(A6,E1:E8,-1,1)", "8"}, {"=XMATCH(\"\",A1:A8,-1,1)", "1"},
      {"=XMATCH(\"\",A1:A8,-1,-1)", "5"}, {"=XMATCH(\"\",D1:D8,0,1)", "#N/A"}, {"=MATCH(A6,A1:A8,0)", "#N/A"},
      {"=XLOOKUP(13,A1:A8,B1:B8,\"nf\",1)", "40"}, {"=XLOOKUP(A6,A1:A8,B1:B8,\"nf\",-1)", "60"},
      // TEXTBEFORE / TEXTAFTER: the instance taken down, the length checked when the instance is given
      {"=TEXTBEFORE(\"a-b-c\",\"-\",-1.5)", "=a"}, {"=TEXTAFTER(\"a-b-c\",\"-\",-0.5)", "=c"},
      {"=TEXTBEFORE(\"a-b-c\",\"-\",0.5)", "#VALUE!"}, {"=TEXTBEFORE(\"abc\",\"-\",4)", "#VALUE!"},
      {"=TEXTBEFORE(\"abc\",\"-\",3)", "#N/A"}, {"=TEXTBEFORE(\"\",\"-\",1)", "#VALUE!"}, {"=TEXTBEFORE(\"\",\"-\")", "#N/A"},
      {"=TEXTBEFORE(\"\",\"-\",,,1)", "#N/A"}, {"=TEXTBEFORE(\"abc\",\"\",5)", "="}, {"=TEXTAFTER(\"abc\",\"\",-6)", "="},
      {"=TEXTAFTER(\"a-b-c\",\"-\",-2.5,0,1)", "=a-b-c"},
      // TEXTSPLIT: each text of a range gives its first part, empty delimiters are #VALUE!, columns before rows
      {J("TEXTSPLIT(D1:D3,\",\")"), "=apple pie|a|x-y-z#3x1"}, {J("TEXTSPLIT({\"a,b\";\"c\"},\",\")"), "=a|c#2x1"},
      {J("TEXTSPLIT(A1:A3,\",\")"), "=3|b|1#3x1"}, {"=TEXTSPLIT(\"a,b\",\",\",\"\")", "#VALUE!"},
      {"=TEXTSPLIT(\"a,b\",{\",\",\"\"})", "#VALUE!"}, {J("TEXTSPLIT(\"a--b\",\"-\",\"--\")"), "=a||b#1x3"},
      {J("TEXTSPLIT(\"a--b\",\"--\",\"-\")"), "=a|b#1x2"}, {J("TEXTSPLIT(\"a-b-c\",\"-\",\"-\")"), "=a|b|c#1x3"},
      {J("TEXTSPLIT(\"ab\",\"ab\",\"b\")"), "=|#1x2"}, {J("TEXTSPLIT(\"a-b;c\",\"-\",{\"-\",\";\"})"), "#N/A"},
      // WRAPROWS / WRAPCOLS longer than the vector: the vector, unpadded
      {J("WRAPROWS({1,2,3},5)"), "=1|2|3#1x3"}, {J("WRAPCOLS({1;2;3},4,\"p\")"), "=1|2|3#3x1"},
      {J("WRAPROWS({1,2,3},1E+9)"), "=1|2|3#1x3"}, {J("WRAPROWS({1,2,3},2,0)"), "=1|2|3|0#2x2"},
      // SUBSTITUTE's instance refuses TRUE / FALSE
      {"=SUBSTITUTE(\"aa\",\"a\",\"b\",TRUE)", "#VALUE!"}, {"=SUBSTITUTE(\"aa\",\"a\",\"b\",A7)", "#VALUE!"},
      {"=SUBSTITUTE(\"aa\",\"a\",\"b\",\"1\")", "=ba"}};
  int row = 0;
  for (const Case& k : cases) {
    INFO(k.f);
    const std::string at = "H" + std::to_string(++row);
    REQUIRE(d.Set(at.c_str(), k.f.c_str()));
    const auto& v = d.V(at.c_str());
    if (k.want[0] == '#') {
      REQUIRE(v.IsError());
      CHECK(std::string(yc::core::ErrorText(v.error())) == k.want);
    } else if (k.want[0] == '=') {
      REQUIRE(v.IsString());
      CHECK(std::string(v.str()) == k.want + 1);
    } else {
      REQUIRE(v.IsNumeric());
      CHECK(v.AsDouble() == std::strtod(k.want, nullptr));
    }
  }
}
