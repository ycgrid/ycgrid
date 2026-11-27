// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "core/decimal.h"
#include "formula/datetime.h"
#include "formula/numfmt.h"

using namespace yc;
using core::Value;

namespace {
std::string F(double v, const char* code) { return formula::FormatValue(Value::Number(v), code).text; }
}  // namespace

TEST_CASE("數值格式") {
  CHECK(F(1234.567, "General") == "1234.567");
  CHECK(F(1234.567, "0.00") == "1234.57");
  CHECK(F(1234567.891, "#,##0") == "1,234,568");
  CHECK(F(-5, "0;(0)") == "(5)");
  CHECK(F(0, "0;-0;\"零\"") == "零");
  CHECK(F(0.256, "0.0%") == "25.6%");
  CHECK(F(12345.678, "0.00E+00") == "1.23E+04");
  CHECK(F(0.5, "# ?/?") == " 1/2");
  CHECK(F(1500000, "#,##0,\"K\"") == "1,500K");
  CHECK(F(45953, "yyyy/m/d") == "2025/10/23");
  CHECK(F(45953.5, "hh:mm:ss") == "12:00:00");
  CHECK(F(1.25, "[h]:mm") == "30:00");
  CHECK(formula::FormatValue(Value::Number(-3), "[Red]0").color != 0);
  CHECK(F(-0.1, "0.0%;[Red]-0.0%") == "-10.0%");
  CHECK(formula::FormatValue(Value::Number(-0.1), "0.0%;[Red]-0.0%").color != 0);
  CHECK(F(-5, "0;-0") == "-5");
}

TEST_CASE("Number formats round half away from zero on 15 digits, like Excel") {
  CHECK(F(2.5, "0") == "3");
  CHECK(F(-2.5, "0") == "-3");
  CHECK(F(0.5, "0") == "1");
  CHECK(F(1234.5, "#,##0") == "1,235");
  CHECK(F(1.005, "0.00") == "1.01");  // the binary value is 1.00499999...
  CHECK(F(0.125, "0.00") == "0.13");
  CHECK(F(9.995, "0.00") == "10.00");
  CHECK(F(99.5, "0") == "100");
  CHECK(F(0.0004, "0.00") == "0.00");
  CHECK(F(0.005, "0.00%") == "0.50%");
  CHECK(F(1e20, "0") == "100000000000000000000");
  CHECK(F(0.1 + 0.2, "0.0000000000000000") == "0.3000000000000000");
  CHECK(F(2.675, "0.00") == "2.68");
}

TEST_CASE("[epoch] 奈秒時間戳逐位精確") {
  core::Dec d;
  REQUIRE(core::DecParse("1729658400.123456789", &d));
  const Value v = core::DecToValue(d);
  CHECK(formula::FormatValue(v, "[epoch][utc]yyyy-mm-dd hh:mm:ss.000000000").text == "2024-10-23 04:40:00.123456789");
  CHECK(formula::FormatValue(v, "[epoch][tz=+08:00]yyyy-mm-dd hh:mm:ss.000000000").text == "2024-10-23 12:40:00.123456789");
  CHECK(formula::FormatValue(v, "0.000000000").text == "1729658400.123456789");
  CHECK(formula::FormatValue(v, "[epoch][utc]hh:mm:ss.000").text == "04:40:00.123");
}

TEST_CASE("輸入解析") {
  auto p = formula::ParseInput("50%");
  CHECK(p.value.number() == 0.5);
  CHECK(p.format == "0%");
  p = formula::ParseInput("1,234");
  CHECK(p.value.number() == 1234);
  // a thousands separator after a number that isn't 0 and before three digits or more (Excel 16, measured)
  CHECK(formula::ParseInput("1,234,567.5").value.AsDouble() == 1234567.5);
  CHECK(formula::ParseInput("-12,345").value.AsDouble() == -12345);
  CHECK(formula::ParseInput("2024,05,06").value.IsString());
  CHECK(formula::ParseInput("1,23").value.IsString());
  CHECK(formula::ParseInput("1234,567").value.AsDouble() == 1234567);
  CHECK(formula::ParseInput("1,2345").value.AsDouble() == 12345);
  p = formula::ParseInput("2026/10/23");
  CHECK(p.value.IsNumber());
  CHECK_FALSE(p.format.empty());
  p = formula::ParseInput("1729658400.123456789");
  CHECK(p.value.IsDecimal());
  p = formula::ParseInput("1729658400.123456789", true);  // Excel-compatible: 15 digits
  CHECK(p.value.IsNumber());
  p = formula::ParseInput("abc");
  CHECK(p.value.IsString());
  p = formula::ParseInput("TRUE");
  CHECK(p.value.IsBool());
}

TEST_CASE("Typed fractions become numbers with a fraction format, like Excel") {
  auto p = formula::ParseInput("0 1/2");
  CHECK(p.value.number() == 0.5);
  CHECK(p.format == "# ?/?");
  p = formula::ParseInput("-1 3/16");
  CHECK(p.value.number() == -1.1875);
  CHECK(p.format == "# ??/??");
  CHECK(F(1.25, "# ?/?") == "1 1/4");
  // a lone fraction is a date of this year, shown as month and day (Excel's Taiwan edition: 3月4日)
  p = formula::ParseInput("3/4");
  CHECK(p.format == "m\"月\"d\"日\"");
  // not a fraction
  CHECK(formula::ParseInput("1 2/x").value.IsString());
}

TEST_CASE("General: small numbers switch to scientific notation when that keeps more digits (like Excel)") {
  using formula::FormatGeneral;
  CHECK(FormatGeneral(0.0001234567, 11) == "0.000123457");
  CHECK(FormatGeneral(0.00001234567, 11) == "1.23457E-05");
  CHECK(FormatGeneral(0.00001, 11) == "0.00001");
  CHECK(FormatGeneral(8.842135729107509e-09, 11) == "8.84214E-09");
  CHECK(FormatGeneral(8.842135729107509e-09, 8) == "8.84E-09");
  CHECK(FormatGeneral(0.25, 11) == "0.25");
  CHECK(FormatGeneral(-0.00001234567, 11) == "-1.2346E-05");
}

TEST_CASE("General: a whole number too wide for its column is shown in scientific notation, as Excel's") {
  // Excel 16, measured (Range.Text in columns of these widths): 1234567890 is 1E+09 in 5 and 1.23E+09 in 8, 100000
  // 1E+05 and 99999 itself in 5, -1234567 -1E+06 in 6; it showed ####. A default cell (11) keeps its digits
  using formula::FormatGeneral;
  CHECK(FormatGeneral(1234567890, 5) == "1E+09");
  CHECK(FormatGeneral(1234567890, 8) == "1.23E+09");
  CHECK(FormatGeneral(100000, 5) == "1E+05");
  CHECK(FormatGeneral(99999, 5) == "99999");
  CHECK(FormatGeneral(-1234567, 6) == "-1E+06");
  CHECK(FormatGeneral(12345.678, 5) == "12346");
  CHECK(FormatGeneral(1234567890, 11) == "1234567890");
}

TEST_CASE("General: a three-digit exponent takes a decimal less instead of ##### (a regression's p-value)") {
  auto W = [](double v, double chars) { return formula::FormatValue(core::Value::Number(v), "General", chars).text; };
  CHECK(W(1.234e-251, 8.43) == "1.2E-251");
  CHECK(W(-1.234e-251, 8.43) == "-1E-251");
  CHECK(W(1.5e300, 8.43) == "1.5E+300");
  CHECK(W(9.99e99, 8.43) == "9.99E+99");
  CHECK(W(9.999e99, 8.43) == "1E+100");
  CHECK(W(1.234e-251, 11) == "1.234E-251");
  CHECK(W(1.234e-12, 8.43) == "1.23E-12");
}

TEST_CASE("General shows at most 11 characters in a wide column, fewer in a narrow one (like Excel)") {
  auto W = [](double v, double chars) { return formula::FormatValue(core::Value::Number(v), "General", chars).text; };
  CHECK(W(1.0 / 3.0, 50) == "0.333333333");
  CHECK(W(123456789.123456, 50) == "123456789.1");
  CHECK(W(0.521561782828453, 50) == "0.521561783");
  CHECK(W(1.0 / 3.0, 7) == "0.33333");
  CHECK(W(23000, 50) == "23000");
}

TEST_CASE("Fraction formats pick the closest denominator without scanning them all") {
  CHECK(F(3.14159265358979, "# ?/?") == "3 1/7");
  CHECK(F(3.14159265358979, "# ?/??") == "3 1/7 ");  // Excel 16: the last convergent that fits, not 14/99 (measured)
  CHECK(F(0.75, "?/?") == "3/4");
  CHECK(F(0.2, "?/??") == "1/5 ");  // the ?? slot pads the denominator
  CHECK(F(0.0, "?/?") == "0/1");
  CHECK_FALSE(F(3.14159265358979, "# ?/?????????").empty());  // 1e9 denominators: finishes at once
  CHECK_FALSE(F(2.718281828, "# ?/????????????????????").empty());  // more slots than fit in 64 bits: still fine
}

TEST_CASE("Typing a number beyond the double range keeps the text, as Excel does") {
  CHECK(formula::ParseInput("1e309").value.IsString());
  CHECK(formula::ParseInput("1e309").value.str() == "1e309");
  CHECK(formula::ParseInput("-1e309").value.IsString());
  CHECK(formula::ParseInput("1e308").value.IsString());  // past Excel's largest, 9.99999999999999E+307 (measured)
  CHECK(formula::ParseInput("9.99999999999999e307").value.AsDouble() == 9.99999999999999e307);
  yctest::Doc t;
  t.Set("A1", "1e309");
  CHECK(t.V("A1").IsString());
  CHECK(t.T("A1") == "1e309");
}

TEST_CASE("Dates typed with a month's name are dates in Excel's formats") {
  struct Case {
    const char* text;
    double serial;
    const char* format;
  };
  const Case cases[] = {
      {"15-Jan-2024", 45306, "d-mmm-yy"},       {"15 Jan 2024", 45306, "d-mmm-yy"},
      {"Jan 15, 2024", 45306, "d-mmm-yy"},      {"January 15 2024", 45306, "d-mmm-yy"},
      {"Monday, January 15, 2024", 45306, "d-mmm-yy"}, {"15-JAN-24", 45306, "d-mmm-yy"},
      {"2024-Jan-15", 45306, "d-mmm-yy"},       {"Sept 1, 2024", 45536, "d-mmm-yy"},
      {"Jan-2024", 45292, "mmm-yy"},            {"Jan 2024", 45292, "mmm-yy"},
      {"Jan 32", 11689, "mmm-yy"},              {"Feb 29, 1900", 60, "d-mmm-yy"},
  };
  for (const Case& c : cases) {
    INFO(c.text);
    const auto p = formula::ParseInput(c.text);
    REQUIRE(p.value.IsNumber());
    CHECK(p.value.AsDouble() == c.serial);
    CHECK(p.format == c.format);
  }
  // a time after the date
  auto p = formula::ParseInput("15-Jan-2024 6:30 PM");
  REQUIRE(p.value.IsNumber());
  CHECK(p.value.AsDouble() == 45306.0 + 18.5 / 24);
  // a day and a month: this year's
  p = formula::ParseInput("15-Jan");
  REQUIRE(p.value.IsNumber());
  CHECK(p.format == "d-mmm");
  int y = 0, m = 0, d = 0;
  REQUIRE(formula::SerialToYmd(p.value.AsDouble(), &y, &m, &d));
  CHECK((m == 1 && d == 15 && y >= 2026));
  // words that only look like dates stay text
  for (const char* text : {"Jan", "May 5 people", "Q1 Jan", "Jan 15 2024 x", "Mon 15", "Jan Feb", "Jan 0 2024", "Feb 30, 2024"}) {
    INFO(text);
    CHECK(formula::ParseInput(text).value.IsString());
  }
  // the date functions read them too
  double v = 0;
  CHECK(formula::ParseDateTime("22-MAY-2011", &v));
  CHECK(v == 40685);
  CHECK(formula::ParseDateTime("22-Aug-2011 6:35 AM", &v));
  CHECK(v - 40777 > 0.274);
}

TEST_CASE("Negative numbers that round to zero lose their sign, as in Excel") {
  // measured against Excel 16: a negative number whose digits all show as 0 has no minus
  CHECK(F(-0.4, "0") == "0");
  CHECK(F(-0.004, "0.00") == "0.00");
  CHECK(F(-0.001, "0%") == "0%");
  CHECK(F(-0.4, "#") == "");
  CHECK(F(-0.4, "0.0") == "-0.4");
  CHECK(F(-1, "#,##0,") == "0");
  CHECK(F(-600, "0,") == "-1");
  CHECK(F(-1, "0.000,") == "-0.001");
  CHECK(F(-0.4, "0;-0") == "-0");  // the minus of the format itself stays
  CHECK(F(-5, "\"a\"") == "-a");   // no digits: the sign stays
  CHECK(F(-0.01, "# ?/?") == "-0    ");  // a fraction keeps it
  CHECK(F(-0.4, "0;(0)") == "(0)");  // a negative section shows no sign of its own
  CHECK(F(0.4, "0") == "0");
}

TEST_CASE("A date outside the calendar fills the cell with #, and TEXT gives #VALUE!") {
  const formula::FormatResult neg = formula::FormatValue(Value::Number(-1), "yyyy-mm-dd");
  CHECK(neg.bad_date);
  CHECK(neg.hashes);
  CHECK(neg.text.find_first_not_of('#') == std::string::npos);
  CHECK(formula::FormatValue(Value::Number(-0.5), "h:mm").bad_date);
  CHECK(formula::FormatValue(Value::Number(3000000), "yyyy").bad_date);
  CHECK(formula::FormatValue(Value::Number(-1), "yyyy-mm-dd", 12).text == std::string(12, '#'));
  CHECK_FALSE(formula::FormatValue(Value::Number(0), "yyyy-mm-dd").bad_date);
  yctest::Doc d;
  d.Set("A1", "=TEXT(-1,\"yyyy-mm-dd\")");
  d.Set("A2", "=TEXT(-0.4,\"0\")");
  CHECK(d.V("A1").IsError());
  CHECK(d.T("A2") == "0");
}

TEST_CASE("Typed times keep Excel's look: AM/PM and hours past 24") {
  auto p = formula::ParseInput("10:30 pm");
  REQUIRE(p.value.IsNumber());
  CHECK(p.format == "h:mm AM/PM");
  CHECK(formula::FormatValue(p.value, p.format).text == "10:30 PM");
  CHECK(formula::ParseInput("9:15:30 AM").format == "h:mm:ss AM/PM");
  p = formula::ParseInput("25:00");
  REQUIRE(p.value.IsNumber());
  CHECK(p.format == "[h]:mm:ss");
  CHECK(formula::FormatValue(p.value, p.format).text == "25:00:00");
  CHECK(formula::ParseInput("12:00").format == "h:mm");
  CHECK(formula::ParseInput("1:30:15").format == "h:mm:ss");
}

TEST_CASE("Chinese number formats: [DBNum1] / [DBNum2] numerals, aaa / aaaa day names, Chinese month names") {
  CHECK(F(1234, "[DBNum1]") == "一千二百三十四");
  CHECK(F(1234, "[DBNum2]") == "壹仟貳佰參拾肆");
  CHECK(F(10, "[DBNum1]") == "一十");  // measured against Excel 16
  CHECK(F(15, "[DBNum1]") == "一十五");
  CHECK(F(10, "[DBNum2]") == "壹拾");
  CHECK(F(105, "[DBNum1]") == "一百○五");
  CHECK(F(1005, "[DBNum1]") == "一千○五");
  CHECK(F(10050, "[DBNum1]") == "一萬○五十");
  CHECK(F(100000, "[DBNum1]") == "一十萬");
  CHECK(F(20000, "[DBNum1]") == "二萬");
  CHECK(F(1000001, "[DBNum1]") == "一百萬○一");
  CHECK(F(0, "[DBNum1]") == "○");  // U+25CB, as NUMBERSTRING(0,1)
  CHECK(F(1234.5, "[DBNum1]") == "一千二百三十四.五");
  CHECK(F(-7, "[DBNum1]") == "-七");
  CHECK(F(1234, "[DBNum1]#,##0") == "一,二三四");  // digit placeholders: digit by digit (Excel)
  CHECK(F(1234, "[DBNum3]0") == "１２３４");
  CHECK(!F(5, "[DBNum1]").empty());  // it showed nothing at all
  const double monday = 45306;  // 2024-01-15
  CHECK(F(monday, "aaaa") == "星期一");
  CHECK(F(monday, "aaa") == "週一");  // Excel's Taiwan edition
  CHECK(F(monday + 6, "aaaa") == "星期日");
  CHECK(F(monday, "yyyy/m/d aaa") == "2024/1/15 週一");
  CHECK(F(monday, "[$-404]mmmm") == "一月");
  CHECK(F(monday, "[$-zh-TW]mmmm") == "一月");
  CHECK(F(monday, "[$-404]mmm") == "一月");
  CHECK(F(monday + 300, "[$-404]mmmm") == "十一月");
  CHECK(F(monday, "[$-zh-TW]dddd") == "星期一");
  CHECK(F(0.75, "AM/PM h:mm") == "PM 6:00");  // a's of AM/PM aren't day names
}

TEST_CASE("A format code of modifiers only shows the value as General; an empty section still hides it") {
  CHECK(F(5, "[Red]") == "5");
  CHECK(formula::FormatValue(Value::Number(-5), "[Red]").color != 0);
  CHECK(F(5, "[$-404]") == "5");
  CHECK(F(5, "[Blue][>3]") == "5");
  CHECK(F(5, ";;;").empty());
  CHECK(F(-5, "0;").empty());
}
