// SPDX-License-Identifier: Apache-2.0
// Round 7 (number formats measured against Excel 16 itself): fractions, placeholders, signs, conditions, TRUE / FALSE
// in text sections, elapsed time, Chinese numerals in numbers and dates, the Republic's and Japan's eras, day names,
// numbers as text, CELL's format codes, the formats formulas give their results and typed full-width and Chinese
// input
#include <catch2/catch_test_macros.hpp>

#include "formula/numfmt.h"
#include "test_helpers.h"

#include <cmath>
#include <string>

namespace formula = yc::formula;
using yc::core::Rect;
using yc::core::Value;

namespace {

std::string F(double v, const char* code) { return formula::FormatValue(Value::Number(v), code).text; }
std::string FB(bool v, const char* code) { return formula::FormatValue(Value::Bool(v), code).text; }

void SetFormat(yctest::Doc& t, const char* a1, const char* code) {
  const auto p = yctest::Doc::P(a1);
  t.d.ModifyStyle(t.sheet(), Rect::Cell(p.row, p.col), [&](yc::model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString(code); });
}

std::string FormatOf(yctest::Doc& t, const char* a1) {
  const auto p = yctest::Doc::P(a1);
  const auto& styles = *t.d.wb().styles;
  return std::string(styles.NumberFormat(styles.Get(t.d.S(t.sheet())->GetStyle(p.row, p.col))));
}

}  // namespace

TEST_CASE("Round7: fractions show a whole number with blanks where the fraction goes, and fixed denominators") {
  CHECK(F(0, "# ?/?") == "0    ");
  CHECK(F(1, "# ?/?") == "1    ");
  CHECK(F(0.001, "# ?/?") == "0    ");
  CHECK(F(0.5, "# ?/?") == " 1/2");
  CHECK(F(-0.2, "# ?/?") == "- 1/5");
  CHECK(F(1, "# ??/??") == "1      ");
  CHECK(F(1, "0 ?/?") == "1    ");
  CHECK(F(0.5, "# ??/100") == " 50/100");  // /100 was read as /1 followed by two 0s: 1   1
  CHECK(F(0.25, "# ??/100") == " 25/100");
  CHECK(F(1234.5678, "# ??/100") == "1234 57/100");
  CHECK(F(1, "# ?/2") == "1    ");
  CHECK(F(2, "# ?/10") == "2     ");
  CHECK(F(1.25, "#-?/4") == "1-1/4");
  CHECK(F(0, "# #/#") == "0");
  CHECK(F(0.5, "# #/#") == "1/2");
  CHECK(F(1, "0 #/#") == "1");
  CHECK(F(1, "# 0/4") == "1 0/4");
  CHECK(F(0, "0 0/0") == "0 0/1");
  CHECK(F(1, "# ?/?0") == "1     ");
  CHECK(F(0, "?/?") == "0/1");
  CHECK(F(1.5, "?/?") == "3/2");
  CHECK(F(2.5, "#,### ?/?") == "2 1/2");
}

TEST_CASE("Round7: digit placeholders after the point close up, and the whole part shows without placeholders") {
  CHECK(F(1234.5678, ".00") == "1234.57");
  CHECK(F(-1, ".00") == "-1.00");
  CHECK(F(0.5, ".00") == ".50");
  CHECK(F(0, ".00") == ".00");
  CHECK(F(1, ".##") == "1.");
  CHECK(F(1234.5, ".0") == "1234.5");
  CHECK(F(0, "#.##0") == ".0");
  CHECK(F(0.5, "#.##0") == ".50");
  CHECK(F(1, "#.##0") == "1.0");
  CHECK(F(1234.5678, "#.##0") == "1234.568");
  CHECK(F(1, "0.#0") == "1.0");
  CHECK(F(0.5, "0.0#") == "0.5");
  CHECK(F(0.5, "0.??") == "0.5 ");
  CHECK(F(0.25, "0.0#0") == "0.250");
}

TEST_CASE("Round7: zero in scientific notation fills the whole placeholders") {
  CHECK(F(0, "##0.0E+0") == "000.0E+0");
  CHECK(F(0, "#.##E+00") == "0.E+00");
  CHECK(F(0, "#E+0") == "0E+0");
  CHECK(F(0, "#0.#E+00") == "00.E+00");
  CHECK(F(0, "#,##0.0E+0") == "0,000.0E+0");
  CHECK(F(0, "0.00E+00") == "0.00E+00");
  CHECK(F(-0.4, "0E+0") == "-4E-1");
}

TEST_CASE("Round7: a conditional section drops the minus only when it shows negative numbers alone") {
  struct C {
    double v;
    const char* code;
    const char* want;
  };
  const C cases[] = {
      {-5, "[<0]0;0", "5"},        {-5, "[<-1]0;0", "5"},          {-5, "[=-5]0;0", "5"},     {-5, "[<0]0", "5"},
      {-5, "[<=-1]0;0", "5"},      {-5, "[<=0]0", "-5"},           {-5, "[<100]0;0", "-5"},   {-5, "[<>0]0", "-5"},
      {-5, "[>=-10]0;0", "-5"},    {-5, "[>-10]0;0", "-5"},        {-5, "[>0]0;0", "5"},      {-5, "[>=0]0;0", "5"},
      {-5, "[>-1]0;0", "5"},       {-5, "[<-10]0;0", "5"},         {-5, "[=0]0;0", "-5"},     {-5, "[=0]0;[=1]0;0", "-5"},
      {-5, "[>0]0;[<0]0;0", "5"},  {-5, "[>0]0;[>-10]0;0", "-5"},  {-1, "[>=1000]#,##0;0", "-1"},
      {-1, "[<0]\"neg\";[>0]\"pos\";\"zero\"", "neg"},               {-1, "[=1]\"one\";[=2]\"two\";0", "-1"},
      {-50, "[<-10]0;[<0]0;0", "50"}, {-5, "[<-10]0;[<0]0;0", "5"},
  };
  for (const C& c : cases) {
    INFO(c.code << " " << c.v);
    CHECK(F(c.v, c.code) == c.want);
  }
}

TEST_CASE("Round7: TRUE and FALSE go through the text section") {
  CHECK(FB(true, "\"Text: \"@") == "Text: TRUE");
  CHECK(FB(true, "@\" units\"") == "TRUE units");
  CHECK(FB(true, "0;-0;0;\"text: \"@") == "text: TRUE");
  CHECK(FB(true, ";;;") == "");
  CHECK(FB(true, "@@") == "TRUETRUE");
  CHECK(FB(false, "\"x\"@") == "xFALSE");
  CHECK(FB(true, "0.00") == "TRUE");
  CHECK(FB(true, "\"a\";\"b\";\"c\"") == "TRUE");
}

TEST_CASE("Round7: elapsed time alone takes negative and very large numbers") {
  CHECK(F(-1.5, "[h]") == "-36");
  CHECK(F(-1, "[h] \"h\"") == "-24 h");
  CHECK(F(-0.4, "[h]") == "-9");
  CHECK(F(-0.00001, "[s]") == "-1");
  CHECK(F(-0.5, "[ss]") == "-43200");
  CHECK(F(1234.5678, "[ss]") == "106666658");
  CHECK(F(12345678901, "[ss]") == "1066666657046400");
  CHECK(F(1e15, "[ss]") == "86400000000000000000");
  CHECK(F(1.5, "[mm]") == "2160");
  CHECK(F(1.5, "[h]\" hours\"") == "36 hours");
  CHECK(formula::FormatValue(Value::Number(-1), "[h]:mm").bad_date);  // with minutes it is still #####
  CHECK(formula::FormatValue(Value::Number(-1), "h:mm").bad_date);
}

TEST_CASE("Round7: [DBNum1] / [DBNum2] read General with units, and write digit placeholders digit by digit") {
  CHECK(F(12, "[DBNum1]") == "一十二");
  CHECK(F(12.5, "[DBNum1]") == "一十二.五");
  CHECK(F(-12, "[DBNum1]") == "-一十二");
  CHECK(F(123456789012, "[DBNum1]") == "一千二百三十四億五千六百七十八萬九千○一十二");
  CHECK(F(1e11, "[DBNum1]") == "一千億");
  CHECK(F(1e15, "[DBNum1]") == "一千兆");
  CHECK(F(1.5e15, "[DBNum1]") == "一千五百兆");
  CHECK(F(1e16, "[DBNum1]") == "一○○○○兆");
  CHECK(F(1e20, "[DBNum1]") == "一○○○○○○○○兆");
  CHECK(F(1000010, "[DBNum1]") == "一百萬○一十");
  CHECK(F(100010000, "[DBNum1]") == "一億○一萬");
  CHECK(F(0.1, "[DBNum1]") == "○.一");
  CHECK(F(10, "[DBNum2]") == "壹拾");
  CHECK(F(12, "[DBNum1]0") == "一二");
  CHECK(F(1012, "[DBNum1]0") == "一○一二");
  CHECK(F(1234.5678, "[DBNum1]0") == "一二三五");
  CHECK(F(1234.5678, "[DBNum2]0") == "壹貳參伍");
  CHECK(F(10, "[DBNum2]0") == "壹零");
  CHECK(F(0, "[DBNum2]0") == "零");
  CHECK(F(1234.5, "[DBNum1]#,##0.00") == "一,二三四.五○");
  CHECK(F(1.5, "[DBNum1]0.00") == "一.五○");
}

TEST_CASE("Round7: Chinese numerals in dates: years digit by digit, the other fields read without one ten") {
  const double nov11 = 45607;  // 2024-11-11
  CHECK(F(nov11, "[DBNum1]yyyy/mm/dd") == "二○二四/十一/十一");
  CHECK(F(45310, "[DBNum1]m\"月\"d\"日\"") == "一月十九日");
  CHECK(F(45310, "[DBNum1]yy") == "二十四");
  CHECK(F(45301, "[DBNum1]d") == "十");
  CHECK(F(45320, "[DBNum1]dd") == "二十九");
  CHECK(F(45293, "[DBNum1]dd") == "○二");
  CHECK(F(45301, "[DBNum1]mm") == "○一");
  CHECK(F(0.5, "[DBNum1]h:mm") == "十二:○○");
  CHECK(F(0.04, "[DBNum1]hh:mm:ss") == "○○:五十七:三十六");
  CHECK(F(45310, "[DBNum2]yyyy\"年\"m\"月\"d\"日\"") == "貳零貳肆年壹月壹拾玖日");
  CHECK(F(nov11, "[DBNum2]m\"月\"d\"日\"") == "壹拾壹月壹拾壹日");
  CHECK(F(45301, "[DBNum3]yyyy/m/d") == "２０２４/１/１０");
  CHECK(F(nov11, "[DBNum1][$-404]e\"年\"") == "一一三年");
  CHECK(F(nov11, "[DBNum2]e") == "壹壹參");
  CHECK(F(nov11, "[DBNum3]e") == "１１３");
  CHECK(F(nov11, "[DBNum1]aaa") == "週一");
  CHECK(F(nov11, "[DBNum1]ddd") == "Mon");
  CHECK(F(nov11, "[DBNum1][$-404]mmm") == "十一月");
}

TEST_CASE("Round7: the Republic's years and era name, before 1912 too, and Japan's eras") {
  const double jan1_1912 = 4384;
  CHECK(F(jan1_1912, "[$-404]ggge") == "中華民國元");
  CHECK(F(jan1_1912, "[$-404]gge") == "民國元");
  CHECK(F(jan1_1912, "[$-404]ge") == "民國元");
  CHECK(F(jan1_1912, "[$-404]ee") == "1");
  CHECK(F(4749, "[$-404]gggee") == "中華民國元");  // 1912-12-31
  CHECK(F(4383, "[$-404]ggge") == "中華民國前1");
  CHECK(F(1, "[$-404]ggge") == "中華民國前12");
  CHECK(F(4383, "[$-404]ggg") == "中華民國前");
  CHECK(F(4383, "[$-404]g") == "民國前");
  CHECK(F(1000, "[$-404]e") == "1902");  // without the era name, the Gregorian year
  CHECK(F(4383, "[$-404]e") == "1911");
  CHECK(F(1000, "[$-404]gg e") == "民國前 10");
  CHECK(F(0, "[$-404]e/m/d") == "1900/1/0");
  CHECK(F(45607, "[$-404]e") == "113");
  CHECK(F(45607, "[$-404]ggg") == "中華民國");
  CHECK(F(45607, "[$-404]ee/mm/dd") == "113/11/11");
  CHECK(F(45607, "gggee") == "中華民國113");
  CHECK(F(45607, "[$-411]ggge") == "令和6");
}

TEST_CASE("Round7: day and month names follow the section's language") {
  const double nov11 = 45607;  // a Monday
  CHECK(F(45298, "aaa") == "週日");
  CHECK(F(45298, "aaaa") == "星期日");
  CHECK(F(45298, "[$-804]aaa") == "周日");
  CHECK(F(nov11, "[$-804]aaaa") == "星期一");
  CHECK(F(nov11, "[$-411]aaaa") == "月曜日");
  CHECK(F(45298, "[$-411]aaa") == "日");
  CHECK(F(nov11, "[$-412]aaa") == "월");
  CHECK(F(45298, "[$-409]aaa") == "Sun");
  CHECK(F(nov11, "[$-404]ddd") == "週一");
  CHECK(F(nov11, "[$-404]dddd") == "星期一");
  CHECK(F(45298, "[$-404]mmm") == "一月");
  CHECK(F(nov11, "[$-404]mmm") == "十一月");
  CHECK(F(nov11, "[$-404]mmmm") == "十一月");
  CHECK(F(nov11, "[$-404]mmmmm") == "十");
}

TEST_CASE("Round7: a number as text has 15 digits, written out up to 20 characters") {
  CHECK(formula::NumberToText(123456789012) == "123456789012");
  CHECK(formula::NumberToText(1e15) == "1000000000000000");
  CHECK(formula::NumberToText(1e19) == "10000000000000000000");
  CHECK(formula::NumberToText(1e20) == "1E+20");
  CHECK(formula::NumberToText(1e21) == "1E+21");
  CHECK(formula::NumberToText(123456789012345000.0) == "123456789012345000");
  CHECK(formula::NumberToText(-1e15) == "-1000000000000000");
  CHECK(formula::NumberToText(1e-10) == "0.0000000001");
  CHECK(formula::NumberToText(1.5e-10) == "0.00000000015");
  CHECK(formula::NumberToText(1e-16) == "0.0000000000000001");
  CHECK(formula::NumberToText(1e-19) == "1E-19");
  CHECK(formula::NumberToText(1.23456789012345e-12) == "1.23456789012345E-12");
  CHECK(formula::NumberToText(2.0 / 3) == "0.666666666666667");
  CHECK(formula::NumberToText(0.1 + 0.2) == "0.3");
  yctest::Doc t;
  CHECK(t.Eval("123456789012&\"\"") == "123456789012");
  CHECK(t.Eval("1E+15&\"\"") == "1000000000000000");
  CHECK(t.Eval("CONCAT(1.5,2)") == "1.52");
  CHECK(t.Eval("TEXTJOIN(\",\",TRUE,1.25,1E-7)") == "1.25,0.0000001");
}

TEST_CASE("Round7: CELL gives Excel's format codes, colors, parentheses, prefixes, locks and widths") {
  struct C {
    const char* code;
    const char* want;
    bool color;
  };
  const C cases[] = {
      {"General", "G", false},          {"0", "F0", false},                 {"0.00", "F2", false},
      {"#,##0", ",0", false},           {"#,##0.00", ",2", false},          {"$#,##0_);($#,##0)", "C0", false},
      {"$#,##0_);[Red]($#,##0)", "C0-", true}, {"$#,##0.00_);[Red]($#,##0.00)", "C2-", true},
      {"0%", "P0", false},              {"0.00%", "P2", false},             {"0.0%", "P1", false},
      {"0.00E+00", "S2", false},        {"##0.0E+0", "S1", false},          {"# ?/?", "G", false},
      {"m/d/yy", "D1", false},          {"d-mmm-yy", "D1", false},          {"d-mmm", "D3", false},
      {"mmm-yy", "D2", false},          {"yyyy/m/d", "D1", false},          {"yyyy-mm-dd hh:mm:ss", "D1", false},
      {"h:mm AM/PM", "D7", false},      {"h:mm:ss AM/PM", "D6", false},     {"h:mm", "D9", false},
      {"h:mm:ss", "D8", false},         {"mm:ss", "G", false},              {"[h]:mm:ss", "G", false},
      {"@", "G", false},                {"0.000", "F3", false},             {"#,##0.000", ",3", false},
      {"\"$\"#,##0", "C0", false},      {"\"NT$\"#,##0", ",0", false},      {"[Red]0", "F0", false},
      {"0;[Red]0", "F0-", true},        {"#,##0_);[Red](#,##0)", ",0-", true}, {"[Blue]0.00", "F2", false},
      {"0.00;(0.00)", "F2", false},     {"[<0]0;0", "F0", false},
  };
  for (const C& c : cases) {
    INFO(c.code);
    bool color = false, parens = false;
    CHECK(formula::CellFormatCode(c.code, &color, &parens) == c.want);
    CHECK(color == c.color);
    CHECK_FALSE(parens);
  }
  bool color = false, parens = false;
  CHECK(formula::CellFormatCode("(0)", &color, &parens) == "F0()");
  CHECK(parens);
  yctest::Doc t;
  t.Set("A1", "5");
  SetFormat(t, "A1", "$#,##0.00_);[Red]($#,##0.00)");
  t.Set("A2", "text");
  CHECK(t.Eval("CELL(\"format\",A1)") == "C2-");
  CHECK(t.Eval("CELL(\"color\",A1)") == "1");
  CHECK(t.Eval("CELL(\"parentheses\",A1)") == "0");
  CHECK(t.Eval("CELL(\"format\",B99)") == "G");
  CHECK(t.Eval("CELL(\"prefix\",A2)") == "'");
  CHECK(t.Eval("CELL(\"prefix\",A1)") == "");
  CHECK(t.Eval("CELL(\"protect\",A1)") == "1");
  CHECK(t.Eval("CELL(\"width\",A1)") == "8");
}

TEST_CASE("Round7: a formula entered in a General cell takes the format its references or function give it") {
  yctest::Doc t;
  t.Set("A1", "5");
  SetFormat(t, "A1", "$#,##0.00");
  t.Set("A2", "45298");
  SetFormat(t, "A2", "yyyy/m/d");
  t.Set("A3", "0.25");
  SetFormat(t, "A3", "0%");
  t.Set("A4", "0.5");
  SetFormat(t, "A4", "h:mm");
  t.Set("A5", "1234.5");
  SetFormat(t, "A5", "#,##0.00");
  t.Set("A6", "7");  // General
  struct C {
    const char* formula;
    const char* format;
  };
  const C cases[] = {
      {"=A1", "$#,##0.00"},          {"=A1+1", "$#,##0.00"},        {"=1+A1", "$#,##0.00"},      {"=A6+A1", "$#,##0.00"},
      {"=-A1", "$#,##0.00"},         {"=(A1)", "$#,##0.00"},        {"=A5+A1", "#,##0.00"},      {"=A1-A5", "$#,##0.00"},
      {"=SUM(A1:A5)", "$#,##0.00"},  {"=SUM(A3,A1)", "0%"},         {"=MIN(A2,A1)", "yyyy/m/d"}, {"=MAX(A5,A1)", "#,##0.00"},
      {"=ROUND(A2,0)", "yyyy/m/d"},  {"=MOD(A1,3)", "$#,##0.00"},  {"=A2+1", "yyyy/m/d"},       {"=A2+A4", "yyyy/m/d"},
      {"=A4+A2", "h:mm"},            {"=A4-A4", "h:mm"},            {"=A3-A3", "0%"},
      {"=A1*2", "General"},                 {"=A1/2", "General"},                 {"=A1+A1*2", "General"},            {"=A1&\"\"", "General"},
      {"=A2-A2", "General"},                {"=A2+A2", "General"},                {"=MAX(A2,A2)", "General"},         {"=ABS(A1)", "General"},
      {"=IF(TRUE,A1,0)", "General"},        {"=INDEX(A1:A3,1)", "General"},       {"=A1%", "General"},                {"=ROUND(A1*2,0)", "General"},
      {"=PMT(0.05/12,360,-200000)", "$#,##0.00_);[Red]($#,##0.00)"}, {"=SLN(1000,100,5)", "$#,##0.00_);[Red]($#,##0.00)"},
      {"=RATE(360,-1073.64,200000)", "0%"}, {"=IRR({-100,50,60})", "0%"}, {"=NPER(0.01,-100,1000)", "General"},
      {"=TIME(9,30,0)", "h:mm AM/PM"}, {"=TIME(1,0,0)+A4", "h:mm AM/PM"}, {"=DATEVALUE(\"2024/1/2\")", "General"},
  };
  int row = 10;
  for (const C& c : cases) {
    INFO(c.formula);
    const std::string a1 = "C" + std::to_string(row++);
    t.Set(a1.c_str(), c.formula);
    CHECK(FormatOf(t, a1.c_str()) == c.format);
  }
  // a cell that has a format keeps it
  SetFormat(t, "D1", "0.000");
  t.Set("D1", "=A1");
  CHECK(FormatOf(t, "D1") == "0.000");
  // the date functions still give dates
  t.Set("D2", "=DATE(2024,1,2)");
  CHECK(formula::IsDateFormat(FormatOf(t, "D2")));
  t.Set("D3", "=TODAY()-A2");
  CHECK(FormatOf(t, "D3") == "General");
}

TEST_CASE("Round7: typed full-width numbers, Chinese dates and times, and the times Excel takes") {
  auto p = formula::ParseInput("\xEF\xBC\x92\xEF\xBC\x90\xEF\xBC\x92\xEF\xBC\x94");  // full-width 2024
  REQUIRE(p.value.IsNumber());
  CHECK(p.value.AsDouble() == 2024);
  p = formula::ParseInput("１２.５％");
  REQUIRE(p.value.IsNumber());
  CHECK(std::fabs(p.value.AsDouble() - 0.125) < 1e-12);
  CHECK(p.format == "0.00%");
  p = formula::ParseInput("＄１００");
  CHECK(p.value.AsDouble() == 100);
  CHECK(p.format == "$#,##0_);[Red]($#,##0)");
  CHECK(formula::ParseInput("１,０００").format == "#,##0");
  CHECK(formula::ParseInput("ＡＢＣ").value.IsString());
  CHECK(formula::ParseInput("ＡＢＣ").value.str() == "ＡＢＣ");
  CHECK(formula::ParseInput("−5").value.IsString());  // U+2212 is not a minus to Excel
  // the Chinese AM / PM markers stay text when typed
  CHECK(formula::ParseInput("下午 9:00").value.IsString());
  CHECK(formula::ParseInput("上午 9:30:15").value.IsString());
  CHECK(formula::ParseInput("2024/1/7 下午 9:00").value.IsString());
  p = formula::ParseInput("2024年1月7日");
  REQUIRE(p.value.IsNumber());
  CHECK(p.value.AsDouble() == 45298);
  CHECK(formula::FormatValue(p.value, p.format).text == "2024年1月7日");
  p = formula::ParseInput("9時30分");
  REQUIRE(p.value.IsNumber());
  CHECK(std::fabs(p.value.AsDouble() - 9.5 / 24) < 1e-12);
  CHECK(formula::FormatValue(p.value, p.format).text == "9時30分");
  p = formula::ParseInput("9時30分15秒");
  REQUIRE(p.value.IsNumber());
  CHECK(formula::FormatValue(p.value, p.format).text == "9時30分15秒");
  CHECK(formula::ParseInput("1月7日").value.IsString());
  CHECK(formula::ParseInput("2024年1月").value.IsString());
  CHECK(formula::ParseInput("民國113年1月7日").value.IsString());
  // fractions of a second, hours past 99, and a date with a time past 24 hours
  p = formula::ParseInput("9:00:00.5");
  CHECK(p.format == "mm:ss.0");
  CHECK(formula::FormatValue(p.value, p.format).text == "00:00.5");
  CHECK(formula::ParseInput("12:30:45.123").format == "mm:ss.0");
  p = formula::ParseInput("100:00");
  REQUIRE(p.value.IsNumber());
  CHECK(std::fabs(p.value.AsDouble() - 100.0 / 24) < 1e-12);
  CHECK(p.format == "[h]:mm:ss");
  p = formula::ParseInput("2024/1/7 25:00");
  REQUIRE(p.value.IsNumber());
  CHECK(std::fabs(p.value.AsDouble() - (45298 + 25.0 / 24)) < 1e-9);
  CHECK(p.format.empty());
  CHECK(formula::ParseInput("2024/1/7 9:00").format == "yyyy/m/d h:mm");
  CHECK(formula::ParseInput("1/7 9:00").format == "yyyy/m/d h:mm");
  CHECK(formula::ParseInput("1/2").format == "m\"月\"d\"日\"");
}

TEST_CASE("Round7: a fill (*x) is placed where the code puts it, and TEXT leaves it out") {
  auto r = formula::FormatValue(Value::Number(1), "_($* #,##0.00_);_($* (#,##0.00);_($* \"-\"??_);_(@_)");
  CHECK(r.text == " $1.00 ");
  CHECK(r.fill_at == 2);
  CHECK(r.fill == " ");
  r = formula::FormatValue(Value::Number(1), "0.00*-");
  CHECK(r.text == "1.00");
  CHECK(r.fill_at == 4);
  CHECK(r.fill == "-");
  r = formula::FormatValue(Value::Number(1), "*-0.00");
  CHECK(r.fill_at == 0);
  r = formula::FormatValue(Value::String("ab"), "@*.");
  CHECK(r.text == "ab");
  CHECK(r.fill_at == 2);
  CHECK(formula::FormatValue(Value::Number(1), "0.00").fill_at == -1);
  yctest::Doc t;
  CHECK(t.Eval("TEXT(1,\"0.00*-\")") == "1.00");
}

TEST_CASE("Round7: General shows 11 characters besides the minus, and formulas keep 15 digits of a number") {
  CHECK(formula::FormatValue(Value::Number(-833.33333333333337), "General").text == "-833.3333333");
  CHECK(formula::FormatValue(Value::Number(-1.4210854715202004e-14), "General").text == "-1.42109E-14");
  CHECK(formula::FormatValue(Value::Number(833.33333333333337), "General").text == "833.3333333");
  yctest::Doc t;
  CHECK(t.Eval("1234567890123456&\"\"") == "1234567890123450");
  CHECK(t.Eval("12345.678901234567&\"\"") == "12345.6789012345");
  CHECK(t.Eval("123456789012345678&\"\"") == "123456789012345000");
  CHECK(t.Eval("0.1234567890123456789*1E+20&\"\"") == "12345678901234500000");
}
