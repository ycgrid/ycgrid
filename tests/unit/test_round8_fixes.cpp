// SPDX-License-Identifier: Apache-2.0
// Round 8 (measured against Excel 16 itself): wildcards on texts holding * and a trailing ~, criteria with an
// operator alone, SUM's cancelling, PROPER beyond ASCII, SUBSTITUTE's overlapping instances, TEXTJOIN's delimiter
// lists, FIXED's sign, T and N of a range, MATCH / XLOOKUP / XMATCH modes and binary search, INDIRECT's R1C1, INDEX
// and ROWS shapes, LOOKUP's one-cell result, ADDRESS's checks and sheet quoting, formulas written before a row is
// deleted, visible cells, AutoFill of month ends and inner numbers, Replace's shortest match, a deleted cell on
// another sheet and insertions that would push data off the sheet
#include <catch2/catch_test_macros.hpp>

#include "core/geometry.h"
#include "engine/document.h"
#include "engine/navigate.h"
#include "test_helpers.h"

#include <cmath>
#include <string>

using yc::core::Rect;

namespace {
void SetFmt(yctest::Doc& t, const char* a1, const char* code) {
  const auto p = yctest::Doc::P(a1);
  t.d.ModifyStyle(t.sheet(), Rect::Cell(p.row, p.col), [&](yc::model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString(code); });
}
}  // namespace

TEST_CASE("Round8: wildcards match texts that hold * themselves, and a trailing ~ is dropped") {
  yctest::Doc t;
  t.Set("G3", "a*c");
  t.Set("G4", "abc*def");
  CHECK(t.Eval("COUNTIF(G3,\"a*\")") == "1");  // the pattern's * met the text's * as a literal: 0
  CHECK(t.Eval("COUNTIF(G4,\"*c*\")") == "1");
  CHECK(t.Eval("COUNTIF(G4,\"abc*\")") == "1");
  CHECK(t.Eval("MATCH(\"a*\",{\"x\",\"a*c\"},0)") == "2");
  CHECK(t.Eval("XLOOKUP(\"a*\",{\"a*c\"},{1},\"nf\",2)") == "1");
  CHECK(t.Eval("SEARCH(\"~\",\"a~b\")") == "1");
  CHECK(t.Eval("MATCH(\"ab~\",{\"ab\"},0)") == "1");
  CHECK(t.Eval("SEARCH(\"a~*\",\"xa*\")") == "2");  // an escaped * is still a literal
  CHECK(t.Eval("COUNTIF(G3,\"a~*c\")") == "1");
}

TEST_CASE("Round8: texts compare in Excel's order (Windows' word sort)") {
  yctest::Doc t;
  CHECK(t.Eval("\"co-op\"<\"coop\"") == "FALSE");
  CHECK(t.Eval("\"coop\"<\"co-op\"") == "TRUE");
  CHECK(t.Eval("\"co-op\"<\"cop\"") == "TRUE");
  CHECK(t.Eval("\"a-c\"<\"ab\"") == "FALSE");
  CHECK(t.Eval("\"'a\"<\"a\"") == "FALSE");
  CHECK(t.Eval("\"_\"<\"1\"") == "TRUE");
  CHECK(t.Eval("\"\xC3\xA9\"<\"f\"") == "TRUE");  // e acute
  CHECK(t.Eval("\"\xC3\xA4\"<\"b\"") == "TRUE");  // a umlaut
  CHECK(t.Eval("\"co-op\"=\"coop\"") == "FALSE");
  CHECK(t.Eval("\"ABC\"=\"abc\"") == "TRUE");
  CHECK(t.Eval("\"a1\"<\"a2\"") == "TRUE");
  CHECK(t.Eval("\"a 1\"<\"a1\"") == "TRUE");
  CHECK(t.Eval("TEXTJOIN(\"|\",,SORT({\"BRK-B\";\"BRK.A\";\"BRKB\";\"BRK B\";\"BRKA\";\"BRK/B\"}))") ==
        "BRK B|BRK.A|BRK/B|BRKA|BRKB|BRK-B");
  CHECK(t.Eval("MATCH(\"coop\",{\"a\",\"co-op\",\"d\"},1)") == "1");
  t.Set("F1", "BRK-B");
  t.Set("F2", "BRKB");
  t.Set("F3", "BRK.A");
  t.Set("F4", "BRKC");
  CHECK(t.Eval("COUNTIF(F1:F4,\">BRK-B\")") == "1");  // BRKC only
}

TEST_CASE("Round8: SORT and SORTBY put blank cells last in either order") {
  yctest::Doc t;
  t.Set("D1", "5");
  t.Set("D2", "3");
  t.Set("D3", "1");
  t.Set("A1", "a");
  t.Set("A2", "b");
  t.Set("A3", "c");
  t.Set("A4", "d");
  t.Set("A5", "e");
  CHECK(t.Eval("TEXTJOIN(\",\",FALSE,SORT(D1:D5))") == "1,3,5,,");  // the blanks last (a cell shows them as 0)
  CHECK(t.Eval("TEXTJOIN(\",\",FALSE,SORT(D1:D5,,-1))") == "5,3,1,,");
  CHECK(t.Eval("TEXTJOIN(\",\",FALSE,SORTBY(A1:A5,D1:D5))") == "c,b,a,d,e");
  CHECK(t.Eval("TEXTJOIN(\",\",FALSE,SORTBY(A1:A5,D1:D5,-1))") == "a,b,c,d,e");
}

TEST_CASE("Round8: SEARCH refuses a start past the text's end, FIND takes one just past it") {
  yctest::Doc t;
  CHECK(t.Eval("SEARCH(\"\",\"abc\",4)") == "#VALUE!");
  CHECK(t.Eval("SEARCH(\"\",\"\",1)") == "#VALUE!");
  CHECK(t.Eval("SEARCH(\"\",\"abc\",3)") == "3");
  CHECK(t.Eval("FIND(\"\",\"abc\",4)") == "4");
}

TEST_CASE("Round8: criteria with an operator alone tell empty cells from empty texts") {
  yctest::Doc t;
  t.Set("D1", "apple");
  t.Set("D2", "=\"\"");
  t.Set("D4", "7");
  CHECK(t.Eval("COUNTIF(D1:D4,\"<>\")") == "3");  // was 2: the ="" cell counts
  CHECK(t.Eval("COUNTIF(D1:D4,\"=\")") == "1");   // only the truly empty D3
  CHECK(t.Eval("COUNTIF(D1:D4,\">\")") == "0");
  CHECK(t.Eval("COUNTIF(D1:D4,\"\")") == "2");    // empty and ="" (unchanged)
  CHECK(t.Eval("COUNTIF(D1:D4,\"<>\"&\"\")") == "3");
}

TEST_CASE("Round8: SUM cancels to 0 where Excel does") {
  yctest::Doc t;
  t.Set("A1", "0.1");
  t.Set("A2", "0.2");
  t.Set("A3", "-0.3");
  CHECK(t.Eval("SUM(A1:A3)") == "0");
  CHECK(t.Eval("SUM(A1:A3)=0") == "TRUE");
  CHECK(t.Eval("SUM(1,-0.9,-0.1)") == "0");
  CHECK(t.Eval("SUM(1E+15,0.1,-1E+15)") == "0");
  CHECK(t.Eval("SUM(0.1,0.2)") == "0.3");
  CHECK(t.Eval("AVERAGE(A1:A3)") == "0");
  CHECK(t.Eval("SUBTOTAL(9,A1:A3)") == "0");
  CHECK(t.Eval("SUBTOTAL(109,A1:A3)") == "0");
  CHECK(t.Eval("AGGREGATE(9,4,A1:A3)") == "0");
  CHECK(t.Eval("SUMPRODUCT(A1:A3)") != "0");  // Excel doesn't cancel in SUMPRODUCT
}

TEST_CASE("Round8: PROPER raises letters beyond ASCII, and only letters with a case are letters") {
  yctest::Doc t;
  CHECK(t.Eval("PROPER(\"\xC3\xA9t\xC3\xA9\")") == "\xC3\x89t\xC3\xA9");  // ete with accents: Ete
  CHECK(t.Eval("PROPER(\"\xC3\xBC" "ber\")") == "\xC3\x9C" "ber");
  CHECK(t.Eval("PROPER(\"2nd place\")") == "2Nd Place");
  CHECK(t.Eval("PROPER(\"x\xC2\xA0y\")") == "X\xC2\xA0Y");  // a non-breaking space starts a word
  CHECK(t.Eval("PROPER(\"o'neil\")") == "O'Neil");
}

TEST_CASE("Round8: SUBSTITUTE counts overlapping occurrences when an instance is given") {
  yctest::Doc t;
  CHECK(t.Eval("SUBSTITUTE(\"aaaaa\",\"aa\",\"X\",2)") == "aXaa");
  CHECK(t.Eval("SUBSTITUTE(\"aaa\",\"aa\",\"b\",2)") == "ab");
  CHECK(t.Eval("SUBSTITUTE(\"aaaaa\",\"aa\",\"X\")") == "XXa");  // all of them: one after another
  CHECK(t.Eval("SUBSTITUTE(\"abc\",\"b\",\"X\",5)") == "abc");
}

TEST_CASE("Round8: TEXTJOIN uses a list of delimiters in turn, FIXED shows no sign on a zero") {
  yctest::Doc t;
  CHECK(t.Eval("TEXTJOIN({\"-\",\"+\"},TRUE,1,2,3)") == "1-2+3");
  CHECK(t.Eval("TEXTJOIN({\"-\",\"+\"},TRUE,1,2,3,4)") == "1-2+3-4");
  CHECK(t.Eval("FIXED(-0.004,2)") == "0.00");
  CHECK(t.Eval("FIXED(-0.4,0)") == "0");
  CHECK(t.Eval("FIXED(-1234.5,1)") == "-1,234.5");
}

TEST_CASE("Round8: ISEVEN and ISODD refuse logical values and read numbers as text") {
  yctest::Doc t;
  t.Set("A3", "TRUE");
  t.Set("A2", "'5");
  CHECK(t.Eval("ISEVEN(TRUE)") == "#VALUE!");
  CHECK(t.Eval("ISEVEN(A3)") == "#VALUE!");
  CHECK(t.Eval("ISODD(A3)") == "#VALUE!");
  CHECK(t.Eval("ISEVEN(\"4\")") == "TRUE");
  CHECK(t.Eval("ISODD(A2)") == "TRUE");
  CHECK(t.Eval("ISODD(-3.7)") == "TRUE");
}

TEST_CASE("Round8: T and N of a range read its first cell") {
  yctest::Doc t;
  t.Set("B1", "x");
  t.Set("B2", "y");
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("Z1", "=T(B1:B3)");
  t.Set("Z2", "=N(A1:A3)");
  CHECK(t.T("Z1") == "x");
  CHECK(t.N("Z2") == 1);
  CHECK(t.T("Z3") == "");  // nothing spilled below
}

TEST_CASE("Round8: MATCH reads the sign of its type and stops at the first equal value going down") {
  yctest::Doc t;
  t.Set("C1", "9");
  t.Set("C2", "7");
  t.Set("C3", "7");
  t.Set("C4", "7");
  t.Set("C5", "5");
  CHECK(t.Eval("MATCH(7,C1:C5,-1)") == "2");  // was 4
  CHECK(t.Eval("MATCH(8,C1:C5,-1)") == "1");
  CHECK(t.Eval("MATCH(2.5,{1,2,3},0.5)") == "2");
  CHECK(t.Eval("MATCH(2,{1,2,3},-0.5)") == "#N/A");
}

TEST_CASE("Round8: XMATCH and XLOOKUP search binary for search mode 2 and -2, and check their arguments") {
  yctest::Doc t;
  CHECK(t.Eval("XMATCH(2.5,{1,2,2,3},-1,2)") == "3");
  CHECK(t.Eval("XLOOKUP(2.5,{1,2,2,3},{10,20,30,40},,-1,2)") == "30");
  CHECK(t.Eval("XMATCH(2.5,{1,2,2,3},1,2)") == "4");
  CHECK(t.Eval("XMATCH(3,{1,2,3,4,5},0,2)") == "3");
  CHECK(t.Eval("XMATCH(4,{5,4,3,2,1},0,-2)") == "2");
  CHECK(t.Eval("XMATCH(3.5,{5,4,3,2,1},-1,-2)") == "3");
  CHECK(t.Eval("XMATCH(3.5,{5,4,3,2,1},1,-2)") == "2");
  CHECK(t.Eval("XMATCH(1,{1,2,3},0,-2)") == "#N/A");  // ascending data searched as descending
  CHECK(t.Eval("XLOOKUP(2,{1,2,3},{4,5})") == "#VALUE!");
  CHECK(t.Eval("XMATCH(2,{1,2,3},5)") == "#VALUE!");
  CHECK(t.Eval("XMATCH(2,{1,2,3},0,0)") == "#VALUE!");
  CHECK(t.Eval("XMATCH(2,{1,2,3},0,-1.5)") == "2");  // INT(-1.5) is -2, a binary search (Excel 16, measured)
  CHECK(t.Eval("XMATCH(2,{1,2;3,4})") == "#VALUE!");
  CHECK(t.Eval("XLOOKUP(2,{1,2,3},{4,5,6})") == "5");
}

TEST_CASE("Round8: INDIRECT reads relative R1C1, whole rows and columns, and trailing spaces") {
  yctest::Doc t;
  t.Set("Y1000", "42");
  t.Set("A1", "7");
  t.Set("A2", "8");
  CHECK(t.Eval("INDIRECT(\"RC[-1]\",FALSE)") == "42");  // Eval writes in Z1000
  CHECK(t.Eval("INDIRECT(\"R[-999]C1\",FALSE)") == "7");
  CHECK(t.Eval("ROWS(INDIRECT(\"C2\",FALSE))") == "1048576");
  CHECK(t.Eval("COLUMNS(INDIRECT(\"R2\",FALSE))") == "16384");
  CHECK(t.Eval("SUM(INDIRECT(\"R1C1:R2C1\",FALSE))") == "15");
  CHECK(t.Eval("INDIRECT(\"A1 \")") == "7");
  CHECK(t.Eval("SUM(INDIRECT(\"A1:A2 \"))") == "15");
  CHECK(t.Eval("INDIRECT(\"R0C1\",FALSE)") == "#REF!");
  t.d.AddSheet("My Sheet");
  CHECK(t.Eval("INDIRECT(\"My Sheet!A1\")") == "#REF!");  // a name that needs quotes must have them
  CHECK(t.Eval("ISREF(INDIRECT(\"'My Sheet'!A1\"))") == "TRUE");
}

TEST_CASE("Round8: INDEX, ROWS and COLUMNS refuse shapes Excel refuses; LOOKUP reads a one-cell result along its row") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  t.Set("B1", "b1");
  t.Set("B2", "b2");
  t.Set("C1", "c1");
  CHECK(t.Eval("INDEX(A1:B3,2)") == "#REF!");
  CHECK(t.Eval("INDEX(A1:A3,2)") == "2");
  CHECK(t.Eval("INDEX(A1:C1,2)") == "b1");
  CHECK(t.Eval("ROWS((A1:A3,C1:C3))") == "#REF!");
  CHECK(t.Eval("COLUMNS((A1:A3,C1:C3))") == "#REF!");
  CHECK(t.Eval("LOOKUP(2,A1:A3,B1)") == "c1");
  CHECK(t.Eval("LOOKUP(2,A1:A3,B1:B3)") == "b2");
}

TEST_CASE("Round8: ADDRESS checks its numbers and quotes sheet names as Excel") {
  yctest::Doc t;
  CHECK(t.Eval("ADDRESS(1,1,0)") == "#VALUE!");
  CHECK(t.Eval("ADDRESS(1,1,5)") == "#VALUE!");
  CHECK(t.Eval("ADDRESS(1048577,1)") == "#VALUE!");
  CHECK(t.Eval("ADDRESS(1048576,16384,4,FALSE)") == "#VALUE!");
  CHECK(t.Eval("ADDRESS(-1,1,4,FALSE)") == "R[-1]C[1]");
  CHECK(t.Eval("ADDRESS(2,3,1,FALSE)") == "R2C3");
  CHECK(t.Eval("ADDRESS(1,1,1,TRUE,\"\")") == "!$A$1");
  CHECK(t.Eval("ADDRESS(1,1,1,TRUE,\".5\")") == "'.5'!$A$1");
  CHECK(t.Eval("ADDRESS(1,1,1,TRUE,\"TRUE\")") == "'TRUE'!$A$1");
  CHECK(t.Eval("ADDRESS(1,1,1,TRUE,\"false\")") == "'false'!$A$1");
  CHECK(t.Eval("ADDRESS(1,1,1,TRUE,\"R1C1x\")") == "'R1C1x'!$A$1");
  CHECK(t.Eval("ADDRESS(1,1,1,TRUE,\"Revenue\")") == "Revenue!$A$1");
  CHECK(yc::core::QuoteSheetName("TRUEX") == "TRUEX");
}

TEST_CASE("Round8: a formula on a sheet named .5 reads back after it is written") {
  yctest::Doc t;
  const uint32_t s2 = t.d.AddSheet(".5");
  t.d.SetInput(s2, 0, 0, "3");
  t.Set("A1", "='.5'!A1+1");
  CHECK(t.N("A1") == 4);
  CHECK(t.F("A1") == "='.5'!A1+1");  // was =.5!A1+1, which no longer parsed
  CHECK(t.Set("A2", t.F("A1")));
  CHECK(t.N("A2") == 4);
}

TEST_CASE("Round8: formulas written but not calculated yet keep calculating after a row is deleted") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (int i = 0; i < 6; ++i) t.d.SetInput(sh, static_cast<uint32_t>(i), 0, std::to_string(i + 1));
  t.d.SetInput(sh, 0, 2, "=A3");
  t.d.SetInput(sh, 1, 2, "=SUM(A1:A6)");
  t.d.SetInput(sh, 4, 3, "=A2*10");
  t.d.DeleteRows(sh, 5, 1);  // before any calculation, as a script that writes and deletes at once
  t.d.Recalc();
  CHECK(t.N("C1") == 3);   // was blank
  CHECK(t.N("C2") == 15);
  CHECK(t.N("D5") == 20);  // was blank
}

TEST_CASE("Round8: visible cells aren't limited to the used range") {
  yctest::Doc t;
  for (int i = 0; i < 4; ++i) t.d.SetInput(t.sheet(), static_cast<uint32_t>(i), 0, std::to_string(i + 1));
  t.d.Recalc();
  t.d.HideRows(t.sheet(), 1, 2, true);
  const auto* s = t.d.S(t.sheet());
  auto v = yc::engine::SpecialCells(*s, Rect{0, 0, 3, 1}, yc::engine::Special::Visible);
  REQUIRE(v.size() == 2);
  CHECK(v[0] == (Rect{0, 0, 0, 1}));  // $A$1:$B$1, B is empty
  CHECK(v[1] == (Rect{3, 0, 3, 1}));
  v = yc::engine::SpecialCells(*s, Rect{0, 2, 3, 2}, yc::engine::Special::Visible);  // C1:C4, all empty
  REQUIRE(v.size() == 2);
  CHECK(v[0] == Rect::Cell(0, 2));
  CHECK(v[1] == Rect::Cell(3, 2));
}

TEST_CASE("Round8: AutoFill continues month ends by months and counts on the last number inside a text") {
  yctest::Doc t;
  t.Set("A1", "2024/1/31");
  t.Set("A2", "2024/2/29");
  t.d.AutoFill(t.sheet(), Rect{0, 0, 1, 0}, Rect{0, 0, 4, 0});
  t.d.Recalc();
  CHECK(t.T("A3") == "2024/3/31");
  CHECK(t.T("A4") == "2024/4/30");
  CHECK(t.T("A5") == "2024/5/31");
  t.Set("B1", "x1y");
  t.Set("C1", "Day1 PM");
  t.Set("D1", "Item99999999999");
  t.d.AutoFill(t.sheet(), Rect{0, 1, 0, 3}, Rect{0, 1, 2, 3});
  t.d.Recalc();
  CHECK(t.T("B2") == "x2y");
  CHECK(t.T("B3") == "x3y");
  CHECK(t.T("C2") == "Day2 PM");
  CHECK(t.T("D2") == "Item99999999999");  // too long a number: it repeats
  t.Set("E1", "1");
  t.Set("E2", "a");
  t.d.AutoFill(t.sheet(), Rect{0, 4, 1, 4}, Rect{0, 4, 4, 4});
  t.d.Recalc();
  CHECK(t.T("E3") == "2");
  CHECK(t.T("E4") == "a");
  CHECK(t.T("E5") == "3");
}

TEST_CASE("Round8: a growth trend follows the fitted curve") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "3");
  t.Set("A3", "4");
  t.d.AutoFill(t.sheet(), Rect{0, 0, 2, 0}, Rect{0, 0, 4, 0}, 10);
  t.d.Recalc();
  CHECK(std::abs(t.N("A4") - 9.15771) < 1e-4);
  CHECK(std::abs(t.N("A5") - 18.31542) < 1e-4);
}

TEST_CASE("Round8: a wildcard replace takes the shortest match unless the pattern ends in *") {
  size_t at = 0, len = 0;
  REQUIRE(yc::engine::WildFind("a*e", "axeye", 0, false, &at, &len));
  CHECK(at == 0);
  CHECK(len == 3);  // axe, not axeye
  REQUIRE(yc::engine::WildFind("a*", "axeye", 0, false, &at, &len));
  CHECK(len == 5);
}

TEST_CASE("Round8: a deleted cell on another sheet keeps the sheet's name in the formula") {
  yctest::Doc t;
  const uint32_t data = t.sheet();
  const uint32_t other = t.d.AddSheet("Other");
  t.d.RenameSheet(data, "Data");
  t.d.SetInput(data, 0, 0, "5");
  t.d.SetInput(other, 0, 0, "=Data!A1+1");
  t.d.Recalc();
  t.d.DeleteRows(data, 0, 1);
  t.d.Recalc();
  CHECK(t.d.GetInputText(other, 0, 0) == "=Data!#REF!+1");
  CHECK(t.d.GetValue(other, 0, 0).IsError());
  // the text reads back
  CHECK(t.d.SetInput(other, 1, 0, "=Data!#REF!+1"));
  t.d.Recalc();
  CHECK(t.d.GetInputText(other, 1, 0) == "=Data!#REF!+1");
  CHECK(t.d.SetInput(other, 2, 0, "=#REF!+1"));
  t.d.Recalc();
  CHECK(t.d.GetInputText(other, 2, 0) == "=#REF!+1");
}

TEST_CASE("Round8: inserting columns that would push data off the sheet is refused; rows grow the sheet instead") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("XFD1", "6");
  t.Set("A2", "=XFD1");
  const uint32_t blocks = t.d.protect_blocks();
  t.d.InsertCols(sh, 2, 1);
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == blocks + 1);
  CHECK(t.d.block_off_sheet());
  CHECK(t.N("XFD1") == 6);  // was lost
  CHECK(t.F("A2") == "=XFD1");
  t.d.InsertCells(sh, Rect{0, 2, 0, 2}, false);  // a cell shifted right on row 1: refused too
  CHECK(t.N("XFD1") == 6);
  t.d.InsertCols(sh, 2, 1, 0);
  CHECK(t.N("XFD1") == 6);
  // rows: the sheet grows past 1,048,576 (YCGrid keeps the data, Excel refuses)
  t.Set("A1048576", "5");
  t.Set("B1", "=A1048576");
  t.d.InsertRows(sh, 1, 1);
  t.d.Recalc();
  CHECK(t.N("A1048577") == 5);
  CHECK(t.F("B1") == "=A1048577");
  CHECK(t.N("B1") == 5);
}

TEST_CASE("Round8: sorting merged cells of different sizes is refused") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "2");
  t.Set("A2", "1");
  t.Set("B1", "x");
  t.d.Merge(sh, Rect{0, 1, 1, 1});  // B1:B2
  const uint32_t blocks = t.d.protect_blocks();
  t.d.Sort(sh, Rect{0, 0, 2, 1}, {yc::engine::SortKey{0, false}}, false);
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == blocks + 1);
  CHECK(t.d.block_merge_size());
  CHECK(t.N("A1") == 2);  // unchanged
  CHECK(t.d.S(sh)->merges->size() == 1);
}

TEST_CASE("Round8: an AutoFilter wildcard matches text only, as Excel's") {
  using yc::core::Value;
  CHECK_FALSE(yc::engine::MatchCriterion("1*", Value::Number(10), "10"));
  CHECK_FALSE(yc::engine::MatchCriterion("=1*", Value::Number(1), "1"));
  CHECK(yc::engine::MatchCriterion("<>1*", Value::Number(10), "10"));
  CHECK(yc::engine::MatchCriterion("1*", Value::String("1x"), "1x"));
  CHECK_FALSE(yc::engine::MatchCriterion("<>1*", Value::String("1x"), "1x"));
  CHECK(yc::engine::MatchCriterion("a*", Value::String("A*C"), "A*C"));  // the text's own * and case
  CHECK(yc::engine::MatchCriterion(">10", Value::Number(11), "11"));
  CHECK(yc::engine::MatchCriterion(">coop", Value::String("co-op"), "co-op"));  // word sort: co-op after coop
}

TEST_CASE("Round8: Text to Columns splits numbers as they show") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "20240115");
  t.Set("A2", "12.5");
  yc::engine::TextToColumnsOptions o;
  o.fixed = true;
  o.breaks = {4, 6};
  t.d.TextToColumns(sh, Rect{0, 0, 0, 0}, 0, 0, o);
  t.d.Recalc();
  CHECK(t.N("A1") == 2024);
  CHECK(t.N("B1") == 1);
  CHECK(t.N("C1") == 15);
  yc::engine::TextToColumnsOptions d;
  d.tab = false;
  d.other = ".";
  t.d.TextToColumns(sh, Rect{1, 0, 1, 0}, 1, 0, d);
  t.d.Recalc();
  CHECK(t.N("A2") == 12);
  CHECK(t.N("B2") == 5);
}

TEST_CASE("Round8: text functions keep to Excel's 32,767 characters and argument rules") {
  yctest::Doc t;
  CHECK(t.Eval("REPT(\"a\",32768)") == "#VALUE!");
  CHECK(t.Eval("LEN(REPT(\"a\",32767))") == "32767");
  CHECK(t.Eval("LEN(REPT(\"ab\",16384))") == "#VALUE!");
  CHECK(t.Eval("SUBSTITUTE(REPT(\"a\",20000),\"a\",\"bb\")") == "#VALUE!");
  CHECK(t.Eval("REPLACE(REPT(\"a\",32767),1,0,\"b\")") == "#VALUE!");
  CHECK(t.Eval("CONCAT(REPT(\"a\",20000),REPT(\"a\",20000))") == "#CALC!");
  CHECK(t.Eval("TEXTJOIN(\",\",,REPT(\"a\",20000),REPT(\"a\",20000))") == "#CALC!");
  CHECK(t.Eval("LEN(REPT(\"a\",20000)&REPT(\"a\",20000))") == "32767");  // & cuts the text
  CHECK(t.Eval("UNICHAR(55296)") == "#N/A");
  CHECK(t.Eval("UNICHAR(57343)") == "#N/A");
  CHECK(t.Eval("UNICHAR(1114111)") == "#N/A");
  CHECK(t.Eval("UNICHAR(1114112)") == "#VALUE!");
  CHECK(t.Eval("UNICODE(UNICHAR(65))") == "65");
  CHECK(t.Eval("TEXTBEFORE(\"a-b\",\"-\",1,2)") == "#VALUE!");
  CHECK(t.Eval("TEXTBEFORE(\"a-b\",\"-\",1,0,2)") == "#VALUE!");
  CHECK(t.Eval("TEXTBEFORE(\"\",\"-\",,,1)") == "#N/A");
  CHECK(t.Eval("TEXTAFTER(\"abc\",\"\",2)") == "abc");
  CHECK(t.Eval("TEXTBEFORE(\"a-b-c\",\"-\",2)") == "a-b");
  CHECK(t.Eval("NUMBERVALUE(\"1.5\",\".\",\".\")") == "#VALUE!");
  CHECK(t.Eval("NUMBERVALUE(\"1.5\",\"\")") == "#VALUE!");
  CHECK(t.Eval("NUMBERVALUE(\"%\")") == "0");
  CHECK(t.Eval("NUMBERVALUE(\"1.234,5\",\",\",\".\")") == "1234.5");
  CHECK(t.Eval("TEXTSPLIT(\"\",\",\")") == "#VALUE!");
  CHECK(t.Eval("TEXTJOIN(\"|\",,TEXTSPLIT(\"a,b\",\",\"))") == "a|b");
}

TEST_CASE("Round8: LAMBDA calls check their arguments as Excel's") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  CHECK(t.Eval("LAMBDA(x,y,x+N(y))(1)") == "#VALUE!");
  CHECK(t.Eval("LAMBDA(x,x)()") == "#VALUE!");
  CHECK(t.Eval("LAMBDA(x,y,IF(ISOMITTED(y),x*100,x+y))(1,)") == "100");  // an empty argument is omitted
  CHECK(t.Eval("TEXTJOIN(\",\",,MAP(A1:A3,LAMBDA(x,y,x)))") == "#N/A");
  CHECK(t.Eval("BYROW(A1:A3,LAMBDA(r,r*{1,2}))") == "#CALC!");
  CHECK(t.Eval("TEXTJOIN(\",\",,BYROW(A1:A3,LAMBDA(r,r*2)))") == "2,4,6");
}

TEST_CASE("Round8: ROWS and COLUMNS pass errors on, SHEETS counts a 3-D span") {
  yctest::Doc t;
  CHECK(t.Eval("ROWS(Nope)") == "#NAME?");
  CHECK(t.Eval("COLUMNS(1/0)") == "#DIV/0!");
  CHECK(t.Eval("ROWS(#N/A)") == "#N/A");
  CHECK(t.Eval("ROWS(A1:A3)") == "3");
  t.d.AddSheet("Two");
  t.d.AddSheet("Three");
  CHECK(t.Eval("SHEETS(Two:Three!A1)") == "2");
  CHECK(t.Eval("SHEETS(A1)") == "1");
}

TEST_CASE("Round8: a copied sheet's name counts on and keeps to 31 characters") {
  yctest::Doc t;
  const uint32_t q = t.sheet();
  t.d.RenameSheet(q, "Q");
  const uint32_t c1 = t.d.CopySheet(q, -1);
  CHECK(t.d.S(c1)->name == "Q (2)");
  const uint32_t c2 = t.d.CopySheet(c1, -1);
  CHECK(t.d.S(c2)->name == "Q (3)");  // was Q (2) (2)
  const uint32_t r = t.d.AddSheet("R(2)");
  CHECK(t.d.S(t.d.CopySheet(r, -1))->name == "R(3)");
  const uint32_t l = t.d.AddSheet(std::string(30, 'x'));
  const std::string ln = t.d.S(t.d.CopySheet(l, -1))->name;
  CHECK(ln == std::string(27, 'x') + " (2)");
  CHECK(ln.size() == 31);
}

TEST_CASE("Round8: a copied sheet's own names and the workbook names that point at it follow the copy") {
  yctest::Doc t;
  const uint32_t data = t.sheet();
  t.d.RenameSheet(data, "Data");
  t.d.SetInput(data, 0, 0, "5");
  t.d.SetInput(data, 2, 0, "7");
  REQUIRE(t.d.AddName("Loc", "=Data!$A$1", static_cast<int32_t>(data)));
  REQUIRE(t.d.AddName("Glob", "=Data!$A$3"));
  t.d.SetInput(data, 0, 1, "=Data!Loc");
  t.d.SetInput(data, 1, 1, "=Glob");
  t.d.Recalc();
  const uint32_t copy = t.d.CopySheet(data, -1);
  t.d.SetInput(copy, 0, 0, "50");
  t.d.SetInput(copy, 2, 0, "70");
  t.d.Recalc();
  CHECK(t.d.GetInputText(copy, 0, 1) == "='Data (2)'!Loc");
  CHECK(t.d.GetValue(copy, 0, 1).AsDouble() == 50);
  CHECK(t.d.GetValue(copy, 1, 1).AsDouble() == 70);  // the copy's own Glob
  CHECK(t.d.GetValue(data, 1, 1).AsDouble() == 7);   // the workbook's Glob still
}

TEST_CASE("Round8: a 3-D reference shrinks when one of its end sheets moves out of the span") {
  yctest::Doc t;
  const uint32_t data = t.sheet();
  t.d.RenameSheet(data, "Data");
  const uint32_t other = t.d.AddSheet("Other");
  const uint32_t s3 = t.d.AddSheet("S3");
  const uint32_t s4 = t.d.AddSheet("S4");
  t.d.SetInput(other, 0, 0, "1");
  t.d.SetInput(s3, 0, 0, "2");
  t.d.SetInput(s4, 0, 0, "4");
  t.d.SetInput(data, 0, 0, "=SUM(Other:S4!A1)");
  t.d.Recalc();
  CHECK(t.N("A1") == 7);
  t.d.MoveSheet(s4, 0);  // S4, Data, Other, S3
  t.d.Recalc();
  CHECK(t.F("A1", data) == "=SUM('Other:S3'!A1)");  // S3 reads as a cell: quoted
  CHECK(t.N("A1", data) == 3);
  t.d.MoveSheet(other, 3);  // S4, Data, S3, Other: the start passed the end
  t.d.Recalc();
  CHECK(t.F("A1", data) == "=SUM('S3'!A1)");
  CHECK(t.N("A1", data) == 2);
}

TEST_CASE("Round8: defined names Excel refuses") {
  yctest::Doc t;
  std::string err;
  CHECK_FALSE(t.d.AddName("TRUE", "=1", -1, &err));
  CHECK_FALSE(t.d.AddName("false", "=1", -1, &err));
  CHECK_FALSE(t.d.AddName("_xlfn.X", "=1", -1, &err));
  CHECK_FALSE(t.d.AddName("\\y", "=1", -1, &err));
  CHECK(t.d.AddName("TRUEX", "=1", -1, &err));
  CHECK(t.d.AddName("\\yz", "=1", -1, &err));
}

TEST_CASE("Round8: deleting the row of a table's header alone is refused") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "Name");
  t.Set("B1", "Qty");
  t.Set("A2", "a");
  t.Set("B2", "1");
  t.Set("A3", "b");
  t.Set("B3", "2");
  std::string err;
  REQUIRE(t.d.AddTable(sh, Rect{0, 0, 2, 1}, true, "TableStyleMedium2", &err) != 0);
  const uint32_t blocks = t.d.protect_blocks();
  t.d.DeleteRows(sh, 0, 1);
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == blocks + 1);
  CHECK(t.d.block_table_shift());
  CHECK(t.T("A1") == "Name");  // was deleted: "a" became the header
  t.d.DeleteCells(sh, Rect{0, 0, 0, 1}, true);
  CHECK(t.T("A1") == "Name");
  t.d.DeleteRows(sh, 0, 3);  // the whole table with its rows: allowed
  t.d.Recalc();
  CHECK(t.d.S(sh)->tables->empty());
}

TEST_CASE("Round8: a number shows its 15 digits to edit, so F2 then Enter changes nothing") {
  yctest::Doc t;
  t.d.SetValue(t.sheet(), 0, 0, yc::core::Value::Number(123456789012.0));
  t.d.SetValue(t.sheet(), 1, 0, yc::core::Value::Number(250000000000.75));
  t.d.SetValue(t.sheet(), 2, 0, yc::core::Value::Number(1.2345678901234567e-15));
  t.d.SetValue(t.sheet(), 3, 0, yc::core::Value::Number(1e20));
  t.d.SetValue(t.sheet(), 4, 0, yc::core::Value::Number(1e21));
  t.d.SetValue(t.sheet(), 5, 0, yc::core::Value::Number(2.5e-7));
  t.d.SetValue(t.sheet(), 6, 0, yc::core::Value::Number(-1234567890123456.0));
  CHECK(t.F("A1") == "123456789012");  // was 1.23457E+11, and Enter wrote 123457000000
  CHECK(t.F("A2") == "250000000000.75");
  CHECK(t.F("A3") == "1.23456789012346E-15");
  CHECK(t.F("A4") == "100000000000000000000");
  CHECK(t.F("A5") == "1E+21");
  CHECK(t.F("A6") == "0.00000025");
  CHECK(t.F("A7") == "-1234567890123460");
  CHECK(yc::formula::EditNumberText(0.1 + 0.2) == "0.3");
  // F2, Enter: the same number, no new format
  t.Set("A1", t.F("A1"));
  CHECK(t.N("A1") == 123456789012.0);
  CHECK(t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(0, 0)).numfmt == 0);  // still General (it became 0.00E+00)
}

TEST_CASE("Round8: a typed entry's format replaces a built-in format of another kind") {
  yctest::Doc t;
  SetFmt(t, "A1", "m/d/yyyy");
  t.Set("A1", "5%");
  CHECK(t.T("A1") == "5%");  // was 1/0/1900
  SetFmt(t, "A2", "0%");
  t.Set("A2", "2024/1/7");
  CHECK(t.T("A2") == "2024/1/7");  // was 4529800%
  SetFmt(t, "A3", "h:mm");
  t.Set("A3", "2024/1/7");
  CHECK(t.T("A3") == "2024/1/7");
  SetFmt(t, "A4", "0.00%");
  t.Set("A4", "5%");
  CHECK(t.T("A4") == "5.00%");  // the same kind: the cell's own stays
  SetFmt(t, "A5", "0.0\" bp\"");
  t.Set("A5", "5%");
  CHECK(t.T("A5") == "0.1 bp");  // a custom format stays
  SetFmt(t, "A6", "0%");
  t.Set("A6", "1,234");
  CHECK(t.T("A6") == "123400%");  // a plain number keeps any format
}

TEST_CASE("Round8: a/b typed into a number format is a fraction") {
  yctest::Doc t;
  SetFmt(t, "A1", "0.00");
  t.Set("A1", "1/2");
  CHECK(t.N("A1") == 0.5);  // was January 2
  CHECK(t.T("A1") == "0.50");
  SetFmt(t, "A2", "# ??/32");
  t.Set("A2", "3/32");
  CHECK(t.N("A2") == 0.09375);  // was the text 3/32
  t.Set("A3", "1/2");
  CHECK(t.N("A3") > 40000);  // General: still a date
  SetFmt(t, "A4", "m/d/yyyy");
  t.Set("A4", "1/2");
  CHECK(t.N("A4") > 40000);
}

TEST_CASE("Round8: criteria compare numbers at 15 digits, lookups exactly") {
  yctest::Doc t;
  t.Set("A1", "=0.1*3");
  t.Set("B2", "=4.35*100");
  CHECK(t.Eval("COUNTIF(A1,0.3)") == "1");
  CHECK(t.Eval("COUNTIF(A1,\"0.3\")") == "1");
  CHECK(t.Eval("COUNTIF(A1,\">0.3\")") == "0");
  CHECK(t.Eval("COUNTIF(A1,\"<>0.3\")") == "0");
  CHECK(t.Eval("COUNTIF(A1,\">=0.3\")") == "1");
  CHECK(t.Eval("COUNTIFS(B2,435)") == "1");
  CHECK(t.Eval("MATCH(0.3,A1,0)") == "#N/A");
  CHECK(t.Eval("MATCH(0.3,A1,1)") == "#N/A");
  CHECK(t.Eval("VLOOKUP(0.3,A1,1,FALSE)") == "#N/A");
  CHECK(t.Eval("XLOOKUP(0.3,A1,A1,\"none\")") == "none");
  CHECK(t.Eval("MATCH(435,B2,0)") == "#N/A");
  CHECK(t.Eval("A1=0.3") == "TRUE");  // the operator compares at 15 digits too
}

TEST_CASE("Round8: INT reads its number at 15 digits") {
  yctest::Doc t;
  CHECK(t.Eval("INT(4.35*100)") == "435");
  CHECK(t.Eval("INT(1.005*1000)") == "1005");
  CHECK(t.Eval("INT(0.29*100)") == "29");
  CHECK(t.Eval("INT((0.1+0.7)*10)") == "8");
  CHECK(t.Eval("INT(-2.5)") == "-3");
  CHECK(t.Eval("INT(434.99999999999994)") == "434");  // a typed constant keeps its 15 digits: 434.999999999999
}
