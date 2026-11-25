// SPDX-License-Identifier: Apache-2.0
// Rows and columns on the whole sheet: hiding, unhiding and resizing every row must stay fast (Ctrl+9 then Ctrl+Shift+9
// with whole columns selected froze YCGrid for minutes: each unhidden row erased the front of a sorted array), and
// give the same rows back.
#include <catch2/catch_test_macros.hpp>

#include "engine/navigate.h"
#include "engine/page_layout.h"
#include "engine/pivot_engine.h"
#include "io/ycg.h"
#include "model/dim_index.h"
#include "test_helpers.h"

#include <chrono>
#include <set>

#include <windows.h>
#include <psapi.h>

using yc::model::DimIndex;

namespace {

double Seconds(const std::chrono::steady_clock::time_point& t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// The limits guard against the freezes these tests were written for (minutes); a debug build runs the same work a
// few times slower, and under a debugger or the coverage runner (OpenCppCoverage debugs the tests) time says nothing.
double Limit(double release_seconds) {
  if (IsDebuggerPresent()) return 1e9;
#ifdef NDEBUG
  return release_seconds;
#else
  return release_seconds * 3;
#endif
}

}  // namespace

TEST_CASE("Rows: hiding and unhiding all 1,048,576 rows is fast and gives the rows back") {
  constexpr int64_t kRows = 1048576;
  DimIndex d(20);
  d.Set(5, 40);  // a custom height survives hiding and unhiding
  const auto t0 = std::chrono::steady_clock::now();
  for (int64_t r = 0; r < kRows; ++r) d.Hide(r, true);
  CHECK(d.Offset(kRows) == 0);
  for (int64_t r = 0; r < kRows; ++r) d.Hide(r, false);
  CHECK(Seconds(t0) < Limit(3.0));  // was minutes
  CHECK(d.Size(5) == 40);
  CHECK(d.Size(6) == 20);
  CHECK(d.Offset(kRows) == kRows * 20 + 20);
  CHECK(d.Entries().size() == 1);
  CHECK(d.custom_count() == 1);
  // every row a height, then back to the default
  const auto t1 = std::chrono::steady_clock::now();
  for (int64_t r = 0; r < kRows; ++r) d.Set(r, 30);
  for (int64_t r = 0; r < kRows; ++r) d.Set(r, 20);
  CHECK(Seconds(t1) < Limit(3.0));
  CHECK(d.Entries().empty());
  CHECK(d.Offset(100) == 2000);
  CHECK(d.IndexAt(2010) == 100);
}

TEST_CASE("Rows: entries set back to the default don't count, and survive a new default size and deleted rows") {
  DimIndex d(20);
  for (int64_t r = 0; r < 10; ++r) d.Set(r, 25);
  for (int64_t r = 0; r < 5; ++r) d.Set(r, 20);  // back to the default: stale until compacted
  CHECK(d.custom_count() == 5);
  CHECK_FALSE(d.IsCustom(2));
  CHECK(d.IsCustom(7));
  d.Hide(2, true);  // a stale entry hidden again
  CHECK(d.IsHidden(2));
  d.Hide(2, false);
  CHECK(d.Size(2) == 20);
  d.Remove(0, 3);  // rows 1-3 deleted: 5..9 (25) are now 2..6
  CHECK(d.Size(2) == 25);
  CHECK(d.Size(1) == 20);
  d.set_default_size(18);
  CHECK(d.Size(0) == 18);
  CHECK(d.Size(4) == 25);
  CHECK(d.Entries().size() == 5);
}

TEST_CASE("Rows: the document hides and unhides every row of the sheet at once, with Undo") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("hide");
  t.d.HideRows(sh, 0, last, true);
  t.d.Commit();
  CHECK(t.d.S(sh)->rows->IsHidden(500000));
  t.d.Begin("unhide");
  t.d.HideRows(sh, 0, last, false);
  t.d.Commit();
  CHECK_FALSE(t.d.S(sh)->rows->IsHidden(500000));
  REQUIRE(t.d.Undo());
  CHECK(t.d.S(sh)->rows->IsHidden(500000));
  REQUIRE(t.d.Undo());
  CHECK_FALSE(t.d.S(sh)->rows->IsHidden(0));
  CHECK(Seconds(t0) < Limit(5.0));
}

TEST_CASE("Rows: hiding to the end marker or past the sheet stops at its last row (Rows.Hidden = False froze)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  const auto t0 = std::chrono::steady_clock::now();
  t.d.HideRows(sh, 0, 0xFFFFFFFFu, true);  // r <= 0xFFFFFFFF never ended
  CHECK(t.d.S(sh)->rows->IsHidden(t.d.S(sh)->row_limit - 1));
  t.d.HideRows(sh, 0, 0x7FFFFFFEu, false);  // 2^31 rows took seconds
  CHECK_FALSE(t.d.S(sh)->rows->IsHidden(7));
  t.d.HideCols(sh, 0, 0xFFFFFFFFu, true);
  CHECK(t.d.S(sh)->cols->IsHidden(16383));
  t.d.HideCols(sh, 0, 0xFFFFFFFFu, false);
  t.d.SetRowHeight(sh, 0, 0xFFFFFFFFu, 30);
  t.d.SetColWidth(sh, 0, 0xFFFFFFFFu, 90);
  CHECK(t.d.S(sh)->rows->Size(5) == 30);
  CHECK(t.d.S(sh)->cols->Size(5) == 90);
  CHECK(Seconds(t0) < Limit(5.0));
}

TEST_CASE("Fill: Ctrl+D down a whole column or the whole sheet stops at the sheet's end and takes no time") {
  // a paste per row took minutes for a whole column; the whole sheet ran past the sheet's end and out of memory
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("C1", "5");
  t.Set("D1", "=C1*2");
  t.Set("E5", "=1+1");
  t.d.ModifyStyle(sh, Rect::Cell(0, 0), [](yc::model::Style& s) { s.bold = true; });
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("fill");
  t.d.FillDown(sh, Rect{0, 2, 0xFFFFFFFFu, 3});  // C:D from Python: the end marker
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.d.S(sh)->row_limit - 1 == last);
  CHECK(t.d.GetValue(sh, last, 2).AsDouble() == 5);
  CHECK(t.d.GetFormulaA1(sh, 500000, 3) == "=C500001*2");
  CHECK(t.d.GetValue(sh, 500000, 3).AsDouble() == 10);
  REQUIRE(t.d.Undo());
  CHECK(t.d.GetValue(sh, 500000, 2).IsEmpty());
  CHECK(t.d.GetFormulaA1(sh, 500000, 3).empty());
  CHECK(t.d.GetValue(sh, 0, 2).AsDouble() == 5);
  // the whole sheet: row 1 into every row (E5's formula goes, A keeps its bold)
  t.d.Begin("fill all");
  t.d.FillDown(sh, Rect{0, 0, 0xFFFFFFFFu, yc::core::kMaxCols - 1});
  t.d.Commit();
  CHECK(t.d.S(sh)->row_limit - 1 == last);
  CHECK(t.d.S(sh)->used.r1 == last);
  CHECK(t.d.GetFormulaA1(sh, 4, 4).empty());
  CHECK(t.d.GetValue(sh, 4, 4).IsEmpty());
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(700000, 0)).bold);
  CHECK_FALSE(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(700000, 1)).bold);
  CHECK(t.d.GetValue(sh, 700000, 2).AsDouble() == 5);
  REQUIRE(t.d.Undo());
  CHECK(t.d.GetFormulaA1(sh, 4, 4) == "=1+1");
  CHECK_FALSE(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(700000, 0)).bold);
  // every column in italics (column styles) and a formula in the last cell: no cell is written for its style
  t.d.ModifyStyle(sh, Rect{0, 0, last, yc::core::kMaxCols - 1}, [](yc::model::Style& s) { s.italic = true; });
  t.Set("XFD1048576", "=E9*2");
  const auto t1 = std::chrono::steady_clock::now();
  t.d.Begin("fill italics");
  t.d.FillDown(sh, Rect{0, 0, last, yc::core::kMaxCols - 1});
  t.d.Commit();
  CHECK(Seconds(t1) < Limit(5.0));
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(900000, 9000)).italic);
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(900000, 0)).bold);
  CHECK(t.d.GetFormulaA1(sh, last, 16383).empty());
  CHECK(Seconds(t0) < Limit(25.0));
}

TEST_CASE("Fill: a large fill gives what a paste per row gives, up and left too") {
  using yc::core::Rect;
  auto build = [](yctest::Doc& t) {
    const uint32_t sh = t.sheet();
    t.Set("A1", "7");
    t.Set("B1", "=A1+1");
    t.Set("C1", "x");
    t.Set("B5", "old");
    t.Set("A200000", "9");
    t.Set("B200000", "=$A$1*A200000");
    t.d.ModifyStyle(sh, Rect::Cell(0, 2), [](yc::model::Style& s) { s.italic = true; });
    t.d.ModifyStyle(sh, Rect::Cell(9, 1), [](yc::model::Style& s) { s.bold = true; });
  };
  yctest::Doc t;
  build(t);
  const uint32_t sh = t.sheet();
  t.d.FillEdge(sh, Rect{0, 0, 199999, 3}, 0);  // 800,000 cells: the bulk way
  t.d.Recalc();
  CHECK(t.d.GetValue(sh, 150000, 0).AsDouble() == 7);
  CHECK(t.d.GetFormulaA1(sh, 4, 1) == "=A5+1");
  CHECK(t.d.GetValue(sh, 4, 1).AsDouble() == 8);
  CHECK(t.d.GetInputText(sh, 199999, 2) == "x");
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(123456, 2)).italic);
  CHECK_FALSE(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(9, 1)).bold);  // B10's own bold went: B1 has none
  CHECK(t.d.GetValue(sh, 199999, 3).IsEmpty());
  // up: the bottom row into the rows above it; left: the right column into the columns before it
  yctest::Doc u;
  build(u);
  u.d.FillEdge(sh, Rect{0, 0, 199999, 1}, 2);
  u.d.Recalc();
  CHECK(u.d.GetValue(sh, 0, 0).AsDouble() == 9);
  CHECK(u.d.GetFormulaA1(sh, 0, 1) == "=$A$1*A1");
  CHECK(u.d.GetValue(sh, 0, 1).AsDouble() == 81);
  yctest::Doc w;
  w.Set("C1", "q");
  w.Set("C9", "z");
  w.d.FillEdge(w.sheet(), Rect{0, 0, 0xFFFFFFFFu, 2}, 3);  // whole columns A:C, filled left from C
  CHECK(w.d.GetInputText(w.sheet(), 8, 0) == "z");
  CHECK(w.d.GetInputText(w.sheet(), 0, 1) == "q");
  CHECK(w.d.S(w.sheet())->row_limit == 1048576);
}

TEST_CASE("Fill: Ctrl+D down a whole column under conditional formatting grows the rule once") {
  // a paste per row grew the rule a row at a time: a million ranges, each paste comparing them all
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("C1", "5");
  yc::model::CfRule cf;
  cf.type = yc::model::CfType::Expression;
  cf.formula1 = "=C1>3";
  cf.ranges = {Rect{0, 2, 9, 2}};
  t.d.AddCondFormat(sh, cf);
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("fill");
  t.d.FillDown(sh, Rect{0, 2, 0xFFFFFFFFu, 2});
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));
  CHECK(t.d.GetValue(sh, last, 2).AsDouble() == 5);
  const auto& rules = *t.d.S(sh)->cond_formats;
  REQUIRE(rules.size() == 1);
  CHECK(rules.front().formula1 == "=C1>3");
  uint64_t covered = 0;
  for (const Rect& r : rules.front().ranges) covered += r.Count();
  CHECK(covered >= last + 1);
  REQUIRE(t.d.Undo());
  REQUIRE(t.d.S(sh)->cond_formats->size() == 1);
  CHECK(t.d.S(sh)->cond_formats->front().ranges.size() == 1);
}

TEST_CASE("The deepest outline level is known at once, however many rows are grouped") {
  // every frame asked for it, and the million rows Subtotal grouped took a frame's time to scan
  DimIndex d;
  std::map<int64_t, uint8_t> lv;
  for (int64_t i = 0; i < 200000; ++i) lv.emplace_hint(lv.end(), i, static_cast<uint8_t>(i % 3 == 0 ? 2 : 1));
  d.LoadLevels(std::move(lv));
  CHECK(d.MaxLevel() == 2);
  const auto t0 = std::chrono::steady_clock::now();
  int sum = 0;
  for (int k = 0; k < 2000; ++k) sum += d.MaxLevel();
  CHECK(sum == 4000);
  CHECK(Seconds(t0) < Limit(0.5));
  // the count follows every change
  d.SetLevel(5, 7);
  CHECK(d.MaxLevel() == 7);
  d.SetLevel(5, 9);
  CHECK(d.Level(5) == 7);
  d.SetLevel(5, 0);
  CHECK(d.MaxLevel() == 2);
  d.Remove(0, 200000);
  CHECK(d.MaxLevel() == 0);
  CHECK(d.Levels().empty());
  d.SetLevel(10, 3);
  d.SetLevel(11, 3);
  d.Insert(11, 2);  // rows inserted inside a group join it
  CHECK(d.Level(11) == 3);
  CHECK(d.Level(12) == 3);
  CHECK(d.Level(13) == 3);
  CHECK(d.Level(14) == 0);
  CHECK(d.MaxLevel() == 3);
  const DimIndex copy = d;
  d.Remove(10, 4);
  CHECK(d.MaxLevel() == 0);
  CHECK(copy.MaxLevel() == 3);
  DimIndex odd;
  odd.LoadLevels({{1, 0}, {2, 9}});  // a damaged file: a level 0 entry and one past the deepest
  CHECK(odd.MaxLevel() == 7);
  odd.SetLevel(2, 0);
  CHECK(odd.MaxLevel() == 0);
}

TEST_CASE("the whole sheet filled down from a short row uses the lanes it filled, and copies") {
  // basics seed 2390: Ctrl+A, Ctrl+D over four cells in row 1 made the whole sheet the used range, and Ctrl+C of it
  // then refused 17 billion cells; Excel's used range is A1:D1048576
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "Name"));
  REQUIRE(t.Set("D1", "Note"));
  t.d.FillDown(sh, yc::core::Rect{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1});
  const auto* s = t.d.S(sh);
  REQUIRE(s->has_used);
  CHECK(s->used == yctest::Doc::Range("A1:D1048576"));
  const yc::engine::ClipData clip = t.d.Copy(sh, yc::core::Rect{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1});
  CHECK(clip.rows == s->row_limit);
  CHECK(clip.cols == 4);
}

TEST_CASE("a million whole rows inserted under a row with a border in every cell take its formats in a moment") {
  // basics seq316723: Outside Borders over the whole sheet gives each cell of row 1 a style of its own, and the
  // million rows then inserted at row 2 took them cell by cell (17 billion cell styles: out of memory)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  using LS = yc::model::LineStyle;
  auto side = [&](uint32_t r, uint32_t c, int e) {
    return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(r, c)).border[e].style;
  };
  const yc::core::Rect all{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1};
  t.d.SetBorders(sh, all, yc::engine::BorderPreset::Outside, LS::Thin, 0);
  t.Set("C1", "x");
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("insert");
  t.d.InsertRows(sh, 1, 1048575, 0);
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));
  // the new rows show what row 1 shows: its top border along the row, the left and right borders at its ends
  for (const uint32_t r : {1u, 500000u, 1048575u}) {
    CHECK(side(r, 0, yc::model::kTop) == LS::Thin);
    CHECK(side(r, 0, yc::model::kLeft) == LS::Thin);
    CHECK(side(r, 7, yc::model::kTop) == LS::Thin);
    CHECK(side(r, 7, yc::model::kLeft) == LS::None);
    CHECK(side(r, 16383, yc::model::kRight) == LS::Thin);
  }
  CHECK(side(1048576, 7, yc::model::kTop) == LS::None);  // the row that was row 2
  CHECK(side(1048576, 0, yc::model::kLeft) == LS::Thin);
  // and Undo gives the sheet back
  REQUIRE(t.d.Undo());
  CHECK(side(1, 7, yc::model::kTop) == LS::None);
  CHECK(side(1, 0, yc::model::kLeft) == LS::Thin);
  // whole columns next to column A, all of whose cells but the last have a style of their own
  yctest::Doc u;
  const uint32_t sh2 = u.sheet();
  u.d.ModifyStyle(sh2, yc::core::Rect{0, 0, 1048574, 0}, [](yc::model::Style& st) { st.bold = true; });
  u.Set("A1048576", "end");
  const auto t1 = std::chrono::steady_clock::now();
  u.d.InsertCols(sh2, 1, 3000, 0);
  CHECK(Seconds(t1) < Limit(5.0));
  auto bold = [&](uint32_t r, uint32_t c) { return u.d.wb().styles->Get(u.d.S(sh2)->GetStyle(r, c)).bold; };
  CHECK(bold(700000, 2000));
  CHECK(bold(0, 1));
  CHECK_FALSE(bold(1048575, 2000));
  CHECK_FALSE(bold(5, 3001));  // the column that was B
}

TEST_CASE("borders over the whole sheet and whole columns are set in a moment, on the right sides") {
  // basics seq268600: Ctrl+Shift+7 (Outside Borders) over the whole sheet looped over 17 billion cells (a freeze), and
  // over whole columns B:C over two million; each preset is now a few rectangles set through ModifyStyle
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  using yc::engine::BorderPreset;
  using LS = yc::model::LineStyle;
  auto side = [&](uint32_t r, uint32_t c, int e) {
    const auto* s = t.d.S(sh);
    return t.d.wb().styles->Get(s->GetStyle(r, c)).border[e].style;
  };
  const yc::core::Rect all{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1};
  const auto t0 = std::chrono::steady_clock::now();
  t.d.SetBorders(sh, all, BorderPreset::Outside, LS::Thin, 0);
  CHECK(Seconds(t0) < Limit(3.0));  // was a freeze
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  CHECK(side(0, 0, yc::model::kTop) == LS::Thin);
  CHECK(side(0, 0, yc::model::kLeft) == LS::Thin);
  CHECK(side(500, 0, yc::model::kLeft) == LS::Thin);
  CHECK(side(500, 16383, yc::model::kRight) == LS::Thin);
  CHECK(side(last, 7, yc::model::kBottom) == LS::Thin);
  CHECK(side(500, 7, yc::model::kLeft) == LS::None);
  CHECK(side(500, 7, yc::model::kTop) == LS::None);
  const auto t1 = std::chrono::steady_clock::now();
  t.d.SetBorders(sh, yc::core::Rect{0, 1, yc::core::kMaxRows - 1, 2}, BorderPreset::Inside, LS::Thin, 0);
  t.d.SetBorders(sh, all, BorderPreset::None, LS::Thin, 0);
  t.d.SetBorders(sh, all, BorderPreset::All, LS::Thin, 0);
  CHECK(Seconds(t1) < Limit(3.0));
  CHECK(side(500, 7, yc::model::kLeft) == LS::Thin);
  CHECK(side(500, 7, yc::model::kBottom) == LS::Thin);
  // a block: the sides each preset sets, as before (B2:D4)
  yctest::Doc u;
  const uint32_t sh2 = u.sheet();
  auto side2 = [&](const char* a1, int e) {
    const auto p = yctest::Doc::P(a1);
    return u.d.wb().styles->Get(u.d.S(sh2)->GetStyle(p.row, p.col)).border[e].style;
  };
  u.d.SetBorders(sh2, yctest::Doc::Range("B2:D4"), BorderPreset::Outside, LS::Thin, 0);
  CHECK(side2("B2", yc::model::kTop) == LS::Thin);
  CHECK(side2("B2", yc::model::kLeft) == LS::Thin);
  CHECK(side2("D4", yc::model::kBottom) == LS::Thin);
  CHECK(side2("D4", yc::model::kRight) == LS::Thin);
  CHECK(side2("C3", yc::model::kTop) == LS::None);
  CHECK(side2("C2", yc::model::kLeft) == LS::None);
  u.d.SetBorders(sh2, yctest::Doc::Range("B2:D4"), BorderPreset::Inside, LS::Thin, 0);
  CHECK(side2("B2", yc::model::kBottom) == LS::Thin);
  CHECK(side2("B4", yc::model::kBottom) == LS::Thin);  // the outside's, kept
  CHECK(side2("C3", yc::model::kRight) == LS::Thin);
  CHECK(side2("D3", yc::model::kRight) == LS::Thin);
  u.d.SetBorders(sh2, yctest::Doc::Range("C3"), BorderPreset::None, LS::Thin, 0);
  CHECK(side2("C3", yc::model::kRight) == LS::None);
  CHECK(side2("C2", yc::model::kBottom) == LS::None);  // the facing edges too
  CHECK(side2("B3", yc::model::kRight) == LS::None);
  CHECK(side2("B2", yc::model::kBottom) == LS::Thin);
  u.d.SetBorders(sh2, yctest::Doc::Range("F6:G6"), BorderPreset::TopThickBottom, LS::Thin, 0);
  CHECK(side2("G6", yc::model::kTop) == LS::Thin);
  CHECK(side2("G6", yc::model::kBottom) == LS::Thick);
}

TEST_CASE("a page layout over the whole sheet makes its first pages only, in a moment") {
  // basics seq277302: a value typed into XFD1048576 in Page Layout view laid the used range out over tens of millions
  // of pages at every edit (18 s each, gigabytes)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "x"));
  REQUIRE(t.Set("XFD1048576", "y"));
  const auto t0 = std::chrono::steady_clock::now();
  const yc::engine::PrintLayout L = yc::engine::LayoutPages(*t.d.S(sh), *t.d.S(sh)->page_setup);
  CHECK(Seconds(t0) < Limit(3.0));
  CHECK(L.truncated);
  CHECK(L.pages.size() == yc::engine::kMaxLayoutPages);
  CHECK(L.row_segs.size() * L.col_segs.size() > yc::engine::kMaxLayoutPages);
  CHECK(L.pages.front().r0 == 0);
  CHECK(L.pages.front().c0 == 0);
  // a sheet of a few pages is laid out whole
  yctest::Doc u;
  REQUIRE(u.Set("A1", "x"));
  REQUIRE(u.Set("Z200", "y"));
  const yc::engine::PrintLayout M = yc::engine::LayoutPages(*u.d.S(u.sheet()), *u.d.S(u.sheet())->page_setup);
  CHECK_FALSE(M.truncated);
  CHECK(M.pages.size() == M.row_segs.size() * M.col_segs.size());
}

TEST_CASE("merging the whole sheet walks the cells that hold something, in a moment") {
  // basics seq280931: Ctrl+A, Merge & Center looped over all 17 billion cells to clear them (a freeze); the first
  // value in reading order moves to A1 at any size, as Excel 16 does (measured for small areas)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("C5", "first"));
  REQUIRE(t.Set("B9", "later"));
  REQUIRE(t.Set("XFD1048576", "far"));
  REQUIRE(t.Set("E7", "=1+1"));
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(t.d.Merge(sh, yc::core::Rect{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1}, false, true));
  CHECK(Seconds(t0) < Limit(3.0));  // was a freeze
  CHECK(t.T("A1") == "first");
  CHECK(t.T("C5").empty());
  CHECK(t.T("B9").empty());
  CHECK(t.T("XFD1048576").empty());
  CHECK(t.F("E7").empty());
  // Merge Across over the whole sheet: a merged area a row, each walked by the cells that hold something
  yctest::Doc u;
  REQUIRE(u.Set("C5", "x"));
  const auto t1 = std::chrono::steady_clock::now();
  u.d.Merge(u.sheet(), yc::core::Rect{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1}, true, false);
  CHECK(Seconds(t1) < Limit(10.0));
  CHECK(u.T("A5") == "x");
}

TEST_CASE("Fill Right over whole rows whose first cells have styles of their own takes a moment, and undoes") {
  // basics seq283522: Outside Borders gave column A's cells of whole rows a left border each, and Ctrl+R then wrote
  // that style into all 16,383 other cells of every row (17 billion)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  using LS = yc::model::LineStyle;
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  t.d.SetBorders(sh, yc::core::Rect{2, 0, last, 0}, yc::engine::BorderPreset::Left, LS::Thin, 0);
  REQUIRE(t.Set("A5", "x"));
  auto left = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(p.row, p.col)).border[yc::model::kLeft].style;
  };
  const size_t depth = t.d.UndoDepth();
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Fill Right");
  t.d.FillRight(sh, yc::core::Rect{2, 0, last, yc::core::kMaxCols - 1});
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));  // was a freeze
  CHECK(left("B500") == LS::Thin);
  CHECK(left("XFD1000000") == LS::Thin);
  CHECK(t.T("XFD5") == "x");
  CHECK(left("B2") == LS::None);  // a row above the fill
  CHECK(t.d.UndoDepth() == depth + 1);
  REQUIRE(t.d.Undo());
  CHECK(left("B500") == LS::None);
  CHECK(left("A500") == LS::Thin);
  CHECK(t.T("XFD5").empty());
}

TEST_CASE("a format over the whole sheet after its inside borders takes a moment, and Clear takes it off") {
  // the inside horizontal lines of the whole sheet were set on rows 1:1048575, a style a row; a format over the whole
  // sheet then gave every cell where those rows met a column a style of its own (17 billion, a freeze), and
  // Cells.Clear left the columns' and rows' own styles, so the borders stayed on every cell
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  using LS = yc::model::LineStyle;
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  const yc::core::Rect all{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1};
  auto st = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(p.row, p.col));
  };
  t.d.SetBorders(sh, all, yc::engine::BorderPreset::InsideH, LS::Thin, 0);
  // whole columns take the lines on every cell, the first row's top and the last row's bottom too (Excel 16, measured),
  // through the columns' styles: no style a row
  CHECK(t.d.S(sh)->row_styles->empty());
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(last, 3)).border[yc::model::kBottom].style == LS::Thin);
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(0, 3)).border[yc::model::kTop].style == LS::Thin);
  const auto t0 = std::chrono::steady_clock::now();
  const size_t depth = t.d.UndoDepth();
  t.d.Begin("Center");
  t.d.ModifyStyle(sh, all, [](yc::model::Style& s) { s.halign = yc::model::HAlign::Center; });
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));  // was a freeze
  CHECK(st("B5").halign == yc::model::HAlign::Center);
  CHECK(st("B5").border[yc::model::kBottom].style == LS::Thin);  // the row's border kept
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(last, 3)).halign == yc::model::HAlign::Center);
  CHECK(t.d.UndoDepth() == depth + 1);
  REQUIRE(t.d.Undo());
  CHECK(st("B5").halign != yc::model::HAlign::Center);
  CHECK(st("B5").border[yc::model::kBottom].style == LS::Thin);
  // Clear over the whole sheet takes every line's style off
  const auto t1 = std::chrono::steady_clock::now();
  t.d.Begin("Clear");
  t.d.ClearAll(sh, all, yc::engine::kSrcUser, false);
  t.d.Commit();
  CHECK(Seconds(t1) < Limit(5.0));
  CHECK(st("B5").border[yc::model::kBottom].style == LS::None);
  CHECK(t.d.S(sh)->row_styles->empty());
  REQUIRE(t.d.Undo());
  CHECK(st("B5").border[yc::model::kBottom].style == LS::Thin);
}

TEST_CASE("Clear Formats over whole columns and whole rows takes their own formats off, and Undo puts them back") {
  // Excel 16: column C filled, then Home > Clear Formats over C:C leaves C9 without the fill; YCGrid cleared only
  // the cells with styles of their own and the column's fill stayed
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  auto fill = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(p.row, p.col)).fill_color;
  };
  auto red = [](yc::model::Style& s) { s.fill_color = 0xFFFF0000; };
  t.d.ModifyStyle(sh, yc::core::Rect{0, 2, yc::core::kMaxRows - 1, 3}, red);   // C:D
  t.d.ModifyStyle(sh, yc::core::Rect{4, 0, 4, yc::core::kMaxCols - 1}, red);   // row 5
  t.d.ModifyStyle(sh, yc::core::Rect{1000, 0, 1999, yc::core::kMaxCols - 1}, red);  // rows 1001:2000, rows' own styles
  REQUIRE(fill("C9") == 0xFFFF0000);
  REQUIRE(fill("H1500") == 0xFFFF0000);
  REQUIRE(t.d.S(sh)->row_styles->size() == 1000);
  t.d.Begin("Clear Formats");
  t.d.ClearFormats(sh, yc::core::Rect{0, 2, last, 2}, false);  // C:C
  t.d.Commit();
  CHECK(fill("C9") == yc::model::kColorAuto);
  CHECK(fill("C3") == yc::model::kColorAuto);
  CHECK(fill("D9") == 0xFFFF0000);  // the column beside it keeps its fill
  t.d.Begin("Clear Formats");
  t.d.ClearFormats(sh, yc::core::Rect{1000, 0, 1999, yc::core::kMaxCols - 1}, false);  // rows 1001:2000
  t.d.Commit();
  CHECK(fill("H1500") == yc::model::kColorAuto);
  CHECK(fill("D1500") == yc::model::kColorAuto);  // the rows' Normal wins over column D's fill, as in Excel
  CHECK(fill("D999") == 0xFFFF0000);
  CHECK(fill("H5") == 0xFFFF0000);  // row 5 isn't in it
  REQUIRE(t.d.Undo());
  CHECK(fill("H1500") == 0xFFFF0000);
  REQUIRE(t.d.Undo());
  CHECK(fill("C9") == 0xFFFF0000);
}

TEST_CASE("a cell set back to Normal under a bold column or row shows Normal, after Undo and a reopen too") {
  // Excel 16: column C bold, then Clear Formats on C5 or Bold off on C6 leaves them not bold, and a plain cell pasted
  // on C7 is plain; YCGrid wrote the default as "no style of its own" and the cells showed the column's bold again
  // (Bold off on C6 changed nothing at all)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  auto bold = [&](const yc::model::Workbook& wb, const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return wb.styles->Get(wb.sheets[0]->GetStyle(p.row, p.col)).bold;
  };
  t.d.ModifyStyle(sh, yc::core::Rect{0, 2, last, 2}, [](yc::model::Style& s) { s.bold = true; });  // C:C
  REQUIRE(bold(t.d.wb(), "C5"));
  t.d.Begin("Clear Formats");
  t.d.ClearFormats(sh, yctest::Doc::Range("C5"), false);
  t.d.Commit();
  CHECK_FALSE(bold(t.d.wb(), "C5"));
  CHECK(bold(t.d.wb(), "C4"));
  t.d.Begin("Bold");
  t.d.ModifyStyle(sh, yctest::Doc::Range("C6"), [](yc::model::Style& s) { s.bold = false; });
  t.d.Commit();
  CHECK_FALSE(bold(t.d.wb(), "C6"));
  t.d.Begin("Paste");
  t.d.Paste(sh, 6, 2, t.d.Copy(sh, yctest::Doc::Range("A1")));  // a plain cell on C7
  t.d.Commit();
  CHECK_FALSE(bold(t.d.wb(), "C7"));
  // the same under a row with formats of its own crossing the column (row 1001's bold, through ModifyRowStyle)
  t.d.ModifyStyle(sh, yc::core::Rect{1000, 0, 1199, yc::core::kMaxCols - 1}, [](yc::model::Style& s) { s.italic = true; });
  REQUIRE(t.d.S(sh)->row_styles->size() == 200);
  t.d.ClearFormats(sh, yctest::Doc::Range("H1100"), false);
  CHECK_FALSE(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(1099, 7)).italic);
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(1099, 8)).italic);
  // saved and opened again, they are still Normal
  auto r = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(r.ok);
  CHECK_FALSE(bold(r.wb, "C5"));
  CHECK_FALSE(bold(r.wb, "C6"));
  CHECK(bold(r.wb, "C8"));
  CHECK_FALSE(r.wb.styles->Get(r.wb.sheets[0]->GetStyle(1099, 7)).italic);
  // Undo gives the column's bold back to each
  REQUIRE(t.d.Undo());
  REQUIRE(t.d.Undo());
  REQUIRE(t.d.Undo());
  CHECK(bold(t.d.wb(), "C5"));
  CHECK(bold(t.d.wb(), "C6"));
  CHECK(bold(t.d.wb(), "C7"));
}

TEST_CASE("Clear Formats over whole rows crossing a column's formats leaves them Normal") {
  // Excel 16 keeps a Normal style on the rows cleared, which wins over the column's (C5 isn't bold after row 5's
  // Clear Formats under a bold column C)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  t.d.ModifyStyle(sh, yc::core::Rect{0, 2, last, 2}, [](yc::model::Style& s) { s.bold = true; });
  t.d.ClearFormats(sh, yc::core::Rect{4, 0, 4, yc::core::kMaxCols - 1}, false);
  CHECK_FALSE(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(4, 2)).bold);
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(5, 2)).bold);
}

TEST_CASE("a format of every row but the last goes through the columns' styles and leaves the last row as it was") {
  // Excel 16, measured: rows 1:1048575 bold leave D1048576 not bold. As a style for each of a million rows, the next
  // format of whole columns crossed every one of them
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  auto st = [&](uint32_t r, uint32_t c) { return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(r, c)); };
  t.d.ModifyStyle(sh, yc::core::Rect{last, 4, last, 4}, [](yc::model::Style& s) { s.italic = true; });  // E1048576
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Bold");
  t.d.ModifyStyle(sh, yc::core::Rect{0, 0, last - 1, yc::core::kMaxCols - 1}, [](yc::model::Style& s) { s.bold = true; });
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(3.0));
  CHECK(t.d.S(sh)->row_styles->size() <= 1);
  CHECK(st(4, 3).bold);
  CHECK(st(last - 1, 16383).bold);
  CHECK_FALSE(st(last, 3).bold);
  CHECK_FALSE(st(last, 4).bold);
  CHECK(st(last, 4).italic);
  const auto t1 = std::chrono::steady_clock::now();
  t.d.ModifyStyle(sh, yc::core::Rect{0, 1, last, 3}, [](yc::model::Style& s) { s.italic = true; });  // B:D
  CHECK(Seconds(t1) < Limit(3.0));
  CHECK(st(500, 2).italic);
  CHECK(st(500, 2).bold);
  REQUIRE(t.d.Undo());
  CHECK_FALSE(st(4, 3).bold);
  CHECK(st(last, 4).italic);
}

TEST_CASE("a cut out of a formatted column leaves the column's format, a plain cell cut into one is Normal there") {
  // Excel 16, measured (linefmt_cut_out, linefmt_cut_plain_in): H3 cut to J3 shows column H's format again, J3 takes it;
  // A2 (General) cut to H5 is General there
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  auto italic = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(p.row, p.col)).italic;
  };
  t.d.ModifyStyle(sh, yc::core::Rect{0, 7, last, 7}, [](yc::model::Style& s) { s.italic = true; });  // H:H
  REQUIRE(t.Set("H3", "7"));
  REQUIRE(t.Set("A2", "Apple"));
  REQUIRE(t.d.MoveCells(sh, yctest::Doc::Range("H3"), sh, 2, 9));  // to J3
  CHECK(italic("H3"));
  CHECK(italic("J3"));
  REQUIRE(t.d.MoveCells(sh, yctest::Doc::Range("A2"), sh, 4, 7));  // to H5
  CHECK_FALSE(italic("H5"));
  CHECK(t.T("H5") == "Apple");
}

TEST_CASE("a PivotTable of 100,000 distinct row items with Show Values As is computed in a moment") {
  // each level's siblings were gathered with a find over the list so far: 100,000 distinct items made ten billion
  // steps (Rank, % of Parent Row and the others that read siblings)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  constexpr uint32_t kRows = 100000;
  std::vector<yc::core::Value> vals;
  vals.reserve(static_cast<size_t>(kRows + 1) * 2);
  vals.push_back(yc::core::Value::String("Key"));
  vals.push_back(yc::core::Value::String("Qty"));
  for (uint32_t i = 0; i < kRows; ++i) {
    vals.push_back(yc::core::Value::String("K" + std::to_string(1000000 + i)));
    vals.push_back(yc::core::Value::Number(static_cast<double>(i % 97)));
  }
  t.d.SetValues(sh, yc::core::Rect{0, 0, kRows, 1}, vals);
  yc::model::PivotTable p;
  p.src_sheet = sh;
  p.src = yc::core::Rect{0, 0, kRows, 1};
  p.c0 = 4;
  yc::model::PivotAxisField a;
  a.field = 0;
  p.rows.push_back(a);
  yc::model::PivotDataField d;
  d.field = 1;
  d.show_as = yc::model::PivotShowAs::RankAscending;
  d.base_field = 0;
  p.data.push_back(d);
  yc::engine::PivotCache cache;
  std::string err;
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  const auto t0 = std::chrono::steady_clock::now();
  const auto rep = yc::engine::ComputePivot(p, cache);
  CHECK(Seconds(t0) < Limit(5.0));
  CHECK(rep.rows >= kRows);
}

TEST_CASE("Remove Duplicates over the whole sheet with a value in its last cell takes a moment") {
  // scale_sweep: every row was compared and copied across 16,384 columns (17 billion cells); its OK never answered.
  // Empty rows are duplicates of each other: the first stays, and the last row's value moves up below it
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "Name"));
  REQUIRE(t.Set("B1", "Qty"));
  const char* names[] = {"a", "b", "a", "c", "d"};
  const char* qty[] = {"1", "2", "1", "3", "4"};
  for (int i = 0; i < 5; ++i) {
    REQUIRE(t.Set(("A" + std::to_string(i + 2)).c_str(), names[i]));
    REQUIRE(t.Set(("B" + std::to_string(i + 2)).c_str(), qty[i]));
  }
  REQUIRE(t.Set("XFD1048576", "far"));
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Remove Duplicates");
  const uint32_t removed = t.d.RemoveDuplicates(sh, yc::core::Rect{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1}, {}, true);
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));  // was a freeze
  CHECK(removed == 1048575u - 6u);  // the second "a 1" and every empty row but the first
  CHECK(t.T("A4") == "c");
  CHECK(t.T("A5") == "d");
  CHECK(t.T("A6").empty());
  CHECK(t.T("XFD7") == "far");
  CHECK(t.T("XFD1048576").empty());
  REQUIRE(t.d.Undo());
  CHECK(t.T("A4") == "a");
  CHECK(t.T("XFD1048576") == "far");
}

TEST_CASE("Advanced Filter and Auto Outline over the whole sheet with a value in its last cell answer in a moment") {
  // scale_sweep: Advanced Filter read the whole sheet into one array (17 billion cells, its OK never answered) and Auto
  // Outline asked every cell for a formula (12 s)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "Qty"));
  REQUIRE(t.Set("A2", "1"));
  REQUIRE(t.Set("A3", "2"));
  REQUIRE(t.Set("A4", "=SUM(A2:A3)"));
  REQUIRE(t.Set("XFD1048576", "far"));
  const yc::core::Rect all{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1};
  auto t0 = std::chrono::steady_clock::now();
  std::string err;
  CHECK_FALSE(t.d.AdvancedFilter(sh, all, sh, yc::core::Rect{}, false, sh, yc::core::Rect{}, true, &err));
  CHECK(err.find("too large") != std::string::npos);
  CHECK(Seconds(t0) < Limit(2.0));
  t0 = std::chrono::steady_clock::now();
  CHECK(t.d.AutoOutline(sh, all) == 1);  // rows 2:3 under the total in A4
  CHECK(Seconds(t0) < Limit(2.0));
}

TEST_CASE("on a protected sheet, unlocked whole columns are cleared, and a locked cell among them is refused") {
  // a range past 2,000,000 cells counted as locked without a look: Delete over B:C, both unlocked, was refused on a
  // protected sheet (Excel 16 clears them and refuses B:D, measured). The cells are read by their own, their rows' and their columns' styles
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  t.d.ModifyStyle(sh, yc::core::Rect{0, 1, last, 2}, [](yc::model::Style& s) { s.locked = false; });  // B:C
  REQUIRE(t.Set("B5", "x"));
  REQUIRE(t.Set("C900000", "y"));
  REQUIRE(t.d.ProtectSheet(sh, yc::model::SheetProtection{}, ""));
  const yc::core::Rect bc{0, 1, last, 2};
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(t.d.IsLockedRange(sh, bc));
  CHECK(t.d.IsLockedRange(sh, yc::core::Rect{0, 1, last, 3}));  // D is locked
  CHECK(Seconds(t0) < Limit(1.0));
  t.d.ClearContents(sh, bc);
  CHECK(t.T("B5").empty());
  CHECK(t.T("C900000").empty());
  // one locked cell inside the unlocked columns, a row of its own, and an unlocked row over locked columns
  t.d.UnprotectSheet(sh, "");
  t.d.ModifyStyle(sh, yctest::Doc::Range("C7"), [](yc::model::Style& s) { s.locked = true; });
  t.d.ModifyStyle(sh, yc::core::Rect{10, 0, 10, yc::core::kMaxCols - 1}, [](yc::model::Style& s) { s.locked = false; });
  REQUIRE(t.d.ProtectSheet(sh, yc::model::SheetProtection{}, ""));
  CHECK(t.d.IsLockedRange(sh, bc));
  CHECK_FALSE(t.d.IsLockedRange(sh, yc::core::Rect{10, 0, 10, yc::core::kMaxCols - 1}));
  CHECK_FALSE(t.d.IsLockedRange(sh, yc::core::Rect{0, 1, 5, 2}));
}

TEST_CASE("a format from row 2 to the sheet's last cell (Ctrl+Shift+End from B2) is set, in a moment") {
  // B2:XFD1048576 is neither whole rows nor whole columns and past 20,000,000 cells: ModifyStyle set nothing at all
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  REQUIRE(t.Set("C1", "head"));
  t.d.ModifyStyle(sh, yctest::Doc::Range("D1"), [](yc::model::Style& s) { s.italic = true; });
  auto st = [&](uint32_t r, uint32_t c) { return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(r, c)); };
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Bold");
  t.d.ModifyStyle(sh, yc::core::Rect{1, 1, last, yc::core::kMaxCols - 1}, [](yc::model::Style& s) { s.bold = true; });
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(3.0));
  CHECK(st(1, 1).bold);
  CHECK(st(last, 16383).bold);
  CHECK(st(500000, 7000).bold);
  CHECK_FALSE(st(0, 1).bold);  // row 1 as it was
  CHECK_FALSE(st(0, 3).bold);
  CHECK(st(0, 3).italic);
  CHECK_FALSE(st(5, 0).bold);  // column A isn't in it
  REQUIRE(t.d.Undo());
  CHECK_FALSE(st(1, 1).bold);
  CHECK_FALSE(st(last, 16383).bold);
}

TEST_CASE("Fill Right over whole rows from a column formatted whole gives every row the column's format") {
  // the line copied for the bulk fill was cut to the used range, and a column's format doesn't make cells used: B500
  // stayed plain under rows 3:1048576 filled right from column A, whose border was the column's own. A fill copies what
  // the source cell shows (Excel 16 didn't answer for minutes over these rows, measured)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  using LS = yc::model::LineStyle;
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  t.d.SetBorders(sh, yc::core::Rect{0, 0, last, 0}, yc::engine::BorderPreset::Left, LS::Thin, 0);  // A:A, the column's
  REQUIRE(t.Set("A5", "x"));
  auto left = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(p.row, p.col)).border[yc::model::kLeft].style;
  };
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Fill Right");
  t.d.FillRight(sh, yc::core::Rect{2, 0, last, yc::core::kMaxCols - 1});
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));
  CHECK(left("B500") == LS::Thin);
  CHECK(left("XFD1000000") == LS::Thin);
  CHECK(left("B2") == LS::None);  // a row above the fill
  CHECK(t.T("XFD5") == "x");
  REQUIRE(t.d.Undo());
  CHECK(left("B500") == LS::None);
}

TEST_CASE("Row and Column Differences look only at the lines that matter, and select what comparing every cell does") {
  // scale_sweep: Column Differences over the whole sheet with a value in XFD1048576 compared 17 billion cells (a freeze).
  // Random sheets: the cells found are the ones a cell-by-cell comparison finds
  auto brute = [](const yc::model::Sheet& s, yc::core::Rect w, uint32_t cr, uint32_t cc, bool rows) {
    std::set<std::pair<uint32_t, uint32_t>> out;
    auto same = [&](uint32_t r, uint32_t c, uint32_t r2, uint32_t c2) {
      const uint32_t f1 = s.GetFormula(r, c), f2 = s.GetFormula(r2, c2);
      if (f1 || f2) return f1 == f2;
      const auto& a = s.GetValue(r, c);
      const auto& b = s.GetValue(r2, c2);
      if (a.IsEmpty() || b.IsEmpty()) return a.IsEmpty() == b.IsEmpty();
      if (a.IsNumeric() && b.IsNumeric()) return a.AsDouble() == b.AsDouble();
      if (a.IsString() && b.IsString()) return a.str() == b.str();
      return false;
    };
    w.r1 = std::min(w.r1, s.used.r1);
    w.c1 = std::min(w.c1, s.used.c1);
    for (uint32_t r = w.r0; r <= w.r1; ++r) {
      for (uint32_t c = w.c0; c <= w.c1; ++c) {
        if (s.rows->IsHidden(r) || s.cols->IsHidden(c)) continue;
        const uint32_t r2 = rows ? r : cr, c2 = rows ? cc : c;
        if ((r != r2 || c != c2) && !same(r, c, r2, c2)) out.insert({r, c});
      }
    }
    return out;
  };
  uint32_t seed = 12345;
  auto rnd = [&](uint32_t n) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) % n;
  };
  for (int round = 0; round < 60; ++round) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    for (int k = 0; k < 40; ++k) {
      const std::string a1 = std::string(1, static_cast<char>('A' + rnd(8))) + std::to_string(1 + rnd(10));
      const uint32_t kind = rnd(4);
      REQUIRE(t.Set(a1.c_str(), kind == 0 ? "x" : kind == 1 ? "1" : kind == 2 ? "=1+1" : "2"));
    }
    if (rnd(2)) {
      const uint32_t h = rnd(10);
      t.d.HideRows(sh, h, h, true);
    }
    const yc::core::Rect w{0, 0, 11, 9};
    const uint32_t cr = rnd(10), cc = rnd(8);
    for (const bool rows : {true, false}) {
      std::set<std::pair<uint32_t, uint32_t>> got;
      for (const auto& x : yc::engine::DifferenceCells(*t.d.S(sh), w, cr, cc, rows)) {
        for (uint32_t r = x.r0; r <= x.r1; ++r) {
          for (uint32_t c = x.c0; c <= x.c1; ++c) got.insert({r, c});
        }
      }
      INFO("round " << round << (rows ? " rows" : " columns") << " at " << cr << "," << cc);
      CHECK(got == brute(*t.d.S(sh), w, cr, cc, rows));
    }
  }
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "Name"));
  REQUIRE(t.Set("B2", "10"));
  REQUIRE(t.Set("XFD1048576", "far"));
  const auto t0 = std::chrono::steady_clock::now();
  const auto found = yc::engine::DifferenceCells(*t.d.S(sh), yc::core::Rect{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1}, 0, 0, false);
  CHECK(Seconds(t0) < Limit(2.0));  // was a freeze
  CHECK_FALSE(found.empty());
}

TEST_CASE("a formula typed in a table over whole columns fills its calculated column in one paste") {
  // a table given whole columns to the hard row limit had two billion rows, and the column was filled a paste a row:
  // doc_fuzz --wide reached 49 GB of memory
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "Qty"));
  REQUIRE(t.Set("B1", "Twice"));
  REQUIRE(t.Set("A2", "5"));
  REQUIRE(t.d.AddTable(sh, yc::core::Rect{0, 0, yc::core::kMaxRows - 1, 1}, true, "") != 0);  // to the hard limit
  REQUIRE(t.d.S(sh)->tables->size() == 1);
  CHECK(t.d.S(sh)->tables->front().range.r1 == t.d.S(sh)->row_limit - 1);  // the sheet's rows
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Type");
  REQUIRE(t.d.SetInput(sh, 1, 1, "=A2*2"));  // B2
  t.d.Commit();
  t.d.Recalc();
  CHECK(Seconds(t0) < Limit(10.0));
  CHECK(t.F("B3") == "=A3*2");
  CHECK(t.F("B1000000") == "=A1000000*2");
  CHECK(t.N("B2") == 10);
}

TEST_CASE("a column inserted into a table across the whole width renames its headers in a moment") {
  // doc_fuzz --wide: each header renamed by the shift walked every formula and rebuilt the engine's index (one walk a
  // header over 16,384 columns), and the next free ColumnN name was looked for in every header for each try
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "Qty"));
  REQUIRE(t.Set("A2", "5"));
  REQUIRE(t.Set("A3", "7"));
  REQUIRE(t.d.AddTable(sh, yc::core::Rect{0, 0, 2, yc::core::kMaxCols - 2}, true, "") != 0);  // A1:XFC3, room to shift
  for (int i = 0; i < 2000; ++i) REQUIRE(t.Set(("A" + std::to_string(10 + i)).c_str(), "=SUM(Table1[Column3])"));
  REQUIRE(t.Set("C2", "4"));
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Insert");
  t.d.InsertCols(sh, 1, 1);  // a column at B, inside the table
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(10.0));
  REQUIRE(t.d.S(sh)->tables->size() == 1);
  CHECK(t.T("A1") == "Qty");
  CHECK(t.d.S(sh)->tables->front().range.c1 == yc::core::kMaxCols - 1);  // it grew by the column
}

TEST_CASE("Fill Right then Fill Down over the whole sheet is refused before it writes billions of values") {
  // basics seq297242: Fill Right copied column A into every column, and Fill Down then wrote row 1 into a million rows
  // (17 billion values, a freeze). A fill past 20,000,000 filled cells is refused, as a copy that large is
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "Name"));
  REQUIRE(t.Set("A2", "AAPL"));
  REQUIRE(t.Set("XFD1048576", "far"));
  const yc::core::Rect all{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1};
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Fill Right");
  t.d.FillRight(sh, all);
  t.d.Commit();
  CHECK(t.T("XFD1") == "Name");  // two values across: 32,766 cells
  const uint32_t refused = t.d.protect_blocks();
  t.d.Begin("Fill Down");
  t.d.FillDown(sh, all);
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));  // was a freeze
  CHECK(t.d.protect_blocks() == refused + 1);
  CHECK(t.d.block_too_large());
  CHECK(t.T("B500").empty());
  CHECK(t.T("A2") == "AAPL");
}

TEST_CASE("Fill Right across a table as wide as the sheet syncs its headers once, in a moment") {
  // basics seq308046: Ctrl+R pasted a column at a time, and each paste made all 16,384 headers text again (268
  // million header texts: a freeze)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "Name"));
  REQUIRE(t.Set("A2", "7"));
  REQUIRE(t.d.AddTable(sh, yc::core::Rect{0, 0, 2, yc::core::kMaxCols - 1}, true, "") != 0);  // 1:3, every column
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("Fill Right");
  t.d.FillRight(sh, yc::core::Rect{1, 0, 2, yc::core::kMaxCols - 1});  // rows 2:3
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));  // was a freeze
  CHECK(t.T("XFD2") == "7");
  CHECK(t.T("A1") == "Name");
  CHECK(t.d.S(sh)->tables->front().columns.size() == yc::core::kMaxCols);
}

TEST_CASE("an array formula over the whole sheet is refused before it takes the machine's memory") {
  // doc_fuzz --wide: Ctrl+Shift+Enter over A1:XFD1048576 through the Document (Python's Range.FormulaArray) calculated
  // an array of 17 billion values, and the process held 80 GB before it died. The UI refused past 20,000,000 cells
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(t.d.SetArrayFormula(sh, yc::core::Rect{0, 0, 1048575, yc::core::kMaxCols - 1}, "ROW()", &err));
  CHECK(err.find("too large") != std::string::npos);
  CHECK(Seconds(t0) < Limit(1.0));
  CHECK(t.d.SetArrayFormula(sh, yc::core::Rect{0, 0, 2, 1}, "ROW()", &err));  // A1:B3
  t.d.Recalc();
  CHECK(t.T("A1") == "1");
}

TEST_CASE("cells inserted down most of the sheet under a row with a style in every cell are refused, not run out of memory") {
  // B2 to the last cell (Ctrl+Shift+End with a value in XFD1048576) inserted shifting down under Outside Borders on
  // the whole sheet: row 1's 16,383 styles copied to a million rows each
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  using LS = yc::model::LineStyle;
  t.d.SetBorders(sh, yc::core::Rect{0, 0, yc::core::kMaxRows - 1, yc::core::kMaxCols - 1}, yc::engine::BorderPreset::Outside, LS::Thin, 0);
  t.Set("XFD1048576", "far");
  const uint32_t refused = t.d.protect_blocks();
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(t.d.InsertCells(sh, yc::core::Rect{1, 1, 1048575, yc::core::kMaxCols - 1}, true));
  CHECK(Seconds(t0) < Limit(5.0));
  CHECK(t.d.protect_blocks() == refused + 1);
  CHECK(t.d.block_too_large());
  // a few rows of it go in, taking the formats above them
  CHECK(t.d.InsertCells(sh, yc::core::Rect{1, 1, 3, yc::core::kMaxCols - 1}, true));
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(2, 7)).border[yc::model::kTop].style == LS::Thin);
}

TEST_CASE("cells inserted with a shift right over a sheet grown past a million rows are checked in a moment") {
  // basics seq321952: Insert Sheet Rows over the whole sheet (a value in XFD1048576) grew it to two million rows, and
  // Ctrl+Shift+= with rows 1:1048576 then read 34 billion cells one by one for a value the shift would push off (33 s)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("XFD1048576", "far");
  t.d.InsertRows(sh, 0, 1048576);
  REQUIRE(t.d.S(sh)->used.r1 == 2097151);
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t refused = t.d.protect_blocks();
  CHECK(t.d.InsertCells(sh, yc::core::Rect{0, 0, 1048575, yc::core::kMaxCols - 1}, false));  // nothing there to push off
  CHECK(t.d.protect_blocks() == refused);
  CHECK(Seconds(t0) < Limit(3.0));
  // cells as wide as the sheet shifted right are rows (Excel 16, measured: edge_insert_cells_right): the value moves
  // down with them instead of being pushed off XFD
  const auto t1 = std::chrono::steady_clock::now();
  CHECK(t.d.InsertCells(sh, yc::core::Rect{1048576, 0, 2097151, yc::core::kMaxCols - 1}, false));
  CHECK(t.d.protect_blocks() == refused);
  CHECK(t.d.S(sh)->used.r1 == 2097151 + 2 * 1048576);  // rows went in both times
  CHECK(Seconds(t1) < Limit(3.0));
}

TEST_CASE("a million rows inserted under rows that keep Normal over formatted columns take it as their own Normal") {
  // basics seq327428: Wrap Text over D5:XFD1048576 goes to the columns' styles, rows 1:4 keeping Normal cell by cell;
  // a million rows inserted at row 5 took that Normal cell by cell (17 billion styles: out of memory)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.d.ModifyStyle(sh, yc::core::Rect{4, 3, 1048575, yc::core::kMaxCols - 1}, [](yc::model::Style& st) { st.wrap = true; });
  auto wrap = [&](uint32_t r, uint32_t c) { return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(r, c)).wrap; };
  REQUIRE(wrap(4, 3));
  REQUIRE_FALSE(wrap(3, 3));
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("insert");
  t.d.InsertRows(sh, 4, 1048572, 0);
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));
  // the new rows show what row 4 shows (Normal), the rows that moved down keep their Wrap Text
  CHECK_FALSE(wrap(4, 3));
  CHECK_FALSE(wrap(600000, 9000));
  CHECK_FALSE(wrap(1048575, 3));
  CHECK(wrap(1048576, 3));
  CHECK_FALSE(wrap(1048576, 2));  // column C was never wrapped
  REQUIRE(t.d.Undo());
  CHECK(wrap(4, 3));
}

TEST_CASE("Clear Formats from a cell to the sheet's last cell under formatted columns ends in a moment") {
  // doc_fuzz --wide --more seed 89031: Clear All over C11:XFD1048576 with columns C:XFD formatted gave each of 17
  // billion cells Normal one by one (out of memory); the columns lose their formats and rows 1:10 keep showing them
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.d.ModifyStyle(sh, yc::core::Rect{0, 2, 1048575, yc::core::kMaxCols - 1}, [](yc::model::Style& st) { st.bold = true; });
  t.d.ModifyStyle(sh, yc::core::Rect{6, 0, 6, yc::core::kMaxCols - 1}, [](yc::model::Style& st) { st.italic = true; });  // row 7
  t.Set("E20", "x");
  auto st = [&](uint32_t r, uint32_t c) { return t.d.wb().styles->Get(t.d.S(sh)->GetStyle(r, c)); };
  REQUIRE(st(500000, 9000).bold);
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("clear");
  t.d.ClearAll(sh, yc::core::Rect{10, 2, 1048575, yc::core::kMaxCols - 1});
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));
  CHECK_FALSE(st(500000, 9000).bold);
  CHECK_FALSE(st(10, 2).bold);
  CHECK(st(9, 2).bold);           // C10, above the cells cleared
  CHECK(st(0, 16383).bold);       // XFD1
  CHECK(st(6, 5).italic);         // row 7's format
  CHECK(st(6, 5).bold);
  CHECK_FALSE(st(20, 1).bold);    // B21: column B was never bold
  CHECK(t.T("E20").empty());
  REQUIRE(t.d.Undo());
  CHECK(st(500000, 9000).bold);
  CHECK(t.T("E20") == "x");
  // a shape no other way does is refused, contents and all, before anything changes
  yctest::Doc u;
  const uint32_t s2 = u.sheet();
  u.d.ModifyStyle(s2, yc::core::Rect{0, 2, 1048575, yc::core::kMaxCols - 1}, [](yc::model::Style& x) { x.bold = true; });
  u.Set("E20", "x");
  const uint32_t refused = u.d.protect_blocks();
  u.d.ClearAll(s2, yc::core::Rect{500000, 2, 1048574, yc::core::kMaxCols - 1});  // not to the end, half a million rows above
  CHECK(u.d.protect_blocks() == refused + 1);
  CHECK(u.d.block_too_large());
  CHECK(u.T("E20") == "x");
}

TEST_CASE("Fill: Ctrl+R across the whole width under conditional formatting keeps the rule's ranges few") {
  // doc_fuzz --wide --more seed 91473: each column's paste added a range to the rule, and each paste's
  // ClearCondFormats walked them all (16,383 ranges: seconds)
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A6", "5");
  yc::model::CfRule cf;
  cf.type = yc::model::CfType::Expression;
  cf.formula1 = "=$A6>3";
  cf.ranges = {Rect{0, 0, 1048575, 0}};  // all of column A
  t.d.AddCondFormat(sh, cf);
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("fill");
  t.d.FillRight(sh, Rect{5, 0, 7, yc::core::kMaxCols - 1});
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(1.5));
  const auto& rules = *t.d.S(sh)->cond_formats;
  REQUIRE(rules.size() == 1);
  CHECK(rules.front().ranges.size() <= 3);
  uint64_t covered = 0;
  for (const Rect& r : rules.front().ranges) covered += r.Count();
  CHECK(covered == 1048576 + 3ull * (yc::core::kMaxCols - 1));
  CHECK(t.d.GetValue(sh, 5, 16383).AsDouble() == 5);
}

TEST_CASE("Fill: Ctrl+R across the whole width of a cell with data validation keeps the rules few") {
  // as the conditional formats (doc_fuzz --wide --more seed 91473): a paste a column added a validation rule and
  // each paste's ClearValidation walked them all
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  yc::model::DvRule dv;
  dv.type = yc::model::DvType::List;
  dv.formula1 = "Buy,Sell";
  t.d.SetValidation(sh, Rect{5, 0, 7, 0}, dv);  // A6:A8
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("fill");
  t.d.FillRight(sh, Rect{5, 0, 7, yc::core::kMaxCols - 1});
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(1.5));
  const auto& rules = *t.d.S(sh)->validations;
  uint64_t covered = 0, ranges = 0;
  for (const auto& r : rules) {
    ranges += r.ranges.size();
    for (const Rect& x : r.ranges) covered += x.Count();
  }
  CHECK(ranges <= 3);
  CHECK(covered == 3ull * yc::core::kMaxCols);
}

TEST_CASE("Paste: one cell with data validation pasted over 50,000 cells keeps the rules few") {
  // a paste a tile (PasteRepeated) added a validation rule at each, and each tile's ClearValidation walked them all:
  // the same shape as Fill Right's conditional formats (doc_fuzz --wide --more seed 91473)
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  yc::model::DvRule dv;
  dv.type = yc::model::DvType::List;
  dv.formula1 = "Buy,Sell";
  t.d.SetValidation(sh, Rect{0, 0, 0, 0}, dv);  // A1
  yc::engine::ClipData clip = t.d.Copy(sh, Rect{0, 0, 0, 0});
  clip.carry_notes = true;  // the clipboard's copy: notes and validation come along
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("paste");
  t.d.PasteRepeated(sh, 0, 1, 50000, 1, clip);
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(3.0));
  uint64_t covered = 0, ranges = 0;
  for (const auto& r : *t.d.S(sh)->validations) {
    ranges += r.ranges.size();
    for (const Rect& x : r.ranges) covered += x.Count();
  }
  CHECK(t.d.S(sh)->validations->size() <= 2);
  CHECK(ranges <= 3);
  CHECK(covered == 50001);
}

TEST_CASE("Paste: a merged cell pasted over 100,000 cells is done in a moment") {
  // each tile's paste removed the merged areas under it by walking every merged area, and added its own: the list
  // grew a tile at a time (the same shape as the rules' ranges)
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.Merge(sh, Rect{0, 0, 1, 0}));  // A1:A2
  const yc::engine::ClipData clip = t.d.Copy(sh, Rect{0, 0, 1, 0});
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("paste");
  t.d.PasteRepeated(sh, 0, 1, 50000, 1, clip);  // B1:B100000
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(3.0));
  CHECK(t.d.S(sh)->merges->size() == 50001);
}

TEST_CASE("50,000 formulas reading a whole column are calculated in a moment, beside it and after an edit in it") {
  // doc_fuzz --wide seed 23400083 (a calculated column of =COUNT(A:A) in a table as tall as the sheet): each formula
  // asked whether A:A was changed by walking the column's tiles (propagation), and whether it waited for a cell of
  // A:A by looking up each cell of tiles the formulas beside it kept pending: a step that never ended
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("A7", "2");
  REQUIRE(t.Set("D1", "=COUNT(A:A)"));
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("fill");
  t.d.FillDown(sh, Rect{0, 3, 49999, 3});
  t.d.Commit();
  t.d.Recalc();
  CHECK(Seconds(t0) < Limit(8.0));  // 0.8 s released; before the fix a minute
  CHECK(t.d.GetValue(sh, 49999, 3).AsDouble() == 2);
  const auto t1 = std::chrono::steady_clock::now();
  t.Set("A9", "3");
  t.d.Recalc();
  CHECK(Seconds(t1) < Limit(8.0));
  CHECK(t.d.GetValue(sh, 23456, 3).AsDouble() == 3);
}

TEST_CASE("Fill: Ctrl+D down a whole column from merged cells repeats them in a moment") {
  // doc_fuzz --wide seed 24120027: a line with merged cells was pasted a line at a time, each paste walking every
  // merged area for those under it while adding one (a million squared steps: a step that never ended)
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "7");
  REQUIRE(t.d.Merge(sh, Rect{0, 0, 0, 1}));  // A1:B1
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("fill");
  t.d.FillDown(sh, Rect{0, 0, 1048575, 1});
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));
  CHECK(t.d.S(sh)->merges->size() == 1048576);
  CHECK(t.d.GetValue(sh, 700000, 0).AsDouble() == 7);
  const yc::core::Rect* m = t.d.S(sh)->MergeAt(700000, 1);
  REQUIRE(m);
  CHECK(*m == Rect{700000, 0, 700000, 1});
  REQUIRE(t.d.Undo());
  CHECK(t.d.S(sh)->merges->size() == 1);
  CHECK(t.d.GetValue(sh, 700000, 0).IsEmpty());
}

TEST_CASE("Fill: the fill handle down a whole column from merged cells repeats them in a moment") {
  // as Ctrl+D (doc_fuzz --wide seed 24120027): each merged area of the sheet was compared with every one the fill made
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "7");
  REQUIRE(t.d.Merge(sh, Rect{0, 0, 0, 1}));  // A1:B1
  const auto t0 = std::chrono::steady_clock::now();
  t.d.Begin("fill");
  t.d.AutoFill(sh, Rect{0, 0, 0, 1}, Rect{0, 0, 299999, 1});
  t.d.Commit();
  CHECK(Seconds(t0) < Limit(5.0));
  CHECK(t.d.S(sh)->merges->size() == 300000);
  const yc::core::Rect* m = t.d.S(sh)->MergeAt(200000, 1);
  REQUIRE(m);
  CHECK(*m == Rect{200000, 0, 200000, 1});
}

TEST_CASE("Undo steps share the set of #SPILL! anchors while it stays the same") {
  // doc_fuzz --wide --more seed 26000048: every undo entry kept its own copy of the blocked anchors (a whole sheet of
  // =SEQUENCE(3) filled down blocks a million of them: 64 MB a step, over 6 GB on the undo list)
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "=SEQUENCE(3)"));
  t.d.Begin("fill");
  t.d.FillDown(sh, Rect{0, 0, 199999, 0});  // each spill runs into the next anchor: 199,999 #SPILL!
  t.d.Commit();
  t.d.Recalc();
  REQUIRE(t.d.spill_blocked().size() > 190000);
  auto private_mb = [] {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
  };
  const double before = private_mb();
  for (int i = 0; i < 60; ++i) {
    t.d.Begin("edit");
    t.d.SetInput(sh, 0, 5, std::to_string(i));  // F1, away from the anchors
    t.d.Commit();
    t.d.Recalc();
  }
  CHECK(private_mb() - before < 200);  // a copy a step was ~13 MB: 60 steps over 700 MB
  REQUIRE(t.d.Undo());
  CHECK(t.d.spill_blocked().size() > 190000);
}

TEST_CASE("Flash Fill's live preview beside a region of many rows reads the rows around the entry only") {
  // chaos seed 6821: the preview asked as a person types made text of every cell of the region (each keystroke),
  // the engine took a minute and the typing behind it waited
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  // a region 1,500 rows by 1,500 columns (the entry in B, the data in A and C onward)
  std::vector<yc::core::Value> col;
  for (int i = 0; i < 1500; ++i) col.push_back(yc::core::Value::String("Ann Lee " + std::to_string(i)));
  t.d.SetValues(sh, Rect{0, 0, 1499, 0}, col);
  std::vector<yc::core::Value> block(static_cast<size_t>(1500) * 1500, yc::core::Value::Number(7));
  t.d.SetValues(sh, Rect{0, 2, 1499, 1501}, block);
  t.Set("B1", "Ann");
  std::vector<std::pair<uint32_t, std::string>> got;
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(t.d.FlashFill(sh, 1, 1, &err, &got, true) > 0);
  CHECK(Seconds(t0) < Limit(0.4));  // 0.1 s; every cell made text was 0.85 s released, a minute on the debug build in chaos
  REQUIRE(!got.empty());
  CHECK(got.front().second == "Ann");
}

TEST_CASE("Trace Dependents of a cell an array formula over millions of cells reads gives the array as one range") {
  // the array's cells were listed one by one (a set entry and a cell each), then joined back into rectangles
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "3");
  std::string err;
  REQUIRE(t.d.SetArrayFormula(sh, Rect{0, 2, 999, 4001}, "$A$1*2", &err));  // C1:FXZ1000, 4,000,000 cells
  const auto t0 = std::chrono::steady_clock::now();
  const auto all = t.d.Dependents(sh, Rect::Cell(0, 0), false);
  const double took = Seconds(t0);
  uint64_t n = 0;
  for (const Rect& x : all) n += x.Count();
  CHECK(n == 4'000'000);
  CHECK(took < Limit(0.2));
}

TEST_CASE("Showing and hiding the levels of a group of all 1,048,576 rows is fast") {
  // Outline.ShowLevels over the whole sheet's rows grouped hid and showed them a row at a time: 3 s and 8 s
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.d.GroupOutline(sh, true, 0, 1048575, true);
  t.d.GroupOutline(sh, true, 10, 20, true);
  auto t0 = std::chrono::steady_clock::now();
  t.d.ShowOutlineLevel(sh, true, 1);
  CHECK(Seconds(t0) < Limit(1.0));
  CHECK(t.d.wb().sheets[0]->rows->IsHidden(0));
  CHECK(t.d.wb().sheets[0]->rows->IsHidden(1048575));
  t0 = std::chrono::steady_clock::now();
  t.d.ShowOutlineLevel(sh, true, 2);
  CHECK(Seconds(t0) < Limit(1.0));
  CHECK_FALSE(t.d.wb().sheets[0]->rows->IsHidden(0));
  CHECK(t.d.wb().sheets[0]->rows->IsHidden(15));  // the inner group, level 2, stays hidden
  CHECK_FALSE(t.d.wb().sheets[0]->rows->IsHidden(21));
  t.d.ShowOutlineLevel(sh, true, 1);
  t0 = std::chrono::steady_clock::now();
  t.d.SetGroupCollapsed(sh, true, 0, 1, false);  // expand the outer group: its own rows show, the inner stays
  CHECK(Seconds(t0) < Limit(1.0));
  CHECK_FALSE(t.d.wb().sheets[0]->rows->IsHidden(1048575));
  CHECK(t.d.wb().sheets[0]->rows->IsHidden(15));
}

TEST_CASE("A series down a table's column grows the table once, and a whole column's series is fast") {
  // Excel 16 (measured 2026-11-20): Range("B2:B20").DataSeries grows the table A1:D6 to A1:D20 and its calculated
  // column with it. A cell at a time, each cell below the table grew it by a row (its calculated column copied, its
  // headers synced), and Python's DataSeries over B2:XFD1048576 didn't answer in two minutes
  using yc::core::Rect;
  for (const bool whole : {false, true}) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    t.Set("A1", "Name");
    t.Set("B1", "Qty");
    t.Set("C1", "Price");
    t.Set("D1", "Total");
    for (int i = 2; i <= 6; ++i) {
      t.Set(("A" + std::to_string(i)).c_str(), "x" + std::to_string(i));
      t.Set(("B" + std::to_string(i)).c_str(), std::to_string(i * 10));
      t.Set(("C" + std::to_string(i)).c_str(), std::to_string(i));
      t.Set(("D" + std::to_string(i)).c_str(), "=B" + std::to_string(i) + "*C" + std::to_string(i));
    }
    REQUIRE(t.d.AddTable(sh, Rect{0, 0, 5, 3}, true, "") != 0);
    const uint32_t last = whole ? 1048575 : 19;
    const auto t0 = std::chrono::steady_clock::now();
    t.d.FillSeries(sh, Rect{1, 1, last, 1}, true, 0, 0, 1.0, false, 0.0);
    CHECK(Seconds(t0) < Limit(5.0));
    const auto& tables = *t.d.wb().sheets[0]->tables;
    REQUIRE(tables.size() == 1);
    CHECK(tables[0].range == Rect{0, 0, last, 3});
    CHECK(t.d.GetValue(sh, last, 1).AsDouble() == 20 + (last - 1));
    if (!whole) {
      CHECK(t.N("B20") == 38);
      CHECK(t.d.GetFormulaA1(sh, 19, 3) == "=B20*C20");
    }
  }
}

TEST_CASE("A data table past 20,000,000 cells is refused with a message") {
  // Range("B2:XFD1048576").Table(...) asked for 17 billion results at once and answered "bad allocation"
  using yc::core::Rect;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("B2", "=A1*2");
  const yc::core::CellPos in{0, 0};
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(t.d.AddDataTable(sh, Rect{1, 1, 1048575, yc::core::kMaxCols - 1}, nullptr, &in, &err));
  CHECK(err.find("too large") != std::string::npos);
  CHECK(Seconds(t0) < Limit(0.5));
  CHECK(t.d.wb().sheets[0]->data_tables->empty());
}

TEST_CASE("a change at the top of a chain of a million formulas marks them pending in a moment") {
  // basics seq430649: =G3 filled down a million rows read each other in a chain, and Ctrl+' over G3 took 7.5 s, nearly
  // all of it finding what to calculate: a round per cell, each scanning its tile's entries (Propagate)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("G1", "1");
  t.Set("G2", "=G1+1");
  t.d.FillDown(sh, yc::core::Rect{1, 6, 1048575, 6});
  t.d.Recalc();
  REQUIRE(t.N("G1048576") == 1048576);
  const auto t0 = std::chrono::steady_clock::now();
  t.Set("G1", "2");
  t.d.Recalc();
  CHECK(t.N("G1048576") == 1048577);
  CHECK(Seconds(t0) < Limit(5.0));  // 1.5 s here; 7.5 s and more before
}

TEST_CASE("a million formulas that each read their own cell are found circular in a moment") {
  // basics seq444742: =G1+1 in G1 filled down the column took 15 s, a thrown exception for each formula's read of
  // itself (EvaluatePendingSerial). What each shows doesn't change: the value it was filled with, and the cycle is
  // reported
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("G1", "=G1+1");
  t.d.Recalc();
  REQUIRE(t.N("G1") == 0);
  const auto t0 = std::chrono::steady_clock::now();
  t.d.FillDown(sh, yc::core::Rect{0, 6, 1048575, 6});
  t.d.Recalc();
  CHECK(t.d.last_stats().self_reads >= 1048575);  // each found without an exception (a time limit can't tell 2.6 s from 5.5)
  CHECK(Seconds(t0) < Limit(10.0));
  CHECK(t.F("G1048576") == "=G1048576+1");
  CHECK(t.N("G1048576") == 0);
  CHECK(!t.d.circular_cells.empty());
}
