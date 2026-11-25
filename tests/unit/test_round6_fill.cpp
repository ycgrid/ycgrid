// SPDX-License-Identifier: Apache-2.0
// Round 6 (typing and filling): typed exponents and 12-hour times as Excel reads them, AutoFill of text series by
// their step and of numbers by their best-fit line
#include <catch2/catch_test_macros.hpp>

#include "formula/datetime.h"
#include "formula/numfmt.h"
#include "test_helpers.h"

#include <cmath>

using yc::core::Rect;
namespace formula = yc::formula;

namespace {
bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }
}  // namespace

TEST_CASE("Round6: an exponent without digits is text") {
  for (const char* text : {"1e", "1E", "1e+", "1e-", "1.5e", "-2E"}) {
    INFO(text);
    CHECK(formula::ParseInput(text).value.IsString());
  }
  auto p = formula::ParseInput("1e5");
  REQUIRE(p.value.IsNumber());
  CHECK(p.value.AsDouble() == 100000);
  CHECK(p.format == "0.00E+00");
  CHECK(formula::ParseInput("2.5E-3").value.AsDouble() == 0.0025);
}

TEST_CASE("Round6: 12-hour times take A / P after a space, any case, and the hour alone") {
  struct Case {
    const char* text;
    double hours;
  };
  for (const Case& c : {Case{"9:00 p", 21}, Case{"9:00 P", 21}, Case{"4:30 p", 16.5}, Case{"3:00 a", 3}, Case{"12:15 a", 0.25},
                        Case{"10:30 Pm", 22.5}, Case{"10:30 pM", 22.5}, Case{"3 PM", 15}, Case{"3 pm", 15}, Case{"3 p", 15},
                        Case{"12 AM", 0}, Case{"12 PM", 12}, Case{"9 am", 9}, Case{"6:00 PM", 18}}) {
    INFO(c.text);
    const auto p = formula::ParseInput(c.text);
    REQUIRE(p.value.IsNumber());
    CHECK(Near(p.value.AsDouble(), c.hours / 24));
    CHECK(p.format == "h:mm AM/PM");
  }
  CHECK(formula::ParseInput("9:15:30 p").format == "h:mm:ss AM/PM");
  // still a number, or text
  CHECK(formula::ParseInput("3").value.AsDouble() == 3);
  for (const char* text : {"13 PM", "3 x", "a", "p", "PM", "3p", "3:00 AM PM", "May 3 PM", "3 apples", "10:30PM", "6:00PM"}) {  // AM / PM after a space (measured)
    INFO(text);
    CHECK(formula::ParseInput(text).value.IsString());
  }
  // a date with such a time, and the functions that read text as a time
  const auto p = formula::ParseInput("1/2/2024 9:00 p");
  REQUIRE(p.value.IsNumber());
  CHECK(Near(p.value.AsDouble(), formula::DateSerial(2024, 1, 2) + 21.0 / 24));
  yctest::Doc d;
  d.Set("A1", "=TIMEVALUE(\"9:00 p\")");
  d.Set("A2", "=TIMEVALUE(\"3 PM\")");
  d.Set("A3", "=VALUE(\"1e\")");
  CHECK(Near(d.N("A1"), 21.0 / 24));
  CHECK(Near(d.N("A2"), 15.0 / 24));
  CHECK(d.V("A3").IsError());
  // typed into a cell
  d.Set("B1", "4:30 p");
  CHECK(Near(d.N("B1"), 16.5 / 24));
  CHECK(d.T("B1") == "4:30 PM");
}

TEST_CASE("Round6: AutoFill continues a text series by the step its cells show") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  auto fill = [&](uint32_t col, std::initializer_list<const char*> src, uint32_t add) {
    uint32_t r = 0;
    for (const char* s : src) t.d.SetValue(sh, r++, col, yc::core::Value::String(s));
    t.d.AutoFill(sh, Rect{0, col, r - 1, col}, Rect{0, col, r - 1 + add, col});
    std::vector<std::string> out;
    for (uint32_t i = 0; i < add; ++i) out.push_back(std::string(t.d.GetValue(sh, r + i, col).str()));
    return out;
  };
  using V = std::vector<std::string>;
  CHECK(fill(0, {"Jan", "Feb"}, 4) == V{"Mar", "Apr", "May", "Jun"});
  CHECK(fill(1, {"Mon", "Tue"}, 2) == V{"Wed", "Thu"});
  CHECK(fill(2, {"Jan", "Apr"}, 3) == V{"Jul", "Oct", "Jan"});  // Excel's own example
  CHECK(fill(3, {"Mon", "Wed"}, 3) == V{"Fri", "Sun", "Tue"});
  CHECK(fill(4, {"Nov", "Dec"}, 2) == V{"Jan", "Feb"});
  CHECK(fill(5, {"Dec", "Nov"}, 2) == V{"Oct", "Sep"});
  CHECK(fill(6, {"Jan", "Feb", "Mar"}, 2) == V{"Apr", "May"});
  CHECK(fill(7, {"Item 1", "Item 2"}, 3) == V{"Item 3", "Item 4", "Item 5"});
  CHECK(fill(8, {"Item 1", "Item 3"}, 2) == V{"Item 5", "Item 7"});
  CHECK(fill(9, {"Item 001", "Item 002"}, 2) == V{"Item 003", "Item 004"});
  CHECK(fill(10, {"Q1", "Q2"}, 3) == V{"Q3", "Q4", "Q1"});
  CHECK(fill(11, {"1st Period", "2nd Period"}, 2) == V{"3rd Period", "4th Period"});
  CHECK(fill(12, {"January", "March"}, 2) == V{"May", "July"});
  // mixed kinds still repeat, each cell counting on by one (Excel's text1, textA example)
  CHECK(fill(13, {"text1", "textA"}, 4) == V{"text2", "textA", "text3", "textA"});
  CHECK(fill(14, {"Jan", "x"}, 4) == V{"Feb", "x", "Mar", "x"});
  CHECK(fill(15, {"A1", "B1"}, 2) == V{"A2", "B2"});
  // upwards: the series goes back
  t.d.SetValue(sh, 4, 16, yc::core::Value::String("Feb"));
  t.d.SetValue(sh, 5, 16, yc::core::Value::String("Mar"));
  t.d.AutoFill(sh, Rect{4, 16, 5, 16}, Rect{2, 16, 5, 16});
  CHECK(t.d.GetValue(sh, 3, 16).str() == "Jan");
  CHECK(t.d.GetValue(sh, 2, 16).str() == "Dec");
  // the tip shown while dragging says the same
  const auto& s = *t.d.S(sh);
  CHECK(yc::engine::Document::AutoFillTip(t.d.wb(), s, Rect{0, 0, 1, 0}, Rect{0, 0, 3, 0}) == "Apr");
}

TEST_CASE("Round6: AutoFill of three or more numbers follows their best-fit line") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("A2", "3");
  t.Set("A3", "4");
  t.d.AutoFill(sh, Rect{0, 0, 2, 0}, Rect{0, 0, 5, 0});
  // Excel's example: 1, 3, 4 -> 5.66667, 7.16667, 8.66667; the starting values stay
  CHECK(Near(t.N("A4"), 17.0 / 3));
  CHECK(Near(t.N("A5"), 43.0 / 6));
  CHECK(Near(t.N("A6"), 26.0 / 3));
  CHECK(t.N("A1") == 1);
  CHECK(t.N("A3") == 4);
  // evenly spaced numbers continue exactly from the first
  t.Set("B1", "1");
  t.Set("B2", "3");
  t.Set("B3", "5");
  t.d.AutoFill(sh, Rect{0, 1, 2, 1}, Rect{0, 1, 4, 1});
  CHECK(t.N("B4") == 7);
  CHECK(t.N("B5") == 9);
  t.Set("C1", "0.1");
  t.Set("C2", "0.2");
  t.d.AutoFill(sh, Rect{0, 2, 1, 2}, Rect{0, 2, 3, 2});
  CHECK(t.T("C3") == "0.3");
  CHECK(t.T("C4") == "0.4");
  // upwards from 1, 2, 4 at rows 5:7: the line at the rows above
  t.Set("D5", "1");
  t.Set("D6", "2");
  t.Set("D7", "4");
  t.d.AutoFill(sh, Rect{4, 3, 6, 3}, Rect{3, 3, 6, 3});
  CHECK(Near(t.N("D4"), 7.0 / 3 - 1.5 * 2));  // the line's mean 7/3 at row 6, 1.5 a row
}
