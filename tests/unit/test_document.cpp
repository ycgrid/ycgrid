// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"
#include "engine/navigate.h"

#include <cmath>

using yc::core::Rect;

TEST_CASE("插入與刪除列會調整公式參照") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  t.Set("B1", "=SUM(A1:A3)");
  t.Set("C5", "=A3*2");
  t.d.InsertRows(t.sheet(), 1, 2);  // insert 2 rows before row 2
  t.d.Recalc();
  CHECK(t.F("B1") == "=SUM(A1:A5)");
  CHECK(t.F("C7") == "=A5*2");
  CHECK(t.N("C7") == 6);
  t.d.DeleteRows(t.sheet(), 4, 1);  // delete row 5 (the old A3)
  t.d.Recalc();
  CHECK(t.F("B1") == "=SUM(A1:A4)");
  CHECK(t.F("C6") == "=#REF!*2");
  CHECK(t.N("B1") == 3);
  t.d.InsertCols(t.sheet(), 0, 1);
  t.d.Recalc();
  CHECK(t.F("C1") == "=SUM(B1:B4)");
}

TEST_CASE("復原與取消復原（COW 快照）") {
  yctest::Doc t;
  t.d.Begin("輸入");
  t.Set("A1", "10");
  t.d.Commit();
  t.d.Begin("輸入");
  t.Set("A2", "=A1*2");
  t.d.Commit();
  t.d.Begin("輸入");
  t.Set("A1", "7");
  t.d.Commit();
  CHECK(t.N("A2") == 14);
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.N("A1") == 10);
  CHECK(t.N("A2") == 20);
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.V("A2").IsEmpty());
  REQUIRE(t.d.Redo());
  t.d.Recalc();
  CHECK(t.N("A2") == 20);
  // structure changes undo too
  t.d.Begin("插入列");
  t.d.InsertRows(t.sheet(), 0, 1);
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.F("A3") == "=A2*2");
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.F("A2") == "=A1*2");
  CHECK(t.N("A2") == 20);
}

TEST_CASE("複製貼上：相對參照位移") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("B1", "=A1*10");
  auto clip = t.d.Copy(t.sheet(), Rect::Cell(0, 1));
  t.d.Paste(t.sheet(), 1, 1, clip);
  t.d.Recalc();
  CHECK(t.F("B2") == "=A2*10");
  CHECK(t.N("B2") == 20);
  t.d.Paste(t.sheet(), 0, 0, clip);  // pasted at A1: a reference left of column A -> #REF!
  t.d.Recalc();
  CHECK(t.F("A1") == "=#REF!*10");
}

TEST_CASE("Paste transposed: references to the copied cells turn with them, absolute ones too (measured)") {
  yctest::Doc t;
  t.Set("A1", "2");
  t.Set("B1", "=A1*10");
  t.Set("C1", "=$A$1+1");
  auto clip = t.d.Copy(t.sheet(), Rect{0, 0, 0, 2});
  t.d.Paste(t.sheet(), 3, 0, clip, yc::engine::PasteMode::All, true, false);  // A4:A6
  t.d.Recalc();
  CHECK(t.N("A4") == 2);
  CHECK(t.F("A5") == "=A4*10");  // one column left became one row up
  CHECK(t.N("A5") == 20);
  CHECK(t.F("A6") == "=$A$4+1");  // Excel 16, measured: $A$1 was copied, so it turns to its new place
}

TEST_CASE("Paste Special operations combine the copied number with each cell") {
  yctest::Doc t;
  t.Set("E1", "10");
  t.Set("A1", "5");
  t.Set("A2", "=A1*2");
  t.Set("A3", "text");
  auto clip = t.d.Copy(t.sheet(), Rect{0, 4, 0, 4});  // E1
  using yc::engine::PasteMode;
  using yc::engine::PasteOp;
  t.d.Paste(t.sheet(), 0, 0, clip, PasteMode::Values, false, false, PasteOp::Multiply);  // A1 * 10
  t.d.Paste(t.sheet(), 1, 0, clip, PasteMode::Values, false, false, PasteOp::Add);       // a formula becomes =(A1*2)+10
  t.d.Paste(t.sheet(), 2, 0, clip, PasteMode::Values, false, false, PasteOp::Subtract);  // text stays
  t.d.Paste(t.sheet(), 3, 0, clip, PasteMode::Values, false, false, PasteOp::Divide);    // blank is 0: 0/10
  t.d.Recalc();
  CHECK(t.N("A1") == 50);
  CHECK(t.F("A2") == "=(A1*2)+10");
  CHECK(t.N("A2") == 110);
  CHECK(t.T("A3") == "text");
  CHECK(t.N("A4") == 0);
  // dividing by a blank copied cell gives #DIV/0!
  auto blank = t.d.Copy(t.sheet(), Rect{9, 9, 9, 9});
  t.d.Paste(t.sheet(), 0, 0, blank, PasteMode::Values, false, false, PasteOp::Divide);
  CHECK(t.T("A1") == "#DIV/0!");
  // Column widths pastes only the widths
  t.d.SetColWidth(t.sheet(), 4, 4, 150);
  auto wide = t.d.Copy(t.sheet(), Rect{0, 4, 0, 4});
  t.d.Paste(t.sheet(), 0, 6, wide, PasteMode::ColumnWidths);
  CHECK(t.d.S(t.sheet())->cols->Size(6) == 150);
  CHECK(t.T("G1") == "");
}

TEST_CASE("自動填滿與排序") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "3");
  t.d.AutoFill(t.sheet(), Rect{0, 0, 1, 0}, Rect{0, 0, 4, 0});
  t.d.Recalc();
  CHECK(t.N("A5") == 9);
  t.Set("B1", "Item1");
  t.d.AutoFill(t.sheet(), Rect{0, 1, 0, 1}, Rect{0, 1, 2, 1});
  CHECK(t.T("B3") == "Item3");
  t.Set("C1", "一月");
  t.d.AutoFill(t.sheet(), Rect{0, 2, 0, 2}, Rect{0, 2, 12, 2});
  CHECK(t.T("C12") == "十二月");
  CHECK(t.T("C13") == "一月");
  // mixed pattern: the number parts continue their step
  t.Set("G1", "1");
  t.Set("G2", "2");
  t.Set("G3", "一月");
  t.d.AutoFill(t.sheet(), Rect{0, 6, 2, 6}, Rect{0, 6, 5, 6});
  CHECK(t.T("G4") == "3");
  CHECK(t.T("G5") == "4");
  CHECK(t.T("G6") == "二月");
  t.Set("E1", "b");
  t.Set("E2", "c");
  t.Set("E3", "a");
  t.Set("F1", "2");
  t.Set("F2", "3");
  t.Set("F3", "1");
  t.d.Sort(t.sheet(), Rect{0, 4, 2, 5}, {{4, false}}, false);
  t.d.Recalc();
  CHECK(t.T("E1") == "a");
  CHECK(t.T("F1") == "1");
  CHECK(t.T("E3") == "c");
}

TEST_CASE("動態列數擴充") {
  yctest::Doc t;
  CHECK(t.d.S(t.sheet())->row_limit == 1048576);
  t.d.SetValue(t.sheet(), 2000000, 0, yc::core::Value::Number(1));
  CHECK(t.d.S(t.sheet())->row_limit == 1048576 * 2);
  t.Set("B1", "=SUM(A:A)");
  CHECK(t.N("B1") == 1);
}

TEST_CASE("工作表新增、改名、刪除") {
  yctest::Doc t;
  const uint32_t s2 = t.d.AddSheet("資料");
  t.d.SetInput(s2, 0, 0, "42");
  t.Set("A1", "=資料!A1+1");
  CHECK(t.N("A1") == 43);
  REQUIRE(t.d.RenameSheet(s2, "報價"));
  CHECK(t.F("A1") == "=報價!A1+1");
  std::string err;
  CHECK_FALSE(t.d.RenameSheet(s2, "a/b", &err));
  // another sheet's name, in any case: Excel 16's message (measured)
  CHECK_FALSE(t.d.RenameSheet(s2, "sheet1", &err));
  CHECK(err == "That name is already taken. Try a different one.");
  REQUIRE(t.d.DeleteSheet(s2));
  t.d.Recalc();
  CHECK(t.T("A1") == "#REF!");
}

TEST_CASE("Deleting a sheet recalculates what read it, not every formula of the workbook") {
  yctest::Doc t;
  const uint32_t s2 = t.d.AddSheet("Data");
  const uint32_t s3 = t.d.AddSheet("Other");
  t.d.SetInput(s2, 0, 0, "42");
  t.d.SetInput(s3, 0, 0, "5");
  REQUIRE(t.d.AddName("Px", "=Data!$A$1"));
  t.Set("A1", "=Data!A1+1");      // reads the sheet: #REF!
  t.Set("B1", "=A1*2");           // follows A1
  t.Set("C1", "=Px");             // through a name
  t.Set("D1", "=INDIRECT(\"Data!A1\")");
  t.Set("E1", "=Other!A1*3");     // unrelated: keeps its value
  for (int r = 1; r <= 2000; ++r) t.d.SetInput(t.sheet(), static_cast<uint32_t>(r), 5, "=Other!A1+" + std::to_string(r));
  t.d.Recalc();
  CHECK(t.N("B1") == 86);
  CHECK(t.N("C1") == 42);
  CHECK(t.N("D1") == 42);
  REQUIRE(t.d.DeleteSheet(s2));
  t.d.Recalc();
  CHECK(t.T("A1") == "#REF!");
  CHECK(t.T("B1") == "#REF!");
  CHECK(t.T("C1") == "#REF!");
  CHECK(t.T("D1") == "#REF!");
  CHECK(t.N("E1") == 15);
  CHECK(t.N("F2001") == 2005);
  CHECK(t.d.last_stats().evaluated < 100);  // not the 2,000 formulas on "Other"
  // what was waiting before the delete is still calculated
  t.d.SetInput(s3, 0, 0, "7");
  REQUIRE(t.d.DeleteSheet(t.d.AddSheet("Spare")));
  t.d.Recalc();
  CHECK(t.N("E1") == 21);
  CHECK(t.N("F2") == 8);
}

namespace {
struct CountingProvider : yc::engine::ExternalProvider {
  std::vector<std::string> keys;
  void RequestUdf(const std::string& key, const std::string&, const std::vector<std::shared_ptr<yc::formula::Array>>&, uint32_t, uint32_t, uint32_t) override {
    keys.push_back(key);
  }
  void Subscribe(const std::string&, const std::vector<yc::core::Value>&) override {}
};
}  // namespace

TEST_CASE("The Python host starting again asks each call once; the cells keep their values until it answers") {
  yctest::Doc t;
  CountingProvider pv;
  t.Set("A1", "=MYFN(2)");   // calculated before the host is there
  CHECK(t.T("A1") == "#NAME?");
  t.d.provider = &pv;
  t.d.ProviderChanged();
  t.d.RecalcUdfCells();       // the host is ready: asked
  CHECK(pv.keys.size() == 1);
  t.d.SetUdfResult(pv.keys[0], yc::core::Value::Number(10));
  t.d.Recalc();
  CHECK(t.N("A1") == 10);
  t.Set("B1", "=A1+1");
  for (int r = 1; r <= 1000; ++r) t.d.SetInput(t.sheet(), static_cast<uint32_t>(r), 2, "=MYFN(2)*" + std::to_string(r));
  for (int r = 1; r <= 3000; ++r) t.d.SetInput(t.sheet(), static_cast<uint32_t>(r), 3, "=" + std::to_string(r) + "*2");
  t.d.Recalc();
  CHECK(t.N("C1001") == 10000);
  // a reload with the same answer: one formula asks, nothing else is calculated
  t.d.provider = nullptr;
  t.d.ProviderChanged();      // the host stops: values stay
  CHECK(t.N("A1") == 10);
  t.d.provider = &pv;
  t.d.ProviderChanged();
  t.d.RecalcUdfCells();
  CHECK(pv.keys.size() == 2);
  CHECK(t.d.last_stats().evaluated < 10);
  CHECK(t.N("C1001") == 10000);  // still shown
  t.d.SetUdfResult(pv.keys[1], yc::core::Value::Number(10));
  t.d.Recalc();
  CHECK(t.d.last_stats().evaluated == 0);
  CHECK(t.N("C1001") == 10000);
  // a reload with another answer: every formula of the call follows, and what depends on them
  t.d.RecalcUdfCells();
  CHECK(pv.keys.size() == 3);
  t.d.SetUdfResult(pv.keys[2], yc::core::Value::Number(20));
  t.d.Recalc();
  CHECK(t.N("A1") == 20);
  CHECK(t.N("B1") == 21);
  CHECK(t.N("C1001") == 20000);
  CHECK(t.N("D3001") == 6000);
  // a formula edited since its call was asked: the call is asked by the others
  t.Set("A1", "=1");
  t.d.RecalcUdfCells();
  CHECK(pv.keys.size() == 4);
  t.d.provider = nullptr;
}

TEST_CASE("Formulas calculated without a host are asked when it comes only if they call one of its functions") {
  yctest::Doc t;
  CountingProvider pv;
  t.Set("A1", "=MYFN(2)");
  for (int r = 1; r <= 2000; ++r) t.d.SetInput(t.sheet(), static_cast<uint32_t>(r), 1, "=NOSUCH(" + std::to_string(r) + ")");
  t.d.Recalc();
  CHECK(t.T("B2") == "#NAME?");
  t.d.provider = &pv;
  t.d.ProviderChanged();
  const std::vector<std::string> names{"MYFN"};
  t.d.RecalcUdfCells(&names);
  CHECK(pv.keys.size() == 1);                // MYFN only: NOSUCH isn't the host's
  CHECK(t.d.last_stats().evaluated < 10);
  CHECK(t.T("B2") == "#NAME?");
  t.d.provider = nullptr;
}

TEST_CASE("Stream formulas subscribe again with a new host") {
  struct Subs : yc::engine::ExternalProvider {
    std::vector<std::string> topics;
    void RequestUdf(const std::string&, const std::string&, const std::vector<std::shared_ptr<yc::formula::Array>>&, uint32_t, uint32_t, uint32_t) override {}
    void Subscribe(const std::string& topic, const std::vector<yc::core::Value>&) override { topics.push_back(topic); }
  };
  yctest::Doc t;
  Subs a, b;
  t.d.provider = &a;
  t.Set("A1", "=QUOTE(\"TXF\")");
  CHECK(a.topics.size() == 1);
  t.d.UpdateTopic(a.topics[0], yc::core::Value::Number(23000));
  t.d.Recalc();
  CHECK(t.N("A1") == 23000);
  t.d.provider = nullptr;   // the host stops: the last value stays
  t.d.ProviderChanged();
  CHECK(t.N("A1") == 23000);
  t.d.provider = &b;        // a new host: the formula subscribes with it
  t.d.ProviderChanged();
  CHECK(b.topics.size() == 1);
  t.d.provider = nullptr;
}

TEST_CASE("Ctrl+Enter fills the selection like a paste of the cell (FillFrom)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (uint32_t r = 0; r < 10; ++r) t.d.SetInput(sh, r, 0, std::to_string(r + 1));
  t.d.Begin("Enter");
  REQUIRE(t.d.SetInput(sh, 4, 2, "=A4*2"));   // C5 refers to the row above
  t.d.FillFrom(sh, 4, 2, {Rect{0, 2, 9, 3}});   // C1:D10
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.F("C2") == "=A1*2");
  CHECK(t.N("C2") == 2);
  CHECK(t.F("C10") == "=A9*2");
  CHECK(t.F("D7") == "=B6*2");
  CHECK(t.T("C1") == "#REF!");                  // the row above row 1 is off the sheet
  t.d.Begin("Enter");
  REQUIRE(t.d.SetInput(sh, 0, 5, "x"));
  t.d.FillFrom(sh, 0, 5, {Rect{0, 5, 1048575, 5}});  // a whole column of a value
  t.d.Commit();
  CHECK(t.d.GetValue(sh, 1048575, 5).str() == "x");
  REQUIRE(t.d.Undo());
  CHECK(t.d.GetValue(sh, 1048575, 5).IsEmpty());
  CHECK(t.d.GetValue(sh, 0, 5).IsEmpty());
}

TEST_CASE("A copied sheet keeps its values, follows its inputs, and the workbook isn't recalculated") {
  yctest::Doc t;
  const uint32_t src = t.sheet();
  const uint32_t other = t.d.AddSheet("Other");
  t.d.SetInput(other, 0, 0, "100");
  t.Set("A1", "5");
  t.Set("B1", "=A1*2");
  t.Set("C1", "=Other!A1+B1");
  t.Set("D1", "=SHEET()");
  for (int r = 1; r <= 3000; ++r) t.d.SetInput(other, static_cast<uint32_t>(r), 1, "=" + std::to_string(r) + "+1");
  t.d.Recalc();
  const uint32_t cp = t.d.CopySheet(src);
  REQUIRE(cp != 0);
  t.d.Recalc();
  CHECK(t.d.last_stats().evaluated < 20);  // not the 3,000 formulas on "Other"
  CHECK(t.N("B1", cp) == 10);
  CHECK(t.N("C1", cp) == 110);
  CHECK(t.N("D1", cp) == 2);               // SHEET() of the copy, calculated again
  t.Set("A1", "7", cp);                     // the copy follows its own inputs
  CHECK(t.N("B1", cp) == 14);
  CHECK(t.N("C1", cp) == 114);
  CHECK(t.N("B1") == 10);                   // the original doesn't
  t.d.SetInput(other, 0, 0, "200");
  t.d.Recalc();
  CHECK(t.N("C1", cp) == 214);
  CHECK(t.N("C1") == 210);
  t.Set("E1", "=SHEETS()");
  CHECK(t.N("E1") == 3);
  REQUIRE(t.d.MoveSheet(cp, 2));            // moved last: SHEET() follows
  t.d.Recalc();
  CHECK(t.N("D1", cp) == 3);
  REQUIRE(t.d.DeleteSheet(other));
  t.d.Recalc();
  CHECK(t.N("D1", cp) == 2);
  CHECK(t.N("E1") == 2);
  t.d.AddSheet("Third");                    // a new sheet counts too
  t.d.Recalc();
  CHECK(t.N("E1") == 3);
}

TEST_CASE("Inserting and deleting rows recalculates what can change, not every formula") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (uint32_t r = 0; r < 10; ++r) t.d.SetInput(sh, r, 0, std::to_string((r + 1) * 10));  // A1:A10 = 10..100
  t.Set("B1", "=SUM(A:A)");
  t.Set("B2", "=SUM(A1:A10)");
  t.Set("B3", "=ROW()");
  t.Set("B4", "=A9*2");          // reads across the insertion point
  t.Set("C1", "=SEQUENCE(3)");   // a dynamic array
  for (uint32_t r = 20; r < 3020; ++r) t.d.SetInput(sh, r, 4, "=" + std::to_string(r) + "+1");  // E21:E3020, no references
  t.d.Recalc();
  CHECK(t.N("B1") == 550);
  t.d.InsertRows(sh, 2, 1);      // a new row 3: B3 moves to B4, B4 to B5
  t.d.Recalc();
  CHECK(t.d.last_stats().evaluated < 50);  // not the 3,000 formulas in column E
  CHECK(t.N("B4") == 4);         // ROW() of the moved cell
  CHECK(t.F("B5") == "=A10*2");
  CHECK(t.N("B5") == 180);
  CHECK(t.F("B2") == "=SUM(A1:A11)");
  CHECK(t.N("C3") == 3);         // the array spilled again
  CHECK(t.N("E3021") == 3020);   // moved with its value
  t.d.DeleteRows(sh, 0, 1);      // row 1 (A1 = 10, B1, C1) goes
  t.d.Recalc();
  CHECK(t.N("B3") == 3);         // ROW() moved up
  CHECK(t.N("B4") == 180);
  t.Set("H5", "=SUM(A:A)");
  CHECK(t.N("H5") == 540);       // A1 = 20, A3:A10 = 30..100
  t.d.DeleteRows(sh, 0, 1);      // A1 (20) goes: SUM(A:A) keeps its text, loses the value
  t.d.Recalc();
  CHECK(t.F("H4") == "=SUM(A:A)");
  CHECK(t.N("H4") == 520);
}

TEST_CASE("Whole-column references follow inserted and deleted rows (MATCH, INDEX)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (uint32_t r = 0; r < 10; ++r) t.d.SetInput(sh, r, 0, std::to_string(r + 1));  // A1:A10 = 1..10
  t.Set("C1", "=MATCH(5,A:A,0)");
  t.Set("C2", "=INDEX(A:A,3)");
  t.Set("D1", "=ROWS(A2:A10)");
  CHECK(t.N("C1") == 5);
  CHECK(t.N("C2") == 3);
  t.d.InsertRows(sh, 2, 2);  // two rows above the 3 and the 5 (C1 and C2 stay where they are)
  t.d.Recalc();
  CHECK(t.N("C1") == 7);
  CHECK(t.T("C2") == "0");   // A3 is empty now
  CHECK(t.N("D1") == 11);
  t.d.DeleteRows(sh, 2, 2);
  t.d.Recalc();
  CHECK(t.N("C1") == 5);
  CHECK(t.N("C2") == 3);
  t.d.InsertCols(sh, 0, 1);  // a column before A: the formulas move to D, their references to B:B
  t.d.Recalc();
  CHECK(t.F("D1") == "=MATCH(5,B:B,0)");
  CHECK(t.N("D1") == 5);
  t.d.DeleteCols(sh, 6, 3);  // columns to the right of everything: nothing moves
  t.d.Recalc();
  CHECK(t.N("D1") == 5);
  CHECK(t.d.last_stats().evaluated < 10);
  t.d.SetInput(sh, 4, 1, "50");  // the dependencies are intact
  t.d.Recalc();
  CHECK(t.T("D1") == "#N/A");
}

TEST_CASE("A reader of a spilled value on another sheet is calculated again after a shift (calc_fuzz seed 3102)") {
  // Data!D12 spills 3,4,5 into D12:F12; Data!A24 looks 24 up in F4:H29 (finds the 5); Sheet1!F21 reads Data!A24.
  // A shift on Data clears its spills and marks A24 again; A24 is calculated before D12 spills again (F12 empty: -1)
  // and F21 reads that; the spill then marks A24 again, which gives 5 within the same pass. F21 has to follow.
  yctest::Doc t;
  const uint32_t s2 = t.d.AddSheet("Data");
  t.d.SetInput(s2, 11, 3, "=SEQUENCE(1,3,3)");
  t.d.SetInput(s2, 23, 0, "=IFERROR(VLOOKUP(24,F4:H29,1,TRUE),-1)");
  t.Set("F21", "=Data!A24");
  CHECK(t.N("F12", s2) == 5);
  CHECK(t.N("A24", s2) == 5);
  CHECK(t.N("F21") == 5);
  t.d.DeleteCols(s2, 9, 1);  // column J: nothing A24 reads moves
  t.d.Recalc();
  CHECK(t.N("A24", s2) == 5);
  CHECK(t.N("F21") == 5);
  t.d.InsertCols(s2, 6, 1);  // column G: A24 reads F4:I29 now
  t.d.Recalc();
  CHECK(t.F("A24", s2) == "=IFERROR(VLOOKUP(24,F4:I29,1,TRUE),-1)");
  CHECK(t.N("A24", s2) == 5);
  CHECK(t.N("F21") == 5);
  t.d.InsertRows(s2, 9, 1);  // row 10: D12 moves to D13 and A24 to A25, F21 follows to Data!A25
  t.d.Recalc();
  CHECK(t.F("F21") == "=Data!A25");
  CHECK(t.N("A25", s2) == 5);
  CHECK(t.N("F21") == 5);
}

TEST_CASE("Undoing a structural change brings back the values without recalculating everything") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.d.SetInput(sh, 0, 0, "5");
  t.Set("B1", "=A1*2");
  t.Set("C1", "=RAND()");
  for (uint32_t r = 10; r < 2010; ++r) t.d.SetInput(sh, r, 3, "=" + std::to_string(r) + "*3");
  t.d.Recalc();
  t.d.Begin("Insert");
  t.d.InsertRows(sh, 0, 1);
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.N("B2") == 10);
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.d.last_stats().evaluated < 20);  // RAND() only, not the 2,000 others
  CHECK(t.N("B1") == 10);
  CHECK(t.N("D2001") == 6000);
  t.Set("A1", "6");                          // the dependencies are there
  CHECK(t.N("B1") == 12);
}

TEST_CASE("A structural undo on one sheet leaves the other sheets' dependencies as they are") {
  yctest::Doc t;
  const uint32_t big = t.sheet();
  const uint32_t small = t.d.AddSheet("Small");
  t.d.SetInput(small, 0, 0, "3");
  t.d.SetInput(small, 0, 1, "=A1*2");
  t.d.SetInput(big, 0, 0, "=Small!A1+1");
  for (uint32_t r = 1; r <= 3000; ++r) t.d.SetInput(big, r, 1, "=A1+" + std::to_string(r));  // B2:B3001 read A1
  t.d.Recalc();
  t.d.Begin("Insert");
  t.d.InsertRows(small, 0, 1);  // Small!A1 moves to A2; big!A1 follows (=Small!A2+1)
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.F("A1", big) == "=Small!A2+1");
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.F("A1", big) == "=Small!A1+1");
  CHECK(t.N("B3001", big) == 3004);
  t.d.SetInput(small, 0, 0, "10");  // both sheets follow their inputs after the undo
  t.d.Recalc();
  CHECK(t.N("B1", small) == 20);
  CHECK(t.N("A1", big) == 11);
  CHECK(t.N("B3001", big) == 3011);
}

TEST_CASE("格式與合併") {
  yctest::Doc t;
  t.Set("A1", "標題");
  t.Set("B1", "x");
  REQUIRE(t.d.Merge(t.sheet(), Rect{0, 0, 0, 3}, false, true));
  CHECK(t.V("B1").IsEmpty());
  REQUIRE(t.d.S(t.sheet())->MergeAt(0, 2) != nullptr);
  t.d.ModifyStyle(t.sheet(), Rect{1, 0, 3, 2}, [](yc::model::Style& st) { st.bold = true; });
  CHECK(t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(2, 1)).bold);
  t.d.SetBorders(t.sheet(), Rect{1, 0, 3, 2}, yc::engine::BorderPreset::Outside);
  const auto& st = t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(1, 0));
  CHECK(st.border[yc::model::kTop].style == yc::model::LineStyle::Thin);
  CHECK(st.border[yc::model::kLeft].style == yc::model::LineStyle::Thin);
  CHECK(st.border[yc::model::kRight].style == yc::model::LineStyle::None);
  t.Set("D5", "50%");
  CHECK(t.T("D5") == "50%");
  CHECK(t.F("D5") == "50%");
}

TEST_CASE("尋找與取代") {
  yctest::Doc t;
  t.Set("A1", "apple");
  t.Set("B3", "Pineapple");
  t.Set("C2", "=\"apple\"&1");
  uint32_t r = 0, c = 0;
  REQUIRE(t.d.Find(t.sheet(), Rect{0, 0, 1000, 100}, "APPLE", 0, 0, true, false, false, false, &r, &c));
  CHECK(r == 1);
  CHECK(c == 2);
  CHECK(t.d.Replace(t.sheet(), Rect{0, 0, 1000, 100}, "apple", "pear", false, true) == 1);
  CHECK(t.T("A1") == "pear");
}

TEST_CASE("Iterative calculation of circular references") {
  yctest::Doc t;
  t.Set("A1", "=(A1+10)/2");  // converges to 10
  CHECK(!t.d.circular_cells.empty());  // iteration off: reported, and the cell shows 0 (Excel)
  CHECK(t.N("A1") == 0);

  t.d.iterate = true;
  t.d.max_change = 1e-9;
  t.d.RecalcAll();
  CHECK(t.d.circular_cells.empty());
  CHECK(std::fabs(t.N("A1") - 10) < 1e-6);

  // two cells referencing each other: B1 = C1/2 + 1, C1 = B1/2 + 1 -> both 2
  t.Set("B1", "=C1/2+1");
  t.Set("C1", "=B1/2+1");
  CHECK(std::fabs(t.N("B1") - 2) < 1e-6);
  CHECK(std::fabs(t.N("C1") - 2) < 1e-6);
  CHECK(t.d.last_stats().iterations > 0);

  // the iteration count is capped
  t.d.max_iterations = 3;
  t.Set("D1", "=D1+1");
  CHECK(t.N("D1") <= 4);
}

TEST_CASE("BindTopics: pushed values go straight into the bound cells") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("E1", "=SUM(A1:B1)");
  t.d.BindTopics(sh, yc::core::Rect{0, 0, 0, 1}, {"QUOTE|x", "QUOTE|y"});
  t.d.UpdateTopic("QUOTE|x", yc::core::Value::Number(2));
  t.d.UpdateTopic("QUOTE|y", yc::core::Value::Number(5));
  t.d.Recalc();
  CHECK(t.N("A1") == 2);
  CHECK(t.N("B1") == 5);
  CHECK(t.N("E1") == 7);  // formulas that use the cells recalculate
  CHECK(t.d.BoundTopics(sh, yc::core::Rect{0, 0, 0, 2}) == std::vector<std::string>{"QUOTE|x", "QUOTE|y", ""});
  // a cell bound later shows the last value at once
  t.d.BindTopics(sh, yc::core::Rect::Cell(1, 0), {"QUOTE|x"});
  CHECK(t.N("A2") == 2);
  CHECK(t.d.GetTopicStats().bound_cells == 3);
  t.d.UnbindTopics(sh, yc::core::Rect{0, 0, 0, 0});
  t.d.UpdateTopic("QUOTE|x", yc::core::Value::Number(9));
  t.d.Recalc();
  CHECK(t.N("A1") == 2);  // unbound: keeps its last value
  CHECK(t.N("A2") == 9);
  CHECK(t.d.GetTopicStats().updates == 3);
}

TEST_CASE("manual calculation: an entered formula is computed, its dependents wait for Recalc (as in Excel)") {
  yctest::Doc t;
  t.d.auto_calc = false;
  auto input = [&](uint32_t r, uint32_t c, const char* text) { t.d.SetInput(t.sheet(), r, c, text); };
  input(0, 0, "5");             // A1
  input(0, 1, "=A1*2");         // B1
  input(0, 2, "=SUM(A1:B1)");   // C1
  t.d.CalcEntered();
  CHECK(t.N("B1") == 10);  // entered: computed at once
  CHECK(t.N("C1") == 15);
  input(0, 0, "7");
  t.d.CalcEntered();
  CHECK(t.N("B1") == 10);  // a precedent changed: stays until F9
  CHECK(t.N("C1") == 15);
  CHECK(t.d.has_pending());
  t.d.Recalc();
  CHECK(t.N("B1") == 14);
  CHECK(t.N("C1") == 21);
  CHECK_FALSE(t.d.has_pending());
}

TEST_CASE("manual calculation: pasted, filled and array formulas are computed as they are entered") {
  yctest::Doc t;
  t.d.auto_calc = false;
  auto input = [&](uint32_t r, uint32_t c, const char* text) { t.d.SetInput(t.sheet(), r, c, text); };
  input(0, 0, "5");       // A1
  input(1, 0, "7");       // A2
  input(2, 0, "9");       // A3
  input(0, 1, "=A1*2");   // B1
  t.d.CalcEntered();
  CHECK(t.N("B1") == 10);
  // Ctrl+Enter / paste / fill down all go through Paste: the copied value must not stay as the shown value
  const auto clip = t.d.Copy(t.sheet(), Rect::Cell(0, 1));
  t.d.Paste(t.sheet(), 1, 1, clip);
  t.d.FillDown(t.sheet(), Rect{1, 1, 2, 1});
  t.d.CalcEntered();
  CHECK(t.N("B2") == 14);
  CHECK(t.N("B3") == 18);
  // Ctrl+Shift+Enter
  CHECK(t.d.SetArrayFormula(t.sheet(), Rect{3, 1, 3, 1}, "SUM(A1:A3)"));
  t.d.CalcEntered();
  CHECK(t.N("B4") == 21);
  CHECK_FALSE(t.d.has_pending());
}

TEST_CASE("a formula past the last column is refused instead of writing outside the tile directory") {
  yctest::Doc t;
  std::string err;
  CHECK_FALSE(t.d.SetFormula(t.sheet(), 0, 16384, "1", &err));
  CHECK_FALSE(t.d.SetFormula(t.sheet(), 0, 0xFFFFFFFFu, "1", &err));
  CHECK(t.d.SetFormula(t.sheet(), 0, 16383, "1", &err));  // XFD1 is fine
}

TEST_CASE("CurrentRegion is linear in the region's size and stops at blank rows and columns") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (uint32_t r = 0; r < 20000; ++r)
    for (uint32_t c = 0; c < 5; ++c) t.d.SetValue(sh, r, c, yc::core::Value::Number(r + c), yc::engine::kSrcPython);
  t.d.SetValue(sh, 29, 6, yc::core::Value::Number(1), yc::engine::kSrcPython);   // an island at G30:H31
  t.d.SetValue(sh, 30, 7, yc::core::Value::Number(1), yc::engine::kSrcPython);
  t.d.Recalc();
  const auto& s = *t.d.S(sh);
  CHECK(yc::engine::CurrentRegion(s, 10, 2) == Rect{0, 0, 19999, 4});   // was quadratic: ~4e8 cell probes for this
  CHECK(yc::engine::CurrentRegion(s, 19999, 0) == Rect{0, 0, 19999, 4});
  CHECK(yc::engine::CurrentRegion(s, 29, 6) == Rect{29, 6, 30, 7});      // diagonal neighbours count, as in Excel
  CHECK(yc::engine::CurrentRegion(s, 25, 6) == Rect{25, 6, 25, 6});      // an empty cell with nothing around it
  CHECK(yc::engine::CurrentRegion(s, 100, 9) == Rect{100, 9, 100, 9});
}

TEST_CASE("Inserting or deleting cells recalculates ranges that reach past the shifted band") {
  yctest::Doc t;
  for (uint32_t r = 0; r < 30; ++r) {
    for (uint32_t c = 0; c < 8; ++c) t.d.SetInput(t.sheet(), r, c, std::to_string(r + 1));
  }
  t.Set("J5", "=SUM(A11:H28)");    // A:C shift, D:H stay: the text stays, the cells read change
  t.Set("J6", "=SUMIF(A11:H28,\">10\")");
  t.Set("J7", "=SUM(B15)");        // moves with its cell: same value
  REQUIRE(t.N("J5") == 2808);
  t.d.InsertCells(t.sheet(), Rect{8, 0, 29, 2}, true);  // A9:C30, shift down
  t.d.Recalc();
  CHECK(t.N("J5") == 1755);
  CHECK(t.N("J6") == 1755);
  CHECK(t.N("J7") == 15);
  t.d.DeleteCells(t.sheet(), Rect{8, 0, 29, 2}, true);  // and back up
  t.d.Recalc();
  // A11:C28 lie wholly in the deleted cells: the range loses those columns (Excel, measured: delete_up_B2_B5)
  CHECK(t.F("J5") == "=SUM(D11:H28)");
  CHECK(t.N("J5") == 1755);
  t.Set("J5", "=SUM(A11:H28)");
  CHECK(t.N("J5") == 2808);
  t.d.InsertCells(t.sheet(), Rect{9, 1, 11, 3}, false);  // B10:D12, shift right
  t.d.Recalc();
  CHECK(t.N("J5") == 2739);
  t.d.DeleteCells(t.sheet(), Rect{9, 1, 11, 3}, false);
  t.d.Recalc();
  CHECK(t.N("J5") == 2808);
}

TEST_CASE("Sheet names: at most 31 characters, counted in characters (not bytes)") {
  yctest::Doc t;
  const uint32_t s = t.sheet();
  std::string err;
  CHECK(t.d.RenameSheet(s, std::string(31, 'A'), &err));
  CHECK_FALSE(t.d.RenameSheet(s, std::string(32, 'A'), &err));  // 32 ASCII letters are only 32 bytes, still too many
  CHECK(err.find("31") != std::string::npos);
  std::string cjk;
  for (int i = 0; i < 31; ++i) cjk += "\xE5\x8F\xB0";  // 31 characters of 3 bytes each
  CHECK(t.d.RenameSheet(s, cjk, &err));
  CHECK_FALSE(t.d.RenameSheet(s, cjk + "\xE5\x8F\xB0", &err));
  CHECK(t.d.S(s)->name == cjk);
}

TEST_CASE("Without iteration a circular reference shows 0 and doesn't grow at each calculation") {
  yctest::Doc t;
  t.Set("C1", "=C1+1");
  REQUIRE(!t.d.circular_cells.empty());
  CHECK(t.N("C1") == 0);
  t.d.CalculateNow();
  CHECK(t.N("C1") == 0);
  t.d.RecalcAll();
  CHECK(t.N("C1") == 0);
  t.Set("F1", "5");  // an edit elsewhere
  CHECK(t.N("C1") == 0);
  t.Set("C1", "=1+1");  // broken: calculated again
  CHECK(t.N("C1") == 2);
}
