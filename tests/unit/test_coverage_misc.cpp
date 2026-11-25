// SPDX-License-Identifier: Apache-2.0
// Coverage of paths that had no test: header / footer codes (page_layout.cpp), the error checker's titles, help and
// text dates (error_check.cpp), GROUPBY's subtotals and filters, PIVOTBY's filter (fn_group.cpp), and the information
// functions SHEET, ISERR, TYPE and CELL("prefix") (fn_info.cpp).
#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "engine/error_check.h"
#include "engine/page_layout.h"
#include "test_helpers.h"

using yc::core::Value;
namespace engine = yc::engine;

TEST_CASE("Header and footer codes: page arithmetic, every field, && and font codes") {
  auto x = [](const std::string& code) { return engine::ExpandHeaderFooter(code, 3, 7, "Book.ycg", "C:\\Data\\", "Prices", "2026/11/13", "09:30"); };
  CHECK(x("Page &P of &N") == "Page 3 of 7");
  CHECK(x("&P+1|&P-2|&P+|&P+12") == "4|1|3+|15");
  CHECK(x("&D &T") == "2026/11/13 09:30");
  CHECK(x("&Z&F / &A") == "C:\\Data\\Book.ycg / Prices");
  CHECK(x("R&&D") == "R&D");
  CHECK(x("&\"Arial,Bold\"&12&B&I&U&S&KFF0000Big") == "Big");  // font codes are dropped
  CHECK(x("&p of &n") == "3 of 7");                              // codes are not case sensitive
  CHECK(x("tail&") == "tail&");
  // the same section split into runs of one font
  auto runs = engine::HeaderFooterRuns("a&Bb&B&Ic&I&Ud&U&Se&S&\"Arial,Bold Italic\"f&\"-,Regular\"g&14h&K00FF00i&Gj&&&P+1", 2, 5, "F", "Z\\", "S", "D", "T");
  REQUIRE(runs.size() >= 9);
  CHECK(runs[0].text == "a");
  CHECK(runs[1].text == "b");
  CHECK(runs[1].bold);
  CHECK_FALSE(runs[2].bold);
  CHECK(runs[2].italic);
  CHECK(runs[3].underline);
  CHECK(runs[4].strike);
  CHECK(runs[5].text == "f");
  CHECK(runs[5].font == "Arial");
  CHECK(runs[5].bold);
  CHECK(runs[5].italic);
  CHECK(runs[6].text == "g");
  CHECK(runs[6].font == "Arial");  // "-" keeps the font
  CHECK_FALSE(runs[6].bold);
  CHECK(runs[7].text == "h");
  CHECK(runs[7].size == 14);
  CHECK(runs[8].text == "i");
  CHECK(runs[8].color == 0xFF00FF00u);
  bool picture = false;
  for (const auto& r : runs) picture = picture || r.picture;
  CHECK(picture);
  CHECK(runs.back().text == "j&3");
  // a font size is kept within Excel's 1..409, and &E underlines as &U does
  auto big = engine::HeaderFooterRuns("&999x&Ey", 1, 1, "", "", "", "", "");
  REQUIRE(big.size() == 2);
  CHECK(big[0].size == 409);
  CHECK(big[1].underline);
}

TEST_CASE("Error checking: a title and help for every error value; text dates with a month's name") {
  using engine::kErrEvaluateToError;
  for (auto e : {yc::core::Err::Div0, yc::core::Err::NA, yc::core::Err::Name, yc::core::Err::Null, yc::core::Err::Num, yc::core::Err::Ref,
                 yc::core::Err::Spill, yc::core::Err::Calc, yc::core::Err::Value}) {
    const Value v = Value::Error(e);
    CAPTURE(static_cast<int>(e));
    CHECK(std::strlen(engine::ErrorRuleTitle(kErrEvaluateToError, v)) > 0);
    CHECK(std::strlen(engine::ErrorRuleHelp(kErrEvaluateToError, v)) > 0);
  }
  CHECK(std::string(engine::ErrorRuleTitle(kErrEvaluateToError, Value::Error(yc::core::Err::Ref))) == "Invalid Cell Reference Error");
  for (int rule = engine::kErrTextDate; rule < engine::kErrRuleCount; ++rule) {
    CAPTURE(rule);
    CHECK(std::strlen(engine::ErrorRuleTitle(rule, Value())) > 0);
    CHECK(std::strlen(engine::ErrorRuleHelp(rule, Value())) > 0);
  }
  CHECK(std::string(engine::ErrorRuleTitle(99, Value())).empty());
  yctest::Doc t;
  t.Set("A1", "'31-Dec-99");  // a two-digit year after a month's name
  t.Set("A2", "'Dec 31, 1999");
  t.Set("A3", "'10:30");
  const engine::ErrorCheckOptions o;
  const yc::model::Sheet& s = *t.d.S(t.sheet());
  CHECK(engine::ErrorCheckRule(t.d.wb(), s, 0, 0, o) == engine::kErrTextDate);
  CHECK(engine::ErrorCheckRule(t.d.wb(), s, 1, 0, o) == engine::kErrNone);
  CHECK(engine::ErrorCheckRule(t.d.wb(), s, 2, 0, o) == engine::kErrNone);
}

TEST_CASE("GROUPBY's subtotals at two levels, at the bottom and at the top, and its filter; PIVOTBY's filter") {
  yctest::Doc t;
  const char* rows[][3] = {{"x", "a", "1"}, {"x", "b", "2"}, {"y", "a", "3"}, {"y", "a", "4"}};
  for (int i = 0; i < 4; ++i) {
    for (int c = 0; c < 3; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(i + 2)).c_str(), rows[i][c]);
  }
  // depth 2: each first key's subtotal after its rows, then the total
  CHECK(t.Eval("ROWS(GROUPBY(A2:B5,C2:C5,SUM,0,2))") == "6");
  CHECK(t.Eval("INDEX(GROUPBY(A2:B5,C2:C5,SUM,0,2),3,3)") == "3");
  CHECK(t.Eval("INDEX(GROUPBY(A2:B5,C2:C5,SUM,0,2),5,1)") == "y");
  CHECK(t.Eval("INDEX(GROUPBY(A2:B5,C2:C5,SUM,0,2),5,3)") == "7");
  CHECK(t.Eval("INDEX(GROUPBY(A2:B5,C2:C5,SUM,0,2),6,3)") == "10");
  // -2: the total first, each subtotal before its rows
  CHECK(t.Eval("INDEX(GROUPBY(A2:B5,C2:C5,SUM,0,-2),1,3)") == "10");
  CHECK(t.Eval("INDEX(GROUPBY(A2:B5,C2:C5,SUM,0,-2),2,3)") == "3");
  CHECK(t.Eval("INDEX(GROUPBY(A2:B5,C2:C5,SUM,0,-2),3,2)") == "a");
  // a filter leaves rows out before grouping
  CHECK(t.Eval("ARRAYTOTEXT(GROUPBY(A2:A5,C2:C5,SUM,0,1,,C2:C5>1),1)") == "{\"x\",2;\"y\",7;\"Total\",9}");
  CHECK(t.Eval("INDEX(PIVOTBY(A2:A5,B2:B5,C2:C5,SUM,0,1,,1,,C2:C5<4),4,4)") == "6");
  CHECK(t.Eval("INDEX(PIVOTBY(A2:A5,B2:B5,C2:C5,SUM,0,1,,1,,C2:C5<4),2,2)") == "1");
}

TEST_CASE("SHEET by name, ISERR over arrays, TYPE of an array and an error, CELL's prefix") {
  yctest::Doc t;
  const std::string name = t.d.S(t.sheet())->name;
  CHECK(t.Eval("SHEET(\"" + name + "\")") == "1");
  CHECK(t.Eval("SHEET(\"no such sheet\")") == "#N/A");
  CHECK(t.Eval("SHEET(A1)") == "1");
  CHECK(t.Eval("ISERR(#N/A)") == "FALSE");
  CHECK(t.Eval("ISERR(1/0)") == "TRUE");
  CHECK(t.Eval("ARRAYTOTEXT(ISERR({1,#DIV/0!,#N/A}))") == "FALSE, TRUE, FALSE");
  CHECK(t.Eval("TYPE({1,2})") == "64");
  CHECK(t.Eval("TYPE(#N/A)") == "16");
  CHECK(t.Eval("TYPE(TRUE)") == "4");
  t.Set("B1", "left");
  t.Set("B2", "right");
  t.Set("B3", "center");
  t.Set("B4", "fill");
  t.d.ModifyStyle(t.sheet(), yc::core::Rect::Cell(0, 1), [](yc::model::Style& s) { s.halign = yc::model::HAlign::Left; });
  t.d.ModifyStyle(t.sheet(), yc::core::Rect::Cell(1, 1), [](yc::model::Style& s) { s.halign = yc::model::HAlign::Right; });
  t.d.ModifyStyle(t.sheet(), yc::core::Rect::Cell(2, 1), [](yc::model::Style& s) { s.halign = yc::model::HAlign::Center; });
  t.d.ModifyStyle(t.sheet(), yc::core::Rect::Cell(3, 1), [](yc::model::Style& s) { s.halign = yc::model::HAlign::Fill; });
  CHECK(t.Eval("CELL(\"prefix\",B1)") == "'");
  CHECK(t.Eval("CELL(\"prefix\",B2)") == "\"");
  CHECK(t.Eval("CELL(\"prefix\",B3)") == "^");
  CHECK(t.Eval("CELL(\"prefix\",B4)") == "\\");
  CHECK(t.Eval("CELL(\"prefix\",B9)") == "");  // an empty cell
}
