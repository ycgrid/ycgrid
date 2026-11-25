// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "core/decimal.h"
#include "formula/numfmt.h"

TEST_CASE("運算子與優先序") {
  yctest::Doc t;
  CHECK(t.Eval("1+2*3") == "7");
  CHECK(t.Eval("-2^2") == "4");
  CHECK(t.Eval("2^3^2") == "64");
  CHECK(t.Eval("10/4") == "2.5");
  CHECK(t.Eval("1/0") == "#DIV/0!");
  CHECK(t.Eval("\"a\"&1&TRUE") == "a1TRUE");
  CHECK(t.Eval("1=1.0") == "TRUE");
  CHECK(t.Eval("\"A\"=\"a\"") == "TRUE");
  CHECK(t.Eval("\"10\"+1") == "11");
  CHECK(t.Eval("\"x\"+1") == "#VALUE!");
  CHECK(t.Eval("50%*2") == "1");
  CHECK(t.Eval("0.1+0.2=0.3") == "TRUE");
}

TEST_CASE("相依重算：連鎖與範圍") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "=A1+1");
  t.Set("A3", "=A2*10");
  t.Set("B1", "=SUM(A:A)");
  CHECK(t.N("A3") == 20);
  CHECK(t.N("B1") == 23);
  t.Set("A1", "5");
  CHECK(t.N("A2") == 6);
  CHECK(t.N("A3") == 60);
  CHECK(t.N("B1") == 71);
  // a long chain built in reverse order (triggers the deferral)
  for (uint32_t i = 200; i >= 2; --i) t.d.SetInput(t.sheet(), i - 1, 2, "=C" + std::to_string(i - 1) + "+1");
  t.Set("C1", "1");
  CHECK(t.N("C200") == 200);
}

TEST_CASE("循環參照不會卡住") {
  yctest::Doc t;
  t.Set("A1", "=B1+1");
  t.Set("B1", "=A1+1");
  CHECK(!t.d.circular_cells.empty());
}

TEST_CASE("A circular reference stays reported until it is broken") {
  yctest::Doc t;
  t.Set("A1", "=B1+1");
  t.Set("B1", "=A1+1");
  REQUIRE(!t.d.circular_cells.empty());
  t.Set("D5", "7");  // an edit elsewhere recalculates without the cycle: it still exists
  CHECK(!t.d.circular_cells.empty());
  t.d.InsertRows(t.sheet(), 0, 2);  // the cycle moves to A3:B3 and is reported there
  t.d.Recalc();
  REQUIRE(!t.d.circular_cells.empty());
  CHECK(yc::engine::KeyRow(t.d.circular_cells.front()) == 2);
  t.Set("D9", "1");
  CHECK(!t.d.circular_cells.empty());
  t.Set("B3", "5");  // broken
  CHECK(t.d.circular_cells.empty());
  t.Set("A3", "=B3+1");
  t.Set("B3", "=A3+1");
  REQUIRE(!t.d.circular_cells.empty());
  t.Set("A3", "");  // cleared: nothing circular left
  CHECK(t.d.circular_cells.empty());
}

TEST_CASE("數學與統計函數") {
  yctest::Doc t;
  for (uint32_t i = 1; i <= 10; ++i) t.d.SetInput(t.sheet(), i - 1, 0, std::to_string(i));
  t.d.Recalc();
  CHECK(t.Eval("SUM(A1:A10)") == "55");
  CHECK(t.Eval("AVERAGE(A1:A10)") == "5.5");
  CHECK(t.Eval("COUNT(A:A)") == "10");
  CHECK(t.Eval("MAX(A1:A10)-MIN(A1:A10)") == "9");
  CHECK(t.Eval("MEDIAN(A1:A10)") == "5.5");
  CHECK(t.Eval("SUMIF(A1:A10,\">5\")") == "40");
  CHECK(t.Eval("COUNTIFS(A1:A10,\">=3\",A1:A10,\"<=4\")") == "2");
  CHECK(t.Eval("SUMPRODUCT(A1:A3,A1:A3)") == "14");
  CHECK(t.Eval("ROUND(2.345,2)") == "2.35");
  CHECK(t.Eval("ROUND(-2.5,0)") == "-3");
  CHECK(t.Eval("MOD(-3,2)") == "1");
  CHECK(t.Eval("LARGE(A1:A10,2)") == "9");
  CHECK(t.Eval("RANK(3,A1:A10)") == "8");
  CHECK(t.Eval("ROUND(STDEV(A1:A10),5)") == "3.02765");
  CHECK(t.Eval("SUBTOTAL(9,A1:A10)") == "55");
}

TEST_CASE("邏輯、查閱、文字、日期") {
  yctest::Doc t;
  t.Set("A1", "蘋果");
  t.Set("B1", "10");
  t.Set("A2", "香蕉");
  t.Set("B2", "20");
  t.Set("A3", "櫻桃");
  t.Set("B3", "30");
  CHECK(t.Eval("VLOOKUP(\"香蕉\",A1:B3,2,FALSE)") == "20");
  CHECK(t.Eval("XLOOKUP(\"櫻桃\",A1:A3,B1:B3)") == "30");
  CHECK(t.Eval("XLOOKUP(\"梨\",A1:A3,B1:B3,\"無\")") == "無");
  CHECK(t.Eval("INDEX(B1:B3,MATCH(\"蘋果\",A1:A3,0))") == "10");
  CHECK(t.Eval("IFERROR(1/0,\"x\")") == "x");
  CHECK(t.Eval("IF(B1>15,\"大\",\"小\")") == "小");
  CHECK(t.Eval("SUM(OFFSET(B1,1,0,2,1))") == "50");
  CHECK(t.Eval("INDIRECT(\"B\"&3)") == "30");
  CHECK(t.Eval("LEN(\"中文abc\")") == "5");
  CHECK(t.Eval("MID(\"YCGrid 試算表\",8,3)") == "試算表");
  CHECK(t.Eval("TEXTJOIN(\",\",TRUE,A1:A3)") == "蘋果,香蕉,櫻桃");
  CHECK(t.Eval("SUBSTITUTE(\"a-b-c\",\"-\",\"+\",2)") == "a-b+c");
  CHECK(t.Eval("TEXT(1234.5,\"#,##0.00\")") == "1,234.50");
  CHECK(t.Eval("TEXT(DATE(2026,10,23),\"yyyy-mm-dd\")") == "2026-10-23");
  CHECK(t.Eval("DATE(2026,13,1)=DATE(2027,1,1)") == "TRUE");
  CHECK(t.Eval("EOMONTH(DATE(2024,1,31),1)=DATE(2024,2,29)") == "TRUE");
  // Excel's 1900/2/29 (serial 60)
  CHECK(t.Eval("DATE(1900,2,28)") == "59");
  CHECK(t.Eval("DATE(1900,2,29)") == "60");
  CHECK(t.Eval("DATE(1900,1,60)") == "60");
  CHECK(t.Eval("DATE(1900,3,0)") == "60");
  CHECK(t.Eval("DATE(1900,3,1)") == "61");
  CHECK(t.Eval("DATEVALUE(\"1900/2/29\")") == "60");
  CHECK(t.Eval("DATEVALUE(\"1900/2/30\")") == "#VALUE!");
  CHECK(t.Eval("DATEDIF(DATE(2020,1,15),DATE(2026,10,23),\"Y\")") == "6");
  CHECK(t.Eval("NETWORKDAYS(DATE(2026,10,19),DATE(2026,10,23))") == "5");
  CHECK(t.Eval("WEEKDAY(DATE(2026,10,23))") == "6");
  CHECK(t.Eval("ISNUMBER(B1)") == "TRUE");
  CHECK(t.Eval("ISBLANK(C1)") == "TRUE");
  CHECK(t.Eval("ISNA(MATCH(\"x\",A1:A3,0))") == "TRUE");
}

TEST_CASE("Decimal 與奈秒時間戳函數") {
  yctest::Doc t;
  t.Set("A1", "1729658400.123456789");  // more than 15 significant digits -> Decimal
  REQUIRE(t.V("A1").IsDecimal());
  t.Set("A2", "1729658400.000000001");
  t.Set("A3", "=A1-A2");
  REQUIRE(t.V("A3").IsDecimal());
  CHECK(t.F("A3") == "=A1-A2");
  CHECK(yc::core::DecToString(yc::core::DecFromValue(t.V("A3"))) == "0.123456788");
  CHECK(t.Eval("TS_PART(A1,\"ns\")") == "123456789");
  CHECK(t.Eval("TS_PART(A1,\"ms\")") == "123");
  CHECK(t.Eval("TEXT(DECIMAL(\"1729658400.123456789\"),\"[epoch][utc]yyyy-mm-dd hh:mm:ss.000000000\")") == "2024-10-23 04:40:00.123456789");
  CHECK(t.Eval("SUM(A1,A2)=DECIMAL(\"3459316800.12345679\")") == "TRUE");
}

#include "formula/parser.h"
#include "io/ycg.h"

TEST_CASE("DECIMAL(text, radix) is Excel's; DECIMAL.SCALE keeps the exact decimal with places") {
  yctest::Doc t;
  CHECK(t.Eval("DECIMAL(\"FF\",16)") == "255");
  CHECK(t.Eval("DECIMAL(\"zap\",36)") == "45745");
  CHECK(t.Eval("DECIMAL(\"111\",2)") == "7");
  CHECK(t.Eval("DECIMAL(\"2\",2)") == "#NUM!");
  CHECK(t.Eval("DECIMAL(\"1\",37)") == "#NUM!");
  CHECK(t.Eval("ISDECIMAL(DECIMAL(\"1.5\"))") == "TRUE");
  CHECK(t.Eval("ISDECIMAL(DECIMAL.SCALE(\"1.23456\",2))") == "TRUE");
  CHECK(t.Eval("DECIMAL.SCALE(\"1.23456\",2)=DECIMAL(\"1.23\")") == "TRUE");
}

TEST_CASE("Files from before Excel's DECIMAL read DECIMAL(x, places) as DECIMAL.SCALE") {
  struct Old : yc::formula::SheetResolver {
    int32_t FindSheet(std::string_view) const override { return -1; }
    std::string SheetName(int32_t) const override { return {}; }
    bool LegacyDecimal() const override { return true; }
  } old;
  auto pr = yc::formula::Parse("DECIMAL(\"1.5\",2)", yc::core::CellPos{0, 0}, &old);
  REQUIRE(pr.ok);
  CHECK(pr.root.str == "DECIMAL.SCALE");
  CHECK(yc::formula::ToA1(pr.root, yc::core::CellPos{0, 0}, &old) == "DECIMAL.SCALE(\"1.5\",2)");
  // one argument stays DECIMAL; new files save the dialect and keep Excel's meaning
  pr = yc::formula::Parse("DECIMAL(\"1.5\")", yc::core::CellPos{0, 0}, &old);
  CHECK(pr.root.str == "DECIMAL");
  yctest::Doc t;
  REQUIRE(t.Set("A1", "=DECIMAL(\"FF\",16)"));
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  CHECK_FALSE(res.wb.legacy_decimal);
  yc::engine::Document d2(std::move(res.wb));
  d2.Recalc();
  CHECK(d2.GetValue(d2.wb().sheets[0]->id, 0, 0).AsDouble() == 255);
}

TEST_CASE("Union references: (A1:B2,D4) in SUM, COUNT, AREAS and INDEX") {
  yctest::Doc t;
  REQUIRE(t.Set("A1", "1"));
  REQUIRE(t.Set("B1", "2"));
  REQUIRE(t.Set("A2", "3"));
  REQUIRE(t.Set("B2", "4"));
  REQUIRE(t.Set("D4", "10"));
  CHECK(t.Eval("SUM((A1:B2,D4))") == "20");
  CHECK(t.Eval("SUM((A1,B1),(A2,D4))") == "16");
  CHECK(t.Eval("COUNT((A1:B2,D4,E5))") == "5");
  CHECK(t.Eval("MAX((A1,D4))") == "10");
  CHECK(t.Eval("AREAS((A1:B2,D4,E5))") == "3");
  CHECK(t.Eval("INDEX((A1:B2,D4),2,2,1)") == "4");
  CHECK(t.Eval("INDEX((A1:B2,D4),1,1,2)") == "10");
  CHECK(t.Eval("INDEX((A1:B2,D4),1,1,3)") == "#REF!");
  REQUIRE(t.Set("C1", "=SUM((A1:B2,D4))"));
  CHECK(t.F("C1") == "=SUM((A1:B2,D4))");
  REQUIRE(t.Set("D4", "20"));  // the union's cells are dependencies
  CHECK(t.N("C1") == 30);
}

TEST_CASE("DETECTLANGUAGE works offline; a date function typed into a General cell shows a date") {
  yctest::Doc t;
  CHECK(t.Eval("DETECTLANGUAGE(\"The quick brown fox jumps over the lazy dog\")") == "en");
  CHECK(t.Eval("DETECTLANGUAGE(\"\xE4\xBB\x8A\xE5\xA4\xA9\xE5\xA4\xA9\xE6\xB0\xA3\xE5\xBE\x88\xE5\xA5\xBD\xEF\xBC\x8C\xE6\x88\x91\xE5\x80\x91\")") == "zh-Hant");
  CHECK(t.Eval("DETECTLANGUAGE(\"\xE6\x88\x91\xE4\xBB\xAC\xE8\xBF\x99\xE4\xB8\xAA\")") == "zh-Hans");
  CHECK(t.Eval("DETECTLANGUAGE(\"\xE3\x81\x93\xE3\x82\x8C\xE3\x81\xAF\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\")") == "ja");
  CHECK(t.Eval("DETECTLANGUAGE(\"Der Hund ist nicht mein Freund\")") == "de");
  CHECK(t.Eval("DETECTLANGUAGE(\"El perro es muy grande\")") == "es");
  CHECK(t.Eval("DETECTLANGUAGE(\"123\")") == "#VALUE!");
  // DATE() typed by the user: the cell gets a date format (a value typed in a formatted cell keeps its format)
  REQUIRE(t.d.SetFormula(t.sheet(), 0, 0, "DATE(2024,1,5)", nullptr, yc::engine::kSrcUser));
  t.d.Recalc();
  const auto& st = t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(0, 0));
  CHECK(yc::formula::IsDateFormat(t.d.wb().styles->NumberFormat(st)));
  REQUIRE(t.d.SetFormula(t.sheet(), 1, 0, "SUM(1,2)", nullptr, yc::engine::kSrcUser));
  CHECK(t.d.S(t.sheet())->GetStyle(1, 0) == 0);
}

TEST_CASE("REPT and wildcard SEARCH finish at once on degenerate input") {
  yctest::Doc t;
  t.Set("A1", "=REPT(\"\", 1E15)");
  CHECK_FALSE(t.V("A1").IsError());  // an empty text, like Excel
  CHECK(t.T("A1").empty());
  t.Set("A2", "=REPT(\"ab\", 1E15)");
  CHECK(t.V("A2").IsError());
  t.Set("A3", "=REPT(\"ab\", 3)");
  CHECK(t.V("A3").str() == "ababab");
  std::string big(30000, 'x');
  t.Set("B1", big);
  t.Set("B2", "=SEARCH(\"x?z\", B1)");  // no match: every start tried once, not every (start, end) pair
  CHECK(t.V("B2").IsError());
  t.Set("B3", "=SEARCH(\"a*c\", \"xxabcxx\")");
  CHECK(t.N("B3") == 3);
  t.Set("B4", "=SEARCH(\"b~*\", \"ab*c\")");  // ~* is a literal *
  CHECK(t.N("B4") == 2);
  t.Set("B5", "=SEARCH(\"c~\", \"abc~d\")");  // a trailing ~ is itself
  CHECK(t.N("B5") == 3);
}

TEST_CASE("INDEX with a row or column number past any range is #REF!, not a wrapped index") {
  yctest::Doc t;
  for (int i = 0; i < 10; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string(i + 1));
  t.Set("C1", "=INDEX(A1:A10, 4294967297)");   // 2^32 + 1 used to become row 1
  CHECK(t.V("C1").IsError());
  t.Set("C2", "=INDEX(A1:A10, 1E20)");
  CHECK(t.V("C2").IsError());
  t.Set("C3", "=INDEX(A1:A10, 3)");
  CHECK(t.N("C3") == 3);
}

TEST_CASE("Undoing a structural change reports a circular reference where it is again") {
  yctest::Doc t;
  t.Set("A1", "=B1+1");
  t.Set("B1", "=A1+1");
  REQUIRE(!t.d.circular_cells.empty());
  t.d.Begin("insert");
  t.d.InsertRows(t.sheet(), 0, 2);
  t.d.Commit();
  t.d.Recalc();
  REQUIRE(!t.d.circular_cells.empty());
  CHECK(yc::engine::KeyRow(t.d.circular_cells.front()) == 2);
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  REQUIRE(!t.d.circular_cells.empty());
  CHECK(yc::engine::KeyRow(t.d.circular_cells.front()) == 0);
  for (uint64_t k : t.d.circular_cells) CHECK(yc::engine::KeyRow(k) == 0);
}

TEST_CASE("The intersection operator: a space between two references") {
  yctest::Doc t;
  for (uint32_t r = 0; r < 4; ++r) {
    for (uint32_t c = 0; c < 4; ++c) t.d.SetInput(t.sheet(), r, c, std::to_string(r * 10 + c));
  }
  t.Set("F1", "=SUM(A1:C3 B2:D4)");  // B2:C3 = 11+12+21+22
  CHECK(t.N("F1") == 66);
  CHECK(t.d.GetFormulaA1(t.sheet(), 0, 5) == "=SUM(A1:C3 B2:D4)");
  t.Set("F2", "=A1:A4 A2:D2");       // one cell: A2
  CHECK(t.N("F2") == 10);
  t.Set("F3", "=SUM(A1:A2 C3:D4)");  // nothing in common
  CHECK(yc::engine::DisplayTextOf(t.V("F3"), "General") == "#NULL!");
  t.Set("F4", "=SUM( A1:B2 , C1 )");  // spaces around arguments are only spaces
  CHECK(t.N("F4") == 1 + 10 + 11 + 2);
  t.Set("F5", "=SUM (A1:A2)");        // a space before the parenthesis: still a call
  CHECK(t.N("F5") == 10);
  t.Set("F6", "=A2 + B2");
  CHECK(t.N("F6") == 21);
  REQUIRE(t.d.AddName("SecondRow", "Sheet1!$A$2:$D$2"));
  REQUIRE(t.d.AddName("ThirdCol", "Sheet1!$C:$C"));
  t.Set("F7", "=SecondRow ThirdCol");          // named ranges intersect, as in Excel
  CHECK(t.N("F7") == 12);
  t.Set("B2", "100");                 // the intersection's cells are read: a change there recalculates
  CHECK(t.N("F1") == 66 - 11 + 100);
}
