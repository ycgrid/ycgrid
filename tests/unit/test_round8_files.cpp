// SPDX-License-Identifier: Apache-2.0
// Round 8 (files): .xlsx files YCGrid writes open in Excel (LET / LAMBDA parameters, spill references, the newer
// error values, references to deleted sheets, the built-in formats 37-40), spill references read back, and CSV
// files in the system's ANSI code page (Big5) and with a "sep=" line
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/document.h"
#include "io/csv.h"
#include "io/xlsx.h"

#include <windows.h>

#include <string>

namespace io = yc::io;

namespace {

std::string SheetXml(const std::string& bytes, const char* part = "xl/worksheets/sheet1.xml") {
  std::vector<io::ZipEntry> entries;
  std::string err;
  if (!io::ReadZip(bytes, &entries, &err)) return {};
  for (const auto& e : entries) {
    if (e.name == part) return e.data;
  }
  return {};
}

}  // namespace

TEST_CASE("Round8 files: LET and LAMBDA parameters are written with _xlpm. and read back") {
  yctest::Doc t;
  t.Set("A1", "=LET(x,2,x*3)");
  t.Set("A2", "=LAMBDA(a,b,a+b)(1,2)");
  t.Set("A3", "=MAP(B1:B2,LAMBDA(v,v*2))");
  t.Set("A5", "=LET(total,SUM(B1:B2),total)");
  const std::string xml = SheetXml(io::ExportXlsxBytes(t.d.wb()));
  CHECK(xml.find("_xlfn.LET(_xlpm.x,2,_xlpm.x*3)") != std::string::npos);  // Excel refused to open the file
  CHECK(xml.find("_xlfn.LAMBDA(_xlpm.a,_xlpm.b,_xlpm.a+_xlpm.b)(1,2)") != std::string::npos);
  CHECK(xml.find("_xlfn.LAMBDA(_xlpm.v,_xlpm.v*2)") != std::string::npos);
  CHECK(xml.find("_xlfn.LET(_xlpm.total,SUM(B1:B2),_xlpm.total)") != std::string::npos);
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(io::ExportXlsxBytes(t.d.wb()), &wb, &err, nullptr));
  yc::engine::Document back(std::move(wb));
  CHECK(back.GetInputText(back.wb().sheets[0]->id, 0, 0) == "=LET(x,2,x*3)");
  CHECK(back.GetValue(back.wb().sheets[0]->id, 0, 0).AsDouble() == 6);
}

TEST_CASE("Round8 files: a spill reference is ANCHORARRAY in the file, both ways") {
  yctest::Doc t;
  t.Set("A1", "=SEQUENCE(3)");
  t.Set("B1", "=SUM(A1#)");
  t.Set("C1", "=SUM(Sheet1!A1#)");
  const std::string bytes = io::ExportXlsxBytes(t.d.wb());
  const std::string xml = SheetXml(bytes);
  CHECK(xml.find("SUM(_xlfn.ANCHORARRAY(A1))") != std::string::npos);
  CHECK(xml.find("SUM(_xlfn.ANCHORARRAY(Sheet1!A1))") != std::string::npos);
  yc::model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(bytes, &wb, &err, nullptr));
  yc::engine::Document back(std::move(wb));
  back.Recalc();
  const uint32_t s = back.wb().sheets[0]->id;
  CHECK(back.GetInputText(s, 0, 1) == "=SUM(A1#)");  // was ANCHORARRAY(A1): #NAME?
  CHECK(back.GetValue(s, 0, 1).AsDouble() == 6);
}

TEST_CASE("Round8 files: newer error values are written #VALUE!, a deleted sheet's reference #REF!") {
  yctest::Doc t;
  t.Set("A2", "x");
  t.Set("A1", "=SEQUENCE(3)");  // #SPILL!: A2 is in the way
  REQUIRE(t.V("A1").IsError());
  const uint32_t gone = t.d.AddSheet("Calc");
  t.Set("C1", "=Calc!A1+1");
  t.d.DeleteSheet(gone);
  t.d.Recalc();
  const std::string xml = SheetXml(io::ExportXlsxBytes(t.d.wb()));
  CHECK(xml.find("#SPILL!") == std::string::npos);  // Excel refused to open the file
  CHECK(xml.find("<v>#VALUE!</v>") != std::string::npos);
  CHECK(xml.find("#REF!A1") == std::string::npos);
  CHECK(xml.find(">#REF!+1</f>") != std::string::npos);
  CHECK(xml.find("<f t=\"array\" ref=\"C1\">") != std::string::npos);  // a one-cell dynamic array, as Excel 365 writes it
}

TEST_CASE("Round8 files: CSV in the system's ANSI code page and with a sep= line") {
  // Big5 bytes of the first two characters of a well-known Taiwanese company name, read where the system code page is
  // Big5 (Traditional Chinese Windows): they were replacement marks, CP_ACP being UTF-8 for YCGrid
  DWORD cp = 0;
  ::GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&cp), sizeof(cp) / sizeof(wchar_t));
  if (cp == 950) CHECK(io::DecodeText("\xA5\x78\xBF\x6E,1\r\n") == "\xE5\x8F\xB0\xE7\xA9\x8D,1\r\n");
  CHECK(io::DecodeText("caf\xE9") == (cp == 1252 ? "caf\xC3\xA9" : io::DecodeText("caf\xE9")));
  const io::CsvTable t = io::ParseCsv("sep=;\r\na;b\r\n1;2\r\n");
  REQUIRE(t.rows.size() == 2);
  CHECK(t.rows[0] == std::vector<std::string>{"a", "b"});
  CHECK(t.rows[1] == std::vector<std::string>{"1", "2"});
  const io::CsvTable u = io::ParseCsv("sep=,\na,b;c\n");
  REQUIRE(u.rows.size() == 1);
  CHECK(u.rows[0].size() == 2);
}
