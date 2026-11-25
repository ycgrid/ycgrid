// SPDX-License-Identifier: Apache-2.0
// Document operations behind the object model and the ribbon that had no unit test: sheets hidden and shown, controls,
// charts and panels updated and deleted, names added and deleted, table rows and columns inserted and deleted; and
// formula pieces: number format sections, conditions and colors, INDIRECT, MATCH, the coercions between types.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/engine.h"
#include "formula/numfmt.h"

#include <atomic>
#include <memory>

using yc::core::Rect;
namespace engine = yc::engine;
namespace model = yc::model;
namespace formula = yc::formula;

namespace {
Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}
std::string F(double v, const char* code) { return formula::FormatValue(yc::core::Value::Number(v), code).text; }
uint32_t Color(double v, const char* code) { return formula::FormatValue(yc::core::Value::Number(v), code).color; }
}  // namespace

TEST_CASE("Sheets hidden and shown; at least one stays visible") {
  yctest::Doc t;
  const uint32_t s1 = t.sheet(), s2 = t.d.AddSheet("Two");
  t.d.SetSheetVisible(s2, false);
  CHECK_FALSE(t.d.S(s2)->visible);
  t.d.SetSheetVisible(s1, false);  // the last visible one: refused
  CHECK(t.d.S(s1)->visible);
  t.d.SetSheetVisible(s2, true);
  CHECK(t.d.S(s2)->visible);
  t.d.SetSheetVisible(999999, false);  // no such sheet: nothing happens
  t.d.wb_mut().protect_structure = true;
  t.d.SetSheetVisible(s2, false);  // a protected structure keeps it
  CHECK(t.d.S(s2)->visible);
}

TEST_CASE("Controls, charts and panels are updated and deleted") {
  yctest::Doc t;
  model::Control c;
  c.caption = "Go";
  const uint32_t id = t.d.AddControl(t.sheet(), c);
  REQUIRE(id);
  CHECK((*t.d.S(t.sheet())->controls)[0].name == "CommandButton1");
  CHECK(t.d.UpdateControl(t.sheet(), id, [](model::Control& x) {
    x.caption = "Run";
    x.width = 100;
  }));
  CHECK((*t.d.S(t.sheet())->controls)[0].caption == "Run");
  CHECK_FALSE(t.d.UpdateControl(t.sheet(), id + 100, [](model::Control&) {}));
  CHECK(t.d.DeleteControl(t.sheet(), id));
  CHECK(t.d.S(t.sheet())->controls->empty());
  CHECK_FALSE(t.d.DeleteControl(t.sheet(), id));
  model::ChartObj ch;
  ch.title = "Px";
  const uint32_t cid = t.d.AddChart(t.sheet(), ch);
  REQUIRE(cid);
  CHECK(t.d.UpdateChart(t.sheet(), cid, [](model::ChartObj& x) { x.title = "Price"; }));
  CHECK((*t.d.S(t.sheet())->charts)[0].title == "Price");
  CHECK_FALSE(t.d.UpdateChart(t.sheet(), cid + 7, [](model::ChartObj&) {}));
  CHECK(t.d.DeleteChart(t.sheet(), cid));
  CHECK(t.d.S(t.sheet())->charts->empty());
  model::Panel p;
  p.name = "Orders";
  p.caption = "Orders";
  t.d.UpsertPanel(p);
  REQUIRE(t.d.wb().panels->size() == 1);
  CHECK(t.d.UpdatePanel("Orders", [](model::Panel& x) { x.caption = "Order entry"; }));
  CHECK((*t.d.wb().panels)[0].caption == "Order entry");
  CHECK_FALSE(t.d.UpdatePanel("Nope", [](model::Panel&) {}));
  p.dock = "left";
  t.d.UpsertPanel(p);  // the same name replaces it
  REQUIRE(t.d.wb().panels->size() == 1);
  CHECK((*t.d.wb().panels)[0].dock == "left");
  CHECK(t.d.DeletePanel("Orders"));
  CHECK(t.d.wb().panels->empty());
  CHECK_FALSE(t.d.DeletePanel("Orders"));
}

TEST_CASE("Names added and deleted, workbook and sheet scope") {
  yctest::Doc t;
  t.Set("A1", "4");
  std::string err;
  REQUIRE(t.d.AddName("Qty", "=Sheet1!$A$1", -1, &err));
  t.Set("B1", "=Qty*2");
  CHECK(t.N("B1") == 8);
  CHECK_FALSE(t.d.AddName("1bad", "=1", -1, &err));  // not a valid name
  CHECK_FALSE(err.empty());
  REQUIRE(t.d.AddName("Qty", "=10", static_cast<int32_t>(t.sheet()), &err));  // the sheet's own Qty wins on its sheet
  t.d.Recalc();
  CHECK(t.N("B1") == 20);
  CHECK(t.d.DeleteName("Qty", static_cast<int32_t>(t.sheet())));
  t.d.Recalc();
  CHECK(t.N("B1") == 8);
  CHECK(t.d.DeleteName("qty"));
  t.d.Recalc();
  CHECK(t.T("B1") == "#NAME?");
  CHECK_FALSE(t.d.DeleteName("Qty"));
}

TEST_CASE("Table rows and columns inserted and deleted keep the table whole") {
  yctest::Doc t;
  t.Set("A1", "Sym");
  t.Set("B1", "Qty");
  t.Set("A2", "TXF");
  t.Set("B2", "1");
  t.Set("A3", "MXF");
  t.Set("B3", "2");
  t.Set("D1", "beside");
  std::string err;
  const uint32_t id = t.d.AddTable(t.sheet(), R("A1:B3"), true, "TableStyleMedium2", &err);
  REQUIRE(id);
  auto table = [&]() { return (*t.d.S(t.sheet())->tables)[0]; };
  REQUIRE(t.d.TableInsertRow(t.sheet(), id, 0));  // above the first data row
  CHECK(table().range == R("A1:B4"));
  CHECK(t.V("A2").IsEmpty());
  CHECK(t.T("A3") == "TXF");
  CHECK(t.T("D1") == "beside");  // only the table's columns shift
  REQUIRE(t.d.TableInsertRow(t.sheet(), id, -1));  // at the end
  CHECK(table().range == R("A1:B5"));
  REQUIRE(t.d.TableInsertColumn(t.sheet(), id, 1));  // between Sym and Qty
  CHECK(table().range == R("A1:C5"));
  REQUIRE(table().columns.size() == 3);
  CHECK(table().columns[0].name == "Sym");
  CHECK(table().columns[2].name == "Qty");
  CHECK_FALSE(table().columns[1].name.empty());  // a new header (Column1)
  REQUIRE(t.d.TableInsertColumn(t.sheet(), id, -1));
  CHECK(table().range == R("A1:D5"));
  REQUIRE(t.d.TableDeleteColumn(t.sheet(), id, 1));
  CHECK(table().range == R("A1:C5"));
  CHECK(table().columns[1].name == "Qty");
  CHECK(t.T("B4") == "2");
  CHECK_FALSE(t.d.TableInsertRow(t.sheet(), id + 50, 0));
  CHECK_FALSE(t.d.TableDeleteColumn(t.sheet(), id, 99));
}

TEST_CASE("The engine's user commands, publish interval and Call") {
  engine::Engine eng(model::Workbook::NewDefault("Sheet"));
  eng.Start();
  const uint32_t sheet = eng.Current()->wb.sheets[0]->id;
  eng.SetPublishInterval(0);
  CHECK(eng.PublishInterval() == 0);
  eng.PostUser("Typing", [sheet](engine::Document& d) { d.SetInput(sheet, 0, 0, "5"); });
  eng.Sync();
  const auto snap = eng.Current();
  CHECK(snap->wb.sheets[0]->GetValue(0, 0).AsDouble() == 5);
  CHECK(snap->can_undo);
  CHECK(snap->undo_label == "Typing");  // a user command is one named undo step
  CHECK(eng.Call([](engine::Document& d) { return d.Undo(); }));
  eng.Sync();
  CHECK(eng.Current()->wb.sheets[0]->GetValue(0, 0).IsEmpty());
  eng.SetPublishInterval(100);
  eng.Stop();
}

TEST_CASE("Number formats: conditions, colors, sections and text") {
  CHECK(F(150, "[>100]\"big\";[<=100]\"small\"") == "big");  // conditions pick the section
  CHECK(F(50, "[>100]\"big\";[<=100]\"small\"") == "small");
  CHECK(F(5, "[<0]\"neg\";[=5]\"five\";0") == "five");
  CHECK(F(7, "[>=10]0.0;0") == "7");
  CHECK(F(-3, "0;[Red]-0") == "-3");
  CHECK(Color(-3, "0;[Red]-0") != 0);  // a color for the negative section
  CHECK(Color(3, "0;[Red]-0") == 0);
  CHECK(Color(1, "[Blue]0") != Color(1, "[Green]0"));
  CHECK(Color(1, "[Color10]0") != 0);
  CHECK(Color(1, "[Magenta]0") != 0);
  CHECK(F(1234.5, "#,##0.00;(#,##0.00);\"zero\"") == "1,234.50");
  CHECK(F(-1234.5, "#,##0.00;(#,##0.00);\"zero\"") == "(1,234.50)");
  CHECK(F(0, "#,##0.00;(#,##0.00);\"zero\"") == "zero");
  CHECK(formula::FormatValue(yc::core::Value::String("abc"), "0;-0;0;\"<\"@\">\"").text == "<abc>");  // the text section
  CHECK(formula::FormatValue(yc::core::Value::String("abc"), "0.00").text == "abc");
  CHECK(F(0.125, "0.0%") == "12.5%");
  CHECK(F(1234567, "#,##0,\"K\"") == "1,235K");  // a comma at the end divides by 1000
  CHECK(F(1234567, "0.0,,\"M\"") == "1.2M");
  CHECK(F(3.5, "# ?/?") == "3 1/2");
  CHECK(F(12345.678, "0.00E+00") == "1.23E+04");
  CHECK(F(5, "000") == "005");
  CHECK(F(1.5, "\"$\"* #0.00").find("1.50") != std::string::npos);
  CHECK(F(46331.75, "yyyy-mm-dd hh:mm") == "2026-11-05 18:00");
  CHECK(F(46331.75, "ddd, mmm d") == "Thu, Nov 5");
  CHECK(F(46331.75, "dddd mmmm yy") == "Thursday November 26");
  CHECK(F(46331.75, "h:mm AM/PM") == "6:00 PM");
  CHECK(F(46331.0, "h:mm a/p") == "12:00 a");  // the marker in the code's case (Excel)
  CHECK(F(46331.75, "h:mm A/P") == "6:00 P");
  CHECK(F(46331.75, "h:mm am/pm") == "6:00 pm");
  CHECK(F(46331.25, "hh AM/PM") == "06 AM");
  CHECK(F(1.5, "[h]:mm") == "36:00");  // elapsed hours past 24
  CHECK(F(0.000694444444, "[mm]:ss") == "01:00");
  CHECK(F(0.5, "hh:mm:ss.00") == "12:00:00.00");
}

TEST_CASE("INDIRECT and MATCH in their less common forms") {
  yctest::Doc t;
  const uint32_t s2 = t.d.AddSheet("Data Sheet");
  t.d.SetInput(s2, 1, 2, "42");  // 'Data Sheet'!C2
  t.Set("A1", "1");
  t.Set("A2", "3");
  t.Set("A3", "5");
  t.Set("B1", "apple");
  t.Set("B2", "banana");
  t.Set("B3", "cherry");
  CHECK(t.Eval("INDIRECT(\"'Data Sheet'!C2\")") == "42");
  CHECK(t.Eval("INDIRECT(\"R2C3\",FALSE)") == "0");  // R1C1 on this sheet: C2 is empty
  CHECK(t.Eval("INDIRECT(\"'Data Sheet'!R2C3\",FALSE)") == "42");
  CHECK(t.Eval("SUM(INDIRECT(\"A1:A3\"))") == "9");
  CHECK(t.Eval("SUM(INDIRECT(\"A:A\"))") == "9");
  CHECK(t.Eval("INDIRECT(\"nonsense!!\")") == "#REF!");
  CHECK(t.Eval("INDIRECT(\"'No Such'!A1\")") == "#REF!");
  CHECK(t.Eval("MATCH(4,A1:A3,1)") == "2");     // the largest value <= 4 (ascending)
  CHECK(t.Eval("MATCH(4,A1:A3)") == "2");
  CHECK(t.Eval("MATCH(0,A1:A3,1)") == "#N/A");
  CHECK(t.Eval("MATCH(\"b*\",B1:B3,0)") == "2");  // wildcards with an exact match
  CHECK(t.Eval("MATCH(\"?herry\",B1:B3,0)") == "3");
  CHECK(t.Eval("MATCH(\"BANANA\",B1:B3,0)") == "2");  // any case
  t.Set("C1", "9");
  t.Set("C2", "6");
  t.Set("C3", "2");
  CHECK(t.Eval("MATCH(5,C1:C3,-1)") == "2");  // the smallest value >= 5 (descending)
  CHECK(t.Eval("MATCH(10,C1:C3,-1)") == "#N/A");
  CHECK(t.Eval("MATCH(3,A1:C1,0)") == "#N/A");  // across a row
  CHECK(t.Eval("MATCH(\"apple\",B1:B3,0)") == "1");
  CHECK(t.Eval("ROWS(A1:C3)") == "3");
  CHECK(t.Eval("COLUMNS(A1:C3)") == "3");
  CHECK(t.Eval("ROWS({1,2;3,4;5,6})") == "3");
  CHECK(t.Eval("COLUMNS(5)") == "1");
}

TEST_CASE("Coercions between text, numbers, booleans and arrays") {
  yctest::Doc t;
  t.Set("A1", "TRUE");
  t.Set("A2", "'5");  // text that looks like a number
  t.Set("A3", "x");
  CHECK(t.Eval("IF(\"TRUE\",1,2)") == "1");
  CHECK(t.Eval("IF(\"false\",1,2)") == "2");
  CHECK(t.Eval("IF(\"maybe\",1,2)") == "#VALUE!");
  CHECK(t.Eval("IF(0.5,1,2)") == "1");
  CHECK(t.Eval("AND(A1,1)") == "TRUE");
  CHECK(t.Eval("NOT(\"x\")") == "#VALUE!");
  CHECK(t.Eval("A2+1") == "6");          // text in a cell is read as a number in arithmetic
  CHECK(t.Eval("A3+1") == "#VALUE!");
  CHECK(t.Eval("TRUE+TRUE") == "2");
  CHECK(t.Eval("\"1/2/2026\"+0") == std::to_string(46023 + 1));  // a date in text
  CHECK(t.Eval("-\"3\"") == "-3");
  CHECK(t.Eval("\"a\"&TRUE&1.5") == "aTRUE1.5");
  CHECK(t.Eval("\"x\"&1/0") == "#DIV/0!");
  CHECK(t.Eval("SUM({1,2,3}*2)") == "12");    // an operation over an array
  CHECK(t.Eval("SUM({1,2,3}+{10;20})") == "102");  // a row and a column broadcast to 2 x 3
  CHECK(t.Eval("SUM(-{1,2})") == "-3");
  CHECK(t.Eval("SUM(({1,2,3}>1)*1)") == "2");
  CHECK(t.Eval("SUM(IFERROR(1/{0,1,2},0))") == "1.5");
  CHECK(t.Eval("{1,2}=\"1\"") == "FALSE");    // a number never equals text
  CHECK(t.Eval("\"b\">\"a\"") == "TRUE");
  CHECK(t.Eval("\"a\"<1") == "FALSE");          // text sorts after numbers
  CHECK(t.Eval("TRUE>\"z\"") == "TRUE");         // and booleans after text
  CHECK(t.Eval("#N/A=1") == "#N/A");
  CHECK(t.Eval("1%") == "0.01");
  CHECK(t.Eval("(1+2)%") == "0.03");
}

TEST_CASE("Renaming a sheet rewrites the references kept as text: names, chart series, rules and validation lists") {
  yctest::Doc t;
  const uint32_t data = t.d.AddSheet("Data");
  t.Set("A1", "1", data);
  t.Set("A2", "2", data);
  std::string err;
  REQUIRE(t.d.AddName("Nums", "=Data!$A$1:$A$2", -1, &err));
  const std::string first = t.d.S(t.sheet())->name;  // "Sheet1" in English
  REQUIRE(t.d.AddName("Other", "='" + first + "'!$B$1", -1, &err));
  model::ChartObj ch;
  model::ChartSeries se;
  se.values = "Data!$A$1:$A$2";
  ch.series.push_back(se);
  ch.source = "Data!$A$1:$A$2";
  t.d.AddChart(t.sheet(), ch);
  model::CfRule cf;
  cf.ranges.push_back(R("C1:C2"));
  cf.type = model::CfType::Expression;
  cf.formula1 = "C1>Data!$A$1";
  t.d.AddCondFormat(t.sheet(), cf);
  model::DvRule list;
  list.ranges.push_back(R("D1"));
  list.type = model::DvType::List;
  list.formula1 = "=Data!$A$1:$A$2";
  t.d.SetValidation(t.sheet(), R("D1"), list);
  model::DvRule words = list;
  words.ranges = {R("D2")};
  words.formula1 = "Data,Other";  // a list of values, not a reference: kept as typed
  t.d.SetValidation(t.sheet(), R("D2"), words);
  t.Set("E1", "=SUM(Nums)");
  REQUIRE(t.N("E1") == 3);

  REQUIRE(t.d.RenameSheet(data, "Quotes 2026", &err));
  const auto& names = *t.d.wb().names;
  auto refers = [&](const char* n) {
    for (const auto& x : names) {
      if (x.name == n) return x.refers_to;
    }
    return std::string("?");
  };
  CHECK(refers("Nums").find("'Quotes 2026'!$A$1:$A$2") != std::string::npos);
  CHECK(refers("Nums").find("Data") == std::string::npos);
  CHECK(refers("Other").find("!$B$1") != std::string::npos);  // names of other sheets keep their text
  const auto& chart = t.d.S(t.sheet())->charts->front();
  CHECK(chart.series[0].values == "'Quotes 2026'!$A$1:$A$2");
  CHECK(chart.source == "'Quotes 2026'!$A$1:$A$2");
  CHECK(t.d.S(t.sheet())->cond_formats->front().formula1 == "C1>'Quotes 2026'!$A$1");
  std::string f1, f2;
  for (const auto& dv : *t.d.S(t.sheet())->validations) {
    if (dv.ranges.front() == R("D1")) f1 = dv.formula1;
    if (dv.ranges.front() == R("D2")) f2 = dv.formula1;
  }
  CHECK(f1 == "='Quotes 2026'!$A$1:$A$2");
  CHECK(f2 == "Data,Other");
  t.Set("A1", "10", data);
  CHECK(t.N("E1") == 12);  // the name still refers to the cells
}

TEST_CASE("A chart's texts linked to cells follow them as its ranges do: inserts, renames, moves, a copied sheet") {
  yctest::Doc t;
  const uint32_t sh = t.d.AddSheet("Data");
  t.Set("A1", "Price", sh);
  t.Set("A2", "1", sh);
  t.Set("A3", "2", sh);
  model::ChartObj ch;
  ch.title = "=Data!$C$1";
  ch.val_axis.title = "=Data!$C$2";
  model::ChartSeries se;
  se.name = "=Data!$A$1";
  se.values = "Data!$A$2:$A$3";
  se.err_plus = "Data!$B$2:$B$3";
  model::ChartPointFormat pf;
  pf.label_text = "=Data!$D$1";
  se.points.push_back(pf);
  model::ChartSeries plain;
  plain.name = "A1";  // a typed name, not a reference: kept as typed
  plain.values = "Data!$A$2:$A$3";
  ch.series = {se, plain};
  t.d.AddChart(sh, ch);
  auto chart = [&](uint32_t sheet) { return t.d.S(sheet)->charts->front(); };
  // rows inserted above (Excel 16: =SERIES(Data!$A$2,,Data!$A$3:$A$4,1))
  t.d.InsertRows(sh, 0, 1);
  CHECK(chart(sh).series[0].name == "=Data!$A$2");
  CHECK(chart(sh).series[0].values == "Data!$A$3:$A$4");
  CHECK(chart(sh).series[0].err_plus == "Data!$B$3:$B$4");
  CHECK(chart(sh).series[0].points[0].label_text == "=Data!$D$2");
  CHECK(chart(sh).title == "=Data!$C$2");
  CHECK(chart(sh).val_axis.title == "=Data!$C$3");
  CHECK(chart(sh).series[1].name == "A1");
  // the sheet renamed
  std::string err;
  REQUIRE(t.d.RenameSheet(sh, "Quotes", &err));
  CHECK(chart(sh).series[0].name == "=Quotes!$A$2");
  CHECK(chart(sh).title == "=Quotes!$C$2");
  // the name's cell moved (cut and paste)
  REQUIRE(t.d.MoveCells(sh, Rect::Cell(1, 0), sh, 9, 5));
  CHECK(chart(sh).series[0].name == "=Quotes!$F$10");
  // a copy of the sheet: its chart shows the copy's cells
  const uint32_t cp = t.d.CopySheet(sh, -1);
  REQUIRE(cp);
  const std::string cn = t.d.S(cp)->name;
  CHECK(chart(cp).series[0].name == "='" + cn + "'!$F$10");
  CHECK(chart(cp).series[0].values == "'" + cn + "'!$A$3:$A$4");
  CHECK(chart(sh).series[0].values == "Quotes!$A$3:$A$4");
}

TEST_CASE("A copied sheet's table formulas, controls, rules and validation point at the copy (Excel 16, measured)") {
  yctest::Doc t;
  const uint32_t sh = t.d.AddSheet("S");
  auto set = [&](uint32_t sheet, uint32_t r, uint32_t c, const char* v) { t.d.SetInput(sheet, r, c, v); };
  set(sh, 0, 0, "k"), set(sh, 0, 1, "v"), set(sh, 1, 0, "a"), set(sh, 1, 1, "1"), set(sh, 2, 0, "b"), set(sh, 2, 1, "2");
  REQUIRE(t.d.AddTable(sh, Rect{0, 0, 2, 1}, true, "TableStyleMedium2"));
  const std::string table = t.d.S(sh)->tables->front().name;
  set(sh, 0, 3, ("=SUM(" + table + "[v])").c_str());  // D1
  model::Control box;
  box.kind = model::ControlKind::CheckBox;
  box.form = true;
  box.linked_cell = "S!$F$2";
  t.d.AddControl(sh, box);
  model::CfRule cf;
  cf.type = model::CfType::Expression;
  cf.ranges = {Rect{0, 7, 2, 7}};
  cf.formula1 = "S!$B1>1";
  t.d.AddCondFormat(sh, cf);
  model::DvRule dv;
  dv.type = model::DvType::List;
  dv.formula1 = "=S!$A$2:$A$3";
  dv.ranges = {Rect::Cell(0, 6)};
  t.d.SetValidation(sh, dv.ranges[0], dv);
  const uint32_t cp = t.d.CopySheet(sh, -1);
  REQUIRE(cp);
  const model::Sheet* c = t.d.S(cp);
  const std::string copy_table = c->tables->front().name;
  REQUIRE(copy_table != table);
  // the copy's formula sums the copy's table (Excel: =SUM(Table1_2[v]) there); it summed the source's
  CHECK(t.d.GetFormulaA1(cp, 0, 3) == "=SUM(" + copy_table + "[v])");
  CHECK(t.d.GetFormulaA1(sh, 0, 3) == "=SUM(" + table + "[v])");
  t.d.SetInput(cp, 1, 1, "10");
  t.d.Recalc();
  CHECK(t.d.S(cp)->GetValue(0, 3).AsDouble() == 12);
  CHECK(t.d.S(sh)->GetValue(0, 3).AsDouble() == 3);
  // a control's linked cell: 'S (2)'!$F$2; a rule and a validation name no sheet then (=$B1>1, =$A$2:$A$3)
  c = t.d.S(cp);
  CHECK(c->controls->front().linked_cell == "'" + c->name + "'!$F$2");
  CHECK(c->cond_formats->front().formula1 == "$B1>1");
  CHECK(c->validations->front().formula1 == "=$A$2:$A$3");
  CHECK(t.d.S(sh)->controls->front().linked_cell == "S!$F$2");
  CHECK(t.d.S(sh)->cond_formats->front().formula1 == "S!$B1>1");
}

TEST_CASE("A control's linked cell and list range follow their cells: inserts, deletes, renames and moves") {
  yctest::Doc t;
  const uint32_t sh = t.d.AddSheet("Data");
  const uint32_t other = t.sheet();
  model::Control box;
  box.kind = model::ControlKind::CheckBox;
  box.form = true;
  box.linked_cell = "$A$5";
  t.d.AddControl(sh, box);
  model::Control list;
  list.kind = model::ControlKind::ComboBox;
  list.form = true;
  list.list_fill_range = "$B$1:$B$3";
  list.linked_cell = "Data!$C$1";
  t.d.AddControl(other, list);
  auto ctl = [&](uint32_t sheet) { return t.d.S(sheet)->controls->front(); };
  t.d.InsertRows(sh, 0, 1);  // Excel 16: LinkedCell $A$6
  CHECK(ctl(sh).linked_cell == "$A$6");
  CHECK(ctl(other).linked_cell == "Data!$C$2");     // a link to the sheet that moved
  CHECK(ctl(other).list_fill_range == "$B$1:$B$3");  // its own sheet didn't
  t.d.InsertRows(other, 0, 2);
  CHECK(ctl(other).list_fill_range == "$B$3:$B$5");
  std::string err;
  REQUIRE(t.d.RenameSheet(sh, "Inputs", &err));
  CHECK(ctl(other).linked_cell == "Inputs!$C$2");
  REQUIRE(t.d.MoveCells(sh, Rect::Cell(5, 0), sh, 9, 3));  // the linked cell cut and pasted
  CHECK(ctl(sh).linked_cell == "$D$10");
  t.d.DeleteCols(sh, 3, 1);
  CHECK(ctl(sh).linked_cell == "#REF!");
}

TEST_CASE("Data labels from cells, bubble sizes and a sparkline's date axis follow their cells") {
  yctest::Doc t;
  const uint32_t sh = t.d.AddSheet("Data");
  model::ChartObj ch;
  model::ChartSeries se;
  se.values = "Data!$A$2:$A$4";
  se.label_range = "Data!$C$2:$C$4";
  se.bubble_sizes = "Data!$D$2:$D$4";
  ch.series = {se};
  t.d.AddChart(sh, ch);
  model::SparklineGroup g;
  g.items.push_back(model::Sparkline{0, 7, sh, Rect{1, 0, 3, 0}});
  g.date_range = "$E$2:$E$4";
  t.d.AddSparklineGroup(sh, g);
  t.d.InsertRows(sh, 0, 1);
  const auto& c = t.d.S(sh)->charts->front().series[0];
  CHECK(c.values == "Data!$A$3:$A$5");
  CHECK(c.label_range == "Data!$C$3:$C$5");
  CHECK(c.bubble_sizes == "Data!$D$3:$D$5");
  CHECK(t.d.S(sh)->sparklines->front().date_range == "$E$3:$E$5");
  REQUIRE(t.d.MoveCells(sh, Rect{2, 4, 4, 4}, sh, 2, 6));  // the dates cut to G3:G5
  CHECK(t.d.S(sh)->sparklines->front().date_range == "$G$3:$G$5");
}

TEST_CASE("A Solver model's cells follow inserts, renames and moves, as Excel's solver_* names do") {
  yctest::Doc t;
  const uint32_t sh = t.d.AddSheet("Model");
  model::SolverModel m;
  m.defined = true;
  m.objective = "$D$10";
  m.changing = "$B$2:$B$5,$C$7";
  m.constraints.push_back(model::SolverConstraint{"$B$2:$B$5", 1, "$E$1"});
  m.constraints.push_back(model::SolverConstraint{"$C$7", 3, "10"});
  t.d.SetSolverModel(sh, m);
  t.d.InsertRows(sh, 0, 2);
  const model::SolverModel& a = *t.d.S(sh)->solver;
  CHECK(a.objective == "$D$12");
  CHECK(a.changing == "$B$4:$B$7,$C$9");
  CHECK(a.constraints[0].ref == "$B$4:$B$7");
  CHECK(a.constraints[0].rhs == "$E$3");
  CHECK(a.constraints[1].rhs == "10");
  t.d.InsertCols(sh, 0, 1);
  CHECK(t.d.S(sh)->solver->objective == "$E$12");
  REQUIRE(t.d.MoveCells(sh, Rect::Cell(11, 4), sh, 0, 9));  // the objective cut to J1
  CHECK(t.d.S(sh)->solver->objective == "$J$1");
}

TEST_CASE("A scenario's changing cells move with inserted and deleted rows and columns, as Excel's do") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  model::Scenario sc;
  sc.name = "Best";
  sc.cells = {yctest::Doc::P("B2"), yctest::Doc::P("C5")};
  sc.values = {yc::core::Value::Number(1), yc::core::Value::Number(2)};
  std::string err;
  REQUIRE(t.d.AddScenario(sh, sc, &err));
  t.d.InsertRows(sh, 0, 1);
  const auto& a = t.d.S(sh)->scenarios->front();
  REQUIRE(a.cells.size() == 2);
  CHECK(a.cells[0].row == 2);
  CHECK(a.cells[1].row == 5);
  t.d.InsertCols(sh, 0, 2);
  CHECK(t.d.S(sh)->scenarios->front().cells[0].col == 3);
  // Show puts the values in the cells where they are now
  REQUIRE(t.d.ShowScenario(sh, "Best"));
  CHECK(t.N("D3") == 1);
  CHECK(t.N("E6") == 2);
  t.d.DeleteRows(sh, 2, 1);  // B2's row: the scenario keeps its other cell
  REQUIRE(t.d.S(sh)->scenarios->front().cells.size() == 1);
  CHECK(t.d.S(sh)->scenarios->front().cells[0].row == 4);
  CHECK(t.d.S(sh)->scenarios->front().values[0].AsDouble() == 2);
}

TEST_CASE("A data table and its input cell move with inserted rows and columns, as in Excel") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "2");
  t.Set("D2", "=A1*10");
  t.Set("C3", "1");
  t.Set("C4", "2");
  t.Set("C5", "3");
  const yc::core::CellPos in = yctest::Doc::P("A1");
  std::string err;
  REQUIRE(t.d.AddDataTable(sh, Rect{1, 2, 4, 3}, nullptr, &in, &err));
  t.d.Recalc();
  REQUIRE(t.N("D4") == 20);
  t.d.InsertRows(sh, 0, 1);  // Excel 16: {=TABLE(,A2)} over D4:D6
  t.d.Recalc();
  CHECK(t.F("D4") == "{=TABLE(,A2)}");
  CHECK(t.F("D3") == "=A2*10");
  t.Set("C5", "7");
  CHECK(t.N("D5") == 70);
  CHECK(t.N("D4") == 10);
  CHECK(t.N("D6") == 30);
  t.d.InsertCols(sh, 0, 1);
  t.Set("D4", "5");
  CHECK(t.N("E4") == 50);
  CHECK(t.F("E4") == "{=TABLE(,B2)}");
}
