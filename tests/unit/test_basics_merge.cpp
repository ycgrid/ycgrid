// SPDX-License-Identifier: Apache-2.0
// Basic operations measured in Excel 16 (tests/data/excel_basics_measured.json and probes of the same Excel): what
// merging does with the values and formats of its cells, and typed month / year input (1/0 is January 2000).
#include <catch2/catch_test_macros.hpp>

#include "formula/numfmt.h"
#include "test_helpers.h"

#include <cmath>
#include <string>

using yc::core::Rect;
namespace model = yc::model;
namespace formula = yc::formula;

namespace {
Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}
const model::Style& StyleAt(yctest::Doc& t, const char* a1) {
  const auto p = yctest::Doc::P(a1);
  return t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(p.row, p.col));
}
std::string Fmt(yctest::Doc& t, const char* a1) { return t.d.wb().styles->NumberFormat(StyleAt(t, a1)); }
void SetFmt(yctest::Doc& t, const char* range, const char* code) {
  const uint32_t nf = t.d.wb_mut().styles.mut().InternString(code);
  t.d.ModifyStyle(t.sheet(), R(range), [nf](model::Style& s) { s.numfmt = nf; });
}
// the "merge" layout of scripts/excel_basics_cases.py
void MergeLayout(yctest::Doc& t) {
  t.Set("A1", "x");
  t.Set("B1", "5");
  SetFmt(t, "B1", "0.00");
  t.Set("C1", "z");
  t.Set("B2", "y");
  t.Set("C2", "7");
  SetFmt(t, "C2", "0%");
  t.Set("A3", "=B1*2");
  t.Set("D1", "=B1");
  t.Set("D2", "=SUM(A1:C2)");
}
}  // namespace

TEST_CASE("Basics merge: every cell of the merge takes the upper-left cell's formats (Excel 16, measured)") {
  yctest::Doc t;
  MergeLayout(t);
  REQUIRE(Fmt(t, "B1") == "0.00");
  REQUIRE(t.d.Merge(t.sheet(), R("A1:B2")));
  t.d.Recalc();
  CHECK(t.V("A1").str() == "x");
  CHECK(t.V("B1").IsEmpty());
  CHECK(t.V("B2").IsEmpty());
  for (const char* a : {"A1", "B1", "A2", "B2"}) CHECK(Fmt(t, a) == "General");
  CHECK(Fmt(t, "C2") == "0%");   // outside the merge
  CHECK(Fmt(t, "D1") == "0.00");  // =B1 took B1's format when it was entered
  CHECK(t.N("D2") == 7);
  // the formats stay after an UnMerge, a Clear and in a copy (merge_unmerge_A1B2, merge_clear_A1B2)
  t.d.Unmerge(t.sheet(), R("A1"));
  CHECK(Fmt(t, "B1") == "General");
}

TEST_CASE("Basics merge: the formats come with the font, the fill and the alignment, not only the number format") {
  yctest::Doc t;
  t.Set("A1", "x");
  t.Set("B1", "5");
  t.d.ModifyStyle(t.sheet(), R("A1"), [](model::Style& s) {
    s.bold = true;
    s.fill_color = 0xFFFFFF00;
  });
  t.d.ModifyStyle(t.sheet(), R("B1"), [](model::Style& s) {
    s.italic = true;
    s.fill_color = 0xFFFF0000;
    s.halign = model::HAlign::Right;
  });
  t.d.ModifyStyle(t.sheet(), R("B2"), [](model::Style& s) { s.wrap = true; });
  REQUIRE(t.d.Merge(t.sheet(), R("A1:B2")));
  for (const char* a : {"A1", "B1", "A2", "B2"}) {
    INFO(a);
    const model::Style& st = StyleAt(t, a);
    CHECK(st.bold);
    CHECK_FALSE(st.italic);
    CHECK(st.fill_color == 0xFFFFFF00u);
    CHECK(st.halign == model::HAlign::General);
    CHECK_FALSE(st.wrap);
  }
}

TEST_CASE("Basics merge: an empty upper-left cell takes the first value, with that cell's formats") {
  yctest::Doc t;
  MergeLayout(t);
  // merge_A2C2: A2 is empty, B2 and C2 hold y and 7: Excel keeps y (the first in reading order) in A2
  REQUIRE(t.d.Merge(t.sheet(), R("A2:C2")));
  t.d.Recalc();
  REQUIRE(t.V("A2").IsString());
  CHECK(t.V("A2").str() == "y");
  CHECK(t.V("B2").IsEmpty());
  CHECK(t.V("C2").IsEmpty());
  for (const char* a : {"A2", "B2", "C2"}) CHECK(Fmt(t, a) == "General");
  CHECK(t.N("D2") == 5);

  // the value cell's formats go to every cell (A5 was 0% and bold; B5 holds 3 in 0.00, italic)
  t.Set("B5", "3");
  SetFmt(t, "A5", "0%");
  SetFmt(t, "B5", "0.00");
  t.d.ModifyStyle(t.sheet(), R("A5"), [](model::Style& s) { s.bold = true; });
  t.d.ModifyStyle(t.sheet(), R("B5"), [](model::Style& s) { s.italic = true; });
  t.Set("C5", "z");
  REQUIRE(t.d.Merge(t.sheet(), R("A5:C5")));
  CHECK(t.N("A5") == 3);
  CHECK(t.V("C5").IsEmpty());
  for (const char* a : {"A5", "B5", "C5"}) {
    INFO(a);
    CHECK(Fmt(t, a) == "0.00");
    CHECK(StyleAt(t, a).italic);
    CHECK_FALSE(StyleAt(t, a).bold);
  }

  // a formula is the first value even in a later row, before a value further on (A7:B9: B8 then A9)
  t.Set("B8", "=1+2");
  t.Set("A9", "q");
  REQUIRE(t.d.Merge(t.sheet(), R("A7:B9")));
  t.d.Recalc();
  CHECK(t.F("A7") == "=1+2");
  CHECK(t.N("A7") == 3);
  CHECK(t.V("B8").IsEmpty());
  CHECK(t.V("A9").IsEmpty());
}

TEST_CASE("Basics merge: Merge Across does the same row by row") {
  yctest::Doc t;
  MergeLayout(t);
  // merge_A1C2_across: row 1 keeps x, row 2 keeps B2's y in A2; each row takes its value cell's formats
  REQUIRE(t.d.Merge(t.sheet(), R("A1:C2"), true));
  t.d.Recalc();
  CHECK(t.V("A1").str() == "x");
  CHECK(t.V("A2").str() == "y");
  for (const char* a : {"B1", "C1", "B2", "C2"}) CHECK(t.V(a).IsEmpty());
  for (const char* a : {"A1", "B1", "C1", "A2", "B2", "C2"}) CHECK(Fmt(t, a) == "General");
  CHECK(t.N("D2") == 0);

  t.Set("A11", "1");
  SetFmt(t, "A11", "0.00");
  t.Set("B11", "b");
  SetFmt(t, "A12", "0%");
  t.Set("B12", "2");
  SetFmt(t, "B12", "#,##0");
  REQUIRE(t.d.Merge(t.sheet(), R("A11:C13"), true));
  CHECK(Fmt(t, "C11") == "0.00");
  CHECK(t.N("A12") == 2);
  CHECK(Fmt(t, "A12") == "#,##0");
  CHECK(Fmt(t, "C12") == "#,##0");
  CHECK(Fmt(t, "B13") == "General");
}

TEST_CASE("Basics merge: a merged area read from a file keeps its cells as saved") {
  yctest::Doc t;
  t.Set("B1", "kept");
  SetFmt(t, "B1", "0.00");
  t.d.AddMergedArea(t.sheet(), R("A1:B1"));
  REQUIRE(t.d.S(t.sheet())->MergeAt(0, 1));
  CHECK(t.V("A1").IsEmpty());
  CHECK(t.V("B1").str() == "kept");
  CHECK(Fmt(t, "B1") == "0.00");
}

namespace {
// the edges of a cell that have a line, in the order L T R B, D (diagonal down) and U (diagonal up)
std::string Lines(yctest::Doc& t, const char* a1) {
  const model::Style& st = StyleAt(t, a1);
  std::string out;
  for (int e = 0; e < model::kEdgeCount; ++e) {
    if (st.border[e].style != model::LineStyle::None) out += "LTRBDU"[e];
  }
  return out;
}
void Line(yctest::Doc& t, const char* a1, int edge, model::LineStyle ls = model::LineStyle::Thin, uint32_t color = 0) {
  t.d.ModifyStyle(t.sheet(), R(a1), [&](model::Style& s) { s.border[edge] = model::BorderSide{ls, color}; });
}
void AllBorders(yctest::Doc& t, const char* range, model::LineStyle ls = model::LineStyle::Thin, uint32_t color = 0) {
  t.d.SetBorders(t.sheet(), R(range), yc::engine::BorderPreset::All, ls, color);
}
}  // namespace

// Borders under a merge, measured in Excel 16 (Range.Merge / Merge(True) / MergeCells = True, then each cell's
// Borders(edge) and again after UnMerge): a side of the area keeps its line only when every cell along it shows the
// same one, the edges inside are cleared, and the diagonals stay only when every cell has the same pair.
TEST_CASE("Basics merge: All Borders then Merge leaves the outline, and UnMerge keeps it (Excel 16, measured)") {
  for (const char* value_at : {"B2", "C3", ""}) {  // the value cell doesn't matter
    INFO(value_at);
    yctest::Doc t;
    if (*value_at) t.Set(value_at, "v");
    AllBorders(t, "B2:C3");
    REQUIRE(t.d.Merge(t.sheet(), R("B2:C3")));
    CHECK(Lines(t, "B2") == "LT");
    CHECK(Lines(t, "C2") == "TR");
    CHECK(Lines(t, "B3") == "LB");
    CHECK(Lines(t, "C3") == "RB");
    t.d.Unmerge(t.sheet(), R("B2"));
    CHECK(Lines(t, "B2") == "LT");
    CHECK(Lines(t, "C3") == "RB");
  }
  // 3x3 in medium red: the outline keeps its weight and color, the middle cell has none
  yctest::Doc t;
  t.Set("C3", "v");
  AllBorders(t, "B2:D4", model::LineStyle::Medium, 0xFFFF0000);
  REQUIRE(t.d.Merge(t.sheet(), R("B2:D4")));
  CHECK(Lines(t, "C3").empty());
  CHECK(Lines(t, "C2") == "T");
  CHECK(Lines(t, "D3") == "R");
  CHECK(Lines(t, "D4") == "RB");
  CHECK(StyleAt(t, "D4").border[model::kBottom] == model::BorderSide{model::LineStyle::Medium, 0xFFFF0000});
}

TEST_CASE("Basics merge: a side whose cells differ loses its line (Excel 16, measured)") {
  yctest::Doc t;
  // a box around the upper-left cell only: every side is partly drawn, so nothing is left
  t.Set("B2", "v");
  for (int e : {model::kLeft, model::kTop, model::kRight, model::kBottom}) Line(t, "B2", e);
  REQUIRE(t.d.Merge(t.sheet(), R("B2:C3")));
  for (const char* a : {"B2", "C2", "B3", "C3"}) CHECK(Lines(t, a).empty());
  // the right and bottom edges of the second cell of B5:C5: the right side is all of it and stays, the bottom isn't
  t.Set("B5", "q");
  Line(t, "C5", model::kRight);
  Line(t, "C5", model::kBottom);
  REQUIRE(t.d.Merge(t.sheet(), R("B5:C5")));
  CHECK(Lines(t, "B5").empty());
  CHECK(Lines(t, "C5") == "R");
  // the same style in another color or weight differs too
  Line(t, "B7", model::kTop, model::LineStyle::Thin, 0xFFFF0000);
  Line(t, "C7", model::kTop, model::LineStyle::Thin, 0xFF0000FF);
  Line(t, "B7", model::kLeft, model::LineStyle::Medium);
  Line(t, "B8", model::kLeft, model::LineStyle::Medium);
  Line(t, "C7", model::kRight, model::LineStyle::Medium);
  Line(t, "C8", model::kRight, model::LineStyle::Thin);
  REQUIRE(t.d.Merge(t.sheet(), R("B7:C8")));
  CHECK(Lines(t, "B7") == "L");
  CHECK(Lines(t, "C7").empty());
  CHECK(Lines(t, "B8") == "L");
  CHECK(Lines(t, "C8").empty());
  // edges between the merged cells go
  Line(t, "B10", model::kRight);
  Line(t, "B10", model::kBottom);
  Line(t, "C10", model::kBottom);
  Line(t, "B11", model::kRight);
  REQUIRE(t.d.Merge(t.sheet(), R("B10:C11")));
  for (const char* a : {"B10", "C10", "B11", "C11"}) CHECK(Lines(t, a).empty());
  // 1x3: top on all three stays, the bottom of the middle one goes, the two ends stay
  Line(t, "B13", model::kTop);
  Line(t, "C13", model::kTop);
  Line(t, "D13", model::kTop);
  Line(t, "C13", model::kBottom);
  Line(t, "B13", model::kLeft);
  Line(t, "D13", model::kRight);
  REQUIRE(t.d.Merge(t.sheet(), R("B13:D13")));
  CHECK(Lines(t, "B13") == "LT");
  CHECK(Lines(t, "C13") == "T");
  CHECK(Lines(t, "D13") == "TR");
}

TEST_CASE("Basics merge: the facing edge of the cell outside counts, and the cells outside keep theirs") {
  yctest::Doc t;
  // B2's own top and C1's bottom: the top is drawn all along, so B2 keeps it (Excel 16, measured)
  t.Set("B2", "v");
  Line(t, "B2", model::kTop);
  Line(t, "C1", model::kBottom);
  REQUIRE(t.d.Merge(t.sheet(), R("B2:C2")));
  CHECK(Lines(t, "B2") == "T");
  CHECK(Lines(t, "C1") == "B");
  // A5's right edge faces half of the left side: the side loses its line, A5 keeps its own
  Line(t, "A5", model::kRight);
  Line(t, "B5", model::kTop);
  REQUIRE(t.d.Merge(t.sheet(), R("B5:C6")));
  CHECK(Lines(t, "A5") == "R");
  CHECK(Lines(t, "B5").empty());
  CHECK(Lines(t, "B6").empty());
}

TEST_CASE("Basics merge: diagonals stay only when every cell has the same pair (Excel 16, measured)") {
  yctest::Doc t;
  for (const char* a : {"B2", "C2", "B3", "C3"}) {
    Line(t, a, model::kDiagDown);
    Line(t, a, model::kDiagUp);
  }
  REQUIRE(t.d.Merge(t.sheet(), R("B2:C3")));
  for (const char* a : {"B2", "C2", "B3", "C3"}) CHECK(Lines(t, a) == "DU");
  // the upper-left cell's alone goes
  Line(t, "B5", model::kDiagDown);
  REQUIRE(t.d.Merge(t.sheet(), R("B5:C6")));
  CHECK(Lines(t, "B5").empty());
  // up on both, down on one: both go
  Line(t, "B8", model::kDiagUp);
  Line(t, "C8", model::kDiagUp);
  Line(t, "C8", model::kDiagDown);
  REQUIRE(t.d.Merge(t.sheet(), R("B8:C8")));
  CHECK(Lines(t, "B8").empty());
  CHECK(Lines(t, "C8").empty());
  // down on both in two colors: goes
  Line(t, "B10", model::kDiagDown);
  Line(t, "C10", model::kDiagDown, model::LineStyle::Thin, 0xFFFF0000);
  REQUIRE(t.d.Merge(t.sheet(), R("B10:C10")));
  CHECK(Lines(t, "B10").empty());
  CHECK(Lines(t, "C10").empty());
}

TEST_CASE("Basics merge: Merge Across and MergeCells = True follow the same rule (Excel 16, measured)") {
  yctest::Doc t;
  // across: each row is a merged area of its own, with its own outline
  t.Set("B2", "v");
  AllBorders(t, "B2:D3");
  REQUIRE(t.d.Merge(t.sheet(), R("B2:D3"), true));
  CHECK(Lines(t, "B2") == "LTB");
  CHECK(Lines(t, "C2") == "TB");
  CHECK(Lines(t, "D3") == "TRB");
  Line(t, "B5", model::kTop);
  Line(t, "C5", model::kTop);
  Line(t, "D5", model::kTop);
  Line(t, "B6", model::kBottom);
  REQUIRE(t.d.Merge(t.sheet(), R("B5:D6"), true));
  CHECK(Lines(t, "C5") == "T");
  CHECK(Lines(t, "B6").empty());
  // MergeCells = True: the outline of All Borders stays; a box around the upper-left cell goes
  t.Set("B8", "v");
  AllBorders(t, "B8:C9");
  REQUIRE(t.d.Merge(t.sheet(), R("B8:C9"), false, false, true));
  CHECK(Lines(t, "B8") == "LT");
  CHECK(Lines(t, "C8") == "TR");
  CHECK(Lines(t, "C9") == "RB");
  t.Set("B11", "v");
  for (int e : {model::kLeft, model::kTop, model::kRight, model::kBottom}) Line(t, "B11", e);
  t.d.ModifyStyle(t.sheet(), R("B11"), [](model::Style& s) { s.bold = true; });
  REQUIRE(t.d.Merge(t.sheet(), R("B11:C12"), false, false, true));
  CHECK(Lines(t, "B11").empty());
  CHECK(StyleAt(t, "B11").bold);  // the upper-left cell keeps its other formats
  CHECK(Lines(t, "C12").empty());
}

TEST_CASE("Basics input: a number that can't be a day of the month makes a month and year (Excel 16, measured)") {
  struct Case {
    const char* text;
    double serial;
  };
  // typed in Excel 16: Value2 and NumberFormat mmm-yy (Jan-00, Dec-99, Feb-30, Jan-32, Apr-31, Dec-24)
  const Case dates[] = {{"1/0", 36526},    {"1/00", 36526},     {"01/0", 36526}, {"1-0", 36526},  {"12/99", 36495},
                        {"12-99", 36495},  {"12/0", 36861},     {"2/30", 10990}, {"2/31", 11355}, {"1/32", 11689},
                        {"1-32", 11689},   {"4/31", 11414},     {"1/50", 18264}, {"6/45", 16589}, {"12/2024", 45627},
                        {"2/1929", 10625}, {"1/1900", 1},       {"1/9999", 2958101}};
  for (const Case& c : dates) {
    INFO(c.text);
    const formula::InputParse p = formula::ParseInput(c.text);
    REQUIRE(p.value.IsNumber());
    CHECK(p.value.AsDouble() == c.serial);
    CHECK(p.format == "mmm-yy");
  }
  // with a time: the date and time (Excel's code is the region's short date with h:mm)
  const formula::InputParse t = formula::ParseInput("1/0 9:30");
  REQUIRE(t.value.IsNumber());
  CHECK(std::abs(t.value.AsDouble() - (36526 + 9.5 / 24)) < 1e-9);
  // text in Excel: no such month, a three- or five-digit year, a year before 1900, three parts with a zero
  for (const char* text : {"13/5", "0/5", "0/0", "13/2024", "1/100", "1/032", "1/1000", "1/1899", "1/10000", "1/0/0",
                           "31/12", "32/1", "2024/1", "2024/13"}) {
    INFO(text);
    CHECK(formula::ParseInput(text).value.IsString());
  }
  // a day of the month stays month / day of this year
  const formula::InputParse md = formula::ParseInput("1/29");
  REQUIRE(md.value.IsNumber());
  CHECK(md.format != "mmm-yy");
  // typed into a cell: shown as Excel's Jan-00
  yctest::Doc d;
  d.Set("A1", "1/0");
  CHECK(d.N("A1") == 36526);
  CHECK(Fmt(d, "A1") == "mmm-yy");
  CHECK(d.T("A1") == "Jan-00");
}
