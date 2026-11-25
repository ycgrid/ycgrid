// SPDX-License-Identifier: Apache-2.0
// Round 8, read-only mode (Trading > Operator Mode off): every change the UI asks for is refused quietly and counted
// as a read-only refusal (the status bar says why), the workbook stays as it was; Python's writes still go through.
// Sheets, protection, notes, rules, validation, names, objects, page setup, outlines, fills, Replace and Undo.
#include <catch2/catch_test_macros.hpp>

#include "core/geometry.h"
#include "engine/document.h"
#include "test_helpers.h"

#include <string>

using yc::core::Rect;
namespace model = yc::model;

namespace {

// read-only mode on, the dirty flag cleared: Refused(t, before) then says whether the next call was refused as
// read-only mode refuses (counted, flagged) and left the workbook clean
struct ReadOnly {
  yctest::Doc& t;
  explicit ReadOnly(yctest::Doc& doc) : t(doc) {
    t.d.ui_read_only = true;
    t.d.wb_mut().dirty = false;
  }
  ~ReadOnly() { t.d.ui_read_only = false; }
  uint32_t mark = 0;
  void Mark() { mark = t.d.protect_blocks(); }
  bool Refused() const { return t.d.protect_blocks() > mark && t.d.block_read_only() && !t.d.wb().dirty; }
};

}  // namespace

TEST_CASE("Round8 read-only: sheets can't be added, deleted, renamed, moved, copied, hidden or colored") {
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  const uint32_t s2 = t.d.AddSheet("Two");
  REQUIRE(s2);
  const size_t n = t.d.wb().sheets.size();
  ReadOnly ro(t);
  ro.Mark();
  CHECK(t.d.AddSheet("Three") == 0);  // Shift+F11 added a sheet
  CHECK(ro.Refused());
  ro.Mark();
  CHECK_FALSE(t.d.DeleteSheet(s2));  // the tab menu's Delete deleted it without a word
  CHECK(ro.Refused());
  ro.Mark();
  std::string err;
  CHECK_FALSE(t.d.RenameSheet(s2, "ZZ", &err));  // a double-click on the tab, ZZ, Enter renamed it
  CHECK(ro.Refused());
  CHECK(t.d.S(s2)->name == "Two");
  ro.Mark();
  CHECK_FALSE(t.d.MoveSheet(s2, 0));
  CHECK(ro.Refused());
  ro.Mark();
  CHECK(t.d.CopySheet(s1) == 0);
  CHECK(ro.Refused());
  ro.Mark();
  CHECK_FALSE(t.d.SetSheetVisible(s2, false));  // Hide Sheet hid it
  CHECK(ro.Refused());
  CHECK(t.d.S(s2)->visible);
  ro.Mark();
  t.d.SetTabColor(s2, 0xFFFF0000u);
  CHECK(ro.Refused());
  CHECK(t.d.S(s2)->tab_color == 0);
  CHECK(t.d.wb().sheets.size() == n);
  CHECK(t.d.wb().IndexOfId(s2) == 1);
  // Python's writes are not affected by read-only mode
  t.d.python_call = true;
  CHECK(t.d.AddSheet("Py") != 0);
  t.d.python_call = false;
  CHECK(t.d.wb().sheets.size() == n + 1);
}

TEST_CASE("Round8 read-only: Protect Sheet / Workbook, Unprotect and Allow Edit Ranges change nothing") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  model::SheetProtection p;
  {
    ReadOnly ro(t);
    ro.Mark();
    CHECK_FALSE(t.d.ProtectSheet(sh, p, ""));  // x:ProtectSheet, Enter made ProtectContents True
    CHECK(ro.Refused());
    CHECK_FALSE(t.d.S(sh)->protection->on);
    ro.Mark();
    CHECK_FALSE(t.d.ProtectWorkbook("", true, false));  // then Python's Worksheets.Add raised ProtectedError
    CHECK(ro.Refused());
    CHECK_FALSE(t.d.wb().protect_structure);
    ro.Mark();
    model::AllowEditRange er;
    er.rect = Rect{0, 0, 1, 1};
    t.d.SetAllowEditRanges(sh, {er});
    CHECK(ro.Refused());
    CHECK(t.d.S(sh)->protection->ranges.empty());
  }
  // protected before read-only mode: Unprotect is refused as read-only, not as a wrong password
  REQUIRE(t.d.ProtectSheet(sh, p, ""));
  REQUIRE(t.d.ProtectWorkbook("", true, false));
  {
    ReadOnly ro(t);
    ro.Mark();
    CHECK_FALSE(t.d.UnprotectSheet(sh, ""));
    CHECK(ro.Refused());
    CHECK(t.d.S(sh)->protection->on);
    ro.Mark();
    CHECK_FALSE(t.d.UnprotectWorkbook(""));
    CHECK(ro.Refused());
    CHECK(t.d.wb().protect_structure);
    ro.Mark();
    CHECK(t.d.AddSheet() == 0);
    CHECK(ro.Refused());  // read-only's message, not the structure's
  }
  // out of read-only mode the structure's refusal no longer carries the read-only flag of the one before
  const uint32_t before = t.d.protect_blocks();
  CHECK(t.d.AddSheet() == 0);
  CHECK(t.d.protect_blocks() == before + 1);
  CHECK_FALSE(t.d.block_read_only());
}

TEST_CASE("Round8 read-only: notes, comments, conditional formats, validation and names stay as they are") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  ReadOnly ro(t);
  ro.Mark();
  t.d.SetNote(sh, 1, 1, "me", "hello");  // Shift+F2, typing and a click away made the note "hello"
  CHECK(ro.Refused());
  CHECK(t.d.S(sh)->notes->empty());
  ro.Mark();
  t.d.SetComment(sh, 1, 1, "me", "hi", 0);
  CHECK(ro.Refused());
  CHECK(t.d.S(sh)->notes->empty());
  ro.Mark();
  model::CfRule rule;
  rule.ranges = {Rect{1, 1, 5, 1}};
  rule.formula1 = "3";
  CHECK(t.d.AddCondFormat(sh, rule) == 0);  // CfGreater, CfBarBlue added rules
  CHECK(ro.Refused());
  CHECK(t.d.S(sh)->cond_formats->empty());
  ro.Mark();
  t.d.SetCondFormats(sh, {rule});
  CHECK(ro.Refused());
  CHECK(t.d.S(sh)->cond_formats->empty());
  ro.Mark();
  model::DvRule dv;
  dv.type = model::DvType::List;
  dv.formula1 = "a,b";
  t.d.SetValidation(sh, Rect{1, 1, 1, 1}, dv);
  CHECK(ro.Refused());
  CHECK(t.d.S(sh)->validations->empty());
  ro.Mark();
  std::string err;
  CHECK_FALSE(t.d.DefineName(model::NameDef{"Px", "Sheet1!$A$1", -1}, &err));
  CHECK(ro.Refused());
  CHECK(t.d.wb().names->empty());
  ro.Mark();
  CHECK_FALSE(t.d.AddName("Qty", "Sheet1!$B$1", -1, &err));
  CHECK(ro.Refused());
  CHECK(t.d.wb().names->empty());
  // Python still writes them
  t.d.python_call = true;
  t.d.SetNote(sh, 1, 1, "py", "from Python");
  CHECK(t.d.DefineName(model::NameDef{"Px", "Sheet1!$A$1", -1}, &err));
  t.d.python_call = false;
  CHECK(t.d.S(sh)->notes->size() == 1);
  CHECK(t.d.wb().names->size() == 1);
  ro.Mark();
  t.d.wb_mut().dirty = false;
  CHECK_FALSE(t.d.DeleteName("Px", -1));
  CHECK(ro.Refused());
  CHECK(t.d.wb().names->size() == 1);
}

TEST_CASE("Round8 read-only: Undo and Redo put back no earlier edit; Python's Application.Undo still does") {
  yctest::Doc t;
  t.d.Begin("Typing");
  t.Set("A1", "5");
  t.d.Commit();
  REQUIRE(t.d.CanUndo());
  {
    ReadOnly ro(t);
    ro.Mark();
    CHECK_FALSE(t.d.Undo());  // Ctrl+Z took back the edit made before read-only mode
    CHECK(ro.Refused());
    CHECK(t.N("A1") == 5);
    CHECK(t.d.CanUndo());  // kept for later
    t.d.python_call = true;
    CHECK(t.d.Undo());
    t.d.python_call = false;
  }
  CHECK(t.V("A1").IsEmpty());
  REQUIRE(t.d.CanRedo());
  ReadOnly ro(t);
  ro.Mark();
  CHECK_FALSE(t.d.Redo());
  CHECK(ro.Refused());
  CHECK(t.V("A1").IsEmpty());
}

TEST_CASE("Round8 read-only: objects, page setup and outlines are refused once; a linked picture still follows its cells") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  model::ShapeObj shape;
  shape.locked = false;  // an unlocked shape was moved and formatted freely
  const uint32_t sid = t.d.AddShape(sh, shape);
  REQUIRE(sid);
  const uint32_t cid = t.d.AddChart(sh, model::ChartObj{});
  REQUIRE(cid);
  t.d.GroupOutline(sh, true, 2, 5, true);
  REQUIRE(t.d.S(sh)->rows->Level(3) == 1);
  ReadOnly ro(t);
  ro.Mark();
  CHECK_FALSE(t.d.UpdateShape(sh, sid, [](model::ShapeObj& x) { x.left = 99; }));
  CHECK(ro.Refused());
  ro.Mark();
  CHECK_FALSE(t.d.UpdateChart(sh, cid, [](model::ChartObj& x) { x.left = 99; }));
  CHECK(ro.Refused());
  for (const auto& x : *t.d.S(sh)->shapes) CHECK(x.left == 0);
  for (const auto& x : *t.d.S(sh)->charts) CHECK(x.left == 0);
  ro.Mark();
  CHECK_FALSE(t.d.UpdatePageSetup(sh, [](model::PageSetup& p) { p.landscape = true; }));
  CHECK(ro.Refused());
  CHECK_FALSE(t.d.S(sh)->page_setup->landscape);
  // printing as set up asks for no change: nothing to refuse
  ro.Mark();
  CHECK(t.d.UpdatePageSetup(sh, [](model::PageSetup&) {}));
  CHECK(t.d.protect_blocks() == ro.mark);
  // a level shown or a group collapsed: one refusal, not one per row
  ro.Mark();
  t.d.SetGroupCollapsed(sh, true, 3, 1, true);
  CHECK(ro.Refused());
  CHECK(t.d.protect_blocks() == ro.mark + 1);
  CHECK_FALSE(t.d.S(sh)->rows->IsHidden(3));
  ro.Mark();
  t.d.ShowOutlineLevel(sh, true, 1);
  CHECK(t.d.protect_blocks() == ro.mark + 1);
  ro.Mark();
  t.d.ClearOutline(sh, true);
  CHECK(ro.Refused());
  CHECK(t.d.S(sh)->rows->Level(3) == 1);
  // the app's own refresh of a linked picture is derived, as a recalculation: read-only mode lets it through
  ro.Mark();
  t.d.app_refresh = true;
  CHECK(t.d.UpdateShape(sh, sid, [](model::ShapeObj& x) { x.image = "png"; }));
  t.d.app_refresh = false;
  CHECK(t.d.protect_blocks() == ro.mark);
}

TEST_CASE("Round8 read-only: Replace, Fill Down and Remove Subtotal are refused as read-only, so the UI can tell it from nothing found") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "abc");
  t.Set("A2", "x");
  t.Set("A3", "y");
  ReadOnly ro(t);
  ro.Mark();
  CHECK(t.d.Replace(sh, Rect{0, 0, 10, 10}, "abc", "zzz", false, false) == 0);
  CHECK(ro.Refused());  // Replace All read the 0 as "We couldn't find anything to replace."
  CHECK(t.d.protect_blocks() == ro.mark + 1);
  CHECK(t.T("A1") == "abc");
  ro.Mark();
  t.d.FillDown(sh, Rect{0, 0, 2, 0});
  CHECK(ro.Refused());
  CHECK(t.d.protect_blocks() == ro.mark + 1);  // once, not once per row
  CHECK(t.T("A2") == "x");
  ro.Mark();
  CHECK(t.d.RemoveSubtotal(sh, Rect{0, 0, 2, 0}) == 0);
  CHECK(ro.Refused());
}

TEST_CASE("Round8 read-only: a data table still follows its formula (its writes are a calculation)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B1", "0.1");
  t.Set("C3", "1");
  t.Set("C4", "2");
  t.Set("C5", "3");
  t.Set("D2", "=B1*10");
  const yc::core::CellPos col_in{0, 1};
  std::string err;
  REQUIRE(t.d.AddDataTable(sh, Rect{1, 2, 4, 3}, nullptr, &col_in, &err));
  REQUIRE(t.N("D5") == 30);
  ReadOnly ro(t);
  // Python changes the formula; the calculation that follows runs as the engine's own (F9, the next command)
  t.d.python_call = true;
  REQUIRE(t.d.SetInput(sh, 1, 3, "=B1*100"));
  t.d.python_call = false;
  ro.Mark();
  t.d.Recalc();
  CHECK(t.N("D5") == 300);  // refused: the table kept 30
  CHECK(t.N("B1") == 0.1);
  CHECK(t.d.protect_blocks() == ro.mark);  // a read-only refusal counted at every calculation
  CHECK_FALSE(t.d.app_refresh);
}

TEST_CASE("Round8 read-only: Protected View refuses the same changes, Python's too") {
  yctest::Doc t;
  t.d.protected_view = true;
  t.d.python_call = true;
  const uint32_t before = t.d.protect_blocks();
  std::string err;
  CHECK_FALSE(t.d.DefineName(model::NameDef{"Px", "Sheet1!$A$1", -1}, &err));
  CHECK(t.d.AddSheet() == 0);
  CHECK(t.d.protect_blocks() == before + 2);
  CHECK(t.d.block_python());
  t.d.python_call = false;
  t.d.protected_view = false;
  CHECK(t.d.wb().names->empty());
}
