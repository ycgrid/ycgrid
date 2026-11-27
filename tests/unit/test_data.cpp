// SPDX-License-Identifier: Apache-2.0
// Data tools: Remove Duplicates, Text to Columns, outline, moving cells, Evaluate Formula
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/ycg.h"

using yc::core::Rect;

TEST_CASE("Remove Duplicates") {
  yctest::Doc t;
  const char* a[] = {"Name", "x", "y", "X", "z", "y"};
  const char* b[] = {"Qty", "1", "2", "1", "3", "9"};
  for (int i = 0; i < 6; ++i) {
    t.Set(("A" + std::to_string(i + 1)).c_str(), a[i]);
    t.Set(("B" + std::to_string(i + 1)).c_str(), b[i]);
  }
  // by both columns: only "X,1" repeats "x,1" (text is case-insensitive)
  CHECK(t.d.RemoveDuplicates(t.sheet(), Rect{0, 0, 5, 1}, {}, true) == 1);
  t.d.Recalc();
  CHECK(t.T("A1") == "Name");
  CHECK(t.T("A4") == "z");
  CHECK(t.T("A5") == "y");
  CHECK(t.V("A6").IsEmpty());
  // by column A only: the second y goes
  CHECK(t.d.RemoveDuplicates(t.sheet(), Rect{0, 0, 4, 1}, {0}, true) == 1);
  t.d.Recalc();
  CHECK(t.T("A4") == "z");
  CHECK(t.V("A5").IsEmpty());
  CHECK(t.N("B4") == 3);
}

TEST_CASE("Remove Duplicates leaves out a last row of totals") {
  // Excel 16 (measured, dedupe_tot_ cases): a last row whose formula calls SUM, AVERAGE, COUNT, COUNTA, MAX, MIN,
  // SUBTOTAL or AGGREGATE is neither compared nor moved; the rows above close up under it
  auto fill = [](yctest::Doc& t, const char* last_b) {
    const char* a[] = {"K", "a", "d", "d", "x", "d"};
    for (int i = 0; i < 6; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), a[i]);
    for (int i = 2; i <= 5; ++i) t.Set(("B" + std::to_string(i)).c_str(), std::to_string(i).c_str());
    t.Set("B6", last_b);
    t.d.Recalc();
  };
  {
    yctest::Doc t;
    fill(t, "=IF(TRUE,SUM(B2:B5))");
    CHECK(t.d.RemoveDuplicates(t.sheet(), Rect{0, 0, 5, 1}, {0}, true) == 1);  // only the second d of the rows above
    t.d.Recalc();
    CHECK(t.T("A3") == "d");
    CHECK(t.T("A4") == "x");
    CHECK(t.V("A5").IsEmpty());
    CHECK(t.T("A6") == "d");  // the total stays where it was
    CHECK(t.F("B6") == "=IF(TRUE,SUM(B2:B5))");
  }
  for (const char* plain : {"9", "=B5", "=PRODUCT(B2:B5)", "=ROWS(B2:B5)"}) {
    yctest::Doc t;
    fill(t, plain);
    CHECK(t.d.RemoveDuplicates(t.sheet(), Rect{0, 0, 5, 1}, {0}, true) == 2);  // a row as any: its d goes too
    t.d.Recalc();
    CHECK(t.T("A4") == "x");
    CHECK(t.V("A5").IsEmpty());
    CHECK(t.V("A6").IsEmpty());
  }
}

TEST_CASE("Sort and Remove Duplicates over cells that meet a table work only inside one") {
  // Excel 16 (measured, tblrange_ cases): a table inside the range, over part of it or below it refuses both; the
  // table's whole range and part of its body are sorted; an array of one cell is sorted as a cell
  auto fill = [](yctest::Doc& t) {
    const char* a[] = {"K", "b", "a", "b", "c"};
    for (int i = 0; i < 5; ++i) {
      t.Set(("A" + std::to_string(i + 1)).c_str(), a[i]);
      t.Set(("B" + std::to_string(i + 1)).c_str(), i == 0 ? "V" : std::to_string(i).c_str());
    }
  };
  for (const Rect tbl : {Rect{1, 0, 2, 1}, Rect{3, 1, 6, 2}, Rect{4, 0, 6, 1}}) {  // A2:B3 inside, B4:C7 across, A5:B7 below
    yctest::Doc t;
    fill(t);
    REQUIRE(t.d.AddTable(t.sheet(), tbl, true, "") != 0);
    t.d.Recalc();
    const std::string a2 = t.T("A2"), a3 = t.T("A3");
    const uint32_t blocks = t.d.protect_blocks();
    t.d.Sort(t.sheet(), Rect{0, 0, 4, 1}, {{0, false}}, true);
    CHECK(t.d.protect_blocks() == blocks + 1);
    CHECK(t.d.RemoveDuplicates(t.sheet(), Rect{0, 0, 4, 1}, {0}, true) == 0);
    CHECK(t.d.protect_blocks() == blocks + 2);
    t.d.Recalc();
    CHECK(t.T("A2") == a2);
    CHECK(t.T("A3") == a3);
  }
  {
    yctest::Doc t;
    fill(t);
    REQUIRE(t.d.AddTable(t.sheet(), Rect{0, 0, 4, 1}, true, "") != 0);
    t.d.Sort(t.sheet(), Rect{0, 0, 4, 1}, {{0, false}}, true);  // the table's whole range
    t.d.Recalc();
    CHECK(t.T("A2") == "a");
    CHECK(t.d.RemoveDuplicates(t.sheet(), Rect{1, 0, 4, 1}, {0}, false) == 1);  // its body
  }
  {
    yctest::Doc t;
    fill(t);
    std::string err;
    REQUIRE(t.d.SetArrayFormula(t.sheet(), Rect{1, 2, 1, 2}, "B2*10", &err));  // C2, one cell
    t.d.Sort(t.sheet(), Rect{0, 0, 4, 2}, {{0, false}}, true);
    t.d.Recalc();
    CHECK(t.T("A2") == "a");
    CHECK(t.T("A3") == "b");
  }
}

TEST_CASE("Sort of cells inside a table sorts its body, and a one-cell array stays an array") {
  yctest::Doc t;
  const char* a[] = {"K", "b", "c", "a", "d"};
  for (int i = 0; i < 5; ++i) {
    t.Set(("A" + std::to_string(i + 1)).c_str(), a[i]);
    t.Set(("B" + std::to_string(i + 1)).c_str(), i == 0 ? "V" : std::to_string(i).c_str());
  }
  REQUIRE(t.d.AddTable(t.sheet(), Rect{0, 0, 4, 1}, true, "") != 0);
  // A2:A3 only, with a header named: Excel 16 sorts A2:B5 (measured, tblsort_ cases)
  t.d.Sort(t.sheet(), Rect{1, 0, 2, 0}, {{0, false}}, true);
  t.d.Recalc();
  CHECK(t.T("A2") == "a");
  CHECK(t.N("B2") == 3);
  CHECK(t.T("A5") == "d");
  std::string err;
  yctest::Doc u;
  for (int i = 0; i < 5; ++i) {
    u.Set(("A" + std::to_string(i + 1)).c_str(), a[i]);
    u.Set(("B" + std::to_string(i + 1)).c_str(), i == 0 ? "V" : std::to_string(i).c_str());
  }
  REQUIRE(u.d.SetArrayFormula(u.sheet(), Rect{1, 2, 1, 2}, "B2:B3*10", &err));  // C2, the row of b: one cell
  u.d.Sort(u.sheet(), Rect{0, 0, 4, 2}, {{0, false}}, true);
  u.d.Recalc();
  CHECK(u.T("A3") == "b");
  CHECK(u.F("C3") == "=B3:B4*10");
  CHECK(u.N("C3") == 10);  // its first value: an array still (as a plain formula it spilled)
  CHECK(u.V("C4").IsEmpty());
  REQUIRE(u.d.S(u.sheet())->spills->size() == 1);
  CHECK(u.d.S(u.sheet())->spills->begin()->second.cse);
}

TEST_CASE("Fill Down past a table's last row makes room where the rows below hold something") {
  // Excel 16 (measured, tblfill_down_ cases): table rows are inserted from the first row below that holds something to
  // the fill's end; empty rows before it are taken in; a fill wider than the table writes over the cells
  auto make = [](yctest::Doc& t, const char* e4, const char* e5) {
    t.Set("E1", "Calc");
    t.Set("E2", "1");
    t.Set("E3", "2");
    if (*e4) t.Set("E4", e4);
    if (*e5) t.Set("E5", e5);
    REQUIRE(t.d.AddTable(t.sheet(), Rect{0, 4, 2, 4}, true, "") != 0);
  };
  {
    yctest::Doc t;
    make(t, "x", "");
    t.d.FillDown(t.sheet(), Rect{2, 4, 4, 4});  // E3:E5: x is pushed below the fill
    t.d.Recalc();
    CHECK(t.N("E4") == 2);
    CHECK(t.N("E5") == 2);
    CHECK(t.T("E6") == "x");
    CHECK(t.d.S(t.sheet())->tables->front().range.r1 == 4);
  }
  {
    yctest::Doc t;
    make(t, "", "y");
    t.d.FillDown(t.sheet(), Rect{2, 4, 4, 4});  // E4 empty is taken in, room is made for E5 only
    t.d.Recalc();
    CHECK(t.N("E5") == 2);
    CHECK(t.T("E6") == "y");
    CHECK(t.V("E7").IsEmpty());
  }
  {
    yctest::Doc t;
    make(t, "x", "y");
    t.d.FillDown(t.sheet(), Rect{2, 3, 3, 5});  // D3:F4, wider than the table: E4 is written over
    t.d.Recalc();
    CHECK(t.N("E4") == 2);
    CHECK(t.T("E5") == "y");
  }
}

TEST_CASE("Text to Columns") {
  yctest::Doc t;
  t.Set("A1", "'a,1,\"x,y\"");
  t.Set("A2", "'b,,2024/1/5");
  t.Set("A3", "7");
  yc::engine::TextToColumnsOptions o;
  o.tab = false;
  o.comma = true;
  CHECK(t.d.TextToColumns(t.sheet(), Rect{0, 0, 2, 0}, 0, 0, o) == 3);
  t.d.Recalc();
  CHECK(t.T("A1") == "a");
  CHECK(t.N("B1") == 1);
  CHECK(t.T("C1") == "x,y");
  CHECK(t.V("B2").IsEmpty());
  CHECK(t.V("C2").IsNumber());  // a date
  CHECK(t.N("A3") == 7);
  // consecutive delimiters, skipped and text columns, another destination
  t.Set("E1", "'p  q r");
  o = {};
  o.tab = false;
  o.space = true;
  o.consecutive = true;
  o.types = {2, 9, 1};
  t.d.TextToColumns(t.sheet(), Rect::Cell(0, 4), 5, 4, o);
  t.d.Recalc();
  CHECK(t.T("E6") == "p");
  CHECK(t.T("F6") == "r");
  // fixed width
  t.Set("H1", "'AB  12345");
  o = {};
  o.fixed = true;
  o.breaks = {2, 4};
  t.d.TextToColumns(t.sheet(), Rect::Cell(0, 7), 0, 7, o);
  t.d.Recalc();
  CHECK(t.T("H1") == "AB");
  CHECK(t.V("I1").IsEmpty());
  CHECK(t.N("J1") == 12345);
}

TEST_CASE("outline groups end at the sheet's last row or column") {
  // a whole column's end marker (0xFFFFFFFF) from Python set four billion levels and ran out of memory
  yctest::Doc t;
  t.d.GroupOutline(t.sheet(), true, 0, 0xFFFFFFFFu, true);
  t.d.GroupOutline(t.sheet(), false, 3, 0xFFFFFFFFu, true);
  const auto& s = *t.d.wb().sheets[0];
  CHECK(s.rows->Levels().size() == 1048576);
  CHECK(s.rows->Level(1048575) == 1);
  CHECK(s.cols->Levels().size() == 16384 - 3);
  CHECK(s.cols->Level(16383) == 1);
}

TEST_CASE("outline groups") {
  yctest::Doc t;
  t.d.GroupOutline(t.sheet(), true, 1, 4, true);
  t.d.GroupOutline(t.sheet(), true, 2, 3, true);
  const auto& rows = *t.d.wb().sheets[0]->rows;
  CHECK(rows.Level(1) == 1);
  CHECK(rows.Level(2) == 2);
  CHECK(rows.MaxLevel() == 2);
  t.d.ShowOutlineLevel(t.sheet(), true, 1);
  CHECK(t.d.wb().sheets[0]->rows->IsHidden(1));
  CHECK(t.d.wb().sheets[0]->rows->IsHidden(3));
  t.d.SetGroupCollapsed(t.sheet(), true, 1, 1, false);
  CHECK_FALSE(t.d.wb().sheets[0]->rows->IsHidden(1));
  CHECK(t.d.wb().sheets[0]->rows->IsHidden(2));  // the inner group stays collapsed
  uint32_t a = 0, b = 0;
  REQUIRE(t.d.OutlineBlock(t.sheet(), true, 3, 2, &a, &b));
  CHECK(a == 2);
  CHECK(b == 3);
  // saved, and shifted by inserted rows
  t.d.GroupOutline(t.sheet(), false, 2, 3, true);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  CHECK(res.wb.sheets[0]->rows->Level(2) == 2);
  CHECK(res.wb.sheets[0]->cols->Level(3) == 1);
  t.d.InsertRows(t.sheet(), 0, 2);
  CHECK(t.d.wb().sheets[0]->rows->Level(4) == 2);
  t.d.GroupOutline(t.sheet(), true, 3, 6, false);
  CHECK(t.d.wb().sheets[0]->rows->Level(3) == 0);
  CHECK(t.d.wb().sheets[0]->rows->Level(4) == 1);
  t.d.ClearOutline(t.sheet(), true);
  CHECK(t.d.wb().sheets[0]->rows->MaxLevel() == 0);
}

TEST_CASE("Ungroup keeps collapsed rows hidden, Clear Outline shows them (Excel 16, measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const auto rows = [&] { return t.d.wb().sheets[0]->rows; };
  t.d.GroupOutline(sh, true, 1, 3, true);
  t.d.ShowOutlineLevel(sh, true, 1);
  REQUIRE(rows()->IsHidden(2));
  t.d.GroupOutline(sh, true, 1, 3, false);  // Alt+Shift+Left, Range.Ungroup
  CHECK(rows()->Level(2) == 0);
  CHECK(rows()->IsHidden(1));
  CHECK(rows()->IsHidden(3));
  t.d.HideRows(sh, 1, 3, false);
  t.d.GroupOutline(sh, true, 1, 3, true);
  t.d.ShowOutlineLevel(sh, true, 1);
  t.d.ClearOutline(sh, true, 1, 2);  // Range("A2:A3").ClearOutline: those rows leave the group and are shown
  CHECK_FALSE(rows()->IsHidden(1));
  CHECK_FALSE(rows()->IsHidden(2));
  CHECK(rows()->IsHidden(3));
  CHECK(rows()->Level(3) == 1);
  t.d.ClearOutline(sh, true);  // the whole sheet's
  CHECK_FALSE(rows()->IsHidden(3));
  CHECK(rows()->MaxLevel() == 0);
}

TEST_CASE("moving cells keeps formulas pointing at them") {
  yctest::Doc t;
  t.Set("A1", "5");
  t.Set("A2", "=A1*2");
  t.Set("C1", "=SUM(A1:A2)");
  t.Set("D1", "=$A$1+1");
  REQUIRE(t.d.MoveCells(t.sheet(), Rect{0, 0, 1, 0}, t.sheet(), 4, 1));  // A1:A2 -> B5:B6
  t.d.Recalc();
  CHECK(t.V("A1").IsEmpty());
  CHECK(t.N("B5") == 5);
  CHECK(t.F("B6") == "=B5*2");
  CHECK(t.N("B6") == 10);
  CHECK(t.F("C1") == "=SUM(B5:B6)");
  CHECK(t.F("D1") == "=$B$5+1");
  CHECK(t.N("C1") == 15);
  // to another sheet
  const uint32_t s2 = t.d.AddSheet("Other");
  REQUIRE(t.d.MoveCells(t.sheet(), Rect::Cell(4, 1), s2, 0, 0));
  t.d.Recalc();
  CHECK(t.F("C1") == "=SUM(B6:B6)");  // its first cell cut to another sheet leaves it (Excel, measured)
  CHECK(t.F("B6") == "=Other!A1*2");
  CHECK(t.N("B6") == 10);
  t.d.Undo();
}

TEST_CASE("Evaluate Formula steps") {
  yctest::Doc t;
  t.Set("A1", "3");
  t.Set("A2", "4");
  t.Set("B1", "=SUM(A1:A2)*A1+1");
  const auto steps = t.d.EvaluateSteps(t.sheet(), 0, 1);
  REQUIRE(steps.size() >= 3);
  CHECK(steps[0].text == "=SUM(A1:A2)*A1+1");
  CHECK(steps[0].text.substr(steps[0].hl_begin, steps[0].hl_len) == "SUM(A1:A2)");
  CHECK(steps[1].text == "=7*A1+1");
  CHECK(steps[1].text.substr(steps[1].hl_begin, steps[1].hl_len) == "A1");
  CHECK(steps.back().text == "=22");
  CHECK(steps.back().hl_len == 0);
}

#include "engine/rule_eval.h"
#include "formula/datetime.h"

TEST_CASE("icon sets") {
  yctest::Doc t;
  for (int i = 0; i < 10; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string((i + 1) * 10));
  yc::model::CfRule r;
  r.type = yc::model::CfType::IconSet;
  r.icon_style = yc::model::kIcon3Arrows;
  r.icon_thresholds = yc::model::DefaultIconThresholds(r.icon_style);
  r.ranges = {Rect{0, 0, 9, 0}};
  t.d.AddCondFormat(t.sheet(), r);
  {
    yc::engine::CondFormatEval ev(t.d.wb(), t.sheet());
    yc::engine::CfVisual v;
    REQUIRE(ev.Eval(0, 0, &v));
    CHECK(v.icon == 0);  // 10: lowest third
    v = {};
    ev.Eval(4, 0, &v);
    CHECK(v.icon == 1);  // 50
    v = {};
    ev.Eval(9, 0, &v);
    CHECK(v.icon == 2);  // 100
  }
  // numbers as thresholds, reversed, icon only; saved
  yc::model::CfRule r2 = r;
  r2.icon_style = yc::model::kIcon4Ratings;
  r2.icon_thresholds = {{1, 25, "", true}, {1, 50, "", true}, {1, 75, "", false}};
  r2.icon_reverse = true;
  r2.icon_only = true;
  r2.ranges = {Rect{0, 1, 9, 1}};
  for (int i = 0; i < 10; ++i) t.Set(("B" + std::to_string(i + 1)).c_str(), std::to_string((i + 1) * 10));
  t.d.AddCondFormat(t.sheet(), r2);
  yc::engine::CondFormatEval ev(t.d.wb(), t.sheet());
  yc::engine::CfVisual v;
  ev.Eval(4, 1, &v);  // 50 >= 25 and >= 50: index 2, reversed -> 1
  CHECK(v.icon == 1);
  CHECK(v.icon_only);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& cfs = *res.wb.sheets[0]->cond_formats;
  REQUIRE(cfs.size() == 2);
  bool found = false;
  for (const auto& c : cfs) {
    if (c.icon_style != yc::model::kIcon4Ratings) continue;
    found = true;
    CHECK(c.icon_reverse);
    REQUIRE(c.icon_thresholds.size() == 3);
    CHECK_FALSE(c.icon_thresholds[2].gte);
  }
  CHECK(found);
}

TEST_CASE("color, dynamic and average filters") {
  yctest::Doc t;
  t.Set("A1", "Val");
  for (int i = 0; i < 6; ++i) t.Set(("A" + std::to_string(i + 2)).c_str(), std::to_string(i + 1));
  t.d.ModifyStyle(t.sheet(), Rect{2, 0, 3, 0}, [](yc::model::Style& s) { s.fill_color = 0xFFFFFF00; });
  REQUIRE(t.d.SetAutoFilter(t.sheet(), Rect{0, 0, 6, 0}));
  auto hidden = [&](int row1) { return t.d.wb().sheets[0]->rows->IsHidden(row1 - 1); };
  yc::model::FilterColumn fc;
  fc.kind = 4;
  fc.color = 0xFFFFFF00;
  t.d.SetFilterColumn(t.sheet(), fc);
  CHECK(hidden(2));
  CHECK_FALSE(hidden(3));
  CHECK_FALSE(hidden(4));
  CHECK(hidden(5));
  fc.kind = 6;
  fc.dynamic = 33;  // above average (3.5)
  t.d.SetFilterColumn(t.sheet(), fc);
  CHECK(hidden(4));   // 3
  CHECK_FALSE(hidden(5));  // 4
  // dates: this month / last year
  const double today = std::floor(yc::formula::NowSerial());
  t.Set("B1", "When");
  t.d.SetValue(t.sheet(), 1, 1, yc::core::Value::Number(today));
  t.d.SetValue(t.sheet(), 2, 1, yc::core::Value::Number(today - 400));
  t.d.ClearAutoFilter(t.sheet());
  REQUIRE(t.d.SetAutoFilter(t.sheet(), Rect{0, 0, 6, 1}));
  fc = {};
  fc.field = 1;
  fc.kind = 6;
  fc.dynamic = 1;  // today
  t.d.SetFilterColumn(t.sheet(), fc);
  CHECK_FALSE(hidden(2));
  CHECK(hidden(3));
}

TEST_CASE("A filter by values is case-insensitive, as Excel's list") {
  yctest::Doc t;
  t.Set("A1", "Sym");
  t.Set("A2", "AAPL");
  t.Set("A3", "MSFT");
  t.Set("A4", "aapl");
  REQUIRE(t.d.SetAutoFilter(t.sheet(), Rect{0, 0, 3, 0}));
  auto hidden = [&](int row1) { return t.d.wb().sheets[0]->rows->IsHidden(row1 - 1); };
  yc::model::FilterColumn fc;
  fc.kind = 1;
  fc.values = {"AAPL"};
  fc.blanks = false;
  t.d.SetFilterColumn(t.sheet(), fc);
  CHECK_FALSE(hidden(2));
  CHECK(hidden(3));
  CHECK_FALSE(hidden(4));
}

TEST_CASE("Text to Columns keeps an apostrophe at the start of a field (measured in Excel 16)") {
  yctest::Doc t;
  t.Set("A1", "''5,6");  // the text '5,6
  yc::engine::TextToColumnsOptions o;
  o.tab = false;
  o.comma = true;
  t.d.TextToColumns(t.sheet(), Rect{0, 0, 0, 0}, 0, 0, o);
  REQUIRE(t.V("A1").IsString());
  CHECK(std::string(t.V("A1").str()) == "'5");
  CHECK(t.N("B1") == 6);
}
