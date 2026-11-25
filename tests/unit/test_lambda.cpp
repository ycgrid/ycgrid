// SPDX-License-Identifier: Apache-2.0
// LET, LAMBDA and the functions that call LAMBDAs
#include <catch2/catch_test_macros.hpp>

#include "formula/parser.h"
#include "io/ycg.h"
#include "test_helpers.h"

namespace {
bool Parses(yctest::Doc& t, const std::string& text) { return yc::formula::Parse(text, yc::core::CellPos{0, 0}, &t.d).ok; }

// Q1:Q3 numbers, R1:R3 texts with R2 empty, U1:U3 numbers with U2 empty: the cells of the measurements below
void Cells(yctest::Doc& t) {
  t.Set("Q1", "5");
  t.Set("Q2", "7");
  t.Set("Q3", "9");
  t.Set("R1", "x");
  t.Set("R3", "z");
  t.Set("U1", "4");
  t.Set("U3", "6");
}
}  // namespace

TEST_CASE("LET") {
  yctest::Doc t;
  CHECK(t.Eval("LET(x,2,y,x*3,x+y)") == "8");
  CHECK(t.Eval("LET(x,1,LET(x,10,x)+x)") == "11");  // inner variables shadow outer ones
  t.Set("A1", "5");
  CHECK(t.Eval("LET(v,A1*2,v+A1)") == "15");
  t.Set("B1", "=LET(r,SEQUENCE(3),r*r)");
  CHECK(t.N("B3") == 9);
  CHECK_FALSE(t.Set("Z1", "=LET(1,2,3)"));  // a name goes first: refused as it is typed (Excel 16, measured)
  // a LET variable isn't a defined name: the formula isn't volatile
  t.Set("C1", "=LET(k,A1,k+1)");
  t.Set("A1", "7");
  CHECK(t.N("C1") == 8);
}

TEST_CASE("LAMBDA calls and defined names") {
  yctest::Doc t;
  CHECK(t.Eval("LAMBDA(x,y,x*y)(3,4)") == "12");
  CHECK(t.Eval("LET(sq,LAMBDA(n,n*n),sq(5)+sq(2))") == "29");
  CHECK(t.Eval("LAMBDA(x,x)") == "#CALC!");
  CHECK(t.Eval("LAMBDA(x,x)(1,2)") == "#VALUE!");
  CHECK(t.Eval("LAMBDA(a,b,IF(ISOMITTED(b),a*100,a+b))(1,)") == "100");
  CHECK(t.Eval("LAMBDA(a,b,IF(ISOMITTED(b),a*100,a+b))(1,2)") == "3");
  // captured variables (closures)
  CHECK(t.Eval("LET(k,10,f,LAMBDA(x,x+k),LET(k,99,f(1)))") == "11");
  // a named LAMBDA, including recursion
  std::string err;
  REQUIRE(t.d.AddName("DOUBLE", "=LAMBDA(x,x*2)", -1, &err));
  REQUIRE(t.d.AddName("FACTR", "=LAMBDA(n,IF(n<=1,1,n*FACTR(n-1)))", -1, &err));
  CHECK(t.Eval("DOUBLE(21)") == "42");
  CHECK(t.Eval("FACTR(6)") == "720");
  t.Set("A1", "=DOUBLE(SEQUENCE(3))");
  CHECK(t.N("A3") == 6);
  CHECK(t.F("Z1000") == "=FACTR(6)");
  t.Set("B1", "=LAMBDA(x,x+1)(41)");
  CHECK(t.F("B1") == "=LAMBDA(x,x+1)(41)");
  CHECK(t.N("B1") == 42);
}

TEST_CASE("MAP REDUCE SCAN BYROW BYCOL MAKEARRAY") {
  yctest::Doc t;
  CHECK(t.Eval("SUM(MAP(SEQUENCE(4),LAMBDA(x,x*10)))") == "100");
  CHECK(t.Eval("SUM(MAP({1,2},{10,20},LAMBDA(a,b,a*b)))") == "50");
  CHECK(t.Eval("REDUCE(0,SEQUENCE(10),LAMBDA(acc,v,acc+v))") == "55");
  CHECK(t.Eval("REDUCE(1,{2,3,4},LAMBDA(a,v,a*v))") == "24");
  CHECK(t.Eval("INDEX(SCAN(0,{1,2,3},LAMBDA(a,v,a+v)),3)") == "6");
  CHECK(t.Eval("ROWS(VSTACK(REDUCE(\"x\",{1,2},LAMBDA(a,v,VSTACK(a,v)))))") == "3");
  t.Set("A1", "=BYROW(SEQUENCE(3,2),LAMBDA(r,SUM(r)))");
  CHECK(t.N("A1") == 3);
  CHECK(t.N("A3") == 11);
  t.Set("C1", "=BYCOL(SEQUENCE(3,2),LAMBDA(c,MAX(c)))");
  CHECK(t.N("C1") == 5);
  CHECK(t.N("D1") == 6);
  t.Set("F1", "=MAKEARRAY(2,3,LAMBDA(r,c,r*10+c))");
  CHECK(t.N("H2") == 23);
  CHECK(t.Eval("MAP(SEQUENCE(2),LAMBDA(x,SEQUENCE(2)))") == "#CALC!");
  CHECK(t.Eval("MAP({1,2},5)") == "#VALUE!");
}

// Everything below marked "measured" is Excel 16's result for that formula

TEST_CASE("LAMBDA: a parameter in brackets is optional") {
  yctest::Doc t;
  // measured
  CHECK(t.Eval("LAMBDA(x,[y],[z],x+ISOMITTED(y)+ISOMITTED(z))(1)") == "3");
  CHECK(t.Eval("LAMBDA(x,[y],[z],x+ISOMITTED(y)+ISOMITTED(z))(1,,3)") == "2");
  CHECK(t.Eval("LAMBDA(x,[y],[z],x+ISOMITTED(y)+ISOMITTED(z))(1,2,3,4)") == "#VALUE!");  // more than it takes
  CHECK(t.Eval("LAMBDA(x,[y],x)()") == "#VALUE!");                                         // a required one left out
  CHECK(t.Eval("LAMBDA([y],IF(ISOMITTED(y),\"none\",y))()") == "none");
  CHECK(t.Eval("LAMBDA([y],IF(ISOMITTED(y),\"none\",y))(7)") == "7");
  CHECK(t.Eval("LAMBDA([_x],_x)(7)") == "7");
  CHECK(t.Eval("LET(f,LAMBDA(a,[b],a+IF(ISOMITTED(b),10,b)),f(1)+f(1,2))") == "14");
  CHECK(t.Eval("LET(f,LAMBDA(a,[b],a+IF(ISOMITTED(b),10,b)),f(1,))") == "11");
  CHECK(t.Eval("LAMBDA(a,[b],LAMBDA(c,[d],ISOMITTED(d)+ISOMITTED(b))(1))(1)") == "2");
  // one left out reads as an argument left empty (measured)
  CHECK(t.Eval("LET(f,LAMBDA(a,[b],a+b),f(1))") == "1");
  CHECK(t.Eval("LET(f,LAMBDA(a,[b],a&b),f(\"p\"))") == "p");
  CHECK(t.Eval("LAMBDA(a,[b],ISBLANK(b))(1)") == "TRUE");
  CHECK(t.Eval("LAMBDA(a,[b],ISNUMBER(b))(1)") == "FALSE");
  CHECK(t.Eval("LAMBDA(a,[b],COUNT(a,b))(1)") == "2");
  CHECK(t.Eval("LAMBDA(a,[b],IF(b,\"t\",\"f\"))(1)") == "f");
  // an empty cell passed isn't left out (measured)
  CHECK(t.Eval("LAMBDA(a,[b],ISOMITTED(b))(1,A8)") == "FALSE");
  CHECK(t.Eval("LAMBDA(a,[b],ISOMITTED(a))(A8)") == "FALSE");
  // the functions that call a LAMBDA give every parameter it requires, and may leave out the optional ones (measured)
  CHECK(t.Eval("SUM(MAP({1,2},{3,4},LAMBDA(x,[y],x+y)))") == "10");
  CHECK(t.Eval("SUM(MAP({1,2},LAMBDA(x,[y],x)))") == "#N/A");
  CHECK(t.Eval("REDUCE(0,{1,2},LAMBDA(a,[x],a+1))") == "2");
  CHECK(t.Eval("SUM(BYROW({1;2},LAMBDA([r],SUM(r))))") == "3");
  CHECK(t.Eval("SUM(MAKEARRAY(2,2,LAMBDA(r,[c],r*c)))") == "9");
  CHECK(t.Eval("SUM(MAKEARRAY(2,2,LAMBDA(r,c,[d],r*c)))") == "#N/A");
  // a defined name
  std::string err;
  REQUIRE(t.d.AddName("ADDOPT", "=LAMBDA(a,[b],IF(ISOMITTED(b),a,a+b))", -1, &err));
  CHECK(t.Eval("ADDOPT(5)") == "5");
  CHECK(t.Eval("ADDOPT(5,2)") == "7");
  CHECK(t.Eval("ADDOPT()") == "#VALUE!");
}

TEST_CASE("LAMBDA: what Excel refuses in its parameters") {
  yctest::Doc t;
  // an optional parameter before a required one (measured)
  CHECK_FALSE(Parses(t, "LAMBDA([x],y,x+y)(1,2)"));
  CHECK_FALSE(Parses(t, "LAMBDA([x],y,ISOMITTED(x))(,2)"));
  CHECK_FALSE(Parses(t, "LAMBDA(a,[x],y,[z],a)(1,2,3)"));
  CHECK_FALSE(t.Set("C1", "=LAMBDA([x],y,y)(2)"));
  CHECK(t.F("C1").empty());
  // one name twice, in brackets too (measured)
  CHECK_FALSE(Parses(t, "LAMBDA([y],[Y],y)(7)"));
  CHECK_FALSE(Parses(t, "LAMBDA(x,[x],x)(7)"));
  // every one after the first optional is optional
  CHECK(Parses(t, "LAMBDA([x],[y],1)()"));
  CHECK(Parses(t, "LAMBDA(a,b,[x],[y],a+b)(1,2)"));
}

TEST_CASE("LAMBDA: the brackets of an optional parameter are written back") {
  yctest::Doc t;
  const std::string typed = "=LAMBDA(x,[y],IF(ISOMITTED(y),x,x+y))(1)";
  REQUIRE(t.Set("B1", typed));
  CHECK(t.F("B1") == typed);
  CHECK(t.N("B1") == 1);
  REQUIRE(t.Set("B2", "=LAMBDA(x,[y],IF(ISOMITTED(y),x,x+y))(1,2)"));
  CHECK(t.N("B2") == 3);
  // the two formulas differ only in a parameter being optional: they aren't one shared formula
  REQUIRE(t.Set("B3", "=LAMBDA(x,[y],x)(1)"));
  REQUIRE(t.Set("B4", "=LAMBDA(x,y,x)(1)"));
  CHECK(t.N("B3") == 1);
  CHECK(t.T("B4") == "#VALUE!");
  CHECK(t.F("B3") == "=LAMBDA(x,[y],x)(1)");
  CHECK(t.F("B4") == "=LAMBDA(x,y,x)(1)");
  // saved and opened
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  yc::engine::Document back(std::move(res.wb));
  back.Recalc();
  const uint32_t s0 = back.wb().sheets[0]->id;
  CHECK(back.GetInputText(s0, 0, 1) == typed);
  CHECK(back.GetValue(s0, 0, 1).AsDouble() == 1);
  CHECK(back.GetValue(s0, 1, 1).AsDouble() == 3);
  CHECK(back.GetInputText(s0, 2, 1) == "=LAMBDA(x,[y],x)(1)");
  CHECK(back.GetValue(s0, 3, 1).IsError());
}

TEST_CASE("LAMBDA: an optional parameter in a table's cell, beside its column references") {
  yctest::Doc t;
  t.Set("A1", "Qty");
  t.Set("B1", "Price");
  t.Set("C1", "Total");
  t.Set("D1", "Scaled");
  t.Set("A2", "1");
  t.Set("B2", "10");
  t.Set("A3", "2");
  t.Set("B3", "20");
  std::string err;
  REQUIRE(t.d.AddTable(t.sheet(), yc::core::Rect{0, 0, 2, 3}, true, "TableStyleMedium2", &err) != 0);
  t.d.Recalc();
  // a bracket alone is still the table's column
  REQUIRE(t.Set("C2", "=[@Qty]*[@Price]"));
  CHECK(t.F("C2") == "=[@Qty]*[@Price]");
  CHECK(t.N("C2") == 10);
  CHECK(t.Eval("SUM(Table1[Qty])") == "3");
  // and before a LAMBDA's calculation it is an optional parameter; the calculation reads the columns
  const std::string typed = "=LAMBDA(k,[m],[@Qty]*IF(ISOMITTED(m),k,m))(3)";
  REQUIRE(t.Set("D2", typed));
  CHECK(t.F("D2") == typed);
  CHECK(t.N("D2") == 3);
  CHECK(t.Eval("LAMBDA(k,[m],SUM(Table1[Qty])*IF(ISOMITTED(m),k,m))(3,5)") == "15");
}

TEST_CASE("REDUCE and SCAN without an initial value start from the first element") {
  yctest::Doc t;
  Cells(t);
  // measured: the LAMBDA is first called for the second element
  CHECK(t.Eval("REDUCE(,Q1:Q3,LAMBDA(a,x,a*x))") == "315");
  CHECK(t.Eval("SUM(SCAN(,Q1:Q3,LAMBDA(a,x,a*x)))") == "355");
  CHECK(t.Eval("REDUCE(,{2,3,4},LAMBDA(a,x,a*x))") == "24");
  CHECK(t.Eval("CONCAT(SCAN(,{2,3,4},LAMBDA(a,x,a*x))&\"|\")") == "2|6|24|");
  CHECK(t.Eval("REDUCE(,{2,3,4},LAMBDA(a,x,a+x^2))") == "27");
  CHECK(t.Eval("REDUCE(,R1:R3,LAMBDA(a,x,a&\"-\"&x))") == "x--z");
  CHECK(t.Eval("REDUCE(,5,LAMBDA(a,x,100))") == "5");
  CHECK(t.Eval("SCAN(,5,LAMBDA(a,x,100))") == "5");
  CHECK(t.Eval("REDUCE(,Q1:Q3,LAMBDA(a,x,a+x))") == "21");
  CHECK(t.Eval("SUM(SCAN(,Q1:Q3,LAMBDA(a,x,a+x)))") == "38");
  CHECK(t.Eval("CONCAT(SCAN(,U1:U3,LAMBDA(a,x,a+x))&\"|\")") == "4|4|10|");
  // an initial value given as an empty argument's result is one: the LAMBDA is called for every element (measured)
  CHECK(t.Eval("REDUCE(IF(TRUE,,),{2,3},LAMBDA(a,x,a+x))") == "5");
  CHECK(t.Eval("REDUCE(\"\",{2,3},LAMBDA(a,x,a&x))") == "23");
  // the elements of a range are references, and REDUCE's first accumulator is the first of them; SCAN's is its
  // value (measured)
  CHECK(t.Eval("REDUCE(,Q1:Q3,LAMBDA(a,x,N(ISREF(x))))") == "1");
  CHECK(t.Eval("REDUCE(,Q1:Q3,LAMBDA(a,x,ROW(x)))") == "3");
  CHECK(t.Eval("ISREF(REDUCE(,Q1:Q1,LAMBDA(a,x,a*x)))") == "TRUE");
  CHECK(t.Eval("REDUCE(,Q1:Q2,LAMBDA(a,x,N(ISREF(a))))") == "1");
  CHECK(t.Eval("SUM(SCAN(,Q1:Q2,LAMBDA(a,x,N(ISREF(a)))))") == "5");
  CHECK(t.Eval("REDUCE(,U2:U3,LAMBDA(a,x,COUNT(a,x)))") == "1");  // U2 is an empty cell, not a 0
  CHECK(t.Eval("REDUCE(,Q1:Q2,LAMBDA(a,x,COUNT(a,x)))") == "2");
  CHECK(t.Eval("ISREF(REDUCE(0,R2,LAMBDA(a,x,x)))") == "TRUE");
}

TEST_CASE("An empty result of a LAMBDA stays empty") {
  yctest::Doc t;
  Cells(t);
  // measured: in the arrays MAP, BYROW, BYCOL, SCAN and MAKEARRAY make
  CHECK(t.Eval("CONCAT(MAP({1,2},LAMBDA(x,IF(TRUE,,)))&\"|\")") == "||");
  CHECK(t.Eval("COUNT(MAP({1,2},LAMBDA(x,IF(TRUE,,))))") == "0");
  CHECK(t.Eval("COUNTA(MAP({1,2},LAMBDA(x,IF(TRUE,,))))") == "2");
  CHECK(t.Eval("CONCAT(MAP(R1:R3,LAMBDA(x,x))&\"|\")") == "x||z|");
  CHECK(t.Eval("COUNT(MAP(U1:U3,LAMBDA(x,x)))") == "2");
  CHECK(t.Eval("MIN(MAP(U1:U3,LAMBDA(x,x)))") == "4");
  CHECK(t.Eval("CONCAT(BYROW(R1:R3,LAMBDA(x,INDEX(x,1,1)))&\"|\")") == "x||z|");
  CHECK(t.Eval("CONCAT(SCAN(\"\",R1:R3,LAMBDA(a,x,x))&\"|\")") == "x||z|");
  CHECK(t.Eval("CONCAT(SCAN(,U2:U3,LAMBDA(a,x,a+x))&\"|\")") == "|6|");
  CHECK(t.Eval("CONCAT(MAKEARRAY(1,2,LAMBDA(r,c,R2))&\"|\")") == "||");
  CHECK(t.Eval("CONCAT(MAKEARRAY(1,2,LAMBDA(r,c,IF(TRUE,,)))&\"|\")") == "||");
  CHECK(t.Eval("COUNTA(MAKEARRAY(1,2,LAMBDA(r,c,R2)))") == "2");
  // and as REDUCE's result (measured)
  CHECK(t.Eval("ISBLANK(REDUCE(0,{2,3},LAMBDA(a,x,IF(TRUE,,))))") == "TRUE");
  CHECK(t.Eval("REDUCE(0,{2,3},LAMBDA(a,x,IF(TRUE,,)))&\"|\"") == "|");
  CHECK(t.Eval("REDUCE(,{2,3},LAMBDA(a,x,IF(TRUE,,)))&\"|\"") == "|");
  CHECK(t.Eval("ISBLANK(REDUCE(,R2,LAMBDA(a,x,a)))") == "TRUE");
  // a cell shows it as 0, as any empty result (measured for REDUCE)
  CHECK(t.Eval("REDUCE(0,{2,3},LAMBDA(a,x,IF(TRUE,,)))") == "0");
  CHECK(t.Eval("TYPE(REDUCE(0,{2,3},LAMBDA(a,x,IF(TRUE,,))))") == "1");
  REQUIRE(t.Set("W1", "=MAP({1,2},LAMBDA(x,IF(TRUE,,)))"));
  CHECK(t.T("W1") == "0");
  CHECK(t.T("X1") == "0");
}

TEST_CASE("TAKE and DROP of a range give a range, and take any count") {
  yctest::Doc t;
  Cells(t);
  // measured
  CHECK(t.Eval("ISREF(TAKE(Q1:Q3,5))") == "TRUE");
  CHECK(t.Eval("ROWS(TAKE(Q1:Q3,5))") == "3");
  CHECK(t.Eval("ISREF(DROP(Q1:Q3,1))") == "TRUE");
  CHECK(t.Eval("ISREF(TAKE(Q1:Q3,,1))") == "TRUE");
  CHECK(t.Eval("ISREF(TAKE({1,2,3},,2))") == "FALSE");
  CHECK(t.Eval("ISREF(TAKE(+Q1:Q3,2))") == "FALSE");
  CHECK(t.Eval("TAKE(Q1:Q3,0)") == "#CALC!");
  CHECK(t.Eval("DROP(Q1:Q3,3)") == "#CALC!");
  CHECK(t.Eval("ROWS(DROP(Q1:Q3,-1))") == "2");
  CHECK(t.Eval("SUM(Q1:TAKE(Q1:Q3,-1))") == "21");
  CHECK(t.Eval("SUM(TAKE(Q1:Q3,-1):DROP(Q1:Q3,2))") == "9");
  CHECK(t.Eval("CELL(\"address\",DROP(Q1:R3,1,1))") == "$R$2");
  CHECK(t.Eval("COLUMN(TAKE(Q1:R3,,-1))") == "18");
  CHECK(t.Eval("COUNTA(TAKE(R1:R3,2))") == "1");  // R2 is an empty cell of the range, not an empty element
  CHECK(t.Eval("COUNTBLANK(TAKE(R1:R3,2))") == "1");
  CHECK(t.Eval("ROWS(TAKE(Q:Q,5))") == "5");
  CHECK(t.Eval("ROW(DROP(Q:Q,1048575))") == "1048576");
  // a count past the range is the whole range, however large (measured)
  CHECK(t.Eval("ISREF(TAKE(Q1:Q3,1E+300))") == "TRUE");
  CHECK(t.Eval("ROWS(TAKE(Q1:Q3,5000000000))") == "3");
  CHECK(t.Eval("ROWS(TAKE(Q1:Q3,-1E+300))") == "3");
  CHECK(t.Eval("COLUMNS(TAKE({1,2,3},,1E+300))") == "3");
  CHECK(t.Eval("ROWS(TAKE({1;2;3},1E+13))") == "3");
  CHECK(t.Eval("SUM(TAKE({1;2;3},2^32+1))") == "6");
  CHECK(t.Eval("SUM(TAKE({1;2;3},-(2^32+1)))") == "6");
  CHECK(t.Eval("ROWS(DROP(Q1:Q3,,-1E+300))") == "#CALC!");
  CHECK(t.Eval("SUM(DROP({1;2;3},2^32+1))") == "#CALC!");
  CHECK(t.Eval("IFERROR(ROWS(DROP(Q1:Q3,1E+300)),\"err\")") == "err");
}

TEST_CASE("An index far past the array is refused, not read") {
  yctest::Doc t;
  Cells(t);
  // measured: #VALUE!, as for any row or column the array doesn't have. A double this large was cast to an integer
  // as it was, and rows outside the array were read
  CHECK(t.Eval("CHOOSEROWS({1;2;3},1E+300)") == "#VALUE!");
  CHECK(t.Eval("CHOOSEROWS({1;2;3},-1E+300)") == "#VALUE!");
  CHECK(t.Eval("CHOOSECOLS({1,2,3},-1E+300)") == "#VALUE!");
  CHECK(t.Eval("SUM(CHOOSEROWS({1;2;3},1,1E+300))") == "#VALUE!");
  CHECK(t.Eval("CHOOSEROWS({1;2;3},2^63)") == "#VALUE!");
  CHECK(t.Eval("CHOOSEROWS({1;2;3},-2^63)") == "#VALUE!");
  CHECK(t.Eval("CHOOSEROWS({1;2;3},2^31)") == "#VALUE!");
  CHECK(t.Eval("CHOOSEROWS({1;2;3},2^32+1)") == "#VALUE!");
  CHECK(t.Eval("CHOOSECOLS({1,2,3},2^32+1)") == "#VALUE!");
  CHECK(t.Eval("SORT({3;1;2},1E+300)") == "#VALUE!");
  CHECK(t.Eval("SORT({3;1;2},1,1E+300)") == "#VALUE!");
  CHECK(t.Eval("SORT({3;1;2},-1E+300)") == "#VALUE!");
  CHECK(t.Eval("INDEX({1;2;3},1E+300)") == "#REF!");
  CHECK(t.Eval("INDEX({1;2;3},2^32+1)") == "#REF!");
  CHECK(t.Eval("INDEX({1;2;3},2^31)") == "#REF!");
  CHECK(t.Eval("INDEX(Q1:Q3,2^32+1)") == "#REF!");
  // an index is cut to a whole number (measured)
  CHECK(t.Eval("CHOOSEROWS({1;2;3},1.9)") == "1");
  CHECK(t.Eval("CHOOSEROWS({1;2;3},0.9)") == "#VALUE!");
  CHECK(t.Eval("CHOOSEROWS({1;2;3},-0.9)") == "#VALUE!");
  CHECK(t.Eval("SUM(CHOOSEROWS({1;2;3},-1.9))") == "3");
}

TEST_CASE("COUNTA counts the empty elements of an array, and not the empty cells of a range") {
  yctest::Doc t;
  Cells(t);
  // measured
  CHECK(t.Eval("COUNTA(R1:R3)") == "2");
  CHECK(t.Eval("COUNTA(UNIQUE(R1:R3))") == "3");
  CHECK(t.Eval("COUNTA(FILTER(R1:R3,Q1:Q3>0))") == "3");
  CHECK(t.Eval("COUNTA(IF({1,0},R2,R2))") == "2");
  CHECK(t.Eval("COUNTA(TOCOL(R1:R3))") == "3");
  CHECK(t.Eval("COUNTA(TOCOL(R1:R3,1))") == "2");
  CHECK(t.Eval("COUNTA(TRANSPOSE(R1:R3))") == "3");
  CHECK(t.Eval("COUNTA(VSTACK(R1:R3,U1:U3))") == "6");
  CHECK(t.Eval("COUNTA(MAP(R1:R3,LAMBDA(x,x)))") == "3");
  // what hands back the range itself still counts cells
  CHECK(t.Eval("COUNTA(IF(TRUE,R1:R3))") == "2");
  CHECK(t.Eval("COUNTA(INDEX(R1:R3,0))") == "2");
  CHECK(t.Eval("COUNTA(CHOOSE(1,R1:R3))") == "2");
  CHECK(t.Eval("COUNTA(LET(a,R1:R3,a))") == "2");
  CHECK(t.Eval("COUNTA(R1:R3,+R1:R3)") == "5");
  // the other functions skip an array's empty elements (measured)
  CHECK(t.Eval("COUNT(+U1:U3)") == "2");
  CHECK(t.Eval("AVERAGE(+U1:U3)") == "5");
  CHECK(t.Eval("MIN(UNIQUE(U1:U3))") == "4");
  CHECK(t.Eval("CONCAT(SORT(U1:U3)&\"|\")") == "4|6||");
}
