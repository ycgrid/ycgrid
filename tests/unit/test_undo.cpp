// SPDX-License-Identifier: Apache-2.0
// Undo covers the user's changes only: Python, live tables and topics write without being undone and without wiping
// the user's undo list (ADR 0013). Undo puts back the cells and rows / columns the change touched, nothing else.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

using yc::core::Rect;
using yc::core::Value;
using yc::engine::kSrcLive;
using yc::engine::kSrcPython;

namespace {

void PyWrite(yctest::Doc& t, uint32_t r, uint32_t c, double v, uint8_t src = kSrcPython) {
  t.d.SetValues(t.sheet(), Rect::Cell(r, c), {Value::Number(v)}, src);
}

void UserSet(yctest::Doc& t, const char* a1, const char* text) {
  t.d.Begin("Typing");
  t.Set(a1, text);
  t.d.Commit();
}

bool Bold(yctest::Doc& t, uint32_t r, uint32_t c) {
  const auto* s = t.d.S(t.sheet());
  return t.d.wb().styles->Get(s->GetStyle(r, c)).bold;
}

}  // namespace

TEST_CASE("undo: a user's change stays undoable while Python writes other cells") {
  yctest::Doc t;
  UserSet(t, "A1", "10");
  UserSet(t, "A1", "20");
  PyWrite(t, 0, 1, 1.5);   // B1 from Python
  PyWrite(t, 5, 5, 7, kSrcLive);  // F6 from a live table
  REQUIRE(t.d.CanUndo());
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.N("A1") == 10);
  CHECK(t.N("B1") == 1.5);  // Python's value isn't undone
  CHECK(t.N("F6") == 7);
  REQUIRE(t.d.Undo());
  CHECK(t.V("A1").IsEmpty());
  REQUIRE(t.d.Redo());
  REQUIRE(t.d.Redo());
  t.d.Recalc();
  CHECK(t.N("A1") == 20);
  CHECK(t.N("B1") == 1.5);
}

TEST_CASE("undo: a cell the user and then Python changed gets the user's previous value back") {
  yctest::Doc t;
  UserSet(t, "C3", "5");
  UserSet(t, "C3", "6");
  PyWrite(t, 2, 2, 99);
  REQUIRE(t.d.Undo());
  CHECK(t.N("C3") == 5);
}

TEST_CASE("undo: formulas depending on undone cells are recalculated, others keep Python's inputs") {
  yctest::Doc t;
  UserSet(t, "A1", "2");
  UserSet(t, "B1", "=A1*A2");
  UserSet(t, "A1", "3");
  PyWrite(t, 1, 0, 10);  // A2
  t.d.Recalc();
  CHECK(t.N("B1") == 30);
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.N("A1") == 2);
  CHECK(t.N("B1") == 20);  // 2 x Python's 10
}

TEST_CASE("undo: moving cells by drag and drop is undone after Python writes, with the formulas pointing at them") {
  yctest::Doc t;
  UserSet(t, "K10", "78000");
  UserSet(t, "K12", "78500");
  UserSet(t, "M1", "=K10+1");
  t.d.Recalc();
  t.d.Begin("Move");
  REQUIRE(t.d.MoveCells(t.sheet(), Rect::Cell(9, 10), t.sheet(), 11, 10));  // K10 -> K12
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.V("K10").IsEmpty());
  CHECK(t.N("K12") == 78000);
  CHECK(t.F("M1") == "=K12+1");
  PyWrite(t, 5, 7, 2595);  // a quote elsewhere
  REQUIRE(t.d.CanUndo());
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.N("K10") == 78000);
  CHECK(t.N("K12") == 78500);
  CHECK(t.F("M1") == "=K10+1");
  CHECK(t.N("M1") == 78001);
  CHECK(t.N("H6") == 2595);
}

TEST_CASE("undo: a format change puts back formats only, not the values written since") {
  yctest::Doc t;
  UserSet(t, "D4", "1");
  t.d.Begin("Bold");
  t.d.ModifyStyle(t.sheet(), Rect{3, 3, 3, 3}, [](yc::model::Style& s) { s.bold = true; });
  t.d.Commit();
  CHECK(Bold(t, 3, 3));
  PyWrite(t, 3, 3, 42);
  REQUIRE(t.d.Undo());
  CHECK_FALSE(Bold(t, 3, 3));
  CHECK(t.N("D4") == 42);  // the live value stays
}

TEST_CASE("undo: a column width change is undone; widths Python set on other columns stay") {
  yctest::Doc t;
  const int32_t w0 = t.d.S(t.sheet())->cols->Size(2);
  t.d.Begin("Column Width");
  t.d.SetColWidth(t.sheet(), 2, 2, 200);
  t.d.Commit();
  t.d.SetColWidth(t.sheet(), 5, 5, 150);  // Python (outside a transaction)
  PyWrite(t, 0, 0, 1);
  REQUIRE(t.d.Undo());
  CHECK(t.d.S(t.sheet())->cols->Size(2) == w0);
  CHECK(t.d.S(t.sheet())->cols->Size(5) == 150);
}

TEST_CASE("undo: a change undone with its whole version is dropped when Python writes") {
  yctest::Doc t;
  UserSet(t, "A1", "1");
  t.d.Begin("Theme");
  yc::model::Theme th = *t.d.wb().theme;
  th.name = "Other";
  t.d.SetTheme(th);
  t.d.Commit();
  UserSet(t, "A2", "2");
  PyWrite(t, 9, 9, 3);
  // the theme change and everything older go; the newer cell change stays
  REQUIRE(t.d.CanUndo());
  REQUIRE(t.d.Undo());
  CHECK(t.V("A2").IsEmpty());
  CHECK_FALSE(t.d.CanUndo());
  CHECK(t.N("A1") == 1);
  CHECK(t.N("J10") == 3);
}

TEST_CASE("undo: Python writes inside the user's own transaction aren't separated (events run after it)") {
  yctest::Doc t;
  UserSet(t, "A1", "1");
  CHECK(t.d.CanUndo());
  t.d.ClearUndo();  // what the bridge does when Python inserts rows or adds a sheet: positions moved
  CHECK_FALSE(t.d.CanUndo());
}

TEST_CASE("undo: UndoTarget names the sheet and the cells the next undo / redo changes") {
  yctest::Doc t;
  uint32_t sheet = 0;
  Rect r{};
  CHECK_FALSE(t.d.UndoTarget(false, &sheet, &r));
  t.d.Begin("Typing");
  t.Set("B2", "1");
  t.Set("D5", "2");
  t.d.Commit();
  REQUIRE(t.d.UndoTarget(false, &sheet, &r));
  CHECK(sheet == t.sheet());
  CHECK(r == Rect{1, 1, 4, 3});  // B2:D5
  REQUIRE(t.d.Undo());
  REQUIRE(t.d.UndoTarget(true, &sheet, &r));
  CHECK(r == Rect{1, 1, 4, 3});
  CHECK_FALSE(t.d.UndoTarget(false, &sheet, &r));
}
