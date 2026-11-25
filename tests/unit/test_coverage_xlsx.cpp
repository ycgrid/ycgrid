// SPDX-License-Identifier: Apache-2.0
// Coverage of .xlsx reading and writing (io/xlsx.cpp) beyond test_xlsx.cpp: what a workbook keeps through a save and an
// open (gradient and pattern fills, double underlines, cell protection, the print area, sheet-level names, sheet names
// that need quotes, array formulas, error values), files written by hand (indexed colors with a tint, a print area,
// a chartsheet, `str` and unknown error cells), and the files that aren't a workbook.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/xlsx.h"

#include <cstdio>
#include <filesystem>
#include <memory>

namespace io = yc::io;
using yc::core::Rect;

namespace {

std::unique_ptr<yc::engine::Document> Reopen(const yc::model::Workbook& wb, std::vector<std::string>* warnings = nullptr) {
  const std::string bytes = io::ExportXlsxBytes(wb);
  yc::model::Workbook back;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(bytes, &back, &err, warnings));
  auto d = std::make_unique<yc::engine::Document>(std::move(back));
  d->RecalcAll();
  d->Recalc();
  return d;
}

const yc::model::Style& StyleAt(const yc::engine::Document& d, uint32_t sheet, uint32_t r, uint32_t c) {
  return d.wb().styles->Get(d.S(sheet)->GetStyle(r, c));
}

const char* kRels =
    R"(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
    R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>)"
    R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/chartsheet" Target="chartsheets/sheet1.xml"/>)"
    R"(<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>)"
    R"(</Relationships>)";

std::string Workbook(const std::string& sheets, const std::string& names = "") {
  return R"(<?xml version="1.0"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" )"
         R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>)" +
         sheets + "</sheets>" + (names.empty() ? std::string() : "<definedNames>" + names + "</definedNames>") + "</workbook>";
}

}  // namespace

TEST_CASE("xlsx coverage: fills, underlines, protection, print area, names and array formulas come back") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.RenameSheet(sh, "Q1 'x' data"));
  t.Set("A1", "a");
  t.d.ModifyStyle(sh, Rect::Cell(0, 0), [](yc::model::Style& s) {
    s.underline = yc::model::Underline::Double;
    s.gradient = yc::model::FillGradient::Linear;
    s.gradient_angle = 90;
    s.fill_color = yc::model::Rgb(255, 0, 0);
    s.gradient_color2 = yc::model::Rgb(0, 0, 255);
  });
  t.Set("A2", "b");
  t.d.ModifyStyle(sh, Rect::Cell(1, 0), [](yc::model::Style& s) {
    s.gradient = yc::model::FillGradient::FromCenter;
    s.fill_color = yc::model::Rgb(0, 255, 0);
  });
  t.Set("A3", "c");
  t.d.ModifyStyle(sh, Rect::Cell(2, 0), [](yc::model::Style& s) {
    s.gradient = yc::model::FillGradient::FromCorner;
    s.gradient_angle = 3;
    s.fill_color = yc::model::Rgb(1, 2, 3);
    s.gradient_color2 = yc::model::Rgb(4, 5, 6);
  });
  t.Set("A4", "d");
  t.d.ModifyStyle(sh, Rect::Cell(3, 0), [](yc::model::Style& s) {
    s.pattern = yc::model::FillPattern::Checker;
    s.pattern_color = yc::model::Rgb(10, 20, 30);
    s.fill_color = yc::model::Rgb(200, 210, 220);
    s.underline = yc::model::Underline::DoubleAccounting;
    s.locked = false;
    s.hidden = true;
  });
  t.d.ModifyStyle(sh, Rect::Cell(4, 0), [](yc::model::Style& s) { s.pattern = yc::model::FillPattern::Gray125; });
  REQUIRE(t.d.UpdatePageSetup(sh, [](yc::model::PageSetup& ps) {
    ps.has_area = true;
    ps.area = Rect{0, 0, 9, 2};
  }));
  REQUIRE(t.d.AddName("Local", "='Q1 ''x'' data'!$B$1", static_cast<int32_t>(sh)));
  t.Set("B1", "5");
  t.Set("B2", "=1/0");
  t.Set("B3", "=NA()");
  REQUIRE(t.d.SetArrayFormula(sh, Rect{0, 3, 1, 3}, "B1:B2*2"));
  const auto dp = Reopen(t.d.wb());
  const auto& d = *dp;
  const uint32_t s = d.wb().sheets[0]->id;
  CHECK(d.wb().sheets[0]->name == "Q1 'x' data");
  const auto& a1 = StyleAt(d, s, 0, 0);
  CHECK(a1.underline == yc::model::Underline::Double);
  CHECK(a1.gradient == yc::model::FillGradient::Linear);
  CHECK(a1.gradient_angle == 90);
  CHECK((a1.fill_color & 0xFFFFFF) == 0xFF0000);
  CHECK((a1.gradient_color2 & 0xFFFFFF) == 0x0000FF);
  const auto& a2 = StyleAt(d, s, 1, 0);
  CHECK(a2.gradient == yc::model::FillGradient::FromCenter);
  CHECK((a2.gradient_color2 & 0xFFFFFF) == 0xFFFFFF);  // an automatic second color is written as white
  const auto& a3 = StyleAt(d, s, 2, 0);
  CHECK(a3.gradient == yc::model::FillGradient::FromCorner);
  CHECK(a3.gradient_angle == 3);
  const auto& a4 = StyleAt(d, s, 3, 0);
  CHECK(a4.pattern == yc::model::FillPattern::Checker);
  CHECK((a4.pattern_color & 0xFFFFFF) == 0x0A141E);
  CHECK((a4.fill_color & 0xFFFFFF) == 0xC8D2DC);
  CHECK(a4.underline == yc::model::Underline::DoubleAccounting);
  CHECK_FALSE(a4.locked);
  CHECK(a4.hidden);
  CHECK(StyleAt(d, s, 4, 0).pattern == yc::model::FillPattern::Gray125);
  REQUIRE(d.S(s)->page_setup->has_area);
  CHECK(d.S(s)->page_setup->area == Rect{0, 0, 9, 2});
  bool local = false;
  for (const auto& n : *d.wb().names) local = local || (n.name == "Local" && n.sheet_scope == static_cast<int32_t>(s));
  CHECK(local);
  CHECK(d.GetValue(s, 1, 1).IsError());
  CHECK(d.GetValue(s, 2, 1).IsError());
  CHECK(d.GetFormulaA1(s, 0, 3).find("B1:B2*2") != std::string::npos);
  CHECK(d.GetValue(s, 1, 3).IsError());  // the array's second cell: B2's #DIV/0! times 2
  CHECK(d.GetValue(s, 0, 3).AsDouble() == 10);
}

TEST_CASE("xlsx coverage: a file written by hand: indexed colors with a tint, a print area, a chartsheet, odd cells") {
  const std::string wbx = Workbook(R"(<sheet name="Data" sheetId="1" r:id="rId1"/><sheet name="Chart1" sheetId="2" r:id="rId2"/>)",
                                   R"(<definedName name="_xlnm.Print_Area" localSheetId="0">Data!$A$1:$C$4</definedName>)"
                                   R"(<definedName name="_xlnm.Print_Titles" localSheetId="0">Data!$1:$1</definedName>)");
  const std::string styles =
      R"(<?xml version="1.0"?><styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
      R"(<fonts count="2"><font><sz val="11"/></font><font><color indexed="10" tint="-0.5"/><sz val="11"/></font></fonts>)"
      R"(<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>)"
      R"(<fill><gradientFill type="path" left="1" right="1" top="0" bottom="0"><stop position="0"><color rgb="FF112233"/></stop>)"
      R"(<stop position="1"><color indexed="12" tint="0.5"/></stop></gradientFill></fill></fills>)"
      R"(<borders count="1"><border/></borders><cellXfs count="2"><xf fontId="0" fillId="0" borderId="0"/>)"
      R"(<xf fontId="1" fillId="2" borderId="0" applyProtection="1"><protection locked="0" hidden="1"/></xf></cellXfs></styleSheet>)";
  const std::string sheet =
      R"(<?xml version="1.0"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>)"
      R"(<row r="1"><c r="A1" s="1" t="str"><v>made by a formula</v></c><c r="B1" t="e"><v>#WHAT?</v></c><c r="C1" t="e"><v>#N/A</v></c></row>)"
      R"(<row r="2"><c r="A2"><f t="array" ref="A2:B2">{1,2}</f><v>1</v></c><c r="C2"><f>SUM(</f><v>0</v></c></row>)"
      R"(</sheetData></worksheet>)";
  const std::string zip = io::WriteZip({{"xl/workbook.xml", wbx}, {"xl/_rels/workbook.xml.rels", kRels}, {"xl/styles.xml", styles},
                                        {"xl/worksheets/sheet1.xml", sheet}});
  yc::model::Workbook wb;
  std::string err;
  std::vector<std::string> warnings;
  REQUIRE(io::ImportXlsxBytes(zip, &wb, &err, &warnings));
  bool skipped_chart = false;
  for (const auto& w : warnings) skipped_chart = skipped_chart || w.find("Chart1") != std::string::npos;
  CHECK(skipped_chart);
  yc::engine::Document d(std::move(wb));
  d.RecalcAll();
  d.Recalc();
  const uint32_t s = d.wb().sheets[0]->id;
  CHECK(d.GetValue(s, 0, 0).str() == "made by a formula");
  CHECK(d.GetValue(s, 0, 1).error() == yc::core::Err::Value);  // an error YCGrid doesn't know
  CHECK(d.GetValue(s, 0, 2).error() == yc::core::Err::NA);
  CHECK(d.GetFormulaA1(s, 1, 0).find("{1,2}") != std::string::npos);
  CHECK(d.GetValue(s, 1, 1).AsDouble() == 2);
  CHECK(d.GetValue(s, 1, 2).AsDouble() == 0);  // a formula YCGrid can't read keeps its value
  const auto& st = StyleAt(d, s, 0, 0);
  CHECK(st.gradient == yc::model::FillGradient::FromCorner);
  CHECK(st.gradient_angle == 1);  // the upper right corner
  CHECK((st.fill_color & 0xFFFFFF) == 0x112233);
  CHECK((st.gradient_color2 & 0xFFFFFF) == 0x8080FF);  // indexed 12 (blue) halfway to white
  CHECK((st.font_color & 0xFFFFFF) == 0x800000);       // indexed 10 (red) half darker
  CHECK_FALSE(st.locked);
  CHECK(st.hidden);
  REQUIRE(d.S(s)->page_setup->has_area);
  CHECK(d.S(s)->page_setup->area == Rect{0, 0, 3, 2});
  CHECK(d.wb().names->empty());  // Excel's own names are not names in YCGrid
}

TEST_CASE("xlsx coverage: files that aren't a workbook are refused with a reason") {
  yc::model::Workbook wb;
  std::string err;
  CHECK_FALSE(io::ImportXlsxBytes("plain text, not a zip", &wb, &err, nullptr));
  CHECK(err == "Not a ZIP file");
  const std::string no_wb = io::WriteZip({{"hello.txt", "hi"}});
  CHECK_FALSE(io::ImportXlsxBytes(no_wb, &wb, &err, nullptr));
  CHECK(err.find("xl/workbook.xml is missing") != std::string::npos);
  const std::string no_sheets = io::WriteZip({{"xl/workbook.xml", Workbook("")}, {"xl/_rels/workbook.xml.rels", kRels}});
  CHECK_FALSE(io::ImportXlsxBytes(no_sheets, &wb, &err, nullptr));
  CHECK(err == "The workbook has no sheets.");
  // a damaged central directory: its offset points past the file
  std::string broken = io::WriteZip({{"xl/workbook.xml", Workbook("")}});
  const size_t eocd = broken.rfind(std::string("PK\x05\x06", 4));
  REQUIRE(eocd != std::string::npos);
  broken[eocd + 16] = '\xFF';
  broken[eocd + 17] = '\xFF';
  broken[eocd + 18] = '\xFF';
  CHECK_FALSE(io::ImportXlsxBytes(broken, &wb, &err, nullptr));
  CHECK_FALSE(err.empty());
  // through a path
  CHECK(io::IsXlsxPath(L"C:\\x\\Book.XLSX"));
  CHECK(io::IsXlsxPath(L"book.xlsm"));
  CHECK_FALSE(io::IsXlsxPath(L"book.ycg"));
  CHECK_FALSE(io::ImportXlsx(L"Z:\\no\\such\\folder\\book.xlsx", &wb, &err, nullptr));
  CHECK(err == "Couldn't read the file.");
  yctest::Doc t;
  t.Set("A1", "42");
  const auto path = std::filesystem::temp_directory_path() / "yc_coverage_xlsx.xlsx";
  REQUIRE(io::ExportXlsx(t.d.wb(), path.wstring(), &err));
  yc::model::Workbook back;
  REQUIRE(io::ImportXlsx(path.wstring(), &back, &err, nullptr));
  std::filesystem::remove(path);
  yc::engine::Document d(std::move(back));
  CHECK(d.GetValue(d.wb().sheets[0]->id, 0, 0).AsDouble() == 42);
}

TEST_CASE("xlsx coverage: a formula keeps its own format, not its references', when the file is opened") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (int i = 1; i <= 5; ++i) t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
  t.Set("F6", "=SUM(A1:A5)");  // entered while A1 is General: General
  t.Set("F8", "=A1");
  auto fmt = [&](const char* code) { return t.d.wb_mut().styles.mut().InternString(code); };
  t.d.ModifyStyle(sh, Rect::Cell(0, 0), [&](yc::model::Style& s) { s.numfmt = fmt("[Red]0.0;[Blue]-0.0"); });
  t.d.ModifyStyle(sh, Rect::Cell(8, 5), [&](yc::model::Style& s) { s.numfmt = fmt("0.000"); });
  t.Set("F9", "=A1");  // a format of its own stays
  REQUIRE(StyleAt(t.d, sh, 5, 5).numfmt == 0);
  const auto d = Reopen(t.d.wb());
  const uint32_t s2 = d->wb().sheets[0]->id;
  CHECK(StyleAt(*d, s2, 5, 5).numfmt == 0);  // F6 stays General (Excel, and a .ycg file, keep it so)
  CHECK(StyleAt(*d, s2, 7, 5).numfmt == 0);
  CHECK(d->wb().styles->Str(StyleAt(*d, s2, 8, 5).numfmt) == "0.000");
  CHECK(d->wb().styles->Str(StyleAt(*d, s2, 0, 0).numfmt) == "[Red]0.0;[Blue]-0.0");
  CHECK(d->GetValue(s2, 5, 5).number() == 15);
}
