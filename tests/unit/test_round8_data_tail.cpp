// SPDX-License-Identifier: Apache-2.0
// Round 8, the last data differences (measured against Excel 16): Paste Special > Transpose turning absolute references
// to the copied cells, operations on formulas, text and logical values; Text to Columns' quotes and formulas; Auto
// Outline's outer levels; a defined name that is a union; filters placed above a PivotTable's body
#include <catch2/catch_test_macros.hpp>

#include "core/i18n.h"
#include "engine/pivot_engine.h"
#include "io/ycg.h"
#include "test_helpers.h"

#include <string>
#include <vector>

using yc::core::Rect;
using yc::engine::PasteMode;
using yc::engine::PasteOp;
namespace engine = yc::engine;
namespace model = yc::model;

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}

void Transposed(yctest::Doc& t, const char* from, const char* to) {
  const auto clip = t.d.Copy(t.sheet(), R(from));
  const auto at = yctest::Doc::P(to);
  t.d.Paste(t.sheet(), at.row, at.col, clip, PasteMode::All, true, false);
  t.d.Recalc();
}

// A1 copied onto D1 with an operation: what D1 then holds (its formula, else what it shows)
std::string Combined(const char* src, const char* dst, PasteMode mode, PasteOp op) {
  yctest::Doc t;
  if (*src) t.Set("A1", src);
  if (*dst) t.Set("D1", dst);
  const auto clip = t.d.Copy(t.sheet(), R("A1"));
  t.d.Paste(t.sheet(), 0, 3, clip, mode, false, false, op);
  t.d.Recalc();
  return t.F("D1").rfind('=', 0) == 0 ? t.F("D1") + " -> " + t.T("D1") : t.T("D1");
}

std::vector<std::string> Split(const char* text) {
  engine::TextToColumnsOptions o;
  o.tab = false;
  o.comma = true;
  return engine::SplitColumns(text, o);
}

std::vector<int> RowLevels(yctest::Doc& t, int n) {
  std::vector<int> out;
  for (int i = 0; i < n; ++i) out.push_back(t.d.S(t.sheet())->rows->Level(i));
  return out;
}

void Column(yctest::Doc& t, const std::vector<const char*>& cells) {
  for (size_t i = 0; i < cells.size(); ++i) {
    if (*cells[i]) t.Set(("A" + std::to_string(i + 1)).c_str(), cells[i]);
  }
}

model::PivotAxisField Axis(int f) {
  model::PivotAxisField a;
  a.field = f;
  return a;
}

// Region, Product, Sales in A1:C9 and a PivotTable of Sales by Region whose body starts at F<row>
uint32_t SalesPivot(yctest::Doc& t, uint32_t row) {
  const char* rows[][3] = {{"Region", "Product", "Sales"}, {"East", "P1", "10"}, {"West", "P2", "20"}, {"East", "P2", "30"}, {"North", "P1", "40"},
                           {"West", "P1", "50"},          {"North", "P2", "60"}, {"East", "P1", "70"}, {"West", "P2", "80"}};
  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 3; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
  }
  model::PivotTable p;
  p.src_sheet = t.sheet();
  p.src = R("A1:C9");
  p.r0 = row - 1;
  p.c0 = 5;  // F
  p.rows = {Axis(0)};
  model::PivotDataField d;
  d.field = 2;
  p.data = {d};
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  return id;
}

void SetPages(yctest::Doc& t, uint32_t id, const std::vector<int>& fields) {
  REQUIRE(t.d.UpdatePivot(t.sheet(), id, [&](model::PivotTable& x) {
    x.pages.clear();
    for (int f : fields) x.pages.push_back(Axis(f));
  }));
  t.d.Recalc();
}

uint32_t Top(yctest::Doc& t, uint32_t id) { return t.d.PivotById(t.sheet(), id)->output.r0 + 1; }

}  // namespace

TEST_CASE("round8 data tail: Transpose turns references to the copied cells, absolute ones too") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("B1", "=$A$1");
  t.Set("B2", "=$A$2+$B$1+A1+$C$9");
  t.Set("A3", "=$A1+A$1");
  t.Set("B3", "=SUM($A$1:$A$2)+SUM($A$1:$B$1)");
  Transposed(t, "A1:B3", "E1");
  // Excel: a reference to a copied cell follows it, and its $ on the row goes to the column
  CHECK(t.F("E2") == "=$E$1");
  CHECK(t.F("F2") == "=$F$1+$E$2+E1+$C$9");
  CHECK(t.F("G1") == "=E$1+$E1");
  CHECK(t.F("G2") == "=SUM($E$1:$F$1)+SUM($E$1:$E$2)");
  CHECK(t.N("F2") == 4);
}

TEST_CASE("round8 data tail: Transpose keeps absolute and mixed references elsewhere, turns relative ones") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("B1", "=$A$1+$D$5+Z$1+$A9+D5");
  t.Set("B2", "=SUM(A1:B1)+SUM($A$1:B1)+SUM($A$1:$C$1)");
  Transposed(t, "A1:B2", "E1");
  CHECK(t.F("E2") == "=$E$1+$D$5+Z$1+$A9+I4");
  // a range that starts in the copied cells turns as a whole ($C$1 lies beyond them: Excel gives $E$3)
  CHECK(t.F("F2") == "=SUM(E1:E2)+SUM($E$1:E2)+SUM($E$1:$E$3)");
  t.Set("C3", "1");
  t.Set("D3", "=A1+B3+C1+$A1+A$1");
  Transposed(t, "C3:D3", "H10");
  CHECK(t.F("H11") == "=F8+H9+F10+$A1+A$1");
}

TEST_CASE("round8 data tail: Paste Special operations combine formulas as Excel does") {
  // a copied formula: =<the cell>+(<the formula>), the cell's own formula in parentheses, a blank as 0
  CHECK(Combined("=1+1", "5", PasteMode::All, PasteOp::Add) == "=5+(1+1) -> 7");
  CHECK(Combined("=1+1", "5", PasteMode::Formulas, PasteOp::Subtract) == "=5-(1+1) -> 3");
  CHECK(Combined("=1+1", "5", PasteMode::All, PasteOp::Divide) == "=5/(1+1) -> 2.5");
  CHECK(Combined("=1+1", "=2+3", PasteMode::All, PasteOp::Add) == "=(2+3)+(1+1) -> 7");
  CHECK(Combined("=1+1", "", PasteMode::All, PasteOp::Add) == "=0+(1+1) -> 2");
  CHECK(Combined("=B9*2", "5", PasteMode::All, PasteOp::Subtract) == "=5-(E9*2) -> 5");  // its references moved
  CHECK(Combined("=1+1", "x", PasteMode::All, PasteOp::Add) == "x");
  // values: the formula's value
  CHECK(Combined("=1+1", "5", PasteMode::Values, PasteOp::Add) == "7");
  CHECK(Combined("2", "=2+3", PasteMode::All, PasteOp::Add) == "=(2+3)+2 -> 7");
}

TEST_CASE("round8 data tail: Paste Special operations leave text, TRUE and errors, read numbers in text") {
  CHECK(Combined("x", "5", PasteMode::All, PasteOp::Add) == "5");  // copied text: the cell stays
  CHECK(Combined("x", "5", PasteMode::Values, PasteOp::Add) == "5");
  CHECK(Combined("x", "", PasteMode::All, PasteOp::Add) == "");
  CHECK(Combined("2", "'5", PasteMode::All, PasteOp::Multiply) == "10");  // text that reads as a number
  CHECK(Combined("'5", "2", PasteMode::All, PasteOp::Multiply) == "10");
  CHECK(Combined("2", "' 5 ", PasteMode::All, PasteOp::Multiply) == "10");
  CHECK(Combined("2", "TRUE", PasteMode::All, PasteOp::Multiply) == "TRUE");  // a logical value stays
  CHECK(Combined("2", "FALSE", PasteMode::Values, PasteOp::Add) == "FALSE");
  CHECK(Combined("TRUE", "5", PasteMode::All, PasteOp::Multiply) == "5");
  CHECK(Combined("", "5", PasteMode::All, PasteOp::Multiply) == "0");  // a blank is 0
  CHECK(Combined("2", "abc", PasteMode::All, PasteOp::Add) == "abc");
  CHECK(Combined("0", "5", PasteMode::All, PasteOp::Divide) == "#DIV/0!");
  CHECK(Combined("2", "#N/A", PasteMode::Values, PasteOp::Add) == "#N/A");
  CHECK(Combined("=NA()", "5", PasteMode::Values, PasteOp::Add) == "5");
}

TEST_CASE("round8 data tail: Text to Columns opens a quoted field only at the field's start") {
  CHECK(Split("ab\"cd,e") == std::vector<std::string>{"ab\"cd", "e"});
  CHECK(Split("x\"a,b\",c") == std::vector<std::string>{"x\"a", "b\"", "c"});
  CHECK(Split("\"a,b\"c,d") == std::vector<std::string>{"a,bc", "d"});
  CHECK(Split(" \"a,b\",c") == std::vector<std::string>{" \"a", "b\"", "c"});
  CHECK(Split("p,\"a,b\",c") == std::vector<std::string>{"p", "a,b", "c"});
  CHECK(Split("\"a\"\"b\",c") == std::vector<std::string>{"a\"b", "c"});
  CHECK(Split("\"a\"b\"c,d\",e") == std::vector<std::string>{"ab\"c", "d\"", "e"});
  CHECK(Split("\"x,y") == std::vector<std::string>{"x,y"});
}

TEST_CASE("round8 data tail: Text to Columns makes a field that is a formula one") {
  engine::TextToColumnsOptions o;
  o.tab = false;
  o.comma = true;
  yctest::Doc t;
  t.Set("D5", "7");
  t.Set("A1", "'=1+1,2");
  t.Set("A2", "'x,=D5*2");
  t.Set("A3", "'=SUM(1,2)");
  t.Set("A4", "'\"=1+1\",b");
  t.d.TextToColumns(t.sheet(), R("A1:A4"), 0, 0, o);
  t.d.Recalc();
  CHECK(t.F("A1") == "=1+1");
  CHECK(t.N("A1") == 2);
  CHECK(t.N("B1") == 2);
  CHECK(t.F("B2") == "=D5*2");
  CHECK(t.N("B2") == 14);
  CHECK(t.V("A3").IsString());  // =SUM(1 isn't a formula: it stays text
  CHECK(t.T("A3") == "=SUM(1");
  CHECK(t.F("A4") == "=1+1");
  // a Text column keeps it as text
  yctest::Doc u;
  u.Set("A1", "'=1+1,b");
  o.types = {2, 1};
  u.d.TextToColumns(u.sheet(), R("A1"), 0, 0, o);
  CHECK(u.V("A1").IsString());
  CHECK(u.T("A1") == "=1+1");
}

TEST_CASE("round8 data tail: Auto Outline nests a total of totals and groups any formula over the cells above") {
  {
    yctest::Doc t;
    Column(t, {"1", "2", "=SUM(A1:A2)", "3", "4", "=SUM(A4:A5)", "=SUM(A3,A6)"});
    CHECK(t.d.AutoOutline(t.sheet(), R("A1:A10")) == 3);
    CHECK(RowLevels(t, 8) == std::vector<int>{2, 2, 1, 2, 2, 1, 0, 0});  // Excel's levels 3,3,2,3,3,2,1
  }
  {
    yctest::Doc t;
    Column(t, {"1", "2", "=SUM(A1:A2)", "3", "4", "=SUM(A4:A5)", "=A3+A6"});
    t.d.AutoOutline(t.sheet(), R("A1:A10"));
    CHECK(RowLevels(t, 8) == std::vector<int>{2, 2, 1, 2, 2, 1, 0, 0});
  }
  {
    yctest::Doc t;
    Column(t, {"1", "2", "3", "=A1+A2+A3"});
    t.d.AutoOutline(t.sheet(), R("A1:A10"));
    CHECK(RowLevels(t, 5) == std::vector<int>{1, 1, 1, 0, 0});
  }
  {
    yctest::Doc t;  // one cell referred to makes no group
    Column(t, {"1", "2", "=SUM(A1:A2)", "=A3*2"});
    t.d.AutoOutline(t.sheet(), R("A1:A10"));
    CHECK(RowLevels(t, 5) == std::vector<int>{1, 1, 0, 0, 0});
  }
  {
    yctest::Doc t;  // blank rows between the cells and the total are in its group
    Column(t, {"1", "2", "", "=SUM(A1:A2)"});
    t.d.AutoOutline(t.sheet(), R("A1:A10"));
    CHECK(RowLevels(t, 5) == std::vector<int>{1, 1, 1, 0, 0});
  }
  {
    yctest::Doc t;  // a value between them: no outer group
    Column(t, {"1", "2", "=SUM(A1:A2)", "3", "4", "=SUM(A4:A5)", "9", "=SUM(A3,A6)"});
    t.d.AutoOutline(t.sheet(), R("A1:A10"));
    CHECK(RowLevels(t, 9) == std::vector<int>{1, 1, 0, 1, 1, 0, 0, 0, 0});
  }
  {
    yctest::Doc t;
    Column(t, {"1", "2", "=SUM(A1:A2)", "3", "=SUM(A4)", "=A3+A5", "5", "=SUM(A6:A7)"});
    t.d.AutoOutline(t.sheet(), R("A1:A10"));
    CHECK(RowLevels(t, 9) == std::vector<int>{3, 3, 2, 2, 2, 1, 1, 0, 0});  // Excel's 4,4,3,3,3,2,2,1,1
  }
  {
    yctest::Doc t;  // columns the same way
    const char* row[] = {"1", "2", "=SUM(A1:B1)", "3", "4", "=SUM(D1:E1)", "=SUM(C1,F1)"};
    for (int c = 0; c < 7; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + "1").c_str(), row[c]);
    t.d.AutoOutline(t.sheet(), R("A1:J1"));
    std::vector<int> levels;
    for (int c = 0; c < 8; ++c) levels.push_back(t.d.S(t.sheet())->cols->Level(c));
    CHECK(levels == std::vector<int>{2, 2, 1, 2, 2, 1, 0, 0});
  }
}

TEST_CASE("round8 data tail: a defined name may be a union, as in Excel") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  std::string err;
  REQUIRE(t.d.DefineName(model::NameDef{"U", "=Sheet1!$A$1,Sheet1!$A$3", -1}, &err));
  // kept as ((...)), which parses as the union wherever names are read, and saves as text
  CHECK(t.d.FindName("U", t.sheet())->refers_to == "((Sheet1!$A$1,Sheet1!$A$3))");
  const std::string bytes = yc::io::SaveYcgToBytes(t.d.wb(), {});
  auto res = yc::io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  REQUIRE(res.wb.names->size() == 1);
  CHECK(res.wb.names->front().refers_to == "((Sheet1!$A$1,Sheet1!$A$3))");
  // a name typed with its parentheses keeps them; a union with a constant is refused, as in Excel
  REQUIRE(t.d.DefineName(model::NameDef{"P", "=(Sheet1!$A$1,Sheet1!$A$3)", -1}, &err));
  CHECK(t.d.FindName("P", t.sheet())->refers_to == "(Sheet1!$A$1,Sheet1!$A$3)");
  CHECK_FALSE(t.d.DefineName(model::NameDef{"K", "=Sheet1!$A$1,5", -1}, &err));
}

TEST_CASE("round8 data tail: a union name is its areas in formulas, and follows its cells") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  std::string err;
  REQUIRE(t.d.DefineName(model::NameDef{"U", "=Sheet1!$A$1,Sheet1!$A$3", -1}, &err));
  // Excel 16: SUM 4, COUNT 2, AREAS 2, INDEX of the second area 3; it was one #VALUE! (the union read as a value)
  t.Set("C1", "=SUM(U)");
  t.Set("C2", "=COUNT(U)");
  t.Set("C3", "=AREAS(U)");
  t.Set("C4", "=INDEX(U,1,1,2)");
  t.d.Recalc();
  CHECK(t.N("C1") == 4);
  CHECK(t.N("C2") == 2);
  CHECK(t.N("C3") == 2);
  CHECK(t.N("C4") == 3);
  t.Set("A3", "10");  // a cell of the union changes: the formulas that use the name follow
  t.d.Recalc();
  CHECK(t.N("C1") == 11);
  CHECK(t.N("C4") == 10);
  // a LET variable of the same name hides the defined name
  CHECK(t.Eval("LET(U,5,SUM(U))") == "5");
}

TEST_CASE("round8 data tail: a name that reads as the start of an R1C1 reference is refused, as Excel's Names.Add") {
  English en;
  yctest::Doc t;
  std::string err;
  for (const char* bad : {"R2D2", "C3PO", "r2d2", "R1C1", "RC1x", "R1x"}) {
    INFO(bad);
    CHECK_FALSE(t.d.DefineName(model::NameDef{bad, "=1", -1}, &err));
  }
  for (const char* ok : {"Rx", "Rate", "R0x", "Cx", "RCx"}) {
    INFO(ok);
    CHECK(t.d.DefineName(model::NameDef{ok, "=1", -1}, &err));
  }
}

TEST_CASE("round8 data tail: filters go above a PivotTable's body, which stays where it is") {
  English en;
  {
    yctest::Doc t;  // body at F3: a filter takes F1, the body stays
    const uint32_t id = SalesPivot(t, 3);
    CHECK(Top(t, id) == 3);
    SetPages(t, id, {1});
    CHECK(Top(t, id) == 1);
    CHECK(t.T("F1") == "Product");
    CHECK(t.T("G1") == "(All)");
    CHECK(t.T("F3") == "Row Labels");
    SetPages(t, id, {});
    CHECK(Top(t, id) == 3);
    CHECK(t.T("F3") == "Row Labels");
    CHECK(t.T("F1") == "");
  }
  {
    yctest::Doc t;  // body at F1: it moves down to F3 and stays there when the filter goes
    const uint32_t id = SalesPivot(t, 1);
    SetPages(t, id, {1});
    CHECK(Top(t, id) == 1);
    CHECK(t.T("F3") == "Row Labels");
    SetPages(t, id, {});
    CHECK(Top(t, id) == 3);
    CHECK(t.T("F3") == "Row Labels");
  }
  {
    yctest::Doc t;  // two filters from F3: the body moves down one row; one removed, the filter area shrinks
    const uint32_t id = SalesPivot(t, 3);
    SetPages(t, id, {1});
    SetPages(t, id, {1, 2});
    CHECK(Top(t, id) == 1);
    CHECK(t.T("F4") == "Row Labels");
    SetPages(t, id, {2});
    CHECK(Top(t, id) == 2);
    CHECK(t.T("F2") == "Sales");
    CHECK(t.T("F4") == "Row Labels");
  }
  {
    yctest::Doc t;  // from F10: the filters take the rows above
    const uint32_t id = SalesPivot(t, 10);
    SetPages(t, id, {1, 2});
    CHECK(Top(t, id) == 7);
    CHECK(t.T("F10") == "Row Labels");
  }
}
