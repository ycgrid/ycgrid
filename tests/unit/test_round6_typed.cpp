// SPDX-License-Identifier: Apache-2.0
// Round 6 (typed input, measured against Excel 16 itself): the formats Excel gives typed numbers, currency amounts
// and percentages, the marks it takes around them, 12-hour times, and Text to Columns' Text columns
#include <catch2/catch_test_macros.hpp>

#include "engine/error_check.h"
#include "formula/numfmt.h"
#include "formula/parser.h"
#include "test_helpers.h"

#include <cmath>
#include <string>

namespace formula = yc::formula;
using yc::core::Rect;

namespace {

struct Typed {
  const char* text;
  double value;
  const char* format;
};

void CheckTyped(const Typed& c) {
  INFO(c.text);
  const auto p = formula::ParseInput(c.text);
  REQUIRE(p.value.IsNumber());
  CHECK(std::fabs(p.value.AsDouble() - c.value) < 1e-12);
  CHECK(p.format == c.format);
}

const char* const kDollars = "$#,##0_);[Red]($#,##0)";
const char* const kNtDollars = "\"NT$\"#,##0_);(\"NT$\"#,##0)";

}  // namespace

TEST_CASE("Round6: a decimal point typed gives two decimals, as Excel's formats") {
  const Typed cases[] = {
      {"12.5%", 0.125, "0.00%"},       {"12%", 0.12, "0%"},          {"0.5%", 0.005, "0.00%"},
      {"12.%", 0.12, "0.00%"},         {".5%", 0.005, "0.00%"},      {"1,000.5%", 10.005, "0.00%"},
      {"-5%", -0.05, "0%"},            {"1,000", 1000, "#,##0"},     {"1,000.5", 1000.5, "#,##0.00"},
      {"1,000.", 1000, "#,##0.00"},    {"1,000.0", 1000, "#,##0.00"}, {"(1,000.5)", -1000.5, "#,##0.00"},
      {"+1,000.5", 1000.5, "#,##0.00"}, {"$1,000", 1000, kDollars},  {"$1,000.5", 1000.5, "$#,##0.00_);[Red]($#,##0.00)"},
      {"$5.", 5, "$#,##0.00_);[Red]($#,##0.00)"},                    {"NT$1,000", 1000, kNtDollars},
      {"NT$0.5", 0.5, "\"NT$\"#,##0.00_);(\"NT$\"#,##0.00)"},       {"1e3", 1000, "0.00E+00"},
      {"1,000e3", 1e6, "0.00E+00"},    {"1e3%", 10, "0.00E+00"},     {"1.5E+3%", 15, "0.00E+00"},
      {"(5)", -5, ""},                 {".5", 0.5, ""},
  };
  for (const Typed& c : cases) CheckTyped(c);
}

TEST_CASE("Round6: the signs, parentheses, spaces and currency marks Excel takes around a typed number") {
  const Typed cases[] = {
      {"$-5", -5, kDollars},     {"-$5", -5, kDollars},      {"$(5)", -5, kDollars},     {"($5)", -5, kDollars},
      {"$ 5", 5, kDollars},      {"$ -5", -5, kDollars},     {"$- 5", -5, kDollars},     {"-$ 5", -5, kDollars},
      {"+$5", 5, kDollars},      {"NT$-5", -5, kNtDollars},  {"-NT$5", -5, kNtDollars},  {"(NT$5)", -5, kNtDollars},
      {"NT$(5)", -5, kNtDollars}, {"nt$-5", -5, kNtDollars}, {"Nt$5", 5, kNtDollars},    {"NT$ 5", 5, kNtDollars},
      {"+NT$5", 5, kNtDollars},  {"( 5 )", -5, ""},          {"(5 )", -5, ""},           {"- 5", -5, ""},
      {"12.5 %", 0.125, "0.00%"}, {"%12.5", 0.125, "0.00%"}, {"% 12.5", 0.125, "0.00%"}, {"%-12.5", -0.125, "0.00%"},
      {"-%12.5", -0.125, "0.00%"}, {"(12.5)%", -0.125, "0.00%"}, {"(12.5%)", -0.125, "0.00%"},
  };
  for (const Typed& c : cases) CheckTyped(c);
  // text, as in Excel: two negative marks ((-5) was 5), a currency amount as a percentage, marks after the number
  for (const char* text : {"(-5)", "-(5)", "($-5)", "-$-5", "$--5", "-$(5)", "$-(5)", "12.5%%", "$5%", "$1,000.5%", "NT $5", "$NT5",
                           "5$", "5 $", "++5"}) {
    INFO(text);
    CHECK(formula::ParseInput(text).value.IsString());
  }
}

TEST_CASE("Round6: an amount after a sign is an amount, not a formula, as Excel reads it first") {
  CHECK_FALSE(formula::LooksLikeFormula("-NT$5"));  // NT$5 is also a cell
  CHECK_FALSE(formula::LooksLikeFormula("+NT$5"));
  CHECK_FALSE(formula::LooksLikeFormula("-$ 5"));
  CHECK_FALSE(formula::LooksLikeFormula("-%12.5"));
  CHECK_FALSE(formula::LooksLikeFormula("- 5"));
  CHECK(formula::LooksLikeFormula("-A1"));
  CHECK(formula::LooksLikeFormula("-NT5"));
  CHECK(formula::LooksLikeFormula("+SUM(A1:A3)"));
  yctest::Doc t;
  t.Set("NT5", "7");
  t.Set("A1", "-NT$5");
  t.Set("A2", "+NT$5");
  t.Set("A3", "$-5");
  t.Set("A4", "-NT5");
  CHECK(t.N("A1") == -5);
  CHECK(t.T("A1") == "(NT$5)");
  CHECK(t.N("A2") == 5);
  CHECK(t.T("A2") == "NT$5 ");
  CHECK(t.N("A3") == -5);
  CHECK(t.T("A3") == "($5)");
  CHECK(t.F("A4") == "=-NT5");
  CHECK(t.N("A4") == -7);
}

TEST_CASE("Round6: a 12-hour time has an hour up to 12, as in Excel") {
  for (const char* text : {"13:00 pm", "13:00 PM", "13:30:00 am", "13 pm"}) {
    INFO(text);
    CHECK(formula::ParseInput(text).value.IsString());
  }
  CHECK(formula::ParseInput("0:00 pm").value.AsDouble() == 0.5);
  CHECK(formula::ParseInput("12:30 pm").value.AsDouble() == (12 * 3600.0 + 30 * 60.0) / 86400.0);
  CHECK(formula::ParseInput("0:30 am").value.AsDouble() == (30 * 60.0) / 86400.0);
  CHECK(formula::ParseInput("13:00").value.AsDouble() == (13 * 3600.0) / 86400.0);  // no marker: a 24-hour time
}

TEST_CASE("Round6: Text to Columns formats a Text column as Text, as Excel does") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "'001,002");
  t.Set("A2", "'0012,0034");
  yc::engine::TextToColumnsOptions o;
  o.tab = false;
  o.comma = true;
  o.types = {2, 1};
  t.d.TextToColumns(sh, Rect{0, 0, 1, 0}, 0, 0, o);
  auto numfmt = [&](uint32_t r, uint32_t c) {
    const auto& styles = *t.d.wb().styles;
    return std::string(styles.NumberFormat(styles.Get(t.d.S(sh)->GetStyle(r, c))));
  };
  CHECK(t.V("A1").IsString());
  CHECK(t.T("A1") == "001");
  CHECK(numfmt(0, 0) == "@");
  CHECK(t.T("A2") == "0012");
  CHECK(numfmt(1, 0) == "@");
  CHECK(t.N("B1") == 2);
  CHECK(t.N("B2") == 34);
  CHECK(numfmt(0, 1) != "@");
  // typed into later, the Text cell keeps the text
  t.Set("A1", "007");
  CHECK(t.V("A1").IsString());
  CHECK(t.T("A1") == "007");
}

TEST_CASE("Round6: a currency symbol in quotes makes a currency format") {
  using namespace yc::engine;
  CHECK(FormatKindOf("\"NT$\"#,##0_);(\"NT$\"#,##0)") == kFmtCurrency);
  CHECK(FormatKindOf("\"NT$\"#,##0.00") == kFmtCurrency);
  CHECK(FormatKindOf("$#,##0_);[Red]($#,##0)") == kFmtCurrency);
  CHECK(FormatKindOf("0 \"items\"") == kFmtNumber);
}
