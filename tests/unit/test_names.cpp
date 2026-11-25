// SPDX-License-Identifier: Apache-2.0
// Defined names (named ranges)
#include <catch2/catch_test_macros.hpp>

#include "engine/navigate.h"
#include "io/ycg.h"
#include "test_helpers.h"

using yc::core::Rect;
namespace model = yc::model;

TEST_CASE("名稱規則") {
  std::string err;
  CHECK(yc::engine::ValidateName("營收", &err));
  CHECK(yc::engine::ValidateName("_tax.rate", &err));
  CHECK_FALSE(yc::engine::ValidateName("A1", &err));
  CHECK_FALSE(yc::engine::ValidateName("R2C3", &err));
  CHECK_FALSE(yc::engine::ValidateName("1abc", &err));
  CHECK_FALSE(yc::engine::ValidateName("my name", &err));
  CHECK_FALSE(yc::engine::ValidateName("a+b", &err));
}

TEST_CASE("活頁簿與工作表層級的名稱、公式使用名稱") {
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  const uint32_t s2 = t.d.AddSheet("第二頁");
  t.Set("A1", "10");
  t.Set("A2", "20");
  t.Set("A1", "100", s2);
  REQUIRE(t.d.DefineName(model::NameDef{"數量", t.d.S(s1)->name + "!$A$1:$A$2", -1}));
  REQUIRE(t.d.DefineName(model::NameDef{"數量", "'第二頁'!$A$1", static_cast<int32_t>(s2)}));
  t.Set("B1", "=SUM(數量)");
  t.Set("B1", "=SUM(數量)", s2);
  CHECK(t.N("B1") == 30);
  CHECK(t.N("B1", s2) == 100);  // the sheet-level name wins
  uint32_t sh = 0;
  Rect r;
  REQUIRE(t.d.ResolveNameRef("數量", s1, &sh, &r));
  CHECK(sh == s1);
  CHECK(r == Rect{0, 0, 1, 0});
  REQUIRE(t.d.ResolveNameRef("第二頁!數量", s1, &sh, &r));
  CHECK(sh == s2);
  CHECK(r == Rect{0, 0, 0, 0});
}

TEST_CASE("動態名稱（OFFSET＋COUNTA）") {
  yctest::Doc t;
  for (int i = 1; i <= 4; ++i) t.Set(("C" + std::to_string(i)).c_str(), std::to_string(i));
  REQUIRE(t.d.DefineName(model::NameDef{"資料", "OFFSET($C$1,0,0,COUNTA($C:$C),1)", -1}));
  uint32_t sh = 0;
  Rect r;
  REQUIRE(t.d.ResolveNameRef("資料", t.sheet(), &sh, &r));
  CHECK(r == Rect{0, 2, 3, 2});
  t.Set("C5", "5");
  REQUIRE(t.d.ResolveNameRef("資料", t.sheet(), &sh, &r));
  CHECK(r.r1 == 4);
  t.Set("D1", "=SUM(資料)");
  CHECK(t.N("D1") == 15);
}

TEST_CASE("名稱改名時更新公式；字串與函數名稱不受影響") {
  yctest::Doc t;
  t.Set("A1", "3");
  REQUIRE(t.d.DefineName(model::NameDef{"rate", "$A$1", -1}));
  t.Set("B1", "=rate*2&\"rate\"");
  t.Set("B2", "=RATE+1");
  REQUIRE(t.d.UpdateName("rate", -1, model::NameDef{"稅率", "$A$1", -1, "營業稅", false}));
  CHECK(t.F("B1") == "=稅率*2&\"rate\"");
  CHECK(t.F("B2") == "=稅率+1");
  CHECK(t.N("B2") == 4);
  CHECK(t.d.FindName("稅率", t.sheet())->comment == "營業稅");
  std::string err;
  REQUIRE(t.d.DefineName(model::NameDef{"other", "$A$2", -1}));
  CHECK_FALSE(t.d.UpdateName("other", -1, model::NameDef{"稅率", "$A$2", -1}, &err));  // a duplicate
}

TEST_CASE("從選取範圍建立名稱") {
  yctest::Doc t;
  t.Set("A1", "代碼");
  t.Set("B1", "價格");
  t.Set("C1", "2024 數量");
  t.Set("A2", "1");
  auto names = t.d.NamesFromSelection(t.sheet(), Rect{0, 0, 5, 2}, true, false, false, false);
  REQUIRE(names.size() == 3);
  CHECK(names[0].name == "代碼");
  CHECK(names[0].refers_to == t.d.S(t.sheet())->name + "!$A$2:$A$6");
  CHECK(names[2].name == "_2024_數量");
}

TEST_CASE(".ycg 保存名稱的註解與隱藏") {
  yctest::Doc t;
  REQUIRE(t.d.DefineName(model::NameDef{"隱藏的", "$A$1", -1, "說明文字", true}));
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.names->size() == 1);
  CHECK((*res.wb.names)[0].comment == "說明文字");
  CHECK((*res.wb.names)[0].hidden);
}

TEST_CASE("特殊儲存格（SpecialCells）") {
  yctest::Doc t;
  t.Set("A1", "代碼");
  t.Set("A2", "10");
  t.Set("A3", "20");
  t.Set("B2", "=A2*2");
  t.Set("B3", "=A3*2");
  const auto& sh = *t.d.S(t.sheet());
  const Rect all{0, 0, 3, 1};
  auto f = yc::engine::SpecialCells(sh, all, yc::engine::Special::Formulas);
  REQUIRE(f.size() == 1);
  CHECK(f[0] == Rect{1, 1, 2, 1});
  auto nums = yc::engine::SpecialCells(sh, all, yc::engine::Special::Constants, 1);
  REQUIRE(nums.size() == 1);
  CHECK(nums[0] == Rect{1, 0, 2, 0});
  auto text = yc::engine::SpecialCells(sh, all, yc::engine::Special::Constants, 2);
  REQUIRE(text.size() == 1);
  CHECK(text[0] == Rect{0, 0, 0, 0});
  auto blanks = yc::engine::SpecialCells(sh, all, yc::engine::Special::Blanks);
  REQUIRE(blanks.size() == 1);
  CHECK(blanks[0] == Rect{0, 1, 0, 1});  // B1
}

TEST_CASE("前導參照與從屬參照") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("B1", "=A1+A2");
  t.Set("C1", "=B1*2");
  REQUIRE(t.d.DefineName(model::NameDef{"係數", "$D$1", -1}));
  t.Set("D1", "3");
  t.Set("E1", "=C1*係數");
  const uint32_t s = t.sheet();
  auto direct = t.d.Precedents(s, Rect{0, 2, 0, 2}, true);  // C1
  REQUIRE(direct.size() == 1);
  CHECK(direct[0] == Rect{0, 1, 0, 1});
  auto all = t.d.Precedents(s, Rect{0, 4, 0, 4}, false);  // E1 -> C1, the factor name (D1), B1, A1, A2
  CHECK(all.size() == 5);
  auto deps = t.d.Dependents(s, Rect{0, 0, 0, 0}, false);  // A1 -> B1, C1; E1 through C1
  REQUIRE(deps.size() == 2);
  CHECK(deps[0] == Rect{0, 1, 0, 2});
  CHECK(deps[1] == Rect{0, 4, 0, 4});
  auto ddeps = t.d.Dependents(s, Rect{0, 0, 0, 0}, true);
  REQUIRE(ddeps.size() == 1);
  CHECK(ddeps[0] == Rect{0, 1, 0, 1});
}

TEST_CASE("A relative name reaches the same offsets from every cell that uses it, wrapping around the sheet") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  std::string err;
  // defined with B1 active as "the cell to the left": Excel keeps it relative to A1, where the left cell is XFD1
  const std::string kept = yc::formula::MoveRelativeText("=Sheet1!A1", yctest::Doc::P("B1"), yctest::Doc::P("A1"), &t.d);
  CHECK(kept == "=Sheet1!XFD1");
  REQUIRE(t.d.AddName("LeftCell", kept, -1, &err));
  t.Set("B2", "=LeftCell");
  t.Set("B3", "=LeftCell*10");
  CHECK(t.N("B2") == 2);
  CHECK(t.N("B3") == 30);
  // shown from another active cell, as Excel's Name Manager does
  CHECK(yc::formula::MoveRelativeText("=" + t.d.FindName("LeftCell", t.sheet())->refers_to, yctest::Doc::P("A1"),
                                      yctest::Doc::P("C5"), &t.d) == "=Sheet1!B5");
  // a range and a mixed reference: "the three cells above, in column A"
  REQUIRE(t.d.AddName("Above3", yc::formula::MoveRelativeText("=SUM(Sheet1!$A1:$A3)", yctest::Doc::P("C4"),
                                                               yctest::Doc::P("A1"), &t.d), -1, &err));
  t.Set("D4", "=Above3");
  CHECK(t.N("D4") == 6);
  // inserting rows leaves a relative name's offsets alone (an absolute one follows its cells)
  REQUIRE(t.d.AddName("Fixed", "=Sheet1!$A$1:$A$3", -1, &err));
  t.d.InsertRows(t.sheet(), 0, 1);
  CHECK(t.d.FindName("LeftCell", t.sheet())->refers_to == "Sheet1!XFD1");
  CHECK(t.d.FindName("Fixed", t.sheet())->refers_to == "Sheet1!$A$2:$A$4");
  t.d.Recalc();
  CHECK(t.N("B3") == 2);  // B2 moved to B3: the cell left of it is A3, which now holds 2
  // kept through a save
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  bool found = false;
  for (const auto& n : *res.wb.names) {
    if (n.name == "LeftCell") found = n.refers_to == "Sheet1!XFD1";
  }
  CHECK(found);
  // names in files saved before this read as the absolute references they worked as
  CHECK(yc::formula::AbsoluteRefText("Sheet1!A1:B2", &t.d) == "Sheet1!$A$1:$B$2");
  CHECK(yc::formula::AbsoluteRefText("=SUM(Sheet1!$A1)+1", &t.d) == "=SUM(Sheet1!$A$1)+1");
  CHECK(yc::formula::AbsoluteRefText("Sheet1!$A$1", &t.d) == "Sheet1!$A$1");
}

TEST_CASE("Sheet1!Name refers to the name defined for that sheet") {
  yctest::Doc t;
  const uint32_t data = t.d.AddSheet("Data");
  t.Set("A1", "5");
  t.Set("A1", "7", data);
  std::string err;
  REQUIRE(t.d.AddName("Pick", "=Sheet1!$A$1", -1, &err));
  REQUIRE(t.d.AddName("Pick", "=Data!$A$1", static_cast<int32_t>(data), &err));
  t.Set("B1", "=Pick");
  t.Set("B2", "=Data!Pick*2");
  t.Set("B3", "=SUM(Data!Pick,Pick)");
  CHECK(t.N("B1") == 5);
  CHECK(t.N("B2") == 14);
  CHECK(t.N("B3") == 12);
  CHECK(t.F("B2") == "=Data!Pick*2");
  // the sheet's new name follows, as for cell references
  REQUIRE(t.d.RenameSheet(data, "My Data", &err));
  CHECK(t.F("B2") == "='My Data'!Pick*2");
  t.Set("A1", "8", data);
  CHECK(t.N("B2") == 16);
  // a sheet name that doesn't exist is still an error, and a name no sheet has is #NAME?
  CHECK_FALSE(t.Set("C1", "=Nowhere!Pick"));
  t.Set("C2", "=Sheet1!NoSuchName");
  CHECK(t.T("C2") == "#NAME?");
}

TEST_CASE("Date functions written by Python get a date format, those Excel leaves as numbers don't") {
  yctest::Doc t;
  std::string err;
  auto fmt = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return t.d.wb().styles->NumberFormat(t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(p.row, p.col)));
  };
  REQUIRE(t.d.SetInput(t.sheet(), 0, 0, "=TODAY()", &err, yc::engine::kSrcPython));
  REQUIRE(t.d.SetInput(t.sheet(), 1, 0, "=NOW()", &err, yc::engine::kSrcPython));
  REQUIRE(t.d.SetInput(t.sheet(), 2, 0, "=EDATE(45306,1)", &err, yc::engine::kSrcPython));
  REQUIRE(t.d.SetInput(t.sheet(), 3, 0, "=DATEVALUE(\"2024/1/15\")", &err, yc::engine::kSrcUser));
  REQUIRE(t.d.SetInput(t.sheet(), 4, 0, "=TIME(6,0,0)", &err, yc::engine::kSrcPython));
  CHECK(fmt("A1") == "yyyy/m/d");
  CHECK(fmt("A2") == "yyyy/m/d h:mm");
  CHECK(fmt("A3") == "General");
  CHECK(fmt("A4") == "General");
  CHECK(fmt("A5") == "h:mm AM/PM");
  // History is kept by Excel for its change history
  CHECK_FALSE(t.d.RenameSheet(t.sheet(), "history", &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("A copied sheet gets its own copies of the sheet-level names, pointing at the copy") {
  yctest::Doc t;
  const uint32_t src = t.d.AddSheet("Src");
  t.Set("A1", "5", src);
  std::string err;
  REQUIRE(t.d.AddName("Local", "=Src!$A$1", static_cast<int32_t>(src), &err));
  REQUIRE(t.d.AddName("Global", "=Src!$A$1*10", -1, &err));
  t.Set("B1", "=Local+1", src);
  const uint32_t copy = t.d.CopySheet(src);
  REQUIRE(copy != 0);
  t.d.Recalc();
  CHECK(t.N("B1", copy) == 6);  // it was #NAME?: the copy had no name Local
  t.Set("A1", "100", copy);
  CHECK(t.N("B1", copy) == 101);  // the copy's own cell, not the source's
  CHECK(t.N("B1", src) == 6);
  const model::NameDef* n = t.d.FindName("Local", copy);
  REQUIRE(n != nullptr);
  CHECK(n->sheet_scope == static_cast<int32_t>(copy));
  CHECK(n->refers_to == "'Src (2)'!$A$1");
  // the source keeps its own; a workbook-level name that refers to the source gets a sheet-level twin on the copy,
  // referring to the copy (measured in Excel 16: 'Src (2)'!Global), and stays as it was for the workbook
  REQUIRE(t.d.FindName("Local", src) != nullptr);
  CHECK(t.d.FindName("Local", src)->sheet_scope == static_cast<int32_t>(src));
  int globals = 0, global_wb = 0;
  for (const auto& d : *t.d.wb().names) {
    globals += d.name == "Global" ? 1 : 0;
    global_wb += d.name == "Global" && d.sheet_scope < 0 ? 1 : 0;
  }
  CHECK(globals == 2);
  CHECK(global_wb == 1);
  REQUIRE(t.d.FindName("Global", copy) != nullptr);
  CHECK(t.d.FindName("Global", copy)->refers_to == "'Src (2)'!$A$1*10");
  // copying again names the next copy's names after it
  const uint32_t copy2 = t.d.CopySheet(src);
  REQUIRE(t.d.FindName("Local", copy2) != nullptr);
  CHECK(t.d.FindName("Local", copy2)->refers_to == "'Src (3)'!$A$1");
}

TEST_CASE("Deleting a sheet takes its sheet-level names and breaks the names that refer to it") {
  yctest::Doc t;
  const uint32_t data = t.d.AddSheet("Data");
  t.Set("A1", "5", data);
  std::string err;
  REQUIRE(t.d.AddName("Total", "=Data!$A$1", -1, &err));
  REQUIRE(t.d.AddName("Own", "=Data!$A$1", static_cast<int32_t>(data), &err));
  REQUIRE(t.d.AddName("Kept", "=Sheet1!$A$1", -1, &err));
  t.Set("A1", "3");
  t.Set("B1", "=Total*2");
  CHECK(t.N("B1") == 10);
  t.d.Begin("Delete");
  REQUIRE(t.d.DeleteSheet(data));
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.T("B1") == "#REF!");
  CHECK(t.d.FindName("Own", data) == nullptr);  // it stayed behind, scoped to a sheet that no longer exists
  REQUIRE(t.d.FindName("Total", t.sheet()) != nullptr);
  CHECK(t.d.FindName("Total", t.sheet())->refers_to.find("#REF!") != std::string::npos);
  CHECK(t.d.FindName("Kept", t.sheet())->refers_to == "Sheet1!$A$1");
  // a new sheet with the deleted one's name doesn't take its place (it did: Total read the new Data!A1)
  t.d.Begin("Insert");
  const uint32_t again = t.d.AddSheet("Data");
  t.d.Commit();
  t.Set("A1", "7", again);
  t.d.Recalc();
  CHECK(t.T("B1") == "#REF!");
  // undo brings the sheet and its names back
  for (int i = 0; i < 4 && t.d.S(data) == nullptr; ++i) REQUIRE(t.d.Undo());  // the new sheet, then the deletion
  REQUIRE(t.d.S(data) != nullptr);
  t.d.Recalc();
  CHECK(t.N("B1") == 10);
  CHECK(t.d.FindName("Own", data) != nullptr);
}

TEST_CASE("Create from Selection: a label like a reference gets an underscore, a number gives no name (Excel 16, measured)") {
  yctest::Doc t;
  const char* labels[] = {"Row1", "Row 2", "1st", "a-b", "C", "12"};
  for (uint32_t i = 0; i < 6; ++i) {
    t.d.SetInput(t.sheet(), 9 + i, 3, labels[i]);  // D10:D16
    t.d.SetInput(t.sheet(), 9 + i, 4, "5");
  }
  auto names = t.d.NamesFromSelection(t.sheet(), Rect{9, 3, 14, 4}, false, true, false, false);
  std::vector<std::string> got;
  for (const auto& n : names) got.push_back(n.name);
  CHECK(got == std::vector<std::string>{"Row1_", "Row_2", "_1st", "a_b", "C_"});
  CHECK(names[4].refers_to == t.d.S(t.sheet())->name + "!$E$14");
  std::string err;
  for (const auto& n : names) CHECK(t.d.DefineName(n, &err));
}

TEST_CASE("The dependents of a cell an array formula reads are every cell of the array (Excel 16, measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B2", "1");
  t.Set("C2", "2");
  t.Set("B3", "3");
  std::string err;
  REQUIRE(t.d.SetArrayFormula(sh, Rect{0, 6, 1, 6}, "B2:B3*2", &err));  // {=B2:B3*2} in G1:G2
  t.Set("H1", "=B2+C2");
  t.Set("H2", "=H1*2");
  t.d.Recalc();
  // Excel: Dependents $G$1:$H$2, DirectDependents $G$1:$G$2,$H$1
  auto count = [](const std::vector<Rect>& v) {
    uint64_t n = 0;
    for (const Rect& x : v) n += x.Count();
    return n;
  };
  const auto all = t.d.Dependents(sh, Rect::Cell(1, 1), false);
  CHECK(count(all) == 4);
  for (const char* a : {"G1", "G2", "H1", "H2"}) {
    const auto p = yctest::Doc::P(a);
    CHECK(std::any_of(all.begin(), all.end(), [&](const Rect& x) { return x.Contains(p.row, p.col); }));
  }
  const auto direct = t.d.Dependents(sh, Rect::Cell(1, 1), true);
  CHECK(count(direct) == 3);
  const auto g2 = yctest::Doc::P("G2");
  CHECK(std::any_of(direct.begin(), direct.end(), [&](const Rect& x) { return x.Contains(g2.row, g2.col); }));
}
