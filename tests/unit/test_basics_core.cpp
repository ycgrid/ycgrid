// SPDX-License-Identifier: Apache-2.0
// Basic operations scripts\basics.py found broken in a state, fixed in the engine so every path (keys, menus, Python)
// gets the fix; each case fails without its fix.
#include <catch2/catch_test_macros.hpp>

#include "io/content_dump.h"
#include "model/theme.h"
#include "test_helpers.h"

#include <functional>
#include <string>

using yc::core::Rect;

TEST_CASE("Basics: copying cells a filter hides copies them, as Excel (it said the selection was too large)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "h");
  for (int i = 1; i <= 5; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string(i * 10));
  REQUIRE(t.d.SetAutoFilter(sh, Rect{0, 0, 5, 0}));
  yc::model::FilterColumn fc;
  fc.kind = 1;
  fc.values = {"10", "30", "40", "50"};
  fc.blanks = false;
  t.d.SetFilterColumn(sh, fc);
  REQUIRE(t.d.S(sh)->rows->IsHidden(2));
  // Excel 16, measured: B3 alone (its row filtered out) copies B3; a range with shown rows copies only those
  const auto one = t.d.CopyShown(sh, Rect{2, 0, 2, 0});
  REQUIRE(one.rows == 1);
  t.d.Paste(sh, 0, 3, one);
  CHECK(t.N("D1") == 20);
  const auto some = t.d.CopyShown(sh, Rect{1, 0, 3, 0});
  CHECK(some.rows == 2);
}

TEST_CASE("Basics: the fill handle on whole rows or whole columns fills the cells that hold something (it crashed)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t last_col = yc::core::kMaxCols - 1, last_row = t.d.S(sh)->row_limit - 1;
  t.Set("A3", "1");
  t.Set("B3", "Item1");
  t.Set("C3", "=A3*2");
  t.Set("A4", "2");
  t.Set("B4", "Item2");
  t.Set("C4", "=A4*2");
  // rows 3:4 dragged down to row 7: each column goes on as its own series, as in Excel
  t.d.AutoFill(sh, Rect{2, 0, 3, last_col}, Rect{2, 0, 6, last_col});
  t.d.Recalc();
  CHECK(t.N("A7") == 5);
  CHECK(t.d.GetInputText(sh, 6, 1) == "Item5");
  CHECK(t.F("C7") == "=A7*2");
  CHECK(t.N("C7") == 10);
  // whole columns A:B dragged right to D
  t.d.AutoFill(sh, Rect{0, 0, last_row, 1}, Rect{0, 0, last_row, 3});
  CHECK_FALSE(t.V("C3").IsEmpty());  // the rows that hold something got their series
  CHECK_FALSE(t.V("D4").IsEmpty());
  CHECK(t.V("C9").IsEmpty());
  // empty rows past the used range: nothing to fill, and nothing breaks
  t.d.AutoFill(sh, Rect{50, 0, 51, last_col}, Rect{50, 0, 55, last_col});
  CHECK(t.V("A53").IsEmpty());
}

TEST_CASE("Basics: Undo of a spill that grew the sheet past its last row takes the rows back (dumps differed)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t rows = t.d.S(sh)->row_limit;
  t.d.Recalc();
  const std::string before = yc::io::ContentDump(t.d.wb());
  t.d.Begin("type");
  t.d.SetInput(sh, rows - 1, 0, "=SEQUENCE(3)");
  t.d.Commit();
  t.d.Recalc();
  REQUIRE(t.d.S(sh)->row_limit > rows);  // kept on purpose: the sheet grows for the spill
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.d.S(sh)->row_limit == rows);
  CHECK(yc::io::ContentDump(t.d.wb()) == before);
  REQUIRE(t.d.Redo());
  t.d.Recalc();
  CHECK(t.d.S(sh)->row_limit > rows);
  CHECK(t.N(("A" + std::to_string(rows + 2)).c_str()) == 3);
}

TEST_CASE("Basics: Redo of a fill through a spill range gives the spill back, not #SPILL! (basics.py sequence)") {
  // basics.py --sequence plain:spill_formula,insert_cells,shift_click,fill_down,autofit: Redo copied the values the
  // array had spilled as typed values, and they blocked it
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("C4", "5");
  t.Set("C5", "6");
  t.Set("B3", "=SEQUENCE(3)");
  t.d.Recalc();
  REQUIRE(t.N("B5") == 3);
  t.d.Begin("fill");
  t.d.FillDown(sh, Rect{3, 1, 4, 3});  // B4:D5 from row 4: through the spill
  t.d.Commit();
  t.d.Recalc();
  const std::string after = yc::io::ContentDump(t.d.wb());
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  REQUIRE(t.d.Redo());
  t.d.Recalc();
  CHECK(yc::io::ContentDump(t.d.wb()) == after);
  CHECK(t.N("B3") == 1);  // Excel 16, measured: the spill stays (B5 is still 3); it was #SPILL!
  CHECK(t.N("B5") == 3);
  CHECK(t.N("C5") == 5);
}

TEST_CASE("Basics: Fill Down from a spilled cell fills blanks; over the anchor it replaces the array (measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("F3", "=SEQUENCE(3)");
  t.Set("F6", "x");
  t.Set("F7", "y");
  t.Set("M2", "7");
  t.Set("M3", "=SEQUENCE(3)");
  t.Set("H3", "=SEQUENCE(1,3)");
  t.Set("K3", "x");
  t.d.Recalc();
  t.d.FillDown(sh, Rect{4, 5, 6, 5});  // F5:F7
  t.d.FillDown(sh, Rect{1, 12, 3, 12});  // M2:M4
  t.d.FillRight(sh, Rect{2, 8, 2, 10});  // I3:K3
  t.d.Recalc();
  CHECK(t.N("F5") == 3);
  CHECK(t.V("F6").IsEmpty());
  CHECK(t.V("F7").IsEmpty());
  CHECK(t.N("M3") == 7);
  CHECK(t.N("M4") == 7);
  CHECK(t.V("M5").IsEmpty());
  CHECK(t.N("J3") == 3);
  CHECK(t.V("K3").IsEmpty());
}

TEST_CASE("Basics: Undo of a column width over a spill leaves the spill (it showed #SPILL!)") {
  // basics.py --sequence plain:spill_formula,insert_cells,shift_click,fill_down,autofit: the width's Undo copied the
  // column back, the spilled values as typed values, and the array was blocked by its own values
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B3", "=SEQUENCE(3)");
  t.d.Recalc();
  REQUIRE(t.N("B5") == 3);
  const std::string before = yc::io::ContentDump(t.d.wb());
  t.d.Begin("width");
  t.d.SetColWidth(sh, 1, 1, 120);
  t.d.Commit();
  t.d.TakeChanges();  // published, as the engine does after every command
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.N("B3") == 1);
  CHECK(t.N("B5") == 3);
  CHECK(yc::io::ContentDump(t.d.wb()) == before);
  REQUIRE(t.d.Redo());
  t.d.Recalc();
  CHECK(t.N("B3") == 1);
}

TEST_CASE("Basics: Undo / Redo of widths and hidden rows across a spill keep it (B3 showed #SPILL!, B4:B5 typed)") {
  // basics.py --sequence plain:spill_formula,insert_cells,shift_click,fill_down,autofit: the Undo of the AutoFit
  // marked the whole column changed as if typed into, which took the array's spill record away and kept its values
  // as typed values; the array then found them in its way
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (int i = 0; i < 5; ++i) {
    const std::string r = std::to_string(i + 2);
    t.Set(("B" + r).c_str(), std::to_string((i + 1) * 10));
    t.Set(("C" + r).c_str(), std::to_string(i + 1));
    t.Set(("D" + r).c_str(), "=B" + r + "*C" + r);
  }
  t.Set("B7", "=SUM(B2:B6)");
  t.d.ClearUndo();
  auto step = [&](const char* label, const std::function<void()>& act) {
    t.d.Begin(label);
    act();
    t.d.Commit();
    t.d.Recalc();
    t.d.TakeChanges();  // published between commands, as the engine does
  };
  auto again = [&](bool redo) {
    REQUIRE((redo ? t.d.Redo() : t.d.Undo()));
    t.d.Recalc();
    t.d.TakeChanges();
  };
  auto spilled = [&] { return t.N("B3") == 1 && t.N("B4") == 2 && t.N("B5") == 3 && t.d.S(sh)->spills->size() == 1; };
  const std::string start = yc::io::ContentDump(t.d.wb());
  step("type", [&] { t.d.SetInput(sh, 2, 1, "=SEQUENCE(3)"); });  // blocked by B4:B5
  step("insert", [&] { t.d.InsertCells(sh, Rect{3, 1, 3, 1}, true); });
  step("fill", [&] { t.d.FillDown(sh, Rect{3, 1, 4, 3}); });  // B4:D5: B5 empties, the array spills
  REQUIRE(spilled());
  step("width", [&] { t.d.SetColWidth(sh, 1, 1, 70); });
  step("autofit", [&] { t.d.SetColWidth(sh, 1, 1, 50); });
  step("hide", [&] { t.d.HideRows(sh, 3, 3, true); });  // row 4, inside the spill
  CHECK(spilled());
  const std::string end = yc::io::ContentDump(t.d.wb());
  for (int i = 0; i < 3; ++i) {  // hide, AutoFit, width: no cell changes
    again(false);
    CHECK(spilled());
  }
  for (int i = 0; i < 3; ++i) again(false);  // fill, insert, typing
  CHECK(yc::io::ContentDump(t.d.wb()) == start);
  for (int i = 0; i < 6; ++i) {
    again(true);
    if (i >= 2) CHECK(spilled());
  }
  CHECK(yc::io::ContentDump(t.d.wb()) == end);
  CHECK(t.d.S(sh)->rows->IsHidden(3));
}

TEST_CASE("Basics: setting a width or height a column or row already has leaves no Undo step (AutoFit made two)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const int32_t w = t.d.S(sh)->cols->Size(1), h = t.d.S(sh)->rows->Size(2);
  const size_t depth = t.d.UndoDepth();
  t.d.Begin("Column Width");
  t.d.SetColWidth(sh, 1, 1, w);  // the first click of a double-click on the border: the width it had
  t.d.Commit();
  t.d.Begin("Row Height");
  t.d.SetRowHeight(sh, 2, 2, h);
  t.d.Commit();
  CHECK(t.d.UndoDepth() == depth);
  t.d.Begin("Column Width");
  t.d.SetColWidth(sh, 1, 1, w + 30);
  t.d.Commit();
  CHECK(t.d.UndoDepth() == depth + 1);
}

TEST_CASE("Basics: MergeCells = True keeps the upper-left cell's formats and resets the others (Excel, measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B5", "3");
  t.Set("C5", "x");
  t.d.ModifyStyle(sh, Rect{4, 0, 4, 0}, [](yc::model::Style& s) { s.bold = true; });
  t.d.ModifyStyle(sh, Rect{4, 1, 4, 1}, [](yc::model::Style& s) { s.italic = true; });
  REQUIRE(t.d.Merge(sh, Rect{4, 0, 4, 2}, false, false, true));
  CHECK(t.N("A5") == 3);
  CHECK(t.V("C5").IsEmpty());
  const auto& st = *t.d.wb().styles;
  CHECK(st.Get(t.d.S(sh)->GetStyle(4, 0)).bold);  // the upper-left cell's own format
  CHECK_FALSE(st.Get(t.d.S(sh)->GetStyle(4, 0)).italic);
  CHECK(t.d.S(sh)->GetOwnStyle(4, 1) == 0);  // the others Normal
  CHECK(t.d.S(sh)->GetOwnStyle(4, 2) == 0);
}

TEST_CASE("Basics: Undo of a theme, its fonts or its effects gives back all of it (the default font and effects stayed)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B2", "x");
  const std::string before = yc::io::ContentDump(t.d.wb());
  const auto& fonts = yc::model::BuiltinThemeFonts();
  REQUIRE(fonts.size() > 1);
  // Page Layout > Fonts with another minor font: the default font follows it, and Undo / Redo take it with the theme
  yc::model::Theme th = *t.d.wb().theme;
  size_t pick = 0;
  while (pick + 1 < fonts.size() && fonts[pick].minor == th.minor_font) ++pick;
  const auto& other = fonts[pick];
  REQUIRE(other.minor != th.minor_font);
  th.minor_font = other.minor;
  th.major_font = other.major;
  t.d.Begin("Theme");
  t.d.SetTheme(th);
  t.d.Commit();
  t.d.TakeChanges();
  REQUIRE(t.d.wb().styles->default_font == other.minor);
  REQUIRE(t.d.Undo());
  t.d.TakeChanges();
  CHECK(yc::io::ContentDump(t.d.wb()) == before);
  CHECK(t.d.wb().styles->default_font != other.minor);
  REQUIRE(t.d.Redo());
  t.d.TakeChanges();
  CHECK(t.d.wb().styles->default_font == other.minor);
  REQUIRE(t.d.Undo());
  t.d.TakeChanges();
  // Page Layout > Effects: one Undo step (it had none), and Undo / Redo take it back and forth
  const size_t depth = t.d.UndoDepth();
  t.d.Begin("Theme Effects");
  t.d.SetThemeEffects("effects-a");
  t.d.Commit();
  t.d.TakeChanges();
  CHECK(t.d.UndoDepth() == depth + 1);
  REQUIRE(t.d.Undo());
  t.d.TakeChanges();
  CHECK(yc::io::ContentDump(t.d.wb()) == before);
  REQUIRE(t.d.Redo());
  t.d.TakeChanges();
  CHECK(t.d.wb().store->at("view.theme_effects") == "effects-a");
  // the same effects again change nothing and leave no step; read-only mode refuses them
  const size_t depth2 = t.d.UndoDepth();
  t.d.Begin("Theme Effects");
  t.d.SetThemeEffects("effects-a");
  t.d.Commit();
  CHECK(t.d.UndoDepth() == depth2);
  t.d.ui_read_only = true;
  t.d.Begin("Theme Effects");
  t.d.SetThemeEffects("effects-b");
  t.d.Commit();
  t.d.ui_read_only = false;
  CHECK(t.d.wb().store->at("view.theme_effects") == "effects-a");
  (void)sh;
}
