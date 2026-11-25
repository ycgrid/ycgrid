// SPDX-License-Identifier: Apache-2.0
// Formulas working as in Excel: the references of a formula being typed (their spans and
// colors), what a #SPILL! formula wants, F9 on part of a formula
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/error_check.h"
#include "formula/numfmt.h"
#include "formula/parser.h"
#include "io/ycg.h"

using yc::core::Rect;

namespace {

std::vector<yc::formula::RefSpan> Spans(yctest::Doc& t, const std::string& text, bool r1c1 = false) {
  return yc::formula::FindRefSpans(text, yc::core::CellPos{4, 4}, &t.d, r1c1);
}

}  // namespace

TEST_CASE("a quote never closed in a formula being typed stays inside the text (chaos 202)") {
  yctest::Doc t;
  // the sheet-name scan went one past the end and read text[n] (a debug build stopped on it)
  for (const char* text : {"=SUM(A1)'0012", "='", "=A1+'Sheet", "=A1:'"}) {
    const std::string s = text;
    for (const auto& sp : Spans(t, s)) {
      CHECK(sp.begin >= 0);
      CHECK(static_cast<size_t>(sp.end) <= s.size());
    }
  }
  auto s = Spans(t, "=SUM(A1)'0012");
  REQUIRE(s.size() == 1);
  CHECK(s[0].begin == 5);
}

TEST_CASE("the references of a formula being typed are found with where they are") {
  yctest::Doc t;
  // each different reference has one group (one color): =A1+B1+A1 draws two boxes, both "A1" texts blue
  auto s = Spans(t, "=A1+B1+A1");
  REQUIRE(s.size() == 3);
  CHECK(s[0].begin == 1);
  CHECK(s[0].end == 3);
  CHECK(s[1].begin == 4);
  CHECK(s[2].begin == 7);
  CHECK(s[0].group == 0);
  CHECK(s[1].group == 1);
  CHECK(s[2].group == 0);
  CHECK(s[1].ref.rect == Rect::Cell(0, 1));
  // a range, a function's name is not a reference, nor is text in quotes
  s = Spans(t, "=SUM(A1:A3)&\"B2\"&LOG10(C3)");
  REQUIRE(s.size() == 2);
  CHECK(s[0].begin == 5);
  CHECK(s[0].end == 10);
  CHECK(s[0].ref.rect == (Rect{0, 0, 2, 0}));
  CHECK(s[1].ref.rect == Rect::Cell(2, 2));
  // whole columns and rows, $ signs, a sheet name, a range still being typed
  s = Spans(t, "=SUM(B:B,3:3,$C$4,'" + t.d.wb().sheets[0]->name + "'!D5,E6:");
  REQUIRE(s.size() == 5);
  CHECK(s[0].ref.rect.c0 == 1);
  CHECK(s[0].ref.rect.c1 == 1);
  CHECK(s[1].ref.rect.r0 == 2);
  CHECK(s[2].ref.rect == Rect::Cell(3, 2));
  CHECK(s[3].ref.rect == Rect::Cell(4, 3));
  CHECK(s[4].ref.rect == Rect::Cell(5, 4));
  // "+" and "-" start formulas too (keypad users): the spans count from the start of the text
  s = Spans(t, "+A1+A2");
  REQUIRE(s.size() == 2);
  CHECK(s[0].begin == 1);
  // R1C1 style: relative to the formula's cell (E5)
  s = Spans(t, "=R[-1]C+RC[1]", true);
  REQUIRE(s.size() == 2);
  CHECK(s[0].ref.rect == Rect::Cell(3, 4));
  CHECK(s[1].ref.rect == Rect::Cell(4, 5));
}

TEST_CASE("a #SPILL! formula tells the range it wants") {
  yctest::Doc t;
  t.Set("A2", "x");
  t.Set("A1", "=SEQUENCE(3)");
  CHECK(t.T("A1") == "#SPILL!");
  const auto& blocked = t.d.spill_blocked();
  const auto it = blocked.find(yc::engine::Key(t.sheet(), 0, 0));
  REQUIRE(it != blocked.end());
  CHECK(it->second == (Rect{0, 0, 2, 0}));
  // the way cleared: it spills and isn't blocked any more
  t.Set("A2", "");
  CHECK(t.N("A3") == 3);
  CHECK(t.d.spill_blocked().count(yc::engine::Key(t.sheet(), 0, 0)) == 0);
}

TEST_CASE("F9 evaluates part of a formula on the formula's sheet") {
  yctest::Doc t;
  t.Set("B2", "6");
  t.Set("C2", "7");
  std::string err;
  CHECK(t.d.EvaluateFormula(t.sheet(), "B2*C2", &err).AsDouble() == 42);
  CHECK(err.empty());
  t.d.EvaluateFormula(t.sheet(), "B2*", &err);
  CHECK_FALSE(err.empty());  // not a whole expression: "There's a problem with this formula"
}

TEST_CASE("background error checking finds the rule a cell breaks") {
  using namespace yc::engine;
  yctest::Doc t;
  t.Set("A1", "'123");      // a number stored as text
  t.Set("A2", "abc");       // text: nothing
  t.Set("B1", "=1/0");      // an error value
  t.Set("C1", "1");
  t.Set("C2", "2");
  t.Set("C3", "3");
  t.Set("D1", "=C1*2");     // D1:D3: D2 differs from its neighbors, which agree
  t.Set("D2", "=C2*3");
  t.Set("D3", "=C3*2");
  const yc::model::Sheet& s = *t.d.S(t.sheet());
  ErrorCheckOptions o;
  CHECK(ErrorCheckRule(t.d.wb(), s, 0, 0, o) == kErrNumberAsText);
  CHECK(ErrorCheckRule(t.d.wb(), s, 1, 0, o) == kErrNone);
  CHECK(ErrorCheckRule(t.d.wb(), s, 0, 1, o) == kErrEvaluateToError);
  CHECK(ErrorCheckRule(t.d.wb(), s, 1, 3, o) == kErrInconsistentFormula);
  CHECK(ErrorCheckRule(t.d.wb(), s, 0, 3, o) == kErrNone);
  CHECK(ErrorCheckRule(t.d.wb(), s, 0, 2, o) == kErrNone);
  // a rule turned off, or background checking off
  o.number_as_text = false;
  CHECK(ErrorCheckRule(t.d.wb(), s, 0, 0, o) == kErrNone);
  o.number_as_text = true;
  o.background = false;
  CHECK(ErrorCheckRule(t.d.wb(), s, 0, 1, o) == kErrNone);
  o.background = true;
  // Ignore Error: the cell is left alone, and comes back when the ignore is taken off
  t.d.Begin("ignore");
  t.d.SetIgnoreErrors(t.sheet(), Rect::Cell(0, 0), true);
  t.d.Commit();
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(t.sheet()), 0, 0, o) == kErrNone);
  // undone like any edit
  t.d.Undo();
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(t.sheet()), 0, 0, o) == kErrNumberAsText);
}

TEST_CASE("Ignore Error is saved in the .ycg file (sheet key 47)") {
  yctest::Doc t;
  t.Set("A1", "'123");
  t.d.SetIgnoreErrors(t.sheet(), Rect{0, 0, 4, 1}, true);
  t.d.SetIgnoreErrors(t.sheet(), Rect::Cell(2, 0), false);  // one cell taken back out of the block
  const std::string bytes = yc::io::SaveYcgToBytes(t.d.wb(), {});
  auto res = yc::io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  const auto& ig = *res.wb.sheets[0]->ignored_errors;
  CHECK(yc::engine::ErrorIgnored(*res.wb.sheets[0], 0, 0));
  CHECK(yc::engine::ErrorIgnored(*res.wb.sheets[0], 4, 1));
  CHECK_FALSE(yc::engine::ErrorIgnored(*res.wb.sheets[0], 2, 0));
  CHECK(ig.size() >= 2);
}

TEST_CASE("CHAR and CODE use Windows-1252 as Excel does (128 is the euro sign)") {
  yctest::Doc t;
  CHECK(t.Eval("CHAR(128)") == "\xE2\x82\xAC");    // U+20AC, not the control U+0080
  CHECK(t.Eval("CHAR(150)") == "\xE2\x80\x93");    // en dash
  CHECK(t.Eval("CHAR(233)") == "\xC3\xA9");        // e acute: Latin-1 range unchanged
  CHECK(t.Eval("CODE(CHAR(128))") == "128");
  CHECK(t.Eval("CODE(CHAR(153))") == "153");       // trade mark sign
  CHECK(t.Eval("CODE(UNICHAR(8364))") == "128");
  CHECK(t.Eval("CODE(UNICHAR(20013))") == "63");   // outside 1252
  CHECK(t.Eval("CODE(\"A\")") == "65");
}

TEST_CASE("Typed numbers longer than 15 digits are cut to 15, not rounded (Excel's 15-digit input)") {
  auto p = yc::formula::ParseInput("1234567890123456789", true);
  REQUIRE(p.value.IsNumber());
  CHECK(p.value.number() == 1234567890123450000.0);  // rounding gave ...3460000
  p = yc::formula::ParseInput("0.1234567890123456789", true);
  CHECK(p.value.number() == 0.123456789012345);
  p = yc::formula::ParseInput("-9999999999999999", true);
  CHECK(p.value.number() == -999999999999999.0 * 10);  // not -1E+16
}
