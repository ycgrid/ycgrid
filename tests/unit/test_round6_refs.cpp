// SPDX-License-Identifier: Apache-2.0
// Round 6 (references): cut and paste onto cells formulas read, references to a deleted sheet, the ends of 3-D
// references, as Excel adjusts them
#include <catch2/catch_test_macros.hpp>

#include "formula/parser.h"
#include "io/ycg.h"
#include "test_helpers.h"

using yc::core::Rect;
namespace io = yc::io;

TEST_CASE("Round6: cut and paste onto cells formulas read makes those references #REF!") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("B1", "2");
  t.Set("C1", "=B1+1");        // reads the cell written over
  t.Set("C2", "=SUM(B1:B3)");  // a range only partly written over keeps its reference
  t.Set("C3", "=A1*10");       // reads the moved cell: follows it
  t.d.Begin("Move");
  REQUIRE(t.d.MoveCells(sh, Rect::Cell(0, 0), sh, 0, 1));  // A1 -> B1
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.F("C1") == "=#REF!+1");
  CHECK(t.T("C1") == "#REF!");
  CHECK(t.F("C2") == "=SUM(B1:B3)");
  CHECK(t.F("C3") == "=B1*10");
  CHECK(t.N("C3") == 10);
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.F("C1") == "=B1+1");
  CHECK(t.N("C1") == 3);
  // a range written over whole
  t.Set("E1", "5");
  t.Set("E2", "6");
  t.Set("F1", "7");
  t.Set("F2", "8");
  t.Set("G1", "=SUM(F1:F2)");
  REQUIRE(t.d.MoveCells(sh, Rect{0, 4, 1, 4}, sh, 0, 5));  // E1:E2 -> F1:F2
  CHECK(t.F("G1") == "=SUM(#REF!)");
  // moved one row down over itself: the moved cells are followed, the one written over is #REF!
  t.Set("A10", "1");
  t.Set("A11", "2");
  t.Set("A12", "3");
  t.Set("B10", "=A10");
  t.Set("B11", "=A12");
  REQUIRE(t.d.MoveCells(sh, Rect{9, 0, 10, 0}, sh, 10, 0));  // A10:A11 -> A11:A12
  CHECK(t.F("B10") == "=A11");
  CHECK(t.F("B11") == "=#REF!");
  // onto another sheet: references there to the cells written over break too
  const uint32_t other = t.d.AddSheet("Other");
  t.Set("A1", "9", other);
  t.Set("B1", "=A1", other);
  t.Set("H1", "4");
  REQUIRE(t.d.MoveCells(sh, Rect::Cell(0, 7), other, 0, 0));  // Sheet1!H1 -> Other!A1
  CHECK(t.F("B1", other) == "=#REF!");
}

TEST_CASE("Round6: a reference to a deleted sheet keeps its cell part, =#REF!A1, as Excel writes it") {
  yctest::Doc t;
  const uint32_t data = t.d.AddSheet("Data");
  t.Set("A1", "4", data);
  t.Set("B1", "=Data!A1+1");
  t.Set("B2", "=SUM(Data!$A$1:$B$3)");
  t.Set("B3", "=IFERROR(Data!A1,0)");
  REQUIRE(t.d.AddName("Px", "=Data!$A$1"));
  t.Set("B4", "=Px");
  t.d.Recalc();
  CHECK(t.N("B1") == 5);
  REQUIRE(t.d.DeleteSheet(data));
  t.d.Recalc();
  CHECK(t.F("B1") == "=#REF!A1+1");
  CHECK(t.T("B1") == "#REF!");
  CHECK(t.F("B2") == "=SUM(#REF!$A$1:$B$3)");
  CHECK(t.N("B3") == 0);
  REQUIRE(t.d.FindName("Px", t.sheet()) != nullptr);
  CHECK(t.d.FindName("Px", t.sheet())->refers_to == "#REF!$A$1");
  CHECK(t.T("B4") == "#REF!");
  // the text reads back as it was written: typed, and saved and opened
  t.Set("C1", "=#REF!A1+1");
  CHECK(t.F("C1") == "=#REF!A1+1");
  CHECK(t.T("C1") == "#REF!");
  t.Set("C2", "=SUM(#REF!A1:B2)");
  CHECK(t.F("C2") == "=SUM(#REF!A1:B2)");
  t.Set("C3", "=#REF!");  // a plain error stays one
  CHECK(t.F("C3") == "=#REF!");
  t.Set("C4", "=#REF!+1");
  CHECK(t.F("C4") == "=#REF!+1");
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  yc::engine::Document back(std::move(res.wb));
  back.Recalc();
  const uint32_t s0 = back.wb().sheets[0]->id;
  CHECK(back.GetInputText(s0, 0, 1) == "=#REF!A1+1");
  CHECK(back.GetInputText(s0, 1, 1) == "=SUM(#REF!$A$1:$B$3)");
  CHECK(back.GetValue(s0, 0, 1).IsError());
  CHECK(back.GetValue(s0, 2, 1).AsDouble() == 0);
  // copied down, the relative part moves as in Excel
  t.d.Paste(t.sheet(), 9, 2, t.d.Copy(t.sheet(), Rect::Cell(0, 2)));
  CHECK(t.F("C10") == "=#REF!A10+1");
}

TEST_CASE("Round6: deleting the sheet at one end of a 3-D reference ends it at the next sheet inside") {
  yctest::Doc t;
  const uint32_t main = t.sheet();
  const uint32_t north = t.d.AddSheet("North");
  const uint32_t east = t.d.AddSheet("East");
  const uint32_t south = t.d.AddSheet("South");
  const uint32_t west = t.d.AddSheet("West");
  t.Set("A1", "1", north);
  t.Set("A1", "10", east);
  t.Set("A1", "100", south);
  t.Set("A1", "1000", west);
  t.Set("B1", "=SUM(North:West!A1)", main);
  t.Set("B2", "=SUM(North:South!A1)", main);
  REQUIRE(t.d.AddName("Span", "=North:West!$A$1"));
  t.Set("B3", "=SUM(Span)", main);
  t.d.Recalc();
  CHECK(t.N("B1") == 1111);
  CHECK(t.N("B3") == 1111);
  auto del = [&](uint32_t id) {
    t.d.Begin("Delete Sheet");
    const bool ok = t.d.DeleteSheet(id);
    t.d.Commit();
    t.d.Recalc();
    return ok;
  };
  REQUIRE(del(west));  // the last end
  CHECK(t.F("B1") == "=SUM(North:South!A1)");
  CHECK(t.N("B1") == 111);
  CHECK(t.F("B2") == "=SUM(North:South!A1)");
  CHECK(t.d.FindName("Span", main)->refers_to == "North:South!$A$1");
  CHECK(t.N("B3") == 111);
  REQUIRE(del(north));  // the first end
  CHECK(t.F("B1") == "=SUM(East:South!A1)");
  CHECK(t.N("B1") == 110);
  REQUIRE(del(south));  // one sheet left: a reference to it
  CHECK(t.F("B1") == "=SUM(East!A1)");
  CHECK(t.N("B1") == 10);
  CHECK(t.d.FindName("Span", main)->refers_to == "East!$A$1");
  CHECK(t.N("B3") == 10);
  REQUIRE(del(east));  // none left
  CHECK(t.F("B1") == "=SUM(#REF!A1)");
  CHECK(t.T("B1") == "#REF!");
  CHECK(t.T("B3") == "#REF!");
  // undo brings the sheets and the references back
  for (int i = 0; i < 4; ++i) REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.F("B1") == "=SUM(North:West!A1)");
  CHECK(t.N("B1") == 1111);
}

TEST_CASE("Round6: names, chart series, conditional formats and validation move with cut cells") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (int i = 1; i <= 3; ++i) t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
  REQUIRE(t.d.AddName("Block", "=Sheet1!$A$1:$A$3"));
  REQUIRE(t.d.AddName("Mid", "=Sheet1!$A$2"));
  REQUIRE(t.d.AddName("Wide", "=Sheet1!$A$1:$A$10"));  // more than the moved cells: it stays
  t.Set("C1", "=SUM(Block)");
  yc::model::ChartObj ch;
  yc::model::ChartSeries se;
  se.values = "Sheet1!$A$1:$A$3";
  ch.series.push_back(se);
  t.d.AddChart(sh, ch);
  yc::model::CfRule cf;
  cf.type = yc::model::CfType::CellValue;
  cf.op = yc::model::CmpOp::Greater;
  cf.formula1 = "1";
  cf.ranges = {Rect{0, 0, 2, 0}};  // A1:A3
  t.d.AddCondFormat(sh, cf);
  yc::model::CfRule ex;  // a rule elsewhere reading a moved cell
  ex.type = yc::model::CfType::Expression;
  ex.formula1 = "$A$1>0";
  ex.ranges = {Rect{0, 7, 1, 7}};  // H1:H2
  t.d.AddCondFormat(sh, ex);
  yc::model::DvRule dv;
  dv.type = yc::model::DvType::WholeNumber;
  dv.op = yc::model::CmpOp::Between;
  dv.formula1 = "0";
  dv.formula2 = "10";
  t.d.SetValidation(sh, Rect{0, 0, 2, 0}, dv);
  t.d.Recalc();
  CHECK(t.N("C1") == 6);
  t.d.Begin("Move");
  REQUIRE(t.d.MoveCells(sh, Rect{0, 0, 2, 0}, sh, 0, 4));  // A1:A3 -> E1:E3
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.d.FindName("Block", sh)->refers_to == "Sheet1!$E$1:$E$3");
  CHECK(t.d.FindName("Mid", sh)->refers_to == "Sheet1!$E$2");
  CHECK(t.d.FindName("Wide", sh)->refers_to == "Sheet1!$A$1:$A$10");
  CHECK(t.N("C1") == 6);  // it was 0: the name kept the emptied cells
  CHECK(t.d.S(sh)->charts->front().series.front().values == "Sheet1!$E$1:$E$3");
  const yc::model::CfRule* value_rule = nullptr;
  const yc::model::CfRule* expr_rule = nullptr;
  for (const auto& r : *t.d.S(sh)->cond_formats) (r.type == yc::model::CfType::CellValue ? value_rule : expr_rule) = &r;
  REQUIRE(value_rule);
  REQUIRE(expr_rule);
  CHECK(value_rule->ranges == std::vector<Rect>{Rect{0, 4, 2, 4}});  // it stayed on A1:A3
  CHECK(expr_rule->ranges == std::vector<Rect>{Rect{0, 7, 1, 7}});
  CHECK(expr_rule->formula1 == "$E$1>0");
  REQUIRE(t.d.S(sh)->validations->size() == 1);  // it was dropped
  CHECK(t.d.S(sh)->validations->front().ranges == std::vector<Rect>{Rect{0, 4, 2, 4}});
  // undo puts everything back
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.d.FindName("Block", sh)->refers_to == "Sheet1!$A$1:$A$3");
  CHECK(t.d.S(sh)->validations->front().ranges == std::vector<Rect>{Rect{0, 0, 2, 0}});
  CHECK(t.N("C1") == 6);
}

TEST_CASE("Round6: a rule partly on the cut cells is split, and the rules of the cells written over go") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  yc::model::CfRule cf;
  cf.type = yc::model::CfType::Expression;
  cf.formula1 = "A1>5";  // each cell itself
  cf.ranges = {Rect{0, 0, 9, 0}};  // A1:A10
  t.d.AddCondFormat(sh, cf);
  yc::model::CfRule there;
  there.type = yc::model::CfType::CellValue;
  there.formula1 = "0";
  there.ranges = {Rect{0, 4, 1, 4}};  // E1:E2, written over
  t.d.AddCondFormat(sh, there);
  REQUIRE(t.d.MoveCells(sh, Rect{0, 0, 2, 0}, sh, 0, 4));  // A1:A3 -> E1:E3
  const auto& v = *t.d.S(sh)->cond_formats;
  REQUIRE(v.size() == 2);
  const yc::model::CfRule* rest = nullptr;
  const yc::model::CfRule* moved = nullptr;
  for (const auto& r : v) {
    if (r.ranges == std::vector<Rect>{Rect{3, 0, 9, 0}}) rest = &r;
    if (r.ranges == std::vector<Rect>{Rect{0, 4, 2, 4}}) moved = &r;
  }
  REQUIRE(rest);
  REQUIRE(moved);
  CHECK(rest->formula1 == "A4>5");  // written for its first cell: still each cell itself
  CHECK(moved->formula1 == "E1>5");
  CHECK(rest->id != moved->id);
  // to another sheet: the list moves there
  const uint32_t other = t.d.AddSheet("Other");
  yc::model::DvRule dv;
  dv.type = yc::model::DvType::List;
  dv.formula1 = "Buy,Sell";
  t.d.SetValidation(sh, Rect{19, 0, 20, 0}, dv);  // A20:A21
  REQUIRE(t.d.MoveCells(sh, Rect{19, 0, 20, 0}, other, 4, 1));  // -> Other!B5:B6
  CHECK(t.d.S(sh)->validations->empty());
  REQUIRE(t.d.S(other)->validations->size() == 1);
  CHECK(t.d.S(other)->validations->front().ranges == std::vector<Rect>{Rect{4, 1, 5, 1}});
  CHECK(t.d.S(other)->validations->front().formula1 == "Buy,Sell");
}
