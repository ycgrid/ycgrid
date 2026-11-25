// SPDX-License-Identifier: Apache-2.0
// .xlsx: the ZIP container, and a workbook saved as xlsx and read back
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/document.h"
#include "io/xlsx.h"
#include "core/i18n.h"

#include <algorithm>

using yc::core::Rect;
namespace io = yc::io;

TEST_CASE("xlsx: ZIP entries round-trip (stored and deflated)") {
  std::vector<io::ZipEntry> in = {{"a.txt", "hello"}, {"dir/big.xml", std::string(100000, 'x') + "end"}, {"empty", ""}};
  const std::string zip = io::WriteZip(in);
  CHECK(zip.size() < 2000);  // the repeated text is deflated
  std::vector<io::ZipEntry> out;
  std::string err;
  REQUIRE(io::ReadZip(zip, &out, &err));
  REQUIRE(out.size() == 3);
  CHECK(out[0].data == "hello");
  CHECK(out[1].name == "dir/big.xml");
  CHECK(out[1].data == in[1].data);
  CHECK(out[2].data.empty());
  CHECK_FALSE(io::ReadZip("not a zip at all, definitely not", &out, &err));
}

TEST_CASE("xlsx: values, formulas, styles, merges, sizes and names survive save and open") {
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  t.Set("A1", "Item");
  t.Set("B1", "Qty");
  t.Set("A2", "Apple");
  t.Set("B2", "3");
  t.Set("A3", "Pear");
  t.Set("B3", "4.5");
  t.Set("B4", "=SUM(B2:B3)");
  t.Set("C2", "=B2*2");
  t.Set("C3", "=B3*2");
  t.Set("D2", "=XLOOKUP(\"Pear\",A2:A3,B2:B3)");
  t.Set("E2", "TRUE");
  t.Set("F2", "=1/0");
  t.d.ModifyStyle(s1, Rect{0, 0, 0, 1}, [](yc::model::Style& s) {
    s.bold = true;
    s.fill_color = yc::model::Rgb(255, 255, 0);
    s.border[yc::model::kBottom].style = yc::model::LineStyle::Double;
  });
  t.d.ModifyStyle(s1, Rect{1, 1, 3, 1}, [&](yc::model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString("0.00"); });
  t.d.ModifyStyle(s1, Rect::Cell(5, 0), [&](yc::model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString("yyyy-mm-dd;@"); });
  t.Set("A6", "45306");
  t.d.Merge(s1, Rect{7, 0, 7, 2});
  t.d.SetColWidth(s1, 0, 0, 120);
  t.d.SetRowHeight(s1, 2, 2, 40);
  REQUIRE(t.d.AddName("Total", "Sheet1!$B$4"));
  const uint32_t s2 = t.d.AddSheet("Data Two");
  t.d.SetInput(s2, 0, 0, "=Sheet1!B4+1");
  t.d.Recalc();

  const std::string bytes = io::ExportXlsxBytes(t.d.wb());
  yc::model::Workbook wb;
  std::string err;
  std::vector<std::string> warnings;
  REQUIRE(io::ImportXlsxBytes(bytes, &wb, &err, &warnings));
  CHECK(warnings.empty());
  yc::engine::Document d(std::move(wb));
  REQUIRE(d.wb().sheets.size() == 2);
  const uint32_t a = d.wb().sheets[0]->id, b = d.wb().sheets[1]->id;
  CHECK(d.wb().sheets[1]->name == "Data Two");
  CHECK(d.GetValue(a, 0, 0).str() == "Item");
  CHECK(d.GetValue(a, 3, 1).AsDouble() == 7.5);
  CHECK(d.GetFormulaA1(a, 3, 1) == "=SUM(B2:B3)");
  CHECK(d.GetFormulaA1(a, 2, 2) == "=B3*2");
  CHECK(d.GetFormulaA1(a, 1, 3) == "=XLOOKUP(\"Pear\",A2:A3,B2:B3)");  // _xlfn. added on save, dropped on open
  CHECK(d.GetValue(a, 1, 3).AsDouble() == 4.5);
  CHECK(d.GetValue(a, 1, 4).IsBool());
  CHECK(d.GetValue(a, 1, 4).boolean());  // TRUE stayed TRUE (it was written as 0)
  CHECK(d.GetValue(a, 1, 5).IsError());
  CHECK(d.GetValue(b, 0, 0).AsDouble() == 8.5);
  const auto& sh = *d.wb().sheets[0];
  const auto& st = d.wb().styles->Get(sh.GetStyle(0, 1));
  CHECK(st.bold);
  CHECK(st.fill_color == yc::model::Rgb(255, 255, 0));
  CHECK(st.border[yc::model::kBottom].style == yc::model::LineStyle::Double);
  CHECK(d.wb().styles->NumberFormat(d.wb().styles->Get(sh.GetStyle(2, 1))) == "0.00");
  CHECK(d.wb().styles->NumberFormat(d.wb().styles->Get(sh.GetStyle(5, 0))) == "yyyy-mm-dd;@");
  REQUIRE(sh.merges->size() == 1);
  CHECK((*sh.merges)[0] == Rect{7, 0, 7, 2});
  CHECK(std::abs(sh.cols->Size(0) - 120) <= 1);
  CHECK(std::abs(sh.rows->Size(2) - 40) <= 1);
  REQUIRE(d.wb().names->size() == 1);
  CHECK((*d.wb().names)[0].name == "Total");
}

TEST_CASE("xlsx: shared formulas are moved to every cell of their range") {
  // a minimal workbook as Excel writes it: B1:B3 share the formula of B1
  const std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>"
      "<row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><f t=\"shared\" ref=\"B1:B3\" si=\"0\">A1*10</f><v>10</v></c></row>"
      "<row r=\"2\"><c r=\"A2\"><v>2</v></c><c r=\"B2\"><f t=\"shared\" si=\"0\"/><v>20</v></c></row>"
      "<row r=\"3\"><c r=\"A3\"><v>3</v></c><c r=\"B3\"><f t=\"shared\" si=\"0\"/><v>30</v></c><c r=\"C3\" t=\"inlineStr\"><is><t>hi</t></is></c></row>"
      "</sheetData></worksheet>";
  const std::string wbx =
      "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
      "<sheets><sheet name=\"S\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
  const std::string rels =
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/></Relationships>";
  const std::string zip = io::WriteZip({{"xl/workbook.xml", wbx}, {"xl/_rels/workbook.xml.rels", rels}, {"xl/worksheets/sheet1.xml", sheet}});
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(zip, &wb, &err, nullptr));
  yc::engine::Document d(std::move(wb));
  const uint32_t s = d.wb().sheets[0]->id;
  CHECK(d.GetFormulaA1(s, 2, 1) == "=A3*10");
  CHECK(d.GetValue(s, 2, 1).AsDouble() == 30);
  CHECK(d.GetValue(s, 2, 2).str() == "hi");
}

TEST_CASE("xlsx: dynamic arrays come back as arrays, not as values that block them") {
  yctest::Doc t;
  t.Set("A1", "3");
  t.Set("A2", "1");
  t.Set("A3", "2");
  t.Set("C1", "=SEQUENCE(2,2)");
  t.Set("F1", "=SORT(A1:A3)");
  t.Set("H1", "=TRUE");
  REQUIRE(t.N("D2") == 4);
  const std::string bytes = io::ExportXlsxBytes(t.d.wb());
  CHECK(bytes.find("xl/metadata.xml") != std::string::npos);  // Excel reads cm="1" as a dynamic array through it
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(bytes, &wb, &err, nullptr));
  yc::engine::Document d(std::move(wb));
  d.RecalcAll();
  d.Recalc();
  const uint32_t a = d.wb().sheets[0]->id;
  CHECK(d.GetFormulaA1(a, 0, 2) == "=SEQUENCE(2,2)");
  CHECK(d.GetValue(a, 0, 2).AsDouble() == 1);  // not #SPILL!
  CHECK(d.GetValue(a, 1, 3).AsDouble() == 4);
  CHECK(d.GetFormulaA1(a, 1, 3).empty());      // a spilled value, not a typed one
  CHECK(d.GetValue(a, 0, 5).AsDouble() == 1);
  CHECK(d.GetValue(a, 2, 5).AsDouble() == 3);
  CHECK(d.GetValue(a, 0, 7).boolean());
  // the members are free again when the array shrinks
  d.SetInput(a, 0, 2, "=SEQUENCE(1,1)");
  d.Recalc();
  CHECK(d.GetValue(a, 1, 3).IsEmpty());
}

TEST_CASE("xlsx: an array formula of one cell (Ctrl+Shift+Enter) comes back as one, not as a spill") {
  // doc_fuzz --xlsx seed 70021: {=A1:B3*2} in F1 opened as a dynamic array and filled F1:G3
  yctest::Doc t;
  const uint32_t sh = t.d.wb().sheets[0]->id;
  t.Set("A1", "5");
  REQUIRE(t.d.SetArrayFormula(sh, yc::core::Rect{0, 5, 0, 5}, "A1:B3*2"));
  t.d.Recalc();
  REQUIRE(t.N("F1") == 10);
  const std::string bytes = io::ExportXlsxBytes(t.d.wb());
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(bytes, &wb, &err, nullptr));
  yc::engine::Document d(std::move(wb));
  d.RecalcAll();
  d.Recalc();
  const uint32_t a = d.wb().sheets[0]->id;
  CHECK(d.GetValue(a, 0, 5).AsDouble() == 10);
  CHECK(d.GetValue(a, 0, 6).IsEmpty());  // G1: nothing spilled
  CHECK(d.GetValue(a, 2, 5).IsEmpty());  // F3
  uint32_t ar = 0, ac = 0;
  const yc::model::SpillRange* sp = d.wb().sheets[0]->SpillAt(0, 5, &ar, &ac);
  REQUIRE(sp != nullptr);
  CHECK(sp->cse);
  CHECK(sp->rect == yc::core::Rect{0, 5, 0, 5});
}

TEST_CASE("xlsx: of two dynamic arrays that meet, the one that held the cells still holds them") {
  // doc_fuzz --more seed 110480: =TRANSPOSE(A1:B2) in B3 held B3:C4 and =SEQUENCE(3) in C2 showed #SPILL!; opened
  // from the Excel copy, C2 was calculated first (reading order) and spilled, and B3 showed #SPILL!
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("B1", "52");
  t.Set("B3", "=TRANSPOSE(A1:B2)");
  t.Set("C2", "=SEQUENCE(3)");
  REQUIRE(t.T("C2") == "#SPILL!");
  REQUIRE(t.T("B4") == "52");
  const std::string bytes = io::ExportXlsxBytes(t.d.wb());
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(bytes, &wb, &err, nullptr));
  CHECK(wb.sheets[0]->spills->size() == 1);  // B3:C4, as the file says
  yc::engine::Document d(std::move(wb));
  const uint32_t a = d.wb().sheets[0]->id;
  std::string all;
  for (const auto& [k, sp] : *d.wb().sheets[0]->spills) all += yc::core::RectA1(sp.rect) + ";";
  CHECK(all == "B3:C4;");
  CHECK(d.GetValue(a, 1, 2).IsError());          // C2: #SPILL!
  CHECK(d.GetValue(a, 3, 1).AsDouble() == 52);    // B4: B3's spill
  CHECK(d.GetFormulaA1(a, 3, 1).empty());
}

TEST_CASE("xlsx: a sheet-level name keeps hiding the workbook name of the same name on its sheet") {
  // doc_fuzz --more --xlsx seed 22032116: a copied sheet's twin of Rate (its own =Copy!$B$2) lost to the workbook's
  // Rate (three cells) once the Excel copy was opened, and =Rate*2 there spilled
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  const uint32_t s2 = t.d.AddSheet("Sheet1 (2)");
  yc::model::NameDef g;
  g.name = "Rate";
  g.refers_to = "=Sheet1!B11:C13";  // relative, as names often are
  t.d.DefineName(g);
  yc::model::NameDef l = g;
  l.sheet_scope = static_cast<int32_t>(s2);
  l.refers_to = "='Sheet1 (2)'!$B$2";
  t.d.DefineName(l);
  REQUIRE(t.d.SetFormula(s2, 1, 0, "Rate*2"));
  REQUIRE(t.d.SetFormula(s1, 1, 0, "Rate*2"));
  t.d.Recalc();
  REQUIRE(t.d.GetValue(s2, 1, 0).AsDouble() == 0);  // its own B2, empty
  REQUIRE(t.d.GetValue(s2, 2, 0).IsEmpty());
  const std::string bytes = io::ExportXlsxBytes(t.d.wb());
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(bytes, &wb, &err, nullptr));
  yc::engine::Document d(std::move(wb));
  const uint32_t b = d.wb().sheets[1]->id;
  CHECK(d.GetValue(b, 1, 0).AsDouble() == 0);
  CHECK(d.GetValue(b, 2, 0).IsEmpty());  // no spill
  CHECK(d.GetValue(d.wb().sheets[0]->id, 3, 0).AsDouble() == 0);  // the workbook's Rate spills on Sheet1
}

TEST_CASE("xlsx: an automatic font color comes back automatic, not as the theme's Text 1") {
  // doc_fuzz --xlsx seed 76000: written <color theme="1"/>, it opened as a theme color of its own
  yctest::Doc t;
  const uint32_t sh = t.d.wb().sheets[0]->id;
  t.Set("A1", "x");
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(0, 0), [](yc::model::Style& st) { st.bold = true; });
  const std::string bytes = io::ExportXlsxBytes(t.d.wb());
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(bytes, &wb, &err, nullptr));
  const yc::model::Style& st = yc::engine::StyleOf(wb, *wb.sheets[0], 0, 0);
  CHECK(st.bold);
  CHECK(st.font_color == yc::model::kColorAuto);
}

TEST_CASE("xlsx: a ZIP directory that lies about sizes or offsets is an error, not a crash") {
  const std::string zip = io::WriteZip({{"a.txt", std::string(1000, 'x')}});
  const size_t cd = zip.find("PK\x01\x02");
  REQUIRE(cd != std::string::npos);
  std::vector<io::ZipEntry> out;
  std::string err;
  const auto lang = yc::i18n::Current();
  yc::i18n::SetCurrent(yc::i18n::Lang::En);  // the message is checked in English (the machine may show Chinese)
  {  // an entry claiming 4 GB uncompressed: refused before any allocation
    std::string lie = zip;
    lie[cd + 24] = lie[cd + 25] = lie[cd + 26] = lie[cd + 27] = '\xFF';
    CHECK_FALSE(io::ReadZip(lie, &out, &err));
    CHECK(err.find("too large") != std::string::npos);
  }
  {  // a local header offset near 4 GB (32-bit arithmetic wrapped past the check and read outside the buffer)
    std::string lie = zip;
    lie[cd + 42] = '\xF0';
    lie[cd + 43] = lie[cd + 44] = lie[cd + 45] = '\xFF';
    CHECK_FALSE(io::ReadZip(lie, &out, &err));
  }
  REQUIRE(io::ReadZip(zip, &out, &err));  // the honest file still opens
  CHECK(out[0].data.size() == 1000);
  yc::i18n::SetCurrent(lang);
}

TEST_CASE("xlsx: what an Excel workbook holds that isn't carried over is counted, kind by kind, so the person is told") {
  const auto lang = yc::i18n::Current();
  yc::i18n::SetCurrent(yc::i18n::Lang::En);
  const std::string ns = "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                         "xmlns:x14=\"http://schemas.microsoft.com/office/spreadsheetml/2009/9/main\"";
  const std::string xdr = "xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\" "
                          "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                          "xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
                          "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
                          "xmlns:sle=\"http://schemas.microsoft.com/office/drawing/2010/slicer\"";
  const std::string chart_frame = "<xdr:twoCellAnchor><xdr:graphicFrame><a:graphic><a:graphicData><c:chart r:id=\"rId1\"/></a:graphicData></a:graphic></xdr:graphicFrame></xdr:twoCellAnchor>";
  std::vector<io::ZipEntry> files = {
      {"xl/workbook.xml", "<workbook " + ns + "><sheets><sheet name=\"Data\" sheetId=\"1\" r:id=\"rId1\"/><sheet name=\"Chart1\" sheetId=\"2\" r:id=\"rId2\"/></sheets></workbook>"},
      {"xl/_rels/workbook.xml.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
       "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chartsheet\" Target=\"chartsheets/sheet1.xml\"/>"
       "</Relationships>"},
      {"xl/worksheets/sheet1.xml",
       "<worksheet " + ns + ">"
       "<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" state=\"frozen\"/></sheetView></sheetViews>"
       "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>"
       "<sheetProtection sheet=\"1\"/>"
       "<conditionalFormatting sqref=\"A1:A9\"><cfRule type=\"cellIs\" priority=\"1\" operator=\"greaterThan\"><formula>5</formula></cfRule>"
       "<cfRule type=\"dataBar\" priority=\"2\"><dataBar/></cfRule></conditionalFormatting>"
       "<dataValidations count=\"1\"><dataValidation type=\"whole\" sqref=\"B1\"><formula1>1</formula1><formula2>9</formula2></dataValidation></dataValidations>"
       "<hyperlinks><hyperlink ref=\"C1\" r:id=\"rId9\"/></hyperlinks>"
       "<extLst><ext uri=\"{78C0D931-6437-407d-A8EE-F0AAD7539E65}\"><x14:conditionalFormattings><x14:conditionalFormatting>"
       "<x14:cfRule type=\"dataBar\" id=\"{1}\"/><x14:cfRule type=\"iconSet\" priority=\"3\"/>"  // the data bar is the plain rule's twin: not a rule of its own
       "</x14:conditionalFormatting></x14:conditionalFormattings></ext>"
       "<ext uri=\"{05C60535-1F16-4fd2-B633-F4F36F0B64E0}\"><x14:sparklineGroups><x14:sparklineGroup><x14:sparklines><x14:sparkline/><x14:sparkline/></x14:sparklines></x14:sparklineGroup></x14:sparklineGroups></ext></extLst>"
       "</worksheet>"},
      {"xl/chartsheets/sheet1.xml", "<chartsheet " + ns + "><drawing r:id=\"rId1\"/></chartsheet>"},
      {"xl/drawings/drawing1.xml",
       "<xdr:wsDr " + xdr + ">" + chart_frame +
       "<xdr:twoCellAnchor><xdr:pic><xdr:blipFill/></xdr:pic></xdr:twoCellAnchor>"
       "<xdr:twoCellAnchor><xdr:sp><xdr:spPr/></xdr:sp></xdr:twoCellAnchor>"
       "<xdr:twoCellAnchor><xdr:cxnSp/></xdr:twoCellAnchor>"
       // a form control: a shape with a macro attribute (its ctrlProps part is what counts it)
       "<mc:AlternateContent><mc:Choice Requires=\"x14\"><xdr:twoCellAnchor><xdr:sp macro=\"\" textlink=\"\"><xdr:spPr/></xdr:sp></xdr:twoCellAnchor></mc:Choice><mc:Fallback/></mc:AlternateContent>"
       // a slicer: counted from its own part, and its Fallback shape is a copy for older programs
       "<mc:AlternateContent><mc:Choice Requires=\"sle15\"><xdr:twoCellAnchor><xdr:graphicFrame><a:graphic><a:graphicData><sle:slicer name=\"S\"/></a:graphicData></a:graphic></xdr:graphicFrame></xdr:twoCellAnchor></mc:Choice>"
       "<mc:Fallback><xdr:twoCellAnchor><xdr:sp><xdr:spPr/></xdr:sp></xdr:twoCellAnchor></mc:Fallback></mc:AlternateContent>"
       "</xdr:wsDr>"},
      {"xl/drawings/drawing2.xml", "<xdr:wsDr " + xdr + ">" + chart_frame + "</xdr:wsDr>"},  // the chart sheet's chart
      {"xl/pivotTables/pivotTable1.xml", "<pivotTableDefinition/>"},
      {"xl/tables/table1.xml", "<table/>"},
      {"xl/slicers/slicer1.xml", "<slicers><slicer name=\"a\"/><slicer name=\"b\"/></slicers>"},
      {"xl/comments1.xml", "<comments><commentList><comment ref=\"A1\"/><comment ref=\"A2\"/></commentList></comments>"},
      {"xl/threadedComments/threadedComment1.xml", "<ThreadedComments><threadedComment id=\"{a}\"/><threadedComment id=\"{b}\" parentId=\"{a}\"/></ThreadedComments>"},
      {"xl/ctrlProps/ctrlProp1.xml", "<formControlPr/>"},
      {"xl/vbaProject.bin", std::string(64, '\0')},
      {"xl/externalLinks/externalLink1.xml", "<externalLink/>"},
      {"xl/connections.xml", "<connections><connection id=\"1\" name=\"Query - prices\"/></connections>"},
  };
  yc::model::Workbook wb;
  std::string err;
  std::vector<std::string> warnings;
  io::XlsxSkipped k;
  REQUIRE(io::ImportXlsxBytes(io::WriteZip(files), &wb, &err, &warnings, &k));
  CHECK(wb.sheets.size() == 2);
  CHECK(wb.sheets[0]->GetValue(0, 0).number() == 1);  // the cells came along
  CHECK(k.charts == 2);  // the sheet's and the chart sheet's
  CHECK(k.pictures == 1);
  CHECK(k.shapes == 2);  // the shape and the connector: not the control, the slicer or the slicer's fallback
  CHECK(k.controls == 1);
  CHECK(k.pivot_tables == 1);
  CHECK(k.slicers == 2);
  CHECK(k.tables == 1);
  CHECK(k.cond_formats == 3);  // two plain rules and the icon set of the extension
  CHECK(k.validations == 1);
  CHECK(k.comments == 2);  // the threaded one has a legacy twin: not counted twice
  CHECK(k.hyperlinks == 1);
  CHECK(k.sparklines == 1);  // one group of two sparklines
  CHECK(k.frozen_sheets == 1);
  CHECK(k.protected_sheets == 1);
  CHECK(k.external_links == 1);
  CHECK(k.queries == 1);
  CHECK(k.macros);
  CHECK(k.Any());
  const std::vector<std::string> lines = k.Lines();
  CHECK(lines.size() == 17);
  CHECK(lines[0] == "Charts: 2");
  CHECK(std::find(lines.begin(), lines.end(), "Conditional formatting rules: 3") != lines.end());
  CHECK(std::find(lines.begin(), lines.end(), "Comments and notes: 2") != lines.end());
  CHECK(lines.back() == "Excel macros (YCGrid runs Python instead)");
  yc::i18n::SetCurrent(lang);
}

TEST_CASE("xlsx: what a workbook holds that a saved copy leaves out is counted, and the copy itself has nothing to skip") {
  const auto lang = yc::i18n::Current();
  yc::i18n::SetCurrent(yc::i18n::Lang::En);
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  t.Set("A1", "1");
  t.Set("A2", "2");
  CHECK_FALSE(io::XlsxExportSkipped(t.d.wb()).Any());  // cells, formulas and formats all go into the copy
  t.d.AddChart(s1, yc::model::ChartObj{});
  yc::model::ShapeObj pic;
  pic.kind = yc::model::ShapeKind::Picture;
  t.d.AddShape(s1, pic);
  t.d.AddShape(s1, yc::model::ShapeObj{});  // a rectangle
  t.d.SetNote(s1, 0, 0, "me", "a note");
  yc::model::Hyperlink h;
  h.address = "https://example.com";
  t.d.SetHyperlink(s1, 1, 0, h);
  yc::model::CfRule r;
  r.ranges = {Rect{0, 0, 1, 0}};
  r.type = yc::model::CfType::Expression;
  r.formula1 = "A1>0";
  t.d.AddCondFormat(s1, r);
  yc::model::DvRule v;
  v.ranges = {Rect::Cell(0, 1)};
  v.type = yc::model::DvType::WholeNumber;
  v.formula1 = "1";
  v.formula2 = "9";
  t.d.SetValidation(s1, Rect::Cell(0, 1), v);
  t.d.wb_mut().MutSheetById(s1)->view.freeze_row = 1;
  const io::XlsxSkipped k = io::XlsxExportSkipped(t.d.wb());
  CHECK(k.charts == 1);
  CHECK(k.pictures == 1);
  CHECK(k.shapes == 1);
  CHECK(k.comments == 1);
  CHECK(k.hyperlinks == 1);
  CHECK(k.cond_formats == 1);
  CHECK(k.validations == 1);
  CHECK(k.frozen_sheets == 1);
  CHECK(k.Lines().size() == 8);
  CHECK(k.Lines()[0] == "Charts: 1");
  // the copy opened again: what it holds is what YCGrid reads, so nothing is reported skipped
  yc::model::Workbook wb;
  std::string err;
  io::XlsxSkipped again;
  REQUIRE(io::ImportXlsxBytes(io::ExportXlsxBytes(t.d.wb()), &wb, &err, nullptr, &again));
  CHECK_FALSE(again.Any());
  CHECK(wb.sheets[0]->GetValue(0, 0).number() == 1);
  yc::i18n::SetCurrent(lang);
}
