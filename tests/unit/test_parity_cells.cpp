// SPDX-License-Identifier: Apache-2.0
// Cells and editing work like Excel: copy / paste sizes, filtered lists, inserted
// rows' formats, Find and Replace wildcards, AutoFill of times, months, quarters and ordinals
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "formula/datetime.h"

#include <cmath>

using yc::core::Rect;
namespace engine = yc::engine;

namespace {

// rows 1:6 = h, 1, 2, 3, 4, 5 with an AutoFilter that hides the row holding 2 (row 3)
void FilteredList(yctest::Doc& t) {
  t.Set("A1", "h");
  for (int i = 1; i <= 5; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string(i));
  REQUIRE(t.d.SetAutoFilter(t.sheet(), Rect{0, 0, 5, 0}));
  yc::model::FilterColumn fc;
  fc.kind = 1;
  fc.values = {"1", "3", "4", "5"};
  fc.blanks = false;
  t.d.SetFilterColumn(t.sheet(), fc);
  REQUIRE(t.d.S(t.sheet())->rows->IsHidden(2));
}

}  // namespace

TEST_CASE("parity cells: a copy keeps the selected size past the used range (M7)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("B2", "2");
  const auto clip = t.d.Copy(sh, Rect{0, 0, 9, 3});  // A1:D10, data only in A1:B2
  CHECK(clip.rows == 10);
  CHECK(clip.cols == 4);
  const uint32_t s2 = t.d.AddSheet("Two");
  for (int r = 0; r < 10; ++r)
    for (int c = 0; c < 4; ++c) t.d.SetInput(s2, r, c, "old");
  t.d.Paste(s2, 0, 0, clip);
  CHECK(t.V("C1", s2).IsEmpty());  // the empty source cells clear the destination
  CHECK(t.V("A6", s2).IsEmpty());
  CHECK(t.N("A1", s2) == 1);
  // whole columns are still cut down to the used range
  const auto whole = t.d.Copy(sh, Rect{0, 0, t.d.S(sh)->row_limit - 1, 0});
  CHECK(whole.rows == 2);
}

TEST_CASE("parity cells: a filtered list's shown rows (H4)") {
  yctest::Doc t;
  FilteredList(t);
  const uint32_t sh = t.sheet();
  const auto runs = engine::FilterShownRuns(*t.d.S(sh), {Rect{1, 0, 5, 0}});
  REQUIRE(runs.size() == 2);
  CHECK(runs[0] == Rect{1, 0, 1, 0});
  CHECK(runs[1] == Rect{3, 0, 5, 0});
  // a copy holds the rows shown, one under the other
  const auto clip = t.d.CopyShown(sh, Rect{1, 0, 5, 0});
  REQUIRE(clip.rows == 4);
  t.d.Paste(sh, 0, 2, clip);
  CHECK(t.N("C1") == 1);
  CHECK(t.N("C2") == 3);
  CHECK(t.N("C3") == 4);
  CHECK(t.N("C4") == 5);
  // without a filter the whole range is one run
  t.d.ClearAutoFilter(sh);
  CHECK(engine::FilterShownRuns(*t.d.S(sh), {Rect{1, 0, 5, 0}}).size() == 1);
}

TEST_CASE("parity cells: inserted rows, columns and cells take their neighbor's formats (M4)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.d.ModifyStyle(sh, Rect{1, 0, 1, 2}, [](yc::model::Style& s) { s.fill_color = 0xFFFFFF00; });  // A2:C2 yellow
  t.d.SetRowHeight(sh, 1, 1, 40);
  t.Set("A2", "x");
  t.d.InsertRows(sh, 2, 1);  // a row under the yellow one takes its format and height, not its value
  const auto* s = t.d.S(sh);
  CHECK(t.d.wb().styles->Get(s->GetStyle(2, 1)).fill_color == 0xFFFFFF00);
  CHECK(s->rows->Size(2) == 40);
  CHECK(t.V("A3").IsEmpty());
  // CopyOrigin = below: from the row under the inserted one
  t.d.InsertRows(sh, 0, 1, 1);  // above row 1 (plain): the new row 1 stays plain
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(0, 0)).fill_color == 0);
  t.d.InsertRows(sh, 2, 1, 1);  // above the yellow row (now row 3): takes it from below
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(2, 0)).fill_color == 0xFFFFFF00);
  t.d.InsertRows(sh, 3, 1, 2);  // plain
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(3, 0)).fill_color == 0);
  // columns: from the left
  yctest::Doc u;
  const uint32_t uh = u.sheet();
  u.d.ModifyStyle(uh, Rect{0, 1, 3, 1}, [](yc::model::Style& x) { x.bold = true; });  // B1:B4 bold
  u.d.InsertCols(uh, 2, 1);
  CHECK(u.d.wb().styles->Get(u.d.S(uh)->GetStyle(3, 2)).bold);
  // cells shifted down: from the cell above, inside the band only
  u.d.InsertCells(uh, Rect{4, 1, 4, 1}, true);
  CHECK(u.d.wb().styles->Get(u.d.S(uh)->GetStyle(4, 1)).bold);
}

TEST_CASE("parity cells: Find and Replace share wildcards, with ~ (M11)") {
  size_t at = 0, len = 0;
  CHECK(engine::WildFind("a*c", "xxabcxx", 0, false, &at, &len));
  CHECK(at == 2);
  CHECK(len == 3);
  CHECK(engine::WildFind("a~*c", "1a*c", 0, false, &at, &len));
  CHECK(at == 1);
  CHECK_FALSE(engine::WildFind("a~*c", "abc", 0, false, &at, &len));
  CHECK(engine::WildFind("a?c", "abc", 0, true, &at, &len));
  CHECK_FALSE(engine::WildFind("a?c", "abcd", 0, true, &at, &len));
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B1", "abc");
  t.Set("B2", "a*c");
  t.Set("B3", "xaxxcx");
  // Find stops on B1 for a*c; Replace now changes the same cells
  CHECK(t.d.Replace(sh, Rect{0, 0, 10, 5}, "a~*c", "Y", false, false) == 1);
  CHECK(t.V("B2").str() == "Y");
  CHECK(t.V("B1").str() == "abc");
  CHECK(t.d.Replace(sh, Rect{0, 0, 10, 5}, "a*c", "X", false, false) == 2);
  CHECK(t.V("B1").str() == "X");
  CHECK(t.V("B3").str() == "xXx");
  CHECK(t.d.Replace(sh, Rect{0, 0, 10, 5}, "zzz", "Q", false, false) == 0);
  uint32_t r = 0, c = 0;
  t.Set("C5", "50*");
  CHECK(t.d.Find(sh, Rect{0, 0, 10, 5}, "~*", 0, 0, true, false, false, false, &r, &c));
  CHECK((r == 4 && c == 2));
}

TEST_CASE("parity cells: AutoFill of times, months, quarters and ordinals (M12)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  // a time steps by an hour
  t.Set("A1", "9:00");
  REQUIRE(engine::IsTimeOnlyFormat(t.d.wb().styles->NumberFormat(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(0, 0)))));
  t.d.AutoFill(sh, Rect{0, 0, 0, 0}, Rect{0, 0, 2, 0});
  CHECK(std::fabs(t.N("A2") - 10.0 / 24) < 1e-9);
  CHECK(std::fabs(t.N("A3") - 11.0 / 24) < 1e-9);
  CHECK_FALSE(engine::IsTimeOnlyFormat("yyyy-mm-dd h:mm"));
  CHECK(engine::IsTimeOnlyFormat("[h]:mm:ss"));
  // two dates a month apart continue by months; a year apart by years
  t.Set("B1", "2026-01-01");
  t.Set("B2", "2026-02-01");
  t.d.AutoFill(sh, Rect{0, 1, 1, 1}, Rect{0, 1, 3, 1});
  CHECK(t.N("B4") == yc::formula::DateSerial(2026, 4, 1));
  t.Set("C1", "2024-03-15");
  t.Set("C2", "2025-03-15");
  t.d.AutoFill(sh, Rect{0, 2, 1, 2}, Rect{0, 2, 3, 2});
  CHECK(t.N("C4") == yc::formula::DateSerial(2027, 3, 15));
  // quarters cycle; ordinals continue
  t.Set("D1", "Q3");
  t.d.AutoFill(sh, Rect{0, 3, 0, 3}, Rect{0, 3, 3, 3});
  CHECK(t.V("D2").str() == "Q4");
  CHECK(t.V("D3").str() == "Q1");
  CHECK(t.V("D4").str() == "Q2");
  t.Set("E1", "Qtr 4");
  t.d.AutoFill(sh, Rect{0, 4, 0, 4}, Rect{0, 4, 1, 4});
  CHECK(t.V("E2").str() == "Qtr 1");
  t.Set("F1", "1st Period");
  t.d.AutoFill(sh, Rect{0, 5, 0, 5}, Rect{0, 5, 12, 5});
  CHECK(t.V("F2").str() == "2nd Period");
  CHECK(t.V("F3").str() == "3rd Period");
  CHECK(t.V("F4").str() == "4th Period");
  CHECK(t.V("F11").str() == "11th Period");
  CHECK(engine::FillTextSeries("Item1"));
  CHECK(engine::FillTextSeries("Mon"));
  CHECK_FALSE(engine::FillTextSeries("apple"));
}

TEST_CASE("parity cells: Paste Special > Formulas and number formats (L11)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "3");
  t.Set("B1", "=A1*2");
  const uint32_t pct = t.d.wb_mut().styles.mut().InternString("0.00%");
  t.d.ModifyStyle(sh, Rect{0, 1, 0, 1}, [pct](yc::model::Style& s) {
    s.fill_color = 0xFFFFFF00;
    s.numfmt = pct;
  });
  const auto clip = t.d.Copy(sh, Rect{0, 1, 0, 1});
  t.d.Paste(sh, 1, 1, clip, engine::PasteMode::FormulasAndFormats);  // into B2
  t.d.Recalc();
  CHECK(t.F("B2") == "=A2*2");
  const auto& st = t.d.wb().styles->Get(t.d.S(sh)->GetStyle(1, 1));
  CHECK(t.d.wb().styles->NumberFormat(st) == "0.00%");
  CHECK(st.fill_color == 0);
}

TEST_CASE("parity cells: Find and Replace skip a text's apostrophe and find nothing for a lone ~ (measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "'123");
  t.Set("A2", "~");
  t.Set("A3", "a~b");
  t.Set("A4", "'abc");
  // Excel 16: '123 searched as 123 and kept text (each ? is one of its three characters)
  CHECK(t.d.Replace(sh, Rect{0, 0, 0, 0}, "?", "~", false, false) == 1);
  CHECK(t.V("A1").IsString());
  CHECK(std::string(t.V("A1").str()) == "~~~");
  t.Set("A1", "'123");
  CHECK(t.d.Replace(sh, Rect{0, 0, 0, 0}, "1", "4", false, false) == 1);
  REQUIRE(t.V("A1").IsString());  // '423, still text
  CHECK(std::string(t.V("A1").str()) == "423");
  CHECK(t.d.Replace(sh, Rect{0, 0, 0, 0}, "'", "q", false, false) == 0);  // the apostrophe isn't searched
  // a lone ~ matches nothing; ~~ is a tilde
  uint32_t r = 0, c = 0;
  CHECK_FALSE(t.d.Find(sh, Rect{0, 0, 3, 0}, "~", 0, 0, true, false, false, true, &r, &c));
  CHECK(t.d.Replace(sh, Rect{0, 0, 3, 0}, "~", "?", false, false) == 0);
  CHECK(std::string(t.V("A2").str()) == "~");
  CHECK(t.d.Replace(sh, Rect{0, 0, 3, 0}, "~~", "?", false, false) == 2);
  CHECK(std::string(t.V("A2").str()) == "?");
  CHECK(std::string(t.V("A3").str()) == "a?b");
  CHECK(t.d.Find(sh, Rect{0, 0, 3, 0}, "abc", 0, 0, true, false, true, true, &r, &c));
  CHECK(r == 3);
}
