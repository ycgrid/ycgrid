// SPDX-License-Identifier: Apache-2.0
// Tables (ListObjects) and structured references
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/ycg.h"

using yc::core::Rect;

namespace {
uint32_t MakeTable(yctest::Doc& t) {
  t.Set("A1", "Item");
  t.Set("B1", "Qty");
  t.Set("C1", "Price");
  const char* items[] = {"pen", "ink", "pad"};
  for (int i = 0; i < 3; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), items[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(i + 1));
    t.Set(("C" + std::to_string(i + 2)).c_str(), std::to_string((i + 1) * 10));
  }
  std::string err;
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 3, 2}, true, "TableStyleMedium2", &err);
  t.d.Recalc();
  return id;
}
}  // namespace

TEST_CASE("structured references") {
  yctest::Doc t;
  const uint32_t id = MakeTable(t);
  REQUIRE(id != 0);
  CHECK(t.d.TableById(t.sheet(), id)->name == "Table1");
  CHECK(t.Eval("SUM(Table1[Qty])") == "6");
  CHECK(t.Eval("ROWS(Table1)") == "3");
  CHECK(t.Eval("ROWS(Table1[#All])") == "4");
  CHECK(t.Eval("INDEX(Table1[[#Headers],[Price]],1)") == "Price");
  CHECK(t.Eval("SUM(Table1[[Qty]:[Price]])") == "66");
  CHECK(t.Eval("COLUMNS(Table1[#Headers])") == "3");
  t.Set("E1", "=SUM(Table1[Price])");
  CHECK(t.F("E1") == "=SUM(Table1[Price])");
  CHECK(t.N("E1") == 60);
  // this row, inside the table: a new column (auto-expansion) with a calculated column
  t.Set("D1", "Total");
  REQUIRE(t.d.TableById(t.sheet(), id)->range == Rect{0, 0, 3, 3});
  t.Set("D2", "=[@Qty]*[@Price]");
  CHECK(t.F("D2") == "=[@Qty]*[@Price]");
  CHECK(t.N("D2") == 10);
  CHECK(t.N("D4") == 90);  // filled down the column
  // a new row below grows the table and continues the formula
  t.Set("A5", "cap");
  t.Set("B5", "2");
  t.Set("C5", "5");
  CHECK(t.d.TableById(t.sheet(), id)->range.r1 == 4);
  CHECK(t.N("D5") == 10);
  CHECK(t.N("E1") == 65);  // the dependency follows the bigger table
  // renaming a header renames the column in formulas
  t.Set("C1", "Cost");
  CHECK(t.F("E1") == "=SUM(Table1[Cost])");
  CHECK(t.F("D3") == "=[@Qty]*[@Cost]");
  // renaming the table
  std::string err;
  REQUIRE(t.d.RenameTable(t.sheet(), id, "Sales", &err));
  CHECK(t.F("E1") == "=SUM(Sales[Cost])");
  CHECK_FALSE(t.d.RenameTable(t.sheet(), id, "A1", &err));
}

TEST_CASE("totals row, rows inserted inside, saved, converted to a range") {
  yctest::Doc t;
  const uint32_t id = MakeTable(t);
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& x) { x.totals = true; }));
  t.d.Recalc();
  CHECK(t.d.TableById(t.sheet(), id)->range.r1 == 4);
  CHECK(t.T("A5") == "Total");
  CHECK(t.F("C5") == "=SUBTOTAL(109,[Price])");  // the table's own column, as Excel writes it
  CHECK(t.N("C5") == 60);
  CHECK(t.Eval("Table1[[#Totals],[Price]]") == "60");
  t.d.SetTableTotal(t.sheet(), id, 1, 101, "");
  t.d.Recalc();
  CHECK(t.N("B5") == 2);  // average of 1, 2, 3
  t.d.InsertRows(t.sheet(), 2, 1);
  t.d.Recalc();
  CHECK(t.d.TableById(t.sheet(), id)->range.r1 == 5);
  t.Set("C3", "40");
  CHECK(t.N("C6") == 100);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets[0]->tables->size() == 1);
  const auto& lt = res.wb.sheets[0]->tables->front();
  CHECK(lt.name == "Table1");
  CHECK(lt.totals);
  CHECK(lt.columns[2].name == "Price");
  CHECK(lt.columns[1].totals_func == 101);
  yc::engine::Document d2(res.wb);
  CHECK(d2.GetValue(t.sheet(), 5, 2).AsDouble() == 100);
  // convert to range: the formulas keep working with ordinary references
  REQUIRE(t.d.DeleteTable(t.sheet(), id, true));
  t.d.Recalc();
  CHECK(t.F("C6") == "=SUBTOTAL(109,$C$2:$C$5)");
  CHECK(t.d.wb().sheets[0]->tables->empty());
}

TEST_CASE("a table without headers gets a header row") {
  yctest::Doc t;
  t.Set("B2", "5");
  t.Set("B3", "6");
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{1, 1, 2, 1}, false, "");
  REQUIRE(id != 0);
  CHECK(t.T("B2") == "Column1");
  CHECK(t.N("B3") == 5);
  CHECK(t.Eval("SUM(Table1[Column1])") == "11");
}

TEST_CASE("numbers written by Python below or right of a table grow it, a block at once") {
  using yc::core::Value;
  yctest::Doc t;
  const uint32_t id = MakeTable(t);
  REQUIRE(id != 0);
  t.Set("D1", "Total");
  t.Set("D2", "=[@Qty]*[@Price]");
  // one number below (Range("B5").Value = 7 takes SetValues' fast path, not SetInput)
  t.d.SetValues(t.sheet(), Rect::Cell(4, 1), {Value::Number(7)}, yc::engine::kSrcPython);
  CHECK(t.d.TableById(t.sheet(), id)->range == Rect{0, 0, 4, 3});
  CHECK(t.F("D5") == "=[@Qty]*[@Price]");  // the calculated column continues
  // a block of three rows grows the table by three rows, the formula fills each
  t.d.SetValues(t.sheet(), Rect{5, 1, 7, 2}, {Value::Number(1), Value::Number(2), Value::Number(3), Value::Number(4), Value::Number(5), Value::Number(6)},
                yc::engine::kSrcPython);
  CHECK(t.d.TableById(t.sheet(), id)->range == Rect{0, 0, 7, 3});
  t.d.Recalc();
  CHECK(t.N("D8") == 30);
  // a number right of it adds a column
  t.d.SetValues(t.sheet(), Rect::Cell(2, 4), {Value::Number(9)}, yc::engine::kSrcPython);
  CHECK(t.d.TableById(t.sheet(), id)->range == Rect{0, 0, 7, 4});
  // blanks and calculated results don't
  t.d.SetValues(t.sheet(), Rect::Cell(8, 0), {Value()}, yc::engine::kSrcPython);
  t.d.SetValues(t.sheet(), Rect::Cell(8, 1), {Value::Number(1)}, yc::engine::kSrcCalc);
  CHECK(t.d.TableById(t.sheet(), id)->range == Rect{0, 0, 7, 4});
}

TEST_CASE("A blank table header gets the first ColumnN no other header has, as in Excel") {
  yctest::Doc t;
  t.Set("A1", "Name");
  t.Set("C1", "Price");
  t.Set("A2", "a");
  t.Set("B2", "1");
  t.Set("C2", "10");
  REQUIRE(t.d.AddTable(t.sheet(), Rect{0, 0, 1, 2}, true, "") != 0);
  CHECK(t.T("B1") == "Column1");  // it was Column2, from its position
  t.Set("E1", "Column1");
  t.Set("F1", "x");
  t.Set("E2", "1");
  REQUIRE(t.d.AddTable(t.sheet(), Rect{0, 4, 1, 6}, true, "") != 0);
  CHECK(t.T("G1") == "Column2");  // Column1 is taken by a header of this table
}
