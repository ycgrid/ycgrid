// SPDX-License-Identifier: Apache-2.0
// Sheet and workbook protection
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/ycg.h"

using yc::core::Rect;

TEST_CASE("password hash matches Excel") {
  CHECK(yc::model::ProtectionHash("password") == 0x83AF);
  CHECK(yc::model::ProtectionHash("") == 0);
}

TEST_CASE("protected sheets refuse changes to locked cells") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.d.ModifyStyle(t.sheet(), Rect::Cell(1, 0), [](yc::model::Style& s) { s.locked = false; });  // A2 is unlocked
  yc::model::SheetProtection p;
  p.insert_rows = true;
  REQUIRE(t.d.ProtectSheet(t.sheet(), p, "pw"));
  const uint32_t b0 = t.d.protect_blocks();
  CHECK_FALSE(t.Set("A1", "2"));
  CHECK(t.N("A1") == 1);
  CHECK(t.d.protect_blocks() == b0 + 1);
  CHECK(t.Set("A2", "5"));
  CHECK(t.N("A2") == 5);
  t.d.ModifyStyle(t.sheet(), Rect::Cell(0, 0), [](yc::model::Style& s) { s.bold = true; });  // formatting not allowed
  CHECK_FALSE(t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(0, 0)).bold);
  t.d.DeleteCols(t.sheet(), 0, 1);
  CHECK(t.N("A1") == 1);
  t.d.InsertRows(t.sheet(), 0, 1);  // allowed
  CHECK(t.N("A2") == 1);
  // Python with UserInterfaceOnly
  t.d.wb_mut().sheets[0].mut().protection.mut().ui_only = true;
  t.d.python_call = true;
  CHECK(t.Set("A2", "9"));
  t.d.python_call = false;
  // allow edit ranges: open without a password, or after unlocking
  t.d.SetAllowEditRanges(t.sheet(), {{"open", Rect::Cell(5, 0), 0}, {"secret", Rect::Cell(6, 0), yc::model::ProtectionHash("k")}});
  CHECK(t.Set("A6", "6"));
  CHECK_FALSE(t.Set("A7", "7"));
  CHECK(t.d.block_range() == 1);
  CHECK_FALSE(t.d.UnlockEditRange(t.sheet(), 1, "x"));
  CHECK(t.d.UnlockEditRange(t.sheet(), 1, "k"));
  CHECK(t.Set("A7", "7"));
  // saved
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& pr = *res.wb.sheets[0]->protection;
  CHECK(pr.on);
  CHECK(pr.insert_rows);
  CHECK_FALSE(pr.ui_only);  // not saved, like Excel
  CHECK(pr.ranges.size() == 2);
  // unprotect
  CHECK_FALSE(t.d.UnprotectSheet(t.sheet(), "nope"));
  CHECK(t.d.UnprotectSheet(t.sheet(), "pw"));
  CHECK(t.Set("A1", "3"));
}

TEST_CASE("protected workbook structure") {
  yctest::Doc t;
  REQUIRE(t.d.ProtectWorkbook("w", true, false));
  CHECK(t.d.AddSheet("X") == 0);
  CHECK_FALSE(t.d.RenameSheet(t.sheet(), "Y"));
  CHECK(t.d.wb().sheets.size() == 1);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  CHECK(res.wb.protect_structure);
  CHECK_FALSE(t.d.UnprotectWorkbook("bad"));
  CHECK(t.d.UnprotectWorkbook("w"));
  CHECK(t.d.AddSheet("X") != 0);
}

TEST_CASE("a fill into a protected sheet is refused whole when any cell it writes is locked (Excel 16, measured)") {
  // measured 2026-11-15: A1:A2 filled to A1:A5 with A1:A3 unlocked raises and leaves A3 empty; with A1:A5 unlocked
  // it fills A5
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("B1", "1");
  t.Set("B2", "2");
  t.d.ModifyStyle(sh, yctest::Doc::Range("A1:A3"), [](yc::model::Style& s) { s.locked = false; });
  t.d.ModifyStyle(sh, yctest::Doc::Range("B1:B5"), [](yc::model::Style& s) { s.locked = false; });
  yc::model::SheetProtection p;
  REQUIRE(t.d.ProtectSheet(sh, p, ""));
  t.d.AutoFill(sh, yctest::Doc::Range("A1:A2"), yctest::Doc::Range("A1:A5"));
  t.d.AutoFill(sh, yctest::Doc::Range("B1:B2"), yctest::Doc::Range("B1:B5"));
  t.d.Recalc();
  CHECK(t.V("A3").IsEmpty());
  CHECK(t.T("B5") == "5");
}
