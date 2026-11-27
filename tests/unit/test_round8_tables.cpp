// SPDX-License-Identifier: Apache-2.0
// Round 8 (tables, measured against Excel 16 itself): a renamed table's name alone, INDIRECT of structured
// references, ListColumns.Add at the left edge, calculated columns overwritten, ShowHeaders, the totals row's default
// for text, header texts and repeated names, references to deleted columns and tables, Resize with a totals row and
// calculated columns, a table without data rows, escaped and spaced column names, Sheet!Table1[...] and the text
// Excel writes for structured references
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/content_dump.h"

#include <string>

using yc::core::Rect;

namespace {
// Table1: Name, Qty, Price in A1:C4 (a 1 10, b 2 20, c 3 30)
uint32_t SalesTable(yctest::Doc& t) {
  t.Set("A1", "Name");
  t.Set("B1", "Qty");
  t.Set("C1", "Price");
  const char* names[] = {"a", "b", "c"};
  for (int i = 0; i < 3; ++i) {
    const std::string row = std::to_string(i + 2);
    t.Set(("A" + row).c_str(), names[i]);
    t.Set(("B" + row).c_str(), std::to_string(i + 1));
    t.Set(("C" + row).c_str(), std::to_string((i + 1) * 10));
  }
  std::string err;
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 3, 2}, true, "TableStyleMedium2", &err);
  t.d.Recalc();
  return id;
}

const yc::model::Table& TableOf(yctest::Doc& t, uint32_t id) { return *t.d.TableById(t.sheet(), id); }

std::string RefersTo(yctest::Doc& t, const std::string& name) {
  for (const auto& n : *t.d.wb().names) {
    if (n.name == name) return n.refers_to;
  }
  return "<none>";
}
}  // namespace

TEST_CASE("Round8 tables: renaming a table renames its name alone in formulas, names and rules") {
  yctest::Doc t;
  const uint32_t id = SalesTable(t);
  REQUIRE(id != 0);
  t.Set("E1", "=ROWS(Table1)");
  t.Set("E2", "=SUM(Table1)");
  t.Set("E3", "=SUM(Table1[Qty])");
  std::string err;
  REQUIRE(t.d.DefineName(yc::model::NameDef{"Tot", "=SUM(Table1[Price])", -1}, &err));
  yc::model::CfRule rule;
  rule.type = yc::model::CfType::Expression;
  rule.formula1 = "ROWS(Table1)>2";
  rule.ranges = {Rect::Cell(5, 0)};
  rule.fmt.bold = 1;
  t.d.AddCondFormat(t.sheet(), rule);
  REQUIRE(t.d.RenameTable(t.sheet(), id, "Sales", &err));
  t.d.Recalc();
  CHECK(t.F("E1") == "=ROWS(Sales)");  // stayed =ROWS(Table1), 1
  CHECK(t.N("E1") == 3);
  CHECK(t.F("E2") == "=SUM(Sales)");  // #NAME?
  CHECK(t.N("E2") == 66);
  CHECK(t.F("E3") == "=SUM(Sales[Qty])");
  CHECK(RefersTo(t, "Tot") == "SUM(Sales[Price])");
  CHECK((*t.d.S(t.sheet())->cond_formats)[0].formula1 == "ROWS(Sales)>2");
}

TEST_CASE("Round8 tables: INDIRECT reads structured references") {
  yctest::Doc t;
  SalesTable(t);
  CHECK(t.Eval("SUM(INDIRECT(\"Table1[Qty]\"))") == "6");  // #REF!
  CHECK(t.Eval("ROWS(INDIRECT(\"Table1[#All]\"))") == "4");
  CHECK(t.Eval("SUM(INDIRECT(\"Table1\"))") == "66");
  CHECK(t.Eval("SUM(INDIRECT(\"Table1[[#Headers],[Qty]]\"))") == "0");
  CHECK(t.Eval("INDIRECT(\"Table1[Nope]\")") == "#REF!");
}

TEST_CASE("Round8 tables: ListColumns.Add(1) adds the table's first column instead of moving the table") {
  yctest::Doc t;
  const uint32_t id = SalesTable(t);
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.totals = true; }));
  t.d.Recalc();
  REQUIRE(t.d.TableInsertColumn(t.sheet(), id, 0));
  t.d.Recalc();
  CHECK(TableOf(t, id).range == Rect{0, 0, 4, 3});  // B1:D5: the table only moved right
  REQUIRE(TableOf(t, id).columns.size() == 4);
  CHECK(t.T("A1") == "Column1");
  CHECK(TableOf(t, id).columns[1].name == "Name");
  CHECK(t.T("B2") == "a");
  CHECK(t.V("A2").IsEmpty());
  CHECK(t.T("B5") == "Total");  // the totals row follows its columns
  CHECK(t.F("D5") == "=SUBTOTAL(109,[Price])");
  CHECK(t.N("D5") == 60);
  CHECK(t.Eval("SUM(Table1[Qty])") == "6");
  // inside the table the totals stay with their columns too
  REQUIRE(t.d.TableInsertColumn(t.sheet(), id, 2));
  CHECK(TableOf(t, id).columns[3].name == "Qty");
  CHECK(TableOf(t, id).columns[4].totals_func == 109);
  CHECK(t.F("E5") == "=SUBTOTAL(109,[Price])");
}

TEST_CASE("Round8 tables: a formula typed over a calculated column replaces it in every row") {
  yctest::Doc t;
  SalesTable(t);
  t.Set("D1", "Twice");
  t.Set("D2", "=[@Qty]*2");
  CHECK(t.F("D4") == "=[@Qty]*2");
  t.Set("D3", "=[@Qty]*3");
  CHECK(t.F("D2") == "=[@Qty]*3");  // only D3 changed
  CHECK(t.F("D4") == "=[@Qty]*3");
  CHECK(t.N("D4") == 9);
  // a column whose cells differ isn't one formula: typing keeps the others
  t.Set("D4", "7");
  t.Set("D2", "=[@Qty]*4");
  CHECK(t.F("D3") == "=[@Qty]*3");
  CHECK(t.N("D4") == 7);
}

TEST_CASE("Round8 tables: ShowHeaders off starts the table at its data, on brings the names back") {
  yctest::Doc t;
  const uint32_t id = SalesTable(t);
  t.Set("E1", "=Table1[[#Headers],[Qty]]");
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.header = false; }));
  t.d.Recalc();
  CHECK(TableOf(t, id).range == Rect{1, 0, 3, 2});  // A2:C4 (it stayed A1:C4 with the header row as data)
  CHECK(TableOf(t, id).Data() == Rect{1, 0, 3, 2});
  CHECK(t.V("A1").IsEmpty());
  CHECK(t.V("B1").IsEmpty());
  CHECK(TableOf(t, id).columns[1].name == "Qty");
  CHECK(t.T("E1") == "#REF!");
  CHECK(t.Eval("SUM(Table1[Qty])") == "6");
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.header = true; }));
  t.d.Recalc();
  CHECK(TableOf(t, id).range == Rect{0, 0, 3, 2});
  CHECK(t.T("A1") == "Name");
  CHECK(t.T("C1") == "Price");
  CHECK(t.T("A2") == "a");
  CHECK(t.T("E1") == "Qty");
}

TEST_CASE("Round8 tables: ShowHeaders on at the top of the sheet makes a row for the headers") {
  yctest::Doc t;
  const uint32_t id = SalesTable(t);
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.header = false; }));
  t.Set("A1", "busy");
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.header = true; }));
  CHECK(TableOf(t, id).range == Rect{1, 0, 4, 2});
  CHECK(t.T("A1") == "busy");
  CHECK(t.T("A2") == "Name");
  CHECK(t.T("A3") == "a");
}

TEST_CASE("Round8 tables: the totals row counts a last column of text") {
  yctest::Doc t;
  t.Set("A1", "Qty");
  t.Set("B1", "Name");
  t.Set("A2", "1");
  t.Set("B2", "a");
  t.Set("A3", "2");
  t.Set("B3", "b");
  std::string err;
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 2, 1}, true, "", &err);
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.totals = true; }));
  t.d.Recalc();
  CHECK(t.F("B4") == "=SUBTOTAL(103,[Name])");  // 109: a sum of text, 0
  CHECK(t.N("B4") == 2);
  CHECK(t.T("A4") == "Total");
}

TEST_CASE("Round8 tables: headers become text, and repeated ones skip the names other headers hold") {
  yctest::Doc t;
  t.Set("A1", "2024");
  t.Set("B1", "TRUE");
  t.Set("C1", "=1+1");
  t.Set("A2", "1");
  t.Set("B2", "1");
  t.Set("C2", "1");
  std::string err;
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 1, 2}, true, "", &err);
  REQUIRE(id != 0);
  CHECK(t.V("A1").IsString());  // kept the number
  CHECK(t.T("A1") == "2024");
  CHECK(t.V("B1").IsString());
  CHECK(t.T("B1") == "TRUE");
  CHECK(t.V("C1").IsString());
  CHECK(t.T("C1") == "2");
  CHECK(t.d.S(t.sheet())->GetFormula(0, 2) == 0);  // stayed a live formula
  CHECK(t.Eval("SUM(Table1[2024])") == "1");
  // a formula written into a header later is its value before it is calculated, as in Excel
  t.Set("C1", "=5*5");
  CHECK(t.V("C1").IsString());
  CHECK(t.T("C1") == "0");
  CHECK(t.d.S(t.sheet())->GetFormula(0, 2) == 0);
  CHECK(TableOf(t, id).columns[2].name == "0");
  // a, a, (blank), A, a2: Excel's a, a3, Column4, A5, a2 (it renamed the user's own a2)
  yctest::Doc u;
  const char* heads[] = {"a", "a", "", "A", "a2"};
  for (int j = 0; j < 5; ++j) {
    const std::string col(1, static_cast<char>('A' + j));
    if (heads[j][0]) u.Set((col + "1").c_str(), heads[j]);
    u.Set((col + "2").c_str(), "1");
  }
  REQUIRE(u.d.AddTable(u.sheet(), Rect{0, 0, 1, 4}, true, "", &err) != 0);
  CHECK(u.T("A1") == "a");
  CHECK(u.T("B1") == "a3");
  CHECK(u.T("C1") == "Column4");
  CHECK(u.T("D1") == "A5");
  CHECK(u.T("E1") == "a2");
}

TEST_CASE("Round8 tables: references to a deleted column or table are #REF!") {
  yctest::Doc t;
  const uint32_t id = SalesTable(t);
  t.Set("E1", "=SUM(Table1[Qty])");
  t.Set("E2", "=SUM(Table1[Price])");
  t.Set("E3", "=SUM(Table1[[Name]:[Price]])");
  std::string err;
  REQUIRE(t.d.DefineName(yc::model::NameDef{"QtyCol", "=Table1[Qty]", -1}, &err));
  REQUIRE(t.d.TableDeleteColumn(t.sheet(), id, 1));
  t.d.Recalc();
  CHECK(t.F("D1") == "=SUM(#REF!)");  // kept Table1[Qty], and came back with a column named Qty
  CHECK(t.F("D2") == "=SUM(Table1[Price])");
  CHECK(t.N("D2") == 60);
  CHECK(t.F("D3") == "=SUM(Table1[[Name]:[Price]])");
  CHECK(RefersTo(t, "QtyCol") == "#REF!");
  REQUIRE(TableOf(t, id).columns.size() == 2);
  CHECK(TableOf(t, id).columns[1].name == "Price");
  t.Set("B1", "Qty");
  CHECK(t.F("D1") == "=SUM(#REF!)");
  // Resize that leaves a column out
  yctest::Doc r;
  const uint32_t rid = SalesTable(r);
  r.Set("E1", "=SUM(Table1[Price])");
  r.Set("E2", "=SUM(Table1[Qty])");
  REQUIRE(r.d.ResizeTable(r.sheet(), rid, Rect{0, 0, 3, 1}, &err));
  r.d.Recalc();
  CHECK(r.F("E1") == "=SUM(#REF!)");
  CHECK(r.F("E2") == "=SUM(Table1[Qty])");
  // ListObject.Delete: the table's references are #REF! (they pointed at the emptied cells)
  yctest::Doc d;
  const uint32_t did = SalesTable(d);
  d.Set("E1", "=SUM(Table1[Qty])");
  d.Set("E2", "=ROWS(Table1)");
  REQUIRE(d.d.DeleteTable(d.sheet(), did, false));
  d.d.Recalc();
  CHECK(d.F("E1") == "=SUM(#REF!)");
  CHECK(d.F("E2") == "=ROWS(#REF!)");
  // Convert to Range keeps them as cells, the name alone too
  yctest::Doc c;
  const uint32_t cid = SalesTable(c);
  c.Set("E1", "=SUM(Table1[Qty])");
  c.Set("E2", "=ROWS(Table1)");
  REQUIRE(c.d.DeleteTable(c.sheet(), cid, true));
  c.d.Recalc();
  CHECK(c.F("E1") == "=SUM($B$2:$B$4)");
  CHECK(c.F("E2") == "=ROWS($A$2:$C$4)");  // #NAME?
  CHECK(c.N("E2") == 3);
}

TEST_CASE("Round8 tables: Resize moves the totals row and continues calculated columns") {
  yctest::Doc t;
  t.Set("A1", "Name");
  t.Set("B1", "Qty");
  t.Set("A2", "a");
  t.Set("B2", "1");
  t.Set("A3", "b");
  t.Set("B3", "2");
  std::string err;
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 2, 1}, true, "", &err);
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.totals = true; }));
  t.d.Recalc();
  REQUIRE(t.F("B4") == "=SUBTOTAL(109,[Qty])");
  REQUIRE(t.d.ResizeTable(t.sheet(), id, Rect{0, 0, 4, 1}, &err));
  t.d.Recalc();
  CHECK(TableOf(t, id).range == Rect{0, 0, 4, 1});
  CHECK(t.T("A5") == "Total");  // stayed in row 4, row 5 blank
  CHECK(t.F("B5") == "=SUBTOTAL(109,[Qty])");
  CHECK(t.N("B5") == 3);
  CHECK(t.V("A4").IsEmpty());
  CHECK(t.V("B4").IsEmpty());
  // a calculated column continues into the new rows
  yctest::Doc u;
  const uint32_t uid = SalesTable(u);
  u.Set("D1", "Twice");
  u.Set("D2", "=[@Qty]*2");
  REQUIRE(u.d.ResizeTable(u.sheet(), uid, Rect{0, 0, 5, 3}, &err));
  u.d.Recalc();
  CHECK(u.F("D5") == "=[@Qty]*2");  // blank
  CHECK(u.F("D6") == "=[@Qty]*2");
  CHECK(u.V("C5").IsEmpty());
}

TEST_CASE("Round8 tables: the last data row can be deleted, the table keeps its header") {
  yctest::Doc t;
  const uint32_t id = SalesTable(t);
  t.Set("A6", "x");
  REQUIRE(t.d.TableDeleteRow(t.sheet(), id, 0));
  REQUIRE(t.d.TableDeleteRow(t.sheet(), id, 0));
  CHECK(t.T("A4") == "x");  // the rows below the table's cells move up with them
  REQUIRE(t.d.TableDeleteRow(t.sheet(), id, 0));  // refused: the table row can't be changed
  t.d.Recalc();
  CHECK(TableOf(t, id).range == Rect{0, 0, 0, 2});
  CHECK_FALSE(TableOf(t, id).HasData());
  CHECK(t.V("A2").IsEmpty());
  CHECK(t.V("C2").IsEmpty());
  CHECK(t.T("A4") == "x");  // the emptied row stays
  CHECK(t.Eval("SUM(Table1[Qty])") == "0");
  // typing under the header gives it a row again
  t.Set("A2", "d");
  CHECK(TableOf(t, id).range == Rect{0, 0, 1, 2});
}

TEST_CASE("Round8 tables: column names with escaped brackets or leading spaces") {
  yctest::Doc t;
  t.Set("A1", "[x]");
  t.d.SetValue(t.sheet(), 0, 1, yc::core::Value::String("  lead"));  // as typed, with its spaces
  t.Set("C1", "Unit Price");
  t.Set("A2", "4");
  t.Set("B2", "8");
  t.Set("C2", "1");
  std::string err;
  REQUIRE(t.d.AddTable(t.sheet(), Rect{0, 0, 1, 2}, true, "", &err) != 0);
  t.Set("E1", "=SUM(Table1['[x']])");
  CHECK(t.F("E1") == "=SUM(Table1['[x']])");  // Table1[[x']]], #REF!
  CHECK(t.N("E1") == 4);
  t.Set("E2", "=SUM(Table1[[  lead]])");
  CHECK(t.F("E2") == "=SUM(Table1[[  lead]])");  // Table1[lead], #REF!
  CHECK(t.N("E2") == 8);
  // one column without inner brackets, as Excel writes it
  t.Set("E3", "=SUM(Table1[[Unit Price]])");
  CHECK(t.F("E3") == "=SUM(Table1[Unit Price])");
  CHECK(t.N("E3") == 1);
  t.Set("E4", "=SUM(Table1[[#Headers],['[x']]])");
  CHECK(t.F("E4") == "=SUM(Table1[[#Headers],['[x']]])");
}

TEST_CASE("Round8 tables: Sheet!Table1[...] is taken and written without the sheet") {
  yctest::Doc t;
  SalesTable(t);
  const std::string sheet = t.d.wb().sheets[0]->name;
  REQUIRE(t.Set("E1", "=SUM(" + sheet + "!Table1[Qty])"));  // a formula error
  CHECK(t.F("E1") == "=SUM(Table1[Qty])");
  CHECK(t.N("E1") == 6);
}

TEST_CASE("Round8 tables: structured references read back as Excel writes them") {
  yctest::Doc t;
  SalesTable(t);
  t.Set("E1", "=ROWS(Table1[#Data])");
  CHECK(t.F("E1") == "=ROWS(Table1[#Data])");  // =ROWS(Table1)
  CHECK(t.N("E1") == 3);
  t.Set("E2", "=SUM(Table1[[#Data],[Qty]:[Price]])");
  CHECK(t.F("E2") == "=SUM(Table1[[#Data],[Qty]:[Price]])");
  t.Set("E3", "=SUM(Table1[[#Headers],[#Data],[Qty]])");
  CHECK(t.F("E3") == "=SUM(Table1[[#Headers],[#Data],[Qty]])");  // #Data first
  t.Set("E4", "=SUM(Table1[@])");
  CHECK(t.F("E4") == "=SUM(Table1[@])");  // Table1[#This Row]
  // inside its own table a formula leaves the table's name out
  t.Set("D1", "Next");
  t.Set("D2", "=Table1[[#This Row],[Qty]]+1");
  CHECK(t.F("D2") == "=[@Qty]+1");  // =Table1[@Qty]+1
  CHECK(t.F("D4") == "=[@Qty]+1");
  CHECK(t.N("D4") == 4);
  t.Set("D3", "=Table1[@Qty]+SUM(Table1[Qty])");
  CHECK(t.F("D3") == "=[@Qty]+SUM([Qty])");
  CHECK(t.N("D2") == 7);
  t.Set("F2", "=Table1[@Qty]");  // outside it keeps it
  CHECK(t.F("F2") == "=Table1[@Qty]");
}

TEST_CASE("A table column deleted any way leaves #REF! where formulas pointed at it (Excel 16, measured)") {
  for (int how = 0; how < 3; ++how) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    const char* cells[3][3] = {{"k", "v", "w"}, {"a", "1", "10"}, {"b", "2", "20"}};
    for (uint32_t r = 0; r < 3; ++r) {
      for (uint32_t c = 0; c < 3; ++c) t.d.SetInput(sh, r, c, cells[r][c]);
    }
    const uint32_t id = t.d.AddTable(sh, yc::core::Rect{0, 0, 2, 2}, true, "TableStyleMedium2");
    REQUIRE(id);
    const std::string tn = t.d.S(sh)->tables->front().name;
    t.d.SetInput(sh, 0, 7, ("=SUM(" + tn + "[v])").c_str());  // H1
    t.d.SetInput(sh, 1, 7, ("=SUM(" + tn + "[w])").c_str());  // H2
    if (how == 0) t.d.DeleteCols(sh, 1, 1);                                     // column B, the middle one
    if (how == 1) t.d.DeleteCells(sh, yc::core::Rect{0, 1, 2, 1}, false);       // B1:B3 shifted left
    if (how == 2) REQUIRE(t.d.TableDeleteColumn(sh, id, 1));                     // Table Design: Delete Table Columns
    t.d.Recalc();
    const uint32_t col = 6;  // H moves to G: with the whole column, or with the table's rows
    CAPTURE(how);
    const auto* tb = t.d.S(sh)->tables->empty() ? nullptr : &t.d.S(sh)->tables->front();
    REQUIRE(tb);
    REQUIRE(tb->columns.size() == 2);
    CHECK(tb->columns[0].name == "k");
    CHECK(tb->columns[1].name == "w");
    // H1 moved to G1 with its row (row 1 is one of the table's rows); H2 too
    CHECK(t.d.GetFormulaA1(sh, 0, col) == "=SUM(#REF!)");
    CHECK(t.d.GetFormulaA1(sh, 1, col) == "=SUM(" + tn + "[w])");
    CHECK(t.d.S(sh)->GetValue(1, col).AsDouble() == 30);
    // a column named v again (typed beside the table, which grows) doesn't take the formula over
    t.d.SetInput(sh, 0, 2, "v");
    t.d.Recalc();
    CHECK(t.d.GetFormulaA1(sh, 0, col) == "=SUM(#REF!)");
  }
}

TEST_CASE("A table deleted with its rows, columns or cells leaves #REF! where formulas named it (Excel 16, measured)") {
  for (int how = 0; how < 3; ++how) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    const char* cells[3][2] = {{"k", "v"}, {"a", "1"}, {"b", "2"}};
    for (uint32_t r = 0; r < 3; ++r) {
      for (uint32_t c = 0; c < 2; ++c) t.d.SetInput(sh, r + 2, c, cells[r][c]);  // A3:B5
    }
    REQUIRE(t.d.AddTable(sh, yc::core::Rect{2, 0, 4, 1}, true, "TableStyleMedium2"));
    const std::string tn = t.d.S(sh)->tables->front().name;
    t.d.SetInput(sh, 0, 7, ("=SUM(" + tn + "[v])").c_str());  // H1
    t.d.SetInput(sh, 1, 7, ("=ROWS(" + tn + ")").c_str());     // H2
    if (how == 0) t.d.DeleteRows(sh, 2, 3);
    if (how == 1) t.d.DeleteCols(sh, 0, 2);
    if (how == 2) t.d.DeleteCells(sh, yc::core::Rect{2, 0, 4, 1}, true);
    t.d.Recalc();
    CAPTURE(how);
    CHECK(t.d.S(sh)->tables->empty());
    const uint32_t col = how == 1 ? 5 : 7;
    CHECK(t.d.GetFormulaA1(sh, 0, col) == "=SUM(#REF!)");
    CHECK(t.d.GetFormulaA1(sh, 1, col) == "=ROWS(#REF!)");
    // a table made again under that name doesn't take them over
    t.d.SetInput(sh, 2, 0, "k");
    t.d.SetInput(sh, 2, 1, "v");
    t.d.SetInput(sh, 3, 0, "a");
    REQUIRE(t.d.AddTable(sh, yc::core::Rect{2, 0, 3, 1}, true, "TableStyleMedium2"));
    t.d.RenameTable(sh, t.d.S(sh)->tables->front().id, tn);
    CHECK(t.d.GetFormulaA1(sh, 0, col) == "=SUM(#REF!)");
  }
}

TEST_CASE("Clearing a whole table deletes it; clearing its body or header keeps it (Excel 16, measured)") {
  for (int how = 0; how < 4; ++how) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    const char* cells[3][2] = {{"k", "v"}, {"a", "1"}, {"b", "2"}};
    for (uint32_t r = 0; r < 3; ++r) {
      for (uint32_t c = 0; c < 2; ++c) t.d.SetInput(sh, r + 2, c, cells[r][c]);  // A3:B5
    }
    REQUIRE(t.d.AddTable(sh, yc::core::Rect{2, 0, 4, 1}, true, "TableStyleMedium2"));
    const std::string tn = t.d.S(sh)->tables->front().name;
    t.d.SetInput(sh, 0, 7, ("=SUM(" + tn + "[v])").c_str());  // H1
    const Rect whole{2, 0, 4, 1}, bigger{0, 0, 8, 3}, body{3, 0, 4, 1};
    const Rect r = how == 0 ? whole : how == 1 ? bigger : body;
    t.d.ClearTablesIn(sh, r);
    if (how == 3) t.d.ClearAll(sh, r, yc::engine::kSrcUser, true);
    else t.d.ClearContents(sh, r);
    t.d.Recalc();
    CAPTURE(how);
    if (how < 2) {
      CHECK(t.d.S(sh)->tables->empty());
      CHECK(t.d.GetFormulaA1(sh, 0, 7) == "=SUM(#REF!)");
    } else {
      REQUIRE(t.d.S(sh)->tables->size() == 1);
      CHECK(t.d.GetFormulaA1(sh, 0, 7) == "=SUM(" + tn + "[v])");
      CHECK(t.d.S(sh)->GetValue(0, 7).AsDouble() == 0);
    }
  }
}

TEST_CASE("A whole table cut and pasted goes with its cells; part of one leaves it (Excel 16, measured)") {
  for (int how = 0; how < 4; ++how) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    const uint32_t other = t.d.AddSheet("Other");
    const char* cells[3][2] = {{"k", "v"}, {"a", "1"}, {"b", "2"}};
    for (uint32_t r = 0; r < 3; ++r) {
      for (uint32_t c = 0; c < 2; ++c) t.d.SetInput(sh, r + 2, c, cells[r][c]);  // A3:B5
    }
    REQUIRE(t.d.AddTable(sh, Rect{2, 0, 4, 1}, true, "TableStyleMedium2"));
    const std::string tn = t.d.S(sh)->tables->front().name;
    t.d.SetInput(sh, 0, 7, ("=SUM(" + tn + "[v])").c_str());  // H1
    t.d.Recalc();
    const std::string before = yc::io::ContentDump(t.d.wb());
    t.d.Begin("move");
    if (how == 0) REQUIRE(t.d.MoveCells(sh, Rect{2, 0, 4, 1}, sh, 9, 3));   // A3:B5 -> D10
    if (how == 1) REQUIRE(t.d.MoveCells(sh, Rect{1, 0, 5, 2}, sh, 9, 3));   // A2:C6 -> D10 (the table at D11)
    if (how == 2) REQUIRE(t.d.MoveCells(sh, Rect{3, 0, 3, 1}, sh, 9, 3));   // A4:B4, a data row
    if (how == 3) REQUIRE(t.d.MoveCells(sh, Rect{2, 0, 4, 1}, other, 9, 3));
    t.d.Commit();
    t.d.Recalc();
    CAPTURE(how);
    CHECK(t.d.GetFormulaA1(sh, 0, 7) == "=SUM(" + tn + "[v])");
    if (how == 2) {
      REQUIRE(t.d.S(sh)->tables->size() == 1);
      CHECK(t.d.S(sh)->tables->front().range == Rect{2, 0, 4, 1});
      CHECK(t.d.S(sh)->GetValue(0, 7).AsDouble() == 2);
    } else {
      const uint32_t at = how == 3 ? other : sh;
      CHECK(t.d.S(how == 3 ? sh : other)->tables->empty());
      REQUIRE(t.d.S(at)->tables->size() == 1);
      const yc::model::Table& tb = t.d.S(at)->tables->front();
      CHECK(tb.name == tn);
      CHECK(tb.range == (how == 1 ? Rect{10, 3, 12, 4} : Rect{9, 3, 11, 4}));
      CHECK(tb.columns[1].name == "v");
      CHECK(t.d.S(sh)->GetValue(0, 7).AsDouble() == 3);
    }
    REQUIRE(t.d.Undo());
    t.d.Recalc();
    CHECK(yc::io::ContentDump(t.d.wb()) == before);
    REQUIRE(t.d.S(sh)->tables->size() == 1);
    CHECK(t.d.S(sh)->tables->front().range == Rect{2, 0, 4, 1});
  }
}

TEST_CASE("Round8 tables: the cells a table grows over take its formats, and only a calculated column continues") {
  // Excel 16 (measured, tblgrow_ cases and rand3_054)
  yctest::Doc t;
  const uint32_t id = SalesTable(t);
  auto numfmt = [&](const char* a1, const char* code) {
    const auto p = t.P(a1);
    t.d.ModifyStyle(t.sheet(), Rect::Cell(p.row, p.col), [&](yc::model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString(code); });
  };
  numfmt("C4", "0.00");
  numfmt("C5", "0%");  // a format of its own, under the table
  t.Set("C3", "=B3*7");
  t.Set("C4", "=B4*7");  // C2 holds a value: not a calculated column
  t.Set("C5", "=B4*2");  // 6
  t.d.Recalc();
  REQUIRE(TableOf(t, id).range == Rect{0, 0, 4, 2});
  CHECK(t.T("C5") == "6.00");  // the last row's 0.00 over its own 0%
  t.Set("A6", "d");
  t.d.Recalc();
  REQUIRE(TableOf(t, id).range == Rect{0, 0, 5, 2});
  CHECK(t.V("C6").IsEmpty());  // =B4*7 in C3:C4 only: nothing continues
  // beside it: the header is the text of what was typed in the cell's own format, then takes the header's format
  numfmt("C1", "0.00");
  numfmt("D1", "0%");
  t.Set("D1", "12.5");
  t.d.Recalc();
  REQUIRE(TableOf(t, id).range == Rect{0, 0, 5, 3});
  CHECK(t.V("D1").IsString());
  CHECK(t.T("D1") == "1250%");
  const auto& st = t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(0, 3));
  CHECK(t.d.wb().styles->NumberFormat(st) == "0.00");
}

TEST_CASE("Round8 tables: a table growing down takes in the ranges that end at its last row") {
  // Excel 16 (measured, tblgrow_refs_ cases): under B1:C3, C4 typed makes =SUM(B2:C3) =SUM(B2:C4), and so the
  // header's C1:C3, $C$2:$C$3 and a name's; one row, one cell and a range wider than the table stay
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.d.RenameSheet(sh, "Sheet1");
  t.Set("B1", "K");
  t.Set("C1", "V");
  for (int r = 2; r <= 3; ++r) {
    t.Set(("B" + std::to_string(r)).c_str(), std::to_string(r).c_str());
    t.Set(("C" + std::to_string(r)).c_str(), std::to_string(r * 10).c_str());
  }
  REQUIRE(t.d.AddTable(sh, Rect{0, 1, 2, 2}, true, "") != 0);
  t.Set("G1", "=SUM(B2:C3)");
  t.Set("G2", "=SUM($C$2:$C$3)");
  t.Set("G3", "=ROWS(C1:C3)");
  t.Set("G4", "=SUM(C3:C3)");
  t.Set("G5", "=C3");
  t.Set("G6", "=SUM(B2:D3)");
  REQUIRE(t.d.AddName("Rng", "=Sheet1!$C$2:$C$3"));
  t.Set("G7", "=ROWS(Rng)");
  t.d.Recalc();
  t.d.Begin("Typing");
  t.Set("C4", "7");
  t.d.Commit();
  t.d.Recalc();
  REQUIRE(t.d.S(sh)->tables->front().range == Rect{0, 1, 3, 2});
  CHECK(t.F("G1") == "=SUM(B2:C4)");
  CHECK(t.N("G1") == 62);
  CHECK(t.F("G2") == "=SUM($C$2:$C$4)");
  CHECK(t.F("G3") == "=ROWS(C1:C4)");
  CHECK(t.F("G4") == "=SUM(C3:C3)");
  CHECK(t.F("G5") == "=C3");
  CHECK(t.F("G6") == "=SUM(B2:D3)");
  CHECK(t.N("G7") == 3);
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.F("G1") == "=SUM(B2:C3)");
  CHECK(t.N("G7") == 2);
}

TEST_CASE("Round8: a formula refused under a table changes nothing, the table's size and the formula's range included") {
  // F2, 9, Enter on =SUM(B2:B4) right under a table: the entry is refused (extra content after the formula), and the
  // table grew over it first, its range followed to =SUM(B2:B5), a circular reference, with an Undo step (basics
  // seq470822). Excel leaves everything as it was
  yctest::Doc t;
  t.Set("A1", "Item");
  t.Set("B1", "Qty");
  for (int i = 2; i <= 4; ++i) {
    t.Set(("A" + std::to_string(i)).c_str(), "x");
    t.Set(("B" + std::to_string(i)).c_str(), std::to_string(i));
  }
  t.Set("B5", "=SUM(B2:B4)");  // before the table: typed under it, it would grow the table
  std::string err;
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 3, 1}, true, "", &err);
  REQUIRE(id);
  t.d.Recalc();
  t.d.ClearUndo();
  const size_t depth = t.d.UndoDepth();
  t.d.Begin("typing");
  std::string why;
  CHECK_FALSE(t.d.SetInput(t.sheet(), 4, 1, "=SUM(B2:B4)9", &why));
  t.d.Commit();
  t.d.Recalc();
  CHECK_FALSE(why.empty());
  CHECK(t.F("B5") == "=SUM(B2:B4)");
  CHECK(t.d.TableById(t.sheet(), id)->range.r1 == 3);
  CHECK(t.d.circular_cells.empty());
  CHECK(t.d.UndoDepth() == depth);
}

TEST_CASE("Round8: typing beside a table grows it only over a line that holds nothing else") {
  // Excel 16, measured (keys posted to its sheet window): 9 typed in C7 under A1:C6 grows the table while row 7 is
  // empty, and doesn't while B7 holds =SUM(B2:B6), 5 or text; the same for D2 beside it with D4 holding something
  auto make = [](yctest::Doc& t, const char* other, const char* text) {
    t.Set("A1", "Item");
    t.Set("B1", "Qty");
    t.Set("C1", "Price");
    for (int i = 2; i <= 6; ++i) {
      const std::string row = std::to_string(i);
      t.Set(("A" + row).c_str(), "x");
      t.Set(("B" + row).c_str(), std::to_string(i));
      t.Set(("C" + row).c_str(), std::to_string(i * 10));
    }
    if (other) t.Set(other, text);
    std::string err;
    const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 5, 2}, true, "", &err);
    REQUIRE(id);
    return id;
  };
  {
    yctest::Doc t;
    const uint32_t id = make(t, nullptr, nullptr);
    t.Set("C7", "9");
    CHECK(t.d.TableById(t.sheet(), id)->range.r1 == 6);
  }
  for (const char* text : {"=SUM(B2:B6)", "5", "note"}) {
    yctest::Doc t;
    const uint32_t id = make(t, "B7", text);
    t.Set("C7", "9");
    CHECK(t.d.TableById(t.sheet(), id)->range.r1 == 5);
    CHECK(t.F("B7") == text);
    CHECK(t.d.circular_cells.empty());
  }
  {
    yctest::Doc t;
    const uint32_t id = make(t, nullptr, nullptr);
    t.Set("D2", "9");
    CHECK(t.d.TableById(t.sheet(), id)->range.c1 == 3);
  }
  {
    yctest::Doc t;
    const uint32_t id = make(t, "D4", "5");
    t.Set("D2", "9");
    CHECK(t.d.TableById(t.sheet(), id)->range.c1 == 2);
  }
}
