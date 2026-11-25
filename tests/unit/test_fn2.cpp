// SPDX-License-Identifier: Apache-2.0
// Engineering, database, financial, math, statistics, text and summary functions added to close the gaps with Excel.
// Expected values are the examples in Excel's documentation.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <cmath>
#include <cstdlib>

namespace {

// numeric result within a relative tolerance
bool Near(yctest::Doc& t, const std::string& f, double want, double tol = 1e-6) {
  const std::string s = t.Eval(f);
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (!end || *end) {
    UNSCOPED_INFO(f << " -> " << s);
    return false;
  }
  const bool ok = std::fabs(v - want) <= tol * std::max(1.0, std::fabs(want));
  if (!ok) UNSCOPED_INFO(f << " -> " << s << " (want " << want << ")");
  return ok;
}

}  // namespace

TEST_CASE("Engineering: bases, bits, CONVERT, Bessel, ERF") {
  yctest::Doc t;
  CHECK(t.Eval("BIN2DEC(\"1100100\")") == "100");
  CHECK(t.Eval("BIN2DEC(\"1111111111\")") == "-1");
  CHECK(t.Eval("BIN2HEX(\"11111011\",4)") == "00FB");
  CHECK(t.Eval("BIN2HEX(\"1111111111\")") == "FFFFFFFFFF");
  CHECK(t.Eval("BIN2OCT(\"1001\",3)") == "011");
  CHECK(t.Eval("DEC2BIN(9,4)") == "1001");
  CHECK(t.Eval("DEC2BIN(-100)") == "1110011100");
  CHECK(t.Eval("DEC2BIN(600)") == "#NUM!");
  CHECK(t.Eval("DEC2HEX(100,4)") == "0064");
  CHECK(t.Eval("DEC2HEX(-54)") == "FFFFFFFFCA");
  CHECK(t.Eval("DEC2OCT(58,3)") == "072");
  CHECK(t.Eval("DEC2OCT(-100)") == "7777777634");
  CHECK(t.Eval("HEX2BIN(\"F\",8)") == "00001111");
  CHECK(t.Eval("HEX2DEC(\"A5\")") == "165");
  CHECK(t.Eval("HEX2DEC(\"FFFFFFFF5B\")") == "-165");
  CHECK(t.Eval("HEX2OCT(\"F\",3)") == "017");
  CHECK(t.Eval("OCT2BIN(3,3)") == "011");
  CHECK(t.Eval("OCT2DEC(54)") == "44");
  CHECK(t.Eval("OCT2HEX(100,4)") == "0040");
  CHECK(t.Eval("BITAND(13,25)") == "9");
  CHECK(t.Eval("BITOR(23,10)") == "31");
  CHECK(t.Eval("BITXOR(5,3)") == "6");
  CHECK(t.Eval("BITLSHIFT(4,2)") == "16");
  CHECK(t.Eval("BITRSHIFT(13,2)") == "3");
  CHECK(t.Eval("DELTA(5,4)") == "0");
  CHECK(t.Eval("GESTEP(-4,-5)") == "1");
  CHECK(Near(t, "ERF(0.745)", 0.707928920));
  CHECK(Near(t, "ERF(0,1)", 0.842700793));
  CHECK(Near(t, "ERFC(1)", 0.157299207));
  CHECK(Near(t, "BESSELJ(1.9,2)", 0.329925829, 1e-6));
  CHECK(Near(t, "BESSELI(1.5,1)", 0.981666428, 1e-6));
  CHECK(Near(t, "BESSELK(1.5,1)", 0.277387804, 1e-6));
  CHECK(Near(t, "BESSELY(2.5,1)", 0.145918138, 1e-6));
  CHECK(Near(t, "CONVERT(1,\"lbm\",\"kg\")", 0.45359237));
  CHECK(Near(t, "CONVERT(68,\"F\",\"C\")", 20));
  CHECK(t.Eval("CONVERT(2.5,\"ft\",\"sec\")") == "#N/A");
  CHECK(Near(t, "CONVERT(1,\"km\",\"m\")", 1000));
  CHECK(Near(t, "CONVERT(100,\"ft2\",\"m2\")", 9.290304));
  CHECK(Near(t, "CONVERT(1,\"Mibyte\",\"byte\")", 1048576));
  CHECK(Near(t, "CONVERT(1,\"hr\",\"mn\")", 60));
}

TEST_CASE("Engineering: complex numbers") {
  yctest::Doc t;
  CHECK(t.Eval("COMPLEX(3,4)") == "3+4i");
  CHECK(t.Eval("COMPLEX(3,4,\"j\")") == "3+4j");
  CHECK(t.Eval("COMPLEX(0,1)") == "i");
  CHECK(t.Eval("COMPLEX(1,0)") == "1");
  CHECK(t.Eval("IMABS(\"5+12i\")") == "13");
  CHECK(t.Eval("IMAGINARY(\"-1-i\")") == "-1");
  CHECK(t.Eval("IMREAL(\"6-9i\")") == "6");
  CHECK(t.Eval("IMCONJUGATE(\"3+4i\")") == "3-4i");
  CHECK(t.Eval("IMSUM(\"3+4i\",\"5-3i\")") == "8+i");
  CHECK(t.Eval("IMSUB(\"13+4i\",\"5+3i\")") == "8+i");
  CHECK(t.Eval("IMPRODUCT(\"3+4i\",\"5-3i\")") == "27+11i");
  CHECK(t.Eval("IMDIV(\"-238+240i\",\"10+24i\")") == "5+12i");
  CHECK(Near(t, "IMARGUMENT(\"3+4i\")", 0.92729522));
  CHECK(Near(t, "IMREAL(IMSQRT(\"1+i\"))", 1.09868411346781));
  CHECK(Near(t, "IMAGINARY(IMEXP(\"1+i\"))", 2.28735528717884));
  CHECK(Near(t, "IMREAL(IMLN(\"3+4i\"))", 1.6094379124341));
  CHECK(Near(t, "IMREAL(IMPOWER(\"2+3i\",3))", -46));
  CHECK(t.Eval("IMLN(0)") == "#NUM!");
}

TEST_CASE("Database functions") {
  yctest::Doc t;
  const char* rows[][4] = {{"Tree", "Height", "Age", "Profit"}, {"Apple", "18", "20", "105"}, {"Pear", "12", "12", "96"},
                           {"Cherry", "13", "14", "105"},       {"Apple", "14", "15", "75"},  {"Pear", "9", "8", "76.8"},
                           {"Apple", "8", "9", "45"}};
  for (int r = 0; r < 7; ++r) {
    for (int c = 0; c < 4; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
  }
  // criteria: Tree = Apple and Height > 10, or Tree = Pear
  t.Set("F1", "Tree");
  t.Set("G1", "Height");
  t.Set("F2", "=\"=Apple\"");
  t.Set("G2", ">10");
  t.Set("F3", "=\"=Pear\"");
  CHECK(t.Eval("DSUM(A1:D7,\"Profit\",F1:G3)") == "352.8");
  CHECK(t.Eval("DCOUNT(A1:D7,\"Age\",F1:G3)") == "4");
  CHECK(t.Eval("DCOUNTA(A1:D7,,F1:G3)") == "4");
  CHECK(t.Eval("DMAX(A1:D7,\"Profit\",F1:G3)") == "105");
  CHECK(t.Eval("DMIN(A1:D7,4,F1:G3)") == "75");
  CHECK(Near(t, "DAVERAGE(A1:D7,\"Profit\",F1:G3)", 88.2));
  CHECK(t.Eval("DGET(A1:D7,\"Tree\",F1:G3)") == "#NUM!");
  CHECK(Near(t, "DPRODUCT(A1:D7,\"Height\",F1:G3)", 18 * 12 * 14 * 9.0));
  CHECK(Near(t, "DSTDEV(A1:D7,\"Profit\",F1:G3)", 14.68877, 1e-6));
  CHECK(Near(t, "DVARP(A1:D7,\"Profit\",F1:G3)", 161.82));
  // text without an operator matches the beginning: "Ch" -> Cherry
  t.Set("H1", "Tree");
  t.Set("H2", "Ch");
  CHECK(t.Eval("DGET(A1:D7,\"Age\",H1:H2)") == "14");
}

TEST_CASE("Financial: coupons, bonds, securities") {
  yctest::Doc t;
  CHECK(t.Eval("COUPDAYBS(DATE(2011,1,25),DATE(2011,11,15),2,1)") == "71");
  CHECK(t.Eval("COUPDAYS(DATE(2011,1,25),DATE(2011,11,15),2,1)") == "181");
  CHECK(t.Eval("COUPDAYSNC(DATE(2011,1,25),DATE(2011,11,15),2,1)") == "110");
  CHECK(t.Eval("COUPNCD(DATE(2011,1,25),DATE(2011,11,15),2,1)=DATE(2011,5,15)") == "TRUE");
  CHECK(t.Eval("COUPPCD(DATE(2011,1,25),DATE(2011,11,15),2,1)=DATE(2010,11,15)") == "TRUE");
  CHECK(t.Eval("COUPNUM(DATE(2007,1,25),DATE(2008,11,15),2,1)") == "4");
  CHECK(Near(t, "PRICE(DATE(2008,2,15),DATE(2017,11,15),0.0575,0.065,100,2,0)", 94.63436, 1e-6));
  CHECK(Near(t, "YIELD(DATE(2008,2,15),DATE(2016,11,15),0.0575,95.04287,100,2,0)", 0.065, 1e-5));
  CHECK(Near(t, "DURATION(DATE(2018,7,1),DATE(2048,1,1),0.08,0.09,2,1)", 10.9191453, 1e-6));
  CHECK(Near(t, "MDURATION(DATE(2008,1,1),DATE(2016,1,1),0.08,0.09,2,1)", 5.73567, 1e-5));
  CHECK(Near(t, "PRICEDISC(DATE(2008,2,16),DATE(2008,3,1),0.0525,100,2)", 99.79583, 1e-6));
  CHECK(Near(t, "YIELDDISC(DATE(2008,2,16),DATE(2008,3,1),99.795,100,2)", 0.052823, 1e-4));
  CHECK(Near(t, "INTRATE(DATE(2008,2,15),DATE(2008,5,15),1000000,1014420,2)", 0.05768, 1e-4));
  CHECK(Near(t, "RECEIVED(DATE(2008,2,15),DATE(2008,5,15),1000000,0.0575,2)", 1014584.65, 1e-7));
  CHECK(Near(t, "PRICEMAT(DATE(2008,2,15),DATE(2008,4,13),DATE(2007,11,11),0.061,0.061,0)", 99.98449888, 1e-7));
  CHECK(Near(t, "YIELDMAT(DATE(2008,3,15),DATE(2008,11,3),DATE(2007,11,8),0.0625,100.0123,0)", 0.060954, 1e-4));
  CHECK(Near(t, "TBILLPRICE(DATE(2008,3,31),DATE(2008,6,1),0.09)", 98.45));
  CHECK(Near(t, "TBILLYIELD(DATE(2008,3,31),DATE(2008,6,1),98.45)", 0.09141696, 1e-6));
  CHECK(Near(t, "TBILLEQ(DATE(2008,3,31),DATE(2008,6,1),0.0914)", 0.094151, 1e-4));
  CHECK(Near(t, "ACCRINT(DATE(2008,3,1),DATE(2008,8,31),DATE(2008,5,1),0.1,1000,2,0)", 16.666667, 1e-6));
  CHECK(Near(t, "ACCRINTM(DATE(2008,4,1),DATE(2008,6,15),0.1,1000,3)", 20.54794521, 1e-7));
  CHECK(Near(t, "ODDLPRICE(DATE(2008,2,7),DATE(2008,6,15),DATE(2007,10,15),0.0375,0.0405,100,2,0)", 99.87829, 1e-6));
  CHECK(Near(t, "ODDLYIELD(DATE(2008,4,20),DATE(2008,6,15),DATE(2007,12,24),0.0375,99.875,100,2,0)", 0.045192, 1e-4));
  CHECK(Near(t, "ODDFPRICE(DATE(2008,11,11),DATE(2021,3,1),DATE(2008,10,15),DATE(2009,3,1),0.0785,0.0625,100,2,1)", 113.597717, 1e-5));
  CHECK(Near(t, "ODDFYIELD(DATE(2008,11,11),DATE(2021,3,1),DATE(2008,10,15),DATE(2009,3,1),0.0575,84.5,100,2,0)", 0.0772, 2e-3));
}

TEST_CASE("Financial: depreciation and rates") {
  yctest::Doc t;
  CHECK(t.Eval("SLN(30000,7500,10)") == "2250");
  CHECK(Near(t, "SYD(30000,7500,10,1)", 4090.909091));
  CHECK(Near(t, "SYD(30000,7500,10,10)", 409.0909091));
  CHECK(Near(t, "DB(1000000,100000,6,1,7)", 186083.33, 1e-7));
  CHECK(Near(t, "DB(1000000,100000,6,2,7)", 259639.42, 1e-7));
  CHECK(Near(t, "DB(1000000,100000,6,7,7)", 15845.10, 1e-6));
  CHECK(Near(t, "DDB(2400,300,10*365,1)", 1.315068, 1e-5));
  CHECK(Near(t, "DDB(2400,300,10*12,1,2)", 40));
  CHECK(Near(t, "DDB(2400,300,10,1,2)", 480));
  CHECK(Near(t, "DDB(2400,300,10,2,1.5)", 306));
  CHECK(Near(t, "DDB(2400,300,10,10)", 22.1225472, 1e-6));
  CHECK(Near(t, "VDB(2400,300,10*365,0,1)", 1.315068, 1e-5));
  CHECK(Near(t, "VDB(2400,300,10*12,6,18)", 396.306053, 1e-6));
  CHECK(Near(t, "VDB(2400,300,10*12,6,18,1.5)", 311.808936, 1e-6));
  CHECK(Near(t, "VDB(2400,300,10,0,0.875,1.5)", 315));
  CHECK(Near(t, "AMORLINC(2400,DATE(2008,8,19),DATE(2008,12,31),300,1,0.15,1)", 360));
  CHECK(Near(t, "AMORDEGRC(2400,DATE(2008,8,19),DATE(2008,12,31),300,1,0.15,1)", 776));
  CHECK(Near(t, "EFFECT(0.0525,4)", 0.053542667));
  CHECK(Near(t, "NOMINAL(0.053543,4)", 0.05250032, 1e-6));
  CHECK(Near(t, "FVSCHEDULE(1,{0.09,0.11,0.1})", 1.33089));
  CHECK(Near(t, "ISPMT(0.1/12,1,3*12,8000000)", -64814.81481));
  CHECK(Near(t, "MIRR({-120000,39000,30000,21000,37000,46000},0.1,0.12)", 0.126094, 1e-5));
  CHECK(Near(t, "PDURATION(0.025,2000,2200)", 3.859866163, 1e-7));
  CHECK(Near(t, "RRI(96,10000,11000)", 0.0009933, 1e-3));
  CHECK(Near(t, "CUMIPMT(0.09/12,30*12,125000,13,24,0)", -11135.23213, 1e-7));
  CHECK(Near(t, "CUMIPMT(0.09/12,30*12,125000,1,1,0)", -937.5));
  CHECK(Near(t, "CUMPRINC(0.09/12,30*12,125000,13,24,0)", -934.1071234, 1e-7));
  CHECK(Near(t, "DOLLARDE(1.02,16)", 1.125));
  CHECK(Near(t, "DOLLARDE(1.1,32)", 1.3125));
  CHECK(Near(t, "DOLLARFR(1.125,16)", 1.02));
  CHECK(Near(t, "DOLLARFR(1.125,32)", 1.04));
}

TEST_CASE("Math: trigonometry, Roman numerals, bases, matrices") {
  yctest::Doc t;
  CHECK(Near(t, "ACOT(2)", 0.463647609));
  CHECK(Near(t, "ACOTH(6)", 0.168236118));
  CHECK(Near(t, "COT(30)", -0.156119952));
  CHECK(Near(t, "COTH(2)", 1.037314721));
  CHECK(Near(t, "CSC(15)", 1.537780562));
  CHECK(Near(t, "CSCH(1.5)", 0.469642441));
  CHECK(Near(t, "SEC(45)", 1.903594407));
  CHECK(t.Eval("ROMAN(499,0)") == "CDXCIX");
  CHECK(t.Eval("ROMAN(499,1)") == "LDVLIV");
  CHECK(t.Eval("ROMAN(499,2)") == "XDIX");
  CHECK(t.Eval("ROMAN(499,3)") == "VDIV");
  CHECK(t.Eval("ROMAN(499,4)") == "ID");
  CHECK(t.Eval("ROMAN(2013)") == "MMXIII");
  CHECK(t.Eval("ARABIC(\"LVII\")") == "57");
  CHECK(t.Eval("ARABIC(\"mcmxii\")") == "1912");
  CHECK(t.Eval("ARABIC(\"-MMXI\")") == "-2011");
  CHECK(t.Eval("BASE(7,2)") == "111");
  CHECK(t.Eval("BASE(100,16)") == "64");
  CHECK(t.Eval("BASE(15,2,10)") == "0000001111");
  CHECK(t.Eval("CEILING.PRECISE(-4.3,2)") == "-4");
  CHECK(t.Eval("ISO.CEILING(4.3,-2)") == "6");
  CHECK(t.Eval("FLOOR.PRECISE(-3.2,-1)") == "-4");
  CHECK(t.Eval("COMBINA(4,3)") == "20");
  CHECK(t.Eval("FACTDOUBLE(7)") == "105");
  CHECK(t.Eval("MULTINOMIAL(2,3,4)") == "1260");
  CHECK(Near(t, "SERIESSUM(PI()/4,0,2,{1,-0.5,0.041666667,-0.001388889})", 0.707103));
  CHECK(Near(t, "SQRTPI(2)", 2.506628275));
  CHECK(t.Eval("SUMX2MY2({2,3,9,1,8,7,5},{6,5,11,7,5,4,4})") == "-55");
  CHECK(t.Eval("SUMX2PY2({2,3,9,1,8,7,5},{6,5,11,7,5,4,4})") == "521");
  CHECK(t.Eval("SUMXMY2({2,3,9,1,8,7,5},{6,5,11,7,5,4,4})") == "79");
  CHECK(Near(t, "MDETERM({1,3,8,5;1,3,6,1;1,1,1,0;7,3,10,2})", 88));
  CHECK(t.Eval("INDEX(MINVERSE({4,-1;2,0}),2,2)") == "2");
  CHECK(t.Eval("INDEX(MINVERSE({4,-1;2,0}),1,2)") == "0.5");
  CHECK(t.Eval("INDEX(MMULT({1,3;7,2},{2,0;0,2}),2,1)") == "14");
  CHECK(t.Eval("SUM(MUNIT(3))") == "3");
}

TEST_CASE("SUMX2MY2, SUMX2PY2 and SUMXMY2 without a pair of numbers, and of arrays of references (measured)") {
  yctest::Doc t;
  for (auto [a1, text] : {std::pair{"A1", "1"}, {"A2", "2"}, {"A3", "3"}, {"A4", "'5"}, {"A5", "TRUE"}, {"A8", "=NA()"},
                          {"B1", "x"}, {"B2", "10"}, {"B3", "20"}}) {
    t.Set(a1, text);
  }
  CHECK(t.Eval("SUMX2MY2(A1:A3,B1:B3)") == "-487");
  // no pair of numbers: #DIV/0! (0 before)
  CHECK(t.Eval("SUMX2MY2(C1:C3,D1:D3)") == "#DIV/0!");
  CHECK(t.Eval("SUMX2PY2(C1:C3,D1:D3)") == "#DIV/0!");
  CHECK(t.Eval("SUMXMY2(C1:C3,D1:D3)") == "#DIV/0!");
  CHECK(t.Eval("SUMX2MY2(B1,A1)") == "#DIV/0!");
  CHECK(t.Eval("SUMX2MY2({\"a\",\"b\"},{1,1})") == "#DIV/0!");
  CHECK(t.Eval("SUMXMY2({TRUE},{1})") == "#DIV/0!");
  CHECK(t.Eval("SUMXMY2({TRUE,1},{1,1})") == "0");
  CHECK(t.Eval("SUMX2MY2(A4:A5,A1:A2)") == "#DIV/0!");
  CHECK(t.Eval("SUMX2MY2(\"a\",1)") == "#DIV/0!");
  CHECK(t.Eval("SUMX2MY2(1,2)") == "-3");
  CHECK(t.Eval("SUMX2MY2(A1:A2,A8:A9)") == "#N/A");
  CHECK(t.Eval("SUMX2MY2(A1:A2,{1;2})") == "0");
  CHECK(t.Eval("SUMX2MY2(A1:A3,{1;2})") == "#N/A");
  CHECK(t.Eval("SUMX2MY2({1,2},{3,\"x\"})") == "-8");
  // an array of references is #NUM!, after an error in the other argument or different sizes (#VALUE! before)
  CHECK(t.Eval("SUMX2MY2(OFFSET(A1,{0,1},0),{1,1})") == "#NUM!");
  CHECK(t.Eval("SUMX2PY2(OFFSET(A1,{0,1},0),{1,1})") == "#NUM!");
  CHECK(t.Eval("SUMXMY2(OFFSET(A1,{0,1},0),{1,1})") == "#NUM!");
  CHECK(t.Eval("SUMX2MY2({1,1},OFFSET(A1,{0,1},0))") == "#NUM!");
  CHECK(t.Eval("SUMX2MY2(INDIRECT({\"A1\",\"A2\"}),{1,1})") == "#NUM!");
  CHECK(t.Eval("SUMX2MY2(OFFSET(A1,{0,1},0),OFFSET(A1,{0,1},0))") == "#NUM!");
  CHECK(t.Eval("SUMX2MY2(OFFSET(A1,{0,1},0),{1,2,3})") == "#N/A");
  CHECK(t.Eval("SUMX2MY2(OFFSET(A1,{0,1},0),1/0)") == "#DIV/0!");
  CHECK(t.Eval("SUMX2MY2(1/0,OFFSET(A1,{0,1},0))") == "#DIV/0!");
}

TEST_CASE("Statistics: distributions, tests, regression, forecasting") {
  yctest::Doc t;
  CHECK(Near(t, "BETA.DIST(2,8,10,TRUE,1,3)", 0.6854706, 1e-6));
  CHECK(Near(t, "BETA.DIST(2,8,10,FALSE,1,3)", 1.4837646, 1e-6));
  CHECK(Near(t, "BETA.INV(0.685470581,8,10,1,3)", 2, 1e-6));
  CHECK(Near(t, "BINOM.DIST.RANGE(60,0.75,48)", 0.083974967, 1e-6));
  CHECK(Near(t, "BINOM.DIST.RANGE(60,0.75,45,50)", 0.523629793, 1e-6));
  CHECK(t.Eval("BINOM.INV(6,0.5,0.75)") == "4");
  CHECK(Near(t, "CHISQ.TEST({58,35;11,25;10,23},{45.35,47.65;17.56,18.44;16.09,16.91})", 0.000308192, 1e-3));
  CHECK(Near(t, "F.TEST({6,7,9,15,21},{20,28,31,38,40})", 0.64831785, 1e-6));
  CHECK(Near(t, "FISHER(0.75)", 0.972955075));
  CHECK(Near(t, "FISHERINV(0.972955)", 0.75, 1e-6));
  CHECK(Near(t, "GAUSS(2)", 0.477249868));
  CHECK(Near(t, "PHI(0.75)", 0.301137432));
  CHECK(Near(t, "HYPGEOM.DIST(1,4,8,20,TRUE)", 0.465428277));
  CHECK(Near(t, "HYPGEOM.DIST(1,4,8,20,FALSE)", 0.363261094));
  CHECK(Near(t, "NEGBINOM.DIST(10,5,0.25,TRUE)", 0.313514058));
  CHECK(Near(t, "NEGBINOM.DIST(10,5,0.25,FALSE)", 0.055048660));
  CHECK(t.Eval("PERMUT(100,3)") == "970200");
  CHECK(t.Eval("PERMUTATIONA(3,2)") == "9");
  CHECK(Near(t, "PROB({0,1,2,3},{0.2,0.3,0.1,0.4},2)", 0.1));
  CHECK(Near(t, "PROB({0,1,2,3},{0.2,0.3,0.1,0.4},1,3)", 0.8));
  CHECK(t.Eval("QUARTILE.EXC({6,7,15,36,39,40,41,42,43,47,49},1)") == "15");
  CHECK(t.Eval("QUARTILE.EXC({6,7,15,36,39,40,41,42,43,47,49},3)") == "43");
  CHECK(Near(t, "SKEW.P({3,4,5,2,3,4,5,6,4,7})", 0.303193339));
  CHECK(Near(t, "STDEVA(1,2,3,4)", 1.290994449));
  CHECK(Near(t, "VARPA(1,2,3,4)", 1.25));
  CHECK(Near(t, "T.TEST({3,4,5,8,9,1,2,4,5},{6,19,3,2,14,4,5,17,1},2,1)", 0.196015785, 1e-6));
  CHECK(Near(t, "Z.TEST({3,6,7,8,6,5,4,2,1,9},4)", 0.090574, 1e-4));
  CHECK(t.Eval("ARRAYTOTEXT(FREQUENCY({79,85,78,85,50,81,95,88,97},{70,79,89}),1)") == "{1;2;4;2}");
  CHECK(t.Eval("ARRAYTOTEXT(MODE.MULT(1,2,3,4,3,2,1,2,3),1)") == "{2;3}");
  CHECK(t.Eval("MODE(2,1,1,2)") == "2");       // a tie: the value that appears first, as MODE.MULT lists them
  CHECK(t.Eval("MODE.SNGL(1,3,3,1)") == "1");
  CHECK(t.Eval("MODE(1,2,3)") == "#N/A");
  CHECK(t.Eval("ARRAYTOTEXT(LINEST({1,9,5,7},{0,4,2,3}),1)") == "{2,1}");
  CHECK(Near(t, "INDEX(LINEST({1,9,5,7},{0,4,2,3},TRUE,TRUE),3,1)", 1));
  CHECK(Near(t, "GROWTH({33100,47300,69000,102000,150000,220000},{11,12,13,14,15,16},17)", 320196.7184, 1e-6));
  CHECK(Near(t, "INDEX(LOGEST({33100,47300,69000,102000,150000,220000},{11,12,13,14,15,16}),1)", 1.463275628, 1e-6));
  CHECK(Near(t, "TREND({1,9,5,7},{0,4,2,3},5)", 11));
  // a linear series with a weekly season: the forecast continues it
  for (int i = 1; i <= 28; ++i) {
    t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
    t.Set(("B" + std::to_string(i)).c_str(), std::to_string(100 + i * 2 + (i % 7 == 0 ? 10 : 0)));
  }
  CHECK(t.Eval("FORECAST.ETS.SEASONALITY(B1:B28,A1:A28)") == "7");
  CHECK(Near(t, "FORECAST.ETS(29,B1:B28,A1:A28)", 158, 0.03));
}

TEST_CASE("Text, web, lookup and summary functions") {
  yctest::Doc t;
  // 1234 baht: "one thousand two hundred thirty four baht exactly" in Thai
  CHECK(t.Eval("BAHTTEXT(1234)") ==
        "\xE0\xB8\xAB\xE0\xB8\x99\xE0\xB8\xB6\xE0\xB9\x88\xE0\xB8\x87\xE0\xB8\x9E\xE0\xB8\xB1\xE0\xB8\x99\xE0\xB8\xAA\xE0\xB8\xAD\xE0\xB8\x87\xE0"
        "\xB8\xA3\xE0\xB9\x89\xE0\xB8\xAD\xE0\xB8\xA2\xE0\xB8\xAA\xE0\xB8\xB2\xE0\xB8\xA1\xE0\xB8\xAA\xE0\xB8\xB4\xE0\xB8\x9A\xE0\xB8\xAA\xE0\xB8"
        "\xB5\xE0\xB9\x88\xE0\xB8\x9A\xE0\xB8\xB2\xE0\xB8\x97\xE0\xB8\x96\xE0\xB9\x89\xE0\xB8\xA7\xE0\xB8\x99");
  CHECK(t.Eval("VALUETOTEXT(\"abc\",1)") == "\"abc\"");
  CHECK(t.Eval("ARRAYTOTEXT({1,2;3,\"x\"})") == "1, 2, 3, x");
  CHECK(t.Eval("ARRAYTOTEXT({1,2;3,\"x\"},1)") == "{1,2;3,\"x\"}");
  CHECK(t.Eval("REGEXTEST(\"abc123\",\"[0-9]+\")") == "TRUE");
  CHECK(t.Eval("REGEXTEST(\"ABC\",\"abc\",1)") == "TRUE");
  CHECK(t.Eval("REGEXEXTRACT(\"abc123def45\",\"[0-9]+\")") == "123");
  CHECK(t.Eval("ARRAYTOTEXT(REGEXEXTRACT(\"abc123def45\",\"[0-9]+\",1),1)") == "{\"123\";\"45\"}");
  CHECK(t.Eval("INDEX(REGEXEXTRACT(\"John Smith\",\"(\\w+) (\\w+)\",2),1,2)") == "Smith");
  CHECK(t.Eval("REGEXREPLACE(\"abc123\",\"[0-9]\",\"#\")") == "abc###");
  CHECK(t.Eval("REGEXREPLACE(\"a-b-c\",\"-\",\"+\",2)") == "a-b+c");
  CHECK(t.Eval("REGEXREPLACE(\"2024-01-31\",\"(\\d+)-(\\d+)-(\\d+)\",\"$3/$2/$1\")") == "31/01/2024");
  CHECK(t.Eval("ENCODEURL(\"a b&c\")") == "a%20b%26c");
  CHECK(t.Eval("INDEX(FILTERXML(\"<a><b>1</b><b>x</b></a>\",\"//b\"),2)") == "x");
  CHECK(t.Eval("FILTERXML(\"<a><b>1</b><b>x</b></a>\",\"//b[1]\")+1") == "2");
  CHECK(t.Eval("AREAS((A1:B2,C3))") == "2");
  CHECK(t.Eval("AREAS(A1)") == "1");
  t.Set("B2", "1");
  t.Set("C3", "2");
  CHECK(t.Eval("ROWS(TRIMRANGE(A1:E10))") == "2");
  CHECK(t.Eval("COLUMNS(TRIMRANGE(A1:E10))") == "2");
  CHECK_FALSE(t.Set("Z1", "=PHONETIC(\"abc\")"));  // a reference only: refused as it is typed (Excel 16, measured)
  // functions passed by name (eta LAMBDA)
  CHECK(t.Eval("ARRAYTOTEXT(BYROW({1,2;3,4},SUM),1)") == "{3;7}");
  CHECK(t.Eval("REDUCE(0,{1,2,3},LAMBDA(a,b,a+b))") == "6");
  // GROUPBY / PIVOTBY
  const char* data[][3] = {{"Region", "Item", "Sales"}, {"East", "A", "10"}, {"West", "B", "20"}, {"East", "B", "5"}, {"West", "A", "7"}, {"East", "A", "3"}};
  for (int r = 0; r < 6; ++r) {
    for (int c = 0; c < 3; ++c) t.Set((std::string(1, static_cast<char>('H' + c)) + std::to_string(r + 1)).c_str(), data[r][c]);
  }
  CHECK(t.Eval("ARRAYTOTEXT(GROUPBY(H2:H6,J2:J6,SUM),1)") == "{\"East\",18;\"West\",27;\"Total\",45}");
  CHECK(t.Eval("ARRAYTOTEXT(GROUPBY(H1:H6,J1:J6,SUM,3,0),1)") == "{\"Region\",\"Sales\";\"East\",18;\"West\",27}");
  CHECK(t.Eval("ARRAYTOTEXT(GROUPBY(H2:H6,J2:J6,COUNT,0,0,-2),1)") == "{\"East\",3;\"West\",2}");
  CHECK(t.Eval("ARRAYTOTEXT(GROUPBY(H2:H6,J2:J6,PERCENTOF,0,0),1)") == "{\"East\",0.4;\"West\",0.6}");
  CHECK(t.Eval("ARRAYTOTEXT(PIVOTBY(H2:H6,I2:I6,J2:J6,SUM),1)") == "{,\"A\",\"B\",\"Total\";\"East\",13,5,18;\"West\",7,20,27;\"Total\",20,25,45}");
  CHECK(t.Eval("PERCENTOF(5,20)") == "0.25");
}

TEST_CASE("NUMBERSTRING, DATESTRING and USDOLLAR (as Excel shows them)") {
  yctest::Doc t;
  CHECK(t.Eval("NUMBERSTRING(123,1)") == "一百二十三");
  CHECK(t.Eval("NUMBERSTRING(123,2)") == "壹佰貳拾參");
  CHECK(t.Eval("NUMBERSTRING(123,3)") == "一二三");
  CHECK(t.Eval("NUMBERSTRING(10,1)") == "一十");  // as [DBNum1]: one ten (measured against Excel 16)
  CHECK(t.Eval("NUMBERSTRING(1001,1)") == "一千○一");  // a zero inside is ○ (Excel), 零 in the financial digits
  CHECK(t.Eval("NUMBERSTRING(1001,2)") == "壹仟零壹");
  CHECK(t.Eval("NUMBERSTRING(1001,3)") == "一○○一");
  CHECK(t.Eval("NUMBERSTRING(20500,1)") == "二萬○五百");
  CHECK(t.Eval("NUMBERSTRING(85073,1)") == "八萬五千○七十三");
  CHECK(t.Eval("NUMBERSTRING(12,1)") == "一十二");
  CHECK(t.Eval("NUMBERSTRING(100015,1)") == "一十萬○一十五");
  CHECK(t.Eval("NUMBERSTRING(85073,2)") == "捌萬伍仟零柒拾參");
  CHECK(t.Eval("NUMBERSTRING(1234,1)") == t.Eval("TEXT(1234,\"[DBNum1]\")"));  // one reading for both
  CHECK(t.Eval("NUMBERSTRING(0,1)") == "○");
  CHECK(t.Eval("NUMBERSTRING(100000000,1)") == "一億");
  CHECK(t.Eval("NUMBERSTRING(110,1)") == "一百一十");
  CHECK(t.Eval("NUMBERSTRING(12345678,2)") == "壹仟貳佰參拾肆萬伍仟陸佰柒拾捌");
  CHECK(t.Eval("NUMBERSTRING(1.4,3)") == "一");
  CHECK(t.Eval("NUMBERSTRING(1000000,1)") == "一百萬");
  CHECK(t.Eval("NUMBERSTRING(1010101,1)") == "一百○一萬○一百○一");
  CHECK(t.Eval("NUMBERSTRING(-12.6,1)") == "#NUM!");
  CHECK(t.Eval("NUMBERSTRING(123,4)") == "#NUM!");
  CHECK(t.Eval("NUMBERSTRING(\"abc\",1)") == "#VALUE!");
  CHECK(t.Eval("DATESTRING(45000)") == "民國112年03月15日");
  CHECK(t.Eval("DATESTRING(\"2024/1/5\")") == "民國113年01月05日");
  CHECK(t.Eval("DATESTRING(1)") == "民國前12年01月01日");
  CHECK(t.Eval("DATESTRING(45000.75)") == "民國112年03月15日");
  CHECK(t.Eval("USDOLLAR(1234.5)") == "US$1,234.50");
  CHECK(t.Eval("USDOLLAR(-1234.567,1)") == "(US$1,234.6)");
  CHECK(t.Eval("USDOLLAR(1234.5,-2)") == "US$1,200");
  CHECK(t.Eval("TYPE(USDOLLAR(1))") == "2");
}

TEST_CASE("INDEX of an array with 0 for the row or column returns the whole column or row (like Excel)") {
  yctest::Doc t;
  CHECK(t.Eval("ROWS(INDEX({1;2;3}>1,0))") == "3");
  CHECK(t.Eval("MATCH(TRUE,INDEX({1;2;3}>1,0),0)") == "2");
  CHECK(t.Eval("SUM(INDEX({1,2;3,4},0,2))") == "6");
  CHECK(t.Eval("SUM(INDEX({1,2;3,4},2,0))") == "7");
  CHECK(t.Eval("INDEX({5;6;7},2)") == "6");
  CHECK(t.Eval("INDEX({5,6,7},3)") == "7");
  CHECK(t.Eval("COLUMNS(INDEX({1,2;3,4},0,0))") == "2");
}

TEST_CASE("Wildcards match whole Chinese characters, and case is ignored beyond ASCII") {
  yctest::Doc t;
  CHECK(t.Eval("SEARCH(\"?字\",\"中文字\")") == "2");  // ? is one character (it matched one byte: 3)
  CHECK(t.Eval("SEARCH(\"文*\",\"中文字\")") == "2");
  t.Set("A1", "王小明");
  t.Set("A2", "王明");
  t.Set("A3", "李明");
  CHECK(t.Eval("COUNTIF(A1:A3,\"王?\")") == "1");
  CHECK(t.Eval("COUNTIF(A1:A3,\"王??\")") == "1");
  CHECK(t.Eval("COUNTIF(A1:A3,\"*明\")") == "3");
  CHECK(t.Eval("COUNTIF(A1:A3,\"*???\")") == "1");  // * gives back whole characters
  CHECK(t.Eval("MATCH(\"?明\",A1:A3,0)") == "2");
  CHECK(t.Eval("UPPER(\"abcａé\")") == "ABCＡÉ");
  CHECK(t.Eval("LOWER(\"ÀÉÖĀŁΣЖＺ\")") == "àéöāłσжｚ");
  CHECK(t.Eval("SEARCH(\"É\",\"café\")") == "4");
  CHECK(t.Eval("EXACT(\"é\",\"É\")") == "FALSE");
  t.Set("B1", "Café");
  CHECK(t.Eval("MATCH(\"CAFÉ\",B1:B1,0)") == "1");
}
