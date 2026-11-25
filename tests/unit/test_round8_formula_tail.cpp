// SPDX-License-Identifier: Apache-2.0
// Round 8, the formula engine's last differences from Excel (each measured against Excel 16 itself): arrays of
// references from OFFSET / INDIRECT, a name used twice in LET, a union read as a value, approximate lookups over mixed
// types, spaces around dates and times, CELL("address") of another sheet, names that read as R1C1 references,
// fractions, negative numbers in "number;@" formats, a spill anchor's deleted row and YIELD over one coupon period
#include <catch2/catch_test_macros.hpp>

#include "core/geometry.h"
#include "engine/document.h"
#include "formula/numfmt.h"
#include "formula/parser.h"
#include "test_helpers.h"

#include <cmath>
#include <initializer_list>
#include <string>

using yc::core::Rect;

namespace {
// fills a column from the top: "" leaves the cell empty, "'5" is the text 5
void Column(yctest::Doc& t, char col, std::initializer_list<const char*> cells) {
  int row = 1;
  for (const char* c : cells) {
    if (*c) t.Set((std::string(1, col) + std::to_string(row)).c_str(), c);
    ++row;
  }
}
bool Parses(yctest::Doc& t, const std::string& text) { return yc::formula::Parse(text, yc::core::CellPos{0, 0}, &t.d).ok; }
}  // namespace

TEST_CASE("Round8 formula tail: OFFSET and INDIRECT given arrays give arrays of references") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  t.Set("B1", "x");
  // as values, one #VALUE! per element (YCGrid took the first element)
  t.Set("E1", "=OFFSET(A1,{0,1},0)");
  CHECK(t.T("E1") == "#VALUE!");
  CHECK(t.T("F1") == "#VALUE!");
  t.Set("E3", "=INDIRECT({\"A1\";\"A2\"})");
  CHECK(t.T("E3") == "#VALUE!");
  CHECK(t.T("E4") == "#VALUE!");
  CHECK(t.Eval("ROWS(OFFSET(A1,{0,1},0))") == "1");
  CHECK(t.Eval("ISREF(OFFSET(A1,{0,1},0))") == "FALSE");
  CHECK(t.Eval("COUNT(OFFSET(A1,{0,1},0))") == "0");
  CHECK(t.Eval("COUNTA(OFFSET(A1,{0,1},0))") == "2");
  CHECK(t.Eval("TEXTJOIN(\",\",,IFERROR(OFFSET(A1,{0,1},0),\"e\"))") == "e,e");
  // the number functions read each reference's first cell
  CHECK(t.Eval("SUM(INDIRECT({\"A1\",\"A2\"}))") == "3");
  CHECK(t.Eval("SUM(OFFSET(A1,{0,1},0))") == "3");
  CHECK(t.Eval("SUM(OFFSET(A1,0,0,{1,2}))") == "2");
  CHECK(t.Eval("SUM(INDIRECT({\"A1:A2\",\"A3\"}))") == "4");
  CHECK(t.Eval("SUM(OFFSET(A1,A1:A2,0))") == "5");
  CHECK(t.Eval("AVERAGE(OFFSET(A1,{0,1,2},0))") == "2");
  CHECK(t.Eval("MAX(INDIRECT({\"B1\",\"A2\"}))") == "2");
  CHECK(t.Eval("SUM(INDIRECT(\"A\"&{1,2,3}))") == "6");
  CHECK(t.Eval("SUM(OFFSET(A1,{0,1},0),10)") == "13");
  CHECK(t.Eval("SUM(INDIRECT({\"A1\",\"Nope!A1\"}))") == "#REF!");
  CHECK(t.Eval("SUM(IF(TRUE,OFFSET(A1,{0,1},0)))") == "3");
  CHECK(t.Eval("SUM(LET(r,OFFSET(A1,{0,1},0),r))") == "3");
  // the functions that take a reference take them one by one
  CHECK(t.Eval("TEXTJOIN(\",\",,N(OFFSET(A1,{0,1},0)))") == "1,2");
  CHECK(t.Eval("TEXTJOIN(\",\",,SUBTOTAL(9,OFFSET(A1,{0,1},0,2)))") == "3,5");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(3,OFFSET(A1,ROW(A1:A3)-1,0)))") == "3");
  CHECK(t.Eval("TEXTJOIN(\",\",,COUNTIF(OFFSET(A1,{0;1},0),{1,2}))") == "1,0,0,1");
  CHECK(t.Eval("TEXTJOIN(\",\",,SUMIF(OFFSET(A1,{0,1},0),\">0\",OFFSET(A1,{1,2},0)))") == "2,3");
  CHECK(t.Eval("TEXTJOIN(\",\",,ROW(OFFSET(A1,{0,1},0)))") == "1,2");
  CHECK(t.Eval("TEXTJOIN(\",\",,CELL(\"address\",OFFSET(A1,{0,1},0)))") == "$A$1,$A$2");
  CHECK(t.Eval("INDEX(T(OFFSET(B1,{0,1},0)),1)") == "x");
  // INDEX picks one reference
  CHECK(t.Eval("INDEX(OFFSET(A1,{0,1},0),2)") == "2");
  CHECK(t.Eval("SUM(INDEX(OFFSET(A1,{0,1},0,2),1,2))") == "5");
  // one value is still one reference
  CHECK(t.Eval("OFFSET(A1,1,0)") == "2");
  CHECK(t.Eval("SUM(OFFSET(A1,0,0,3))") == "6");
  CHECK(t.Eval("INDIRECT(\"A3\")") == "3");
}

TEST_CASE("Round8 formula tail: LET and LAMBDA refuse a name given twice") {
  yctest::Doc t;
  CHECK_FALSE(Parses(t, "LET(x,1,x,2,x)"));
  CHECK_FALSE(Parses(t, "LET(x,1,X,2,x)"));
  CHECK_FALSE(Parses(t, "LAMBDA(x,x,x)(1)"));
  CHECK_FALSE(t.Set("C1", "=LET(x,1,x,2,x)"));
  CHECK(t.Eval("LET(x,1,y,2,x+y)") == "3");
  CHECK(t.Eval("LET(x,1,LET(x,2,x))") == "2");  // a LET inside may take the name again
}

TEST_CASE("Round8 formula tail: a union read as a value is one #VALUE!") {
  yctest::Doc t;
  for (int i = 1; i <= 3; ++i) {
    t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
    t.Set(("D" + std::to_string(i)).c_str(), std::to_string(i * 10));
  }
  t.Set("F1", "=(A1:A3,D1:D3)");
  CHECK(t.T("F1") == "#VALUE!");
  CHECK(t.V("F2").IsEmpty());  // it doesn't spill
  CHECK(t.Eval("(A1,D1)") == "#VALUE!");
  CHECK(t.Eval("IFERROR((A1:A3,D1:D3),\"e\")") == "e");
  CHECK(t.Eval("SUM((A1:A3,D1:D3))") == "66");
  CHECK(t.Eval("INDEX((A1:A3,D1:D3),2,1,2)") == "20");
}

TEST_CASE("Round8 formula tail: approximate lookups search mixed types as Excel's binary search") {
  yctest::Doc t;
  Column(t, 'A', {"5", "'5", "TRUE", "abc", "", "2.5"});
  CHECK(t.Eval("VLOOKUP(2.5,A1:A6,1,TRUE)") == "2.5");  // #N/A before
  CHECK(t.Eval("LOOKUP(2.5,A1:A6)") == "2.5");
  CHECK(t.Eval("MATCH(FALSE,A1:A6,1)") == "#N/A");
  CHECK(t.Eval("MATCH(TRUE,A1:A6,1)") == "3");
  CHECK(t.Eval("MATCH(\"abc\",A1:A6,1)") == "4");
  CHECK(t.Eval("MATCH(\"5\",A1:A6,1)") == "2");
  CHECK(t.Eval("MATCH(5,A1:A6,1)") == "6");
  CHECK(t.Eval("MATCH(3,A1:A6,1)") == "6");
  CHECK(t.Eval("HLOOKUP(2,{1,\"a\",2,3},1,TRUE)") == "2");
  // an equal value ends the search, even in unsorted data
  CHECK(t.Eval("MATCH(3,{1,2,2.5,3,1,4,6},1)") == "4");
  CHECK(t.Eval("MATCH(2,{1,2,2,2,3},1)") == "4");  // the last of the equal values after it
  // a value of another type is passed over forward
  CHECK(t.Eval("MATCH(2,{5,2,\"d\",2.5,6},1)") == "2");
  CHECK(t.Eval("MATCH(FALSE,{\"b\",TRUE,FALSE,4,2.5,TRUE,2},1)") == "3");
  CHECK(t.Eval("MATCH(5,{7,8,8,4,\"x\",\"x\",\"x\",8,4,TRUE,5},1)") == "4");
  CHECK(t.Eval("MATCH(6,{3,1,6,TRUE,TRUE,3,\"x\",9,8,6,8,3,8},1)") == "6");
  CHECK(t.Eval("MATCH(5,{9,1,\"x\",9,1},1)") == "2");
  Column(t, 'B', {"7", "3", "9", "4", "", "6", "1", "6", "FALSE", "2"});
  CHECK(t.Eval("MATCH(5,B1:B10,1)") == "#N/A");
  Column(t, 'C', {"1", "6", "TRUE", "3", "", "x", "6", "7", ""});
  CHECK(t.Eval("MATCH(4,C1:C9,1)") == "4");
  Column(t, 'D', {"3", "4", "c", "b", "4", "4", "", "d", "c"});
  CHECK(t.Eval("MATCH(\"c\",D1:D9,1)") == "4");
  Column(t, 'E', {"1", "3", "1", "3", "c", "a", "5", "4", "", ""});
  CHECK(t.Eval("MATCH(2,E1:E10,1)") == "3");
  Column(t, 'F', {"5", "d", "FALSE", "'5", "", "c", "4", "", "1", "a"});
  CHECK(t.Eval("VLOOKUP(\"b\",F1:F10,1,TRUE)") == "5");
  // sorted data as before
  CHECK(t.Eval("MATCH(2.5,{1,2,3},1)") == "2");
  CHECK(t.Eval("VLOOKUP(25,{10,\"a\";20,\"b\";30,\"c\"},2,TRUE)") == "b");
}

TEST_CASE("Round8 formula tail: a date or time after a leading space is read only by VALUE, DATEVALUE and TIMEVALUE") {
  yctest::Doc t;
  CHECK(t.Eval("HOUR(\" 12:00 \")") == "#VALUE!");  // 12 before
  CHECK(t.Eval("HOUR(\" 12:00\")") == "#VALUE!");
  CHECK(t.Eval("HOUR(\"12:00 \")") == "12");
  CHECK(t.Eval("TIMEVALUE(\" 12:00 \")") == "0.5");
  CHECK(t.Eval("VALUE(\" 12:00\")") == "0.5");
  CHECK(t.Eval("DATEVALUE(\" 2024/1/2\")") == "45293");
  CHECK(t.Eval("DAY(\" 2024/1/2\")") == "#VALUE!");
  CHECK(t.Eval("DAY(\"2024/1/2 \")") == "2");
  CHECK(t.Eval("\" 12:00\"+0") == "#VALUE!");
  CHECK(t.Eval("\" 1/2\"+0") == "#VALUE!");
  CHECK(t.Eval("\"12:00 \"+0") == "0.5");
  CHECK(t.Eval("\" 5\"+0") == "5");  // a number takes the space
  CHECK(t.Eval("\" 50%\"+0") == "0.5");
  CHECK(t.Eval("DAY(\" 5\")") == "5");
  CHECK(t.Eval("TEXT(\" 12:00\",\"0.00\")") == " 12:00");
  CHECK(t.Eval("TEXT(\"12:00 \",\"0.00\")") == "0.50");
}

TEST_CASE("Round8 formula tail: CELL(\"address\") of another sheet names the workbook and the sheet") {
  yctest::Doc t;
  t.d.RenameSheet(t.sheet(), "Sheet1");
  t.d.AddSheet("Other");
  t.d.AddSheet("My Sheet");
  CHECK(t.Eval("CELL(\"address\",Other!B2)") == "[Book]Other!$B$2");
  CHECK(t.Eval("CELL(\"address\",'My Sheet'!C3)") == "'[Book]My Sheet'!$C$3");
  CHECK(t.Eval("CELL(\"address\",B2)") == "$B$2");
  CHECK(t.Eval("CELL(\"address\",Sheet1!B2)") == "$B$2");
  CHECK(t.Eval("CELL(\"filename\",Other!B2)") == "[Book]Other");  // the same workbook part
}

TEST_CASE("Round8 formula tail: names that read as R1C1 references are refused outside LET and LAMBDA") {
  yctest::Doc t;
  t.d.AddSheet("Other");
  for (const char* f : {"R2D2", "C3PO", "R1x", "r2d2", "R1C1", "RC1x", "R1_", "R", "C", "RC", "R2C", "SUM(R2D2)", "R2D2+1", "Other!R2D2"}) {
    CAPTURE(f);
    CHECK_FALSE(Parses(t, f));
  }
  CHECK_FALSE(t.Set("A1", "=R2D2"));
  for (const char* f : {"Rx", "Cx", "RCx", "Rate", "R0x", "C0x", "C16385x", "R_", "xR2D2", "_R2D2", "Other!Rx"}) {
    CAPTURE(f);
    CHECK(Parses(t, f));
  }
  CHECK(t.Eval("Rx") == "#NAME?");
  CHECK(t.Eval("RC2") == "0");  // column RC, row 2
  CHECK(t.Eval("LET(r,1,r)") == "1");
  CHECK(t.Eval("LET(R2D2,1,R2D2)") == "1");
  CHECK(t.Eval("LAMBDA(r,c,r+c)(1,2)") == "3");
  CHECK(t.Eval("SUM(MAKEARRAY(2,2,LAMBDA(r,c,r*c)))") == "9");
  CHECK_FALSE(Parses(t, "LET(x,1,R)"));
}

TEST_CASE("Round8 formula tail: fractions are Excel's continued-fraction convergents") {
  yctest::Doc t;
  CHECK(t.Eval("TEXT(0.1,\"0 0/0\")") == "0 0/1");  // 0 1/9 before
  CHECK(t.Eval("TEXT(0.1,\"?/?\")") == "0/1");
  CHECK(t.Eval("TEXT(0.11,\"?/?\")") == "1/9");
  CHECK(t.Eval("TEXT(0.7,\"?/?\")") == "2/3");    // not the closer 5/7
  CHECK(t.Eval("TEXT(0.3,\"?/?\")") == "2/7");    // 1/0.333... is 2.9999999999999987 in doubles
  CHECK(t.Eval("TEXT(0.46,\"?/?\")") == "1/2");
  CHECK(t.Eval("TEXT(0.27,\"?/?\")") == "1/4");
  CHECK(t.Eval("TEXT(0.15,\"# ?/?\")") == " 1/7");
  CHECK(t.Eval("TEXT(0.93,\"0 0/0\")") == "1 0/1");
  CHECK(t.Eval("TEXT(0.95,\"# ?/?\")") == "1    ");
  CHECK(t.Eval("TEXT(1.1,\"?/?\")") == "10/9");
  CHECK(t.Eval("TEXT(0.99,\"??/??\")") == "98/99");
  CHECK(t.Eval("TEXT(0.05,\"??/??\")") == " 1/20");
  CHECK(t.Eval("TEXT(PI(),\"# ?/??\")") == "3 1/7 ");  // not 3 14/99
  CHECK(t.Eval("TEXT(PI(),\"# ???/???\")") == "3  16/113");
  CHECK(t.Eval("TEXT(EXP(1),\"?/??\")") == "193/71");
  CHECK(t.Eval("TEXT(0.618,\"# ???/???\")") == " 309/500");
  // without a whole part a numerator of 0 has no minus
  CHECK(t.Eval("TEXT(-0.1,\"?/?\")") == "0/1");
  CHECK(t.Eval("TEXT(-0.001,\"??/??\")") == " 0/1 ");
  CHECK(t.Eval("TEXT(-0.1,\"??/??\")") == "- 1/10");
  CHECK(t.Eval("TEXT(-1.5,\"?/?\")") == "-3/2");
  CHECK(t.Eval("TEXT(-0.1,\"# ?/?\")") == "-0    ");
  CHECK(t.Eval("TEXT(-0.1,\"0 0/0\")") == "-0 0/1");
}

TEST_CASE("Round8 formula tail: number;@ shows negative numbers by the first section, big fractions are #VALUE!") {
  yctest::Doc t;
  CHECK(t.Eval("TEXT(-1,\"m/d/yy h:mm;@\")") == "#VALUE!");  // -1 before
  CHECK(t.Eval("TEXT(-1,\"0.0;@\")") == "-1.0");
  CHECK(t.Eval("TEXT(-1,\"#,##0.00;@\")") == "-1.00");
  CHECK(t.Eval("TEXT(-1,\"0%;@\")") == "-100%");
  CHECK(t.Eval("TEXT(-1.5,\"# ?/?;@\")") == "-1 1/2");
  CHECK(t.Eval("TEXT(-1,\"0;0;@\")") == "1");
  CHECK(t.Eval("TEXT(-1,\"h:mm;0\")") == "1");
  CHECK(t.Eval("TEXT(1,\"m/d/yy h:mm;@\")") == "1/1/00 0:00");
  CHECK(t.Eval("TEXT(12345678901,\"?/?\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(1E+15,\"??/??\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(2147483647,\"0/0\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(-2147483647,\"?/?\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(2147483646.5,\"?/?\")") == "2147483647/1");
  CHECK(t.Eval("TEXT(-2147483646,\"?/?\")") == "-2147483646/1");
  CHECK(t.Eval("TEXT(12345678901,\"# ?/?\")") == "12345678901    ");
  CHECK(t.Eval("TEXT(12345678901.5,\"0 ?/?\")") == "12345678901 1/2");
  t.Set("A1", "12345678901");
  const auto p = yctest::Doc::P("A1");
  t.d.ModifyStyle(t.sheet(), Rect::Cell(p.row, p.col), [&](yc::model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString("?/?"); });
  const std::string shown = t.T("A1");
  CHECK_FALSE(shown.empty());
  CHECK(shown.find_first_not_of('#') == std::string::npos);  // the cell is filled with #
}

TEST_CASE("Round8 formula tail: a spill reference whose anchor's row is deleted is written #REF!#") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("F2", "=SEQUENCE(3)");
  t.Set("G8", "=SUM(F2#)");
  t.Set("H8", "=F2#+1");
  t.Set("I9", "=ROWS(F2#)");
  CHECK(t.N("G8") == 6);
  t.d.DeleteRows(sh, 1, 1);
  t.d.Recalc();
  CHECK(t.F("G7") == "=SUM(#REF!#)");  // =SUM(#REF!) before
  CHECK(t.F("H7") == "=#REF!#+1");
  CHECK(t.F("I8") == "=ROWS(#REF!#)");
  CHECK(t.T("G7") == "#REF!");
  // the text reads back
  REQUIRE(t.Set("J1", "=SUM(#REF!#)"));
  CHECK(t.F("J1") == "=SUM(#REF!#)");
  CHECK(t.T("J1") == "#REF!");
  CHECK(Parses(t, "#REF!#+1"));
}

TEST_CASE("Round8 formula tail: YIELD over one coupon period counts days as Excel") {
  yctest::Doc t;
  auto ev = [&](const char* f) {
    t.Set("Z1", std::string("=") + f);
    return t.N("Z1");
  };
  auto near = [](double a, double b) { return std::fabs(a - b) <= 1e-12; };
  // bases 1 to 3: actual days (0.0594 for basis 2 and 0.0617 for basis 3 before)
  CHECK(near(ev("YIELD(DATE(2024,1,15),DATE(2024,3,15),0.05,99.8,100,2,1)"), 0.061229690754397395));
  CHECK(near(ev("YIELD(DATE(2024,1,15),DATE(2024,3,15),0.05,99.8,100,2,2)"), 0.061229690754397395));
  CHECK(near(ev("YIELD(DATE(2024,1,15),DATE(2024,3,15),0.05,99.8,100,2,3)"), 0.061229690754397395));
  CHECK(near(ev("YIELD(DATE(2024,5,10),DATE(2024,12,31),0.05,99.8,100,1,3)"), 0.052283781484032128));
  // basis 0: the days to the redemption move only the settlement's day
  CHECK(near(ev("YIELD(DATE(2024,1,15),DATE(2024,3,15),0.05,99.8,100,2,0)"), 0.0611038107752955));
  CHECK(near(ev("YIELD(DATE(2024,2,29),DATE(2024,8,31),0.05,99.8,100,2,0)"), 0.053809276010584529));
  CHECK(near(ev("YIELD(DATE(2023,11,30),DATE(2024,2,29),0.05,99.8,100,4,0)"), 0.058769223840940139));
  CHECK(near(ev("YIELD(DATE(2024,5,10),DATE(2024,12,31),0.05,99.8,100,1,0)"), 0.052064508295029545));
  CHECK(near(ev("YIELD(DATE(2024,7,1),DATE(2024,12,31),0.05,99.8,100,2,0)"), 0.05382239167339193));
  CHECK(near(ev("YIELD(DATE(2024,2,29),DATE(2024,8,31),0.06,101,100,2,0)"), 0.039385153985011793));
  // basis 4 as before
  CHECK(near(ev("YIELD(DATE(2023,3,1),DATE(2023,8,31),0.05,99.8,100,2,4)"), 0.0535484722318717));
}

TEST_CASE("Round8 formula tail: SUMPRODUCT, AND and OR of arrays of references, as measured in Excel 16") {
  yctest::Doc t;
  Column(t, 'A', {"1", "2", "3", "'5", "TRUE", "FALSE", "", "=NA()"});
  Column(t, 'B', {"x", "10", "20"});
  // SUMPRODUCT reads every reference as a value that isn't a number: 0 (#VALUE! before); errors and shapes still count
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0,1},0))") == "0");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0;1;2},0))") == "0");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0},0))") == "0");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0,1},0,2))") == "0");
  CHECK(t.Eval("SUMPRODUCT(INDIRECT({\"A1\",\"A2\"}))") == "0");
  CHECK(t.Eval("SUMPRODUCT(INDIRECT(\"A\"&{1,2,3}))") == "0");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0,1},0),{1,2})") == "0");
  CHECK(t.Eval("SUMPRODUCT(A1:A2,OFFSET(A1,{0;1},0))") == "0");
  CHECK(t.Eval("SUMPRODUCT(IF(TRUE,OFFSET(A1,{0,1},0)))") == "0");
  CHECK(t.Eval("SUMPRODUCT(CHOOSE(1,OFFSET(A1,{0,1},0)))") == "0");
  CHECK(t.Eval("SUMPRODUCT(LET(r,OFFSET(A1,{0,1},0),r))") == "0");
  CHECK(t.Eval("SUMPRODUCT(INDEX(OFFSET(A1,{0,1},0),0))") == "0");
  CHECK(t.Eval("SUM(INDEX(OFFSET(A1,{0,1},0),0))") == "3");
  CHECK(t.Eval("SUMPRODUCT(INDIRECT({\"A1\",\"Nope!A1\"}))") == "#REF!");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0,1},0),INDIRECT({\"A1\",\"Nope!A1\"}))") == "#REF!");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0,1},0),{1,#N/A})") == "#N/A");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0,1},0),{1,2,3})") == "#VALUE!");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,0,0,2),OFFSET(A1,{0,1},0))") == "#VALUE!");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,{0,1},0)*1)") == "#VALUE!");
  CHECK(t.Eval("SUMPRODUCT(--OFFSET(A1,{0,1},0))") == "#VALUE!");
  CHECK(t.Eval("SUMPRODUCT(OFFSET(A1,0,0,2))") == "3");
  CHECK(t.Eval("SUMPRODUCT(N(OFFSET(A1,{0,1},0)))") == "3");
  CHECK(t.Eval("SUMPRODUCT(IFERROR(OFFSET(A1,{0,1},0),5))") == "10");
  CHECK(t.Eval("SUM(IFERROR(OFFSET(A1,{0,1},0),5))") == "10");
  CHECK(t.Eval("SUMPRODUCT(ISNUMBER(OFFSET(A1,{0,1},0))*1)") == "0");
  CHECK(t.Eval("TYPE(OFFSET(A1,{0,1},0))") == "64");
  // the idiom: SUBTOTAL per reference (visible rows only), summed by SUMPRODUCT
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0,1},0)))") == "3");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0,1,2},0)))") == "6");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0;1;2},0)))") == "6");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,ROW(A1:A3)-1,0)))") == "6");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0,1},0)),{1,2})") == "5");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0;1;2},0))*(A1:A3>1))") == "5");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(103,OFFSET(A1,ROW(A1:A3)-1,0)),A1:A3)") == "6");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0,1},0,2)))") == "8");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,INDIRECT(\"A\"&{1,2,3})))") == "6");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0,1,2},0)),SUBTOTAL(9,OFFSET(B1,{0,1,2},0)))") == "80");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0,1,2},0))/{1,2,3})") == "3");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(1,OFFSET(A1,{0,1,2},0,2)))") == "7");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1,{0,1,2},0,1,2)))") == "36");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(9,OFFSET(A1:A3,ROW(A1:A3)-ROW(A1),0,1)))") == "6");
  CHECK(t.Eval("SUMPRODUCT(SUBTOTAL(3,OFFSET(B1,ROW(B1:B3)-ROW(B1),0)),--(A1:A3>1))") == "2");
  CHECK(t.Eval("SUMPRODUCT(--(SUBTOTAL(103,OFFSET(A1,ROW(A1:A3)-1,0))>0))") == "3");
  CHECK(t.Eval("SUMPRODUCT(SUMIF(OFFSET(A1,{0,1,2},0),\">1\"))") == "5");
  CHECK(t.Eval("SUMPRODUCT(COUNTIF(OFFSET(A1,{0,1,2},0),\">1\"))") == "2");
  CHECK(t.Eval("SUM(SUBTOTAL(9,OFFSET(A1,{0,1,2},0)))") == "6");
  CHECK(t.Eval("MAX(SUBTOTAL(9,OFFSET(A1,{0,1,2},0)))") == "3");
  CHECK(t.Eval("COUNT(SUBTOTAL(9,OFFSET(A1,{0,1,2},0)))") == "3");
  // AND, OR and XOR are #N/A, whatever the references hold; an error in another argument wins
  CHECK(t.Eval("AND(OFFSET(A1,{0,1},0))") == "#N/A");
  CHECK(t.Eval("OR(OFFSET(A1,{0,1},0))") == "#N/A");
  CHECK(t.Eval("XOR(INDIRECT({\"A5\",\"A6\"}))") == "#N/A");
  CHECK(t.Eval("AND(OFFSET(A5,{0,1},0))") == "#N/A");
  CHECK(t.Eval("AND(OFFSET(A5,{0},0))") == "#N/A");
  CHECK(t.Eval("AND(OFFSET(A1,0,0,{1,2}))") == "#N/A");
  CHECK(t.Eval("AND(OFFSET(A1,{0,1},0),TRUE)") == "#N/A");
  CHECK(t.Eval("OR(TRUE,OFFSET(A5,{0,1},0))") == "#N/A");
  CHECK(t.Eval("AND(OFFSET(A5,{0,1},0),\"x\")") == "#N/A");
  CHECK(t.Eval("AND(INDIRECT({\"A1\",\"Nope!A1\"}))") == "#N/A");
  CHECK(t.Eval("AND(IF(TRUE,OFFSET(A5,{0,1},0)))") == "#N/A");
  CHECK(t.Eval("ERROR.TYPE(AND(OFFSET(A1,{0,1},0)))") == "7");
  CHECK(t.Eval("AND(1/0,OFFSET(A5,{0,1},0))") == "#DIV/0!");
  CHECK(t.Eval("AND(OFFSET(A5,{0,1},0),1/0)") == "#DIV/0!");
  CHECK(t.Eval("AND(IFERROR(OFFSET(A5,{0,1},0),TRUE))") == "TRUE");
  CHECK(t.Eval("AND(N(OFFSET(A1,{0,1},0)))") == "TRUE");
  CHECK(t.Eval("AND(OFFSET(A1,{0,1},0)>0)") == "#VALUE!");
  // more functions read each reference's first cell; the rest see #VALUE!
  CHECK(t.Eval("MINA(OFFSET(A1,{0,1,2},0))") == "1");
  CHECK(t.Eval("VAR.S(OFFSET(A1,{0,1,2},0))") == "1");
  CHECK(t.Eval("VARA(OFFSET(A1,{0,1,2},0))") == "1");
  CHECK(t.Eval("STDEVA(OFFSET(A1,{0,1,2},0))") == "1");
  CHECK(t.Eval("STDEV.P(OFFSET(A1,{0,1,2},0))") == "0.816496581");  // Value2 0.816496580927726
  CHECK(t.Eval("STDEVPA(OFFSET(A1,{0,1,2},0))") == "0.816496581");  // Value2 0.816496580927726
  CHECK(t.Eval("DEVSQ(OFFSET(A1,{0,1,2},0))") == "2");
  CHECK(t.Eval("NPV(0.1,OFFSET(A1,{0,1},0))") == "2.561983471");  // Value2 2.56198347107438
  CHECK(t.Eval("HARMEAN(OFFSET(A1,{0,1,2},0))") == "#VALUE!");
  CHECK(t.Eval("MEDIAN(OFFSET(A1,{0,1,2},0))") == "#VALUE!");
  CHECK(t.Eval("LARGE(OFFSET(A1,{0,1,2},0),1)") == "#VALUE!");
}

TEST_CASE("Round8 formula tail: TEXT refuses the codes Excel refuses, and the text section takes no numbers") {
  yctest::Doc t;
  // an @ outside the last section, a number part in the text section, five sections: #VALUE! (measured)
  for (const char* code : {"0;@;0", "@;0", "@;@", "0;@;@", "@;0;0", "0;0;@;@", "0;@;0;@", "0;@;", "@;;", "@;",
                           "0.0;@;0", "0;\\-@;0", "0;@\\z;0", "[<0]@;0", "[>0]0;@;0", "0;0;@;0", "0;0;0;0",
                           "0;0;0;\\x;\\y", "0;0;0;@;", "@0", "0@", "@#", "#@", "0;0;@0", "0;0;0;#", "0;0;0;?",
                           "0;0;0;.", "0;0;0;%", "0;0;0;E+0", "0;0;0;yy", "0;0;0;h", "0;0;0;e", "0;0;0;[s]",
                           "0;0;0;0.0", "@.", "@%", "@/", "@E+", "@y", "@d", "m/d/yy;@;0", "0%;@;0"}) {
    INFO(code);
    for (const char* v : {"1", "-1", "0", "\"a\""}) CHECK(t.Eval(std::string("TEXT(") + v + ",\"" + code + "\")") == "#VALUE!");
  }
  // codes with General or a color, measured as cell formats (a Chinese Excel's TEXT spells those in Chinese)
  CHECK(yc::engine::DisplayTextOf(yc::core::Value::Number(-1), "0;[Red]@") == "-1");
  CHECK(yc::engine::DisplayTextOf(yc::core::Value::String("a"), "0;0;0;General") == "a");
  CHECK(yc::engine::DisplayTextOf(yc::core::Value::String("a"), "0;0;0;[Red]") == "a");
  CHECK(yc::engine::DisplayTextOf(yc::core::Value::String("a"), "0;[Red]\\x@") == "xa");
  CHECK(yc::engine::DisplayTextOf(yc::core::Value::Number(-1), "General;@") == "-1");
  CHECK(t.Eval("TEXT(1,\"@ General\")") == "#VALUE!");
  // a cell with a code Excel refuses shows its value as General
  CHECK(yc::engine::DisplayTextOf(yc::core::Value::Number(-1.5), "0;@;0") == "-1.5");
  // the last section with an @ is the text section, also with two or three sections
  CHECK(t.Eval("TEXT(-1,\"0;@\")") == "-1");
  CHECK(t.Eval("TEXT(-0.5,\"0;@\")") == "-1");
  CHECK(t.Eval("TEXT(\"a\",\"0;\\n@\")") == "na");
  CHECK(t.Eval("TEXT(TRUE,\"0;\\b@\")") == "bTRUE");
  CHECK(t.Eval("TEXT(\"a\",\"0;0;\\z@\")") == "za");
  CHECK(t.Eval("TEXT(\"a\",\"0;\"\"n\"\"@\")") == "na");
  CHECK(t.Eval("TEXT(-1,\"0;0;\\z@\")") == "1");
  CHECK(t.Eval("TEXT(0,\"0;0;\\z@\")") == "0");
  CHECK(t.Eval("TEXT(0,\"0.0;0.0;@\")") == "0.0");
  CHECK(t.Eval("TEXT(0,\"0.0;;@\")") == "0.0");
  CHECK(t.Eval("TEXT(-2,\"0.0;\\m0.0;@\")") == "m2.0");
  CHECK(t.Eval("TEXT(0,\"0.0;\\m0.0;\\z@\")") == "0.0");
  CHECK(t.Eval("TEXT(-1,\"0;;@\")") == "");
  CHECK(t.Eval("TEXT(1,\"\\x;@\")") == "x");
  CHECK(t.Eval("TEXT(-1,\"\\x;@\")") == "-x");
  CHECK(t.Eval("TEXT(-1,\"0;-0;;@\")") == "-1");
  CHECK(t.Eval("TEXT(0,\"0;-0;;@\")") == "");
  CHECK(t.Eval("TEXT(\"a\",\"0;0;0;\\0\")") == "0");
  CHECK(t.Eval("TEXT(\"a\",\"0;0;0;_0@\")") == " a");
  CHECK(t.Eval("TEXT(\"a\",\"0;0;0;@@\")") == "aa");
  CHECK(t.Eval("TEXT(\"a\",\"@@\")") == "aa");
  CHECK(t.Eval("TEXT(-1,\"@@\")") == "-1");
  CHECK(t.Eval("TEXT(1,\"\"\"x\"\"@\")") == "1");
  // a number only the text section would take is #VALUE!, as one that matches neither of two conditions
  CHECK(t.Eval("TEXT(-1,\"[>0]0;@\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(0,\"[>0]0;@\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(1,\"[<0]0;@\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(-1,\"[<0]0;@\")") == "-1");
  CHECK(t.Eval("TEXT(1,\"[>5]0;@\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(6,\"[>5]0;@\")") == "6");
  CHECK(t.Eval("TEXT(-1,\"[>=0]0;@\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(\"a\",\"[>0]0;@\")") == "a");
  CHECK(t.Eval("TEXT(-1,\"[>0]0;[<0]0;@\")") == "1");
  CHECK(t.Eval("TEXT(0,\"[>0]0;[<0]0;@\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(0,\"[>0]0;[<0]0\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(-5,\"[<0]0;0;@\")") == "5");
  CHECK(t.Eval("TEXT(5,\"[<0]0;0;@\")") == "5");
  CHECK(t.Eval("TEXT(0,\"[<0]0;0;@\")") == "0");
  // one section whose condition fails still shows the number, without its minus
  CHECK(t.Eval("TEXT(-1,\"[>0]0\")") == "1");
  CHECK(t.Eval("TEXT(1,\"[<0]0\")") == "1");
  CHECK(t.Eval("TEXT(-1,\"[<0]0\")") == "1");
}

TEST_CASE("Round8 formula tail: the codes Range.NumberFormat refuses, as measured in Excel 16") {
  yctest::Doc t;
  // Excel raises "Unable to set the NumberFormat property" for these (measured), and TEXT of them is #VALUE!
  for (const char* code : {"0;@;0", "@;0", "@;@", "0;0;0;0", "0;0;0;0;0", "@0", "\"abc", "0\"", "[Blue", "[>0", "0_",
                           "0*", "[Color57]0", "[>100][<200]0"}) {
    INFO(code);
    CHECK_FALSE(yc::formula::ValidFormatCode(code));
  }
  // ...and takes these (measured)
  for (const char* code : {"General", "", "0.00", "0;0;@", "[Red]0", "0;0;0;General", "[Bluex]0", "[>0]0", "\"a\"\"b\"",
                           "0]", "[$-404]0", "[DBNum1]0", "[Color5]0", "[Color56]0", "abc", "0;;", ";;;", "[h]:mm", "[hh]",
                           "[s].00", "0\"x\"0", "General;General", "0;General", "0;-0;;@", "#,##0;[Red]-#,##0",
                           "[Red][>100]0", "[=1]0;[=2]0;0", "0_)", "* #,##0", "\\$0"}) {
    INFO(code);
    CHECK(yc::formula::ValidFormatCode(code));
  }
  CHECK(t.Eval("TEXT(1,\"[>0\")") == "#VALUE!");
  CHECK(t.Eval("TEXT(1,\"0_\")") == "#VALUE!");
}
