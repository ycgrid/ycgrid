// SPDX-License-Identifier: Apache-2.0
// Worksheet functions people use every day, checked against Excel's documented results: dates and times, math and
// rounding, statistics, text, information and lookup. Each case also covers the errors Excel gives for bad input.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <cmath>
#include <string>

namespace {

bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps * std::max(1.0, std::fabs(b)); }

// a formula's numeric result
double N(yctest::Doc& t, const std::string& f) {
  t.Set("Z999", "=" + f);
  return t.N("Z999");
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Dates and times (serials of the 1900 system: 2026-11-05 is 46331, a Thursday)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("TIME, HOUR, MINUTE, SECOND and TIMEVALUE") {
  yctest::Doc t;
  CHECK(Near(N(t, "TIME(12,30,0)"), 12.5 / 24));
  CHECK(Near(N(t, "TIME(25,0,0)"), 1.0 / 24));  // Excel keeps the part of a day
  CHECK(Near(N(t, "TIME(0,90,0)"), 1.5 / 24));
  CHECK(t.Eval("TIME(-1,0,0)") == "#NUM!");
  CHECK(t.Eval("HOUR(0.75)") == "18");
  CHECK(t.Eval("MINUTE(\"12:45\")") == "45");
  CHECK(t.Eval("SECOND(TIME(1,2,3))") == "3");
  CHECK(t.Eval("HOUR(46331.25)") == "6");
  CHECK(t.Eval("HOUR(-1)") == "#NUM!");
  CHECK(t.Eval("MINUTE(\"x\")") == "#VALUE!");
  CHECK(Near(N(t, "TIMEVALUE(\"6:00 PM\")"), 0.75));
  CHECK(Near(N(t, "TIMEVALUE(\"2026-11-05 06:00\")"), 0.25));  // the date part goes
  CHECK(t.Eval("TIMEVALUE(\"later\")") == "#VALUE!");
}

TEST_CASE("WEEKDAY, WEEKNUM and ISOWEEKNUM") {
  yctest::Doc t;
  CHECK(t.Eval("WEEKDAY(46331)") == "5");    // Thursday, Sunday = 1
  CHECK(t.Eval("WEEKDAY(46331,2)") == "4");  // Monday = 1
  CHECK(t.Eval("WEEKDAY(46331,3)") == "3");  // Monday = 0
  CHECK(t.Eval("WEEKDAY(46331,11)") == "4");
  CHECK(t.Eval("WEEKDAY(46331,17)") == "5");
  CHECK(t.Eval("WEEKDAY(46331,9)") == "#NUM!");
  CHECK(t.Eval("WEEKNUM(DATE(2026,1,1))") == "1");
  CHECK(t.Eval("WEEKNUM(DATE(2026,1,4))") == "2");  // a Sunday starts week 2
  CHECK(t.Eval("WEEKNUM(DATE(2026,1,4),2)") == "1");  // weeks from Monday
  CHECK(t.Eval("WEEKNUM(DATE(2026,3,15))") == "12");
  CHECK(t.Eval("WEEKNUM(DATE(2026,3,15),2)") == "11");
  CHECK(t.Eval("WEEKNUM(DATE(2026,12,31))") == "53");
  CHECK(t.Eval("WEEKNUM(DATE(2026,12,31),21)") == "53");  // ISO
  CHECK(t.Eval("WEEKNUM(DATE(2026,1,1),5)") == "#NUM!");
  CHECK(t.Eval("ISOWEEKNUM(DATE(2026,1,1))") == "1");
  CHECK(t.Eval("ISOWEEKNUM(DATE(2021,1,3))") == "53");  // a Sunday that belongs to the last week of 2020
  CHECK(t.Eval("ISOWEEKNUM(DATE(2026,11,5))") == "45");
}

TEST_CASE("EDATE, DAYS and DAYS360") {
  yctest::Doc t;
  CHECK(t.Eval("EDATE(DATE(2026,1,31),1)") == "46081");  // Feb 28: the month's last day
  CHECK(t.Eval("EDATE(DATE(2026,3,31),-1)") == "46081");
  CHECK(t.Eval("EDATE(DATE(2026,1,15),12)") == std::to_string(46023 + 14 + 365));
  CHECK(t.Eval("EDATE(\"x\",1)") == "#VALUE!");
  CHECK(t.Eval("DAYS(DATE(2026,3,1),DATE(2026,2,1))") == "28");
  CHECK(t.Eval("DAYS(\"2026-02-01\",\"2026-03-01\")") == "-28");
  CHECK(t.Eval("DAYS360(DATE(2011,1,1),DATE(2011,12,31))") == "360");  // Microsoft's examples
  CHECK(t.Eval("DAYS360(DATE(2011,1,30),DATE(2011,12,31))") == "330");
  CHECK(t.Eval("DAYS360(DATE(2011,1,1),DATE(2011,12,31),TRUE)") == "359");
  CHECK(t.Eval("DAYS360(DATE(2026,1,15),DATE(2026,3,31))") == "76");
}

TEST_CASE("DATEDIF in every unit") {
  yctest::Doc t;
  const std::string a = "DATE(2020,1,15)", b = "DATE(2026,3,10)";
  CHECK(t.Eval("DATEDIF(" + a + "," + b + ",\"Y\")") == "6");
  CHECK(t.Eval("DATEDIF(" + a + "," + b + ",\"M\")") == "73");
  CHECK(t.Eval("DATEDIF(" + a + "," + b + ",\"D\")") == "2246");
  CHECK(t.Eval("DATEDIF(" + a + "," + b + ",\"YM\")") == "1");
  CHECK(t.Eval("DATEDIF(" + a + "," + b + ",\"MD\")") == "23");
  CHECK(t.Eval("DATEDIF(" + a + "," + b + ",\"YD\")") == "54");
  CHECK(t.Eval("DATEDIF(" + a + "," + b + ",\"md\")") == "23");  // any case
  CHECK(t.Eval("DATEDIF(" + b + "," + a + ",\"D\")") == "#NUM!");
  CHECK(t.Eval("DATEDIF(" + a + "," + b + ",\"W\")") == "#NUM!");
}

TEST_CASE("NETWORKDAYS, WORKDAY and their .INTL forms with holidays") {
  yctest::Doc t;
  t.Set("H1", "=DATE(2026,11,11)");  // a holiday on a Wednesday
  t.Set("H2", "=DATE(2026,11,14)");  // and one on a Saturday (no effect)
  const std::string from = "DATE(2026,11,2)", to = "DATE(2026,11,13)";  // Monday to Friday a week later
  CHECK(t.Eval("NETWORKDAYS(" + from + "," + to + ")") == "10");
  CHECK(t.Eval("NETWORKDAYS(" + from + "," + to + ",H1:H2)") == "9");
  CHECK(t.Eval("NETWORKDAYS(" + to + "," + from + ")") == "-10");
  CHECK(t.Eval("NETWORKDAYS.INTL(" + from + "," + to + ",1)") == "10");
  CHECK(t.Eval("NETWORKDAYS.INTL(" + from + "," + to + ",11)") == "11");  // Sunday only
  CHECK(t.Eval("NETWORKDAYS.INTL(" + from + "," + to + ",\"0000011\")") == "10");
  CHECK(t.Eval("NETWORKDAYS.INTL(" + from + "," + to + ",\"0000000\",H1)") == "11");
  CHECK(t.Eval("NETWORKDAYS.INTL(" + from + "," + to + ",\"111111\")") == "#VALUE!");  // six characters
  CHECK(t.Eval("NETWORKDAYS.INTL(" + from + "," + to + ",99)") == "#NUM!");
  CHECK(t.Eval("WORKDAY(DATE(2026,11,6),1)") == "46335");   // Friday + 1 = Monday
  CHECK(t.Eval("WORKDAY(DATE(2026,11,9),-1)") == "46332");  // Monday - 1 = Friday
  CHECK(t.Eval("WORKDAY(DATE(2026,11,6),5)") == "46339");
  CHECK(t.Eval("WORKDAY(DATE(2026,11,10),1,H1)") == "46338");  // Tuesday + 1 skips Wednesday's holiday
  CHECK(t.Eval("WORKDAY.INTL(DATE(2026,11,5),1,7)") == "46334");  // Friday-Saturday weekend: Thursday + 1 = Sunday
  CHECK(t.Eval("WORKDAY.INTL(DATE(2026,11,5),1,\"0000111\")") == "46335");
  CHECK(t.Eval("WORKDAY.INTL(DATE(2026,11,5),1,\"1111111\")") == "#VALUE!");  // no working day at all
}

TEST_CASE("YEARFRAC in every basis") {
  yctest::Doc t;
  const std::string a = "DATE(2026,1,1),DATE(2026,7,1)";
  CHECK(Near(N(t, "YEARFRAC(" + a + ")"), 0.5));
  CHECK(Near(N(t, "YEARFRAC(" + a + ",1)"), 181.0 / 365));
  CHECK(Near(N(t, "YEARFRAC(" + a + ",2)"), 181.0 / 360));
  CHECK(Near(N(t, "YEARFRAC(" + a + ",3)"), 181.0 / 365));
  CHECK(Near(N(t, "YEARFRAC(" + a + ",4)"), 0.5));
  CHECK(Near(N(t, "YEARFRAC(DATE(2026,7,1),DATE(2026,1,1))"), 0.5));  // either order
  CHECK(t.Eval("YEARFRAC(" + a + ",5)") == "#NUM!");
  CHECK(Near(N(t, "YEARFRAC(DATE(2024,1,1),DATE(2025,1,1),1)"), 1.0));  // a whole leap year
}

// ---------------------------------------------------------------------------------------------------------------------
// Math and rounding
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("PRODUCT, SUMSQ and SUMIFS") {
  yctest::Doc t;
  t.Set("A1", "2");
  t.Set("A2", "3");
  t.Set("A3", "text");
  t.Set("A4", "4");
  CHECK(t.Eval("PRODUCT(A1:A4)") == "24");  // text in a range is left out
  CHECK(t.Eval("PRODUCT(2,3,0.5)") == "3");
  CHECK(t.Eval("PRODUCT(A1,\"x\")") == "#VALUE!");
  CHECK(t.Eval("SUMSQ(3,4)") == "25");
  CHECK(t.Eval("SUMSQ(A1:A4)") == "29");
  // SUMIFS: sums where every criterion holds
  t.Set("C1", "TXF");
  t.Set("D1", "B");
  t.Set("E1", "1");
  t.Set("C2", "TXF");
  t.Set("D2", "S");
  t.Set("E2", "2");
  t.Set("C3", "MXF");
  t.Set("D3", "B");
  t.Set("E3", "4");
  t.Set("C4", "TXF");
  t.Set("D4", "B");
  t.Set("E4", "8");
  CHECK(t.Eval("SUMIFS(E1:E4,C1:C4,\"TXF\",D1:D4,\"B\")") == "9");
  CHECK(t.Eval("SUMIFS(E1:E4,C1:C4,\"T*\")") == "11");
  CHECK(t.Eval("SUMIFS(E1:E4,E1:E4,\">=2\",E1:E4,\"<8\")") == "6");
  CHECK(t.Eval("SUMIFS(E1:E4,C1:C4,\"none\")") == "0");
  CHECK(t.Eval("SUMIFS(E1:E4,C1:C3,\"TXF\")") == "#VALUE!");  // ranges of different sizes
}

TEST_CASE("Logarithms, EXP and POWER") {
  yctest::Doc t;
  CHECK(Near(N(t, "LN(EXP(1))"), 1.0));
  CHECK(t.Eval("LN(0)") == "#NUM!");
  CHECK(t.Eval("LOG10(1000)") == "3");
  CHECK(t.Eval("LOG10(-1)") == "#NUM!");
  CHECK(t.Eval("LOG(8,2)") == "3");
  CHECK(t.Eval("LOG(100)") == "2");
  CHECK(t.Eval("LOG(8,1)") == "#DIV/0!");
  CHECK(t.Eval("LOG(8,0)") == "#NUM!");
  CHECK(t.Eval("LOG(-8,2)") == "#NUM!");
  CHECK(t.Eval("EXP(0)") == "1");
  CHECK(t.Eval("EXP(1000)") == "#NUM!");
  CHECK(t.Eval("POWER(2,10)") == "1024");
  CHECK(t.Eval("POWER(0,0)") == "#NUM!");
  CHECK(t.Eval("POWER(0,-1)") == "#DIV/0!");
  CHECK(t.Eval("POWER(-8,1/3)") == "-2");  // an odd root of a negative number (Excel 16)
  CHECK(t.Eval("ABS(-3)") == "3");
  CHECK(t.Eval("ABS(\"x\")") == "#VALUE!");
}

TEST_CASE("ROUNDUP, ROUNDDOWN, TRUNC, MROUND and QUOTIENT") {
  yctest::Doc t;
  CHECK(t.Eval("ROUNDUP(3.14159,2)") == "3.15");
  CHECK(t.Eval("ROUNDUP(-3.14159,1)") == "-3.2");
  CHECK(t.Eval("ROUNDUP(31415.92654,-2)") == "31500");
  CHECK(t.Eval("ROUNDDOWN(3.789,1)") == "3.7");
  CHECK(t.Eval("ROUNDDOWN(-3.14159,1)") == "-3.1");
  CHECK(t.Eval("TRUNC(-8.9)") == "-8");
  CHECK(t.Eval("TRUNC(8.987,2)") == "8.98");
  CHECK(t.Eval("MROUND(10,3)") == "9");
  CHECK(t.Eval("MROUND(-10,-3)") == "-9");
  CHECK(t.Eval("MROUND(1.3,0.2)") == "1.4");
  CHECK(t.Eval("MROUND(5,-2)") == "#NUM!");
  CHECK(t.Eval("MROUND(5,0)") == "0");
  CHECK(t.Eval("QUOTIENT(7,2)") == "3");
  CHECK(t.Eval("QUOTIENT(-7,2)") == "-3");
  CHECK(t.Eval("QUOTIENT(1,0)") == "#DIV/0!");
}

TEST_CASE("CEILING, FLOOR and their .MATH forms") {
  yctest::Doc t;
  CHECK(t.Eval("CEILING(2.5,1)") == "3");
  CHECK(t.Eval("CEILING(-2.5,-2)") == "-4");
  CHECK(t.Eval("CEILING(1.5,0.1)") == "1.5");
  CHECK(t.Eval("CEILING(2.5,-1)") == "#NUM!");
  CHECK(t.Eval("FLOOR(3.7,2)") == "2");
  CHECK(t.Eval("FLOOR(-2.5,-2)") == "-2");
  CHECK(t.Eval("FLOOR(1.58,0.1)") == "1.5");
  CHECK(t.Eval("FLOOR(2.5,-2)") == "#NUM!");
  CHECK(t.Eval("CEILING.MATH(24.3,5)") == "25");
  CHECK(t.Eval("CEILING.MATH(6.7)") == "7");
  CHECK(t.Eval("CEILING.MATH(-8.1,2)") == "-8");
  CHECK(t.Eval("CEILING.MATH(-5.5,2,-1)") == "-6");  // away from zero
  CHECK(t.Eval("FLOOR.MATH(24.3,5)") == "20");
  CHECK(t.Eval("FLOOR.MATH(6.7)") == "6");
  CHECK(t.Eval("FLOOR.MATH(-8.1,2)") == "-10");
  CHECK(t.Eval("FLOOR.MATH(-5.5,2,-1)") == "-4");  // toward zero
}

TEST_CASE("EVEN, ODD, GCD, LCM, FACT and COMBIN") {
  yctest::Doc t;
  CHECK(t.Eval("EVEN(1.5)") == "2");
  CHECK(t.Eval("EVEN(3)") == "4");
  CHECK(t.Eval("EVEN(-1)") == "-2");
  CHECK(t.Eval("EVEN(0)") == "0");
  CHECK(t.Eval("ODD(1.5)") == "3");
  CHECK(t.Eval("ODD(2)") == "3");
  CHECK(t.Eval("ODD(-1)") == "-1");
  CHECK(t.Eval("ODD(-2)") == "-3");
  CHECK(t.Eval("ODD(0)") == "1");
  CHECK(t.Eval("GCD(24,36)") == "12");
  CHECK(t.Eval("GCD(5,0)") == "5");
  CHECK(t.Eval("GCD(7.9,14)") == "7");
  CHECK(t.Eval("GCD(-1,2)") == "#NUM!");
  CHECK(t.Eval("LCM(4,6)") == "12");
  CHECK(t.Eval("LCM(4,6,10)") == "60");
  CHECK(t.Eval("LCM(5,0)") == "0");
  CHECK(t.Eval("LCM(-2,3)") == "#NUM!");
  CHECK(t.Eval("FACT(5)") == "120");
  CHECK(t.Eval("FACT(0)") == "1");
  CHECK(t.Eval("FACT(5.9)") == "120");
  CHECK(t.Eval("FACT(-1)") == "#NUM!");
  CHECK(t.Eval("FACT(200)") == "#NUM!");
  CHECK(t.Eval("COMBIN(8,2)") == "28");
  CHECK(t.Eval("COMBIN(8,0)") == "1");
  CHECK(t.Eval("COMBIN(2,3)") == "#NUM!");
}

TEST_CASE("Inverse trigonometry and RANDBETWEEN") {
  yctest::Doc t;
  const double pi = 3.14159265358979323846;
  CHECK(Near(N(t, "ASIN(1)"), pi / 2));
  CHECK(Near(N(t, "ACOS(-1)"), pi));
  CHECK(t.Eval("ASIN(2)") == "#NUM!");
  CHECK(t.Eval("ACOS(1.5)") == "#NUM!");
  CHECK(Near(N(t, "ATAN2(1,1)"), pi / 4));
  CHECK(Near(N(t, "ATAN2(-1,0)"), pi));
  CHECK(t.Eval("ATAN2(0,0)") == "#DIV/0!");
  CHECK(t.Eval("RANDBETWEEN(3,3)") == "3");
  CHECK(t.Eval("RANDBETWEEN(5,1)") == "#NUM!");
  for (int i = 0; i < 20; ++i) {
    const double v = N(t, "RANDBETWEEN(-2,2)");
    CHECK((v >= -2 && v <= 2 && v == std::floor(v)));
  }
}

TEST_CASE("AGGREGATE ignores errors when asked to") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "=1/0");
  t.Set("A3", "5");
  t.Set("A4", "3");
  CHECK(t.Eval("AGGREGATE(9,6,A1:A4)") == "9");    // SUM, errors left out
  CHECK(t.Eval("AGGREGATE(9,0,A1:A4)") == "#DIV/0!");
  CHECK(t.Eval("AGGREGATE(1,6,A1:A4)") == "3");    // AVERAGE
  CHECK(t.Eval("AGGREGATE(4,6,A1:A4)") == "5");    // MAX
  CHECK(t.Eval("AGGREGATE(5,6,A1:A4)") == "1");    // MIN
  CHECK(t.Eval("AGGREGATE(2,6,A1:A4)") == "3");    // COUNT
  CHECK(t.Eval("AGGREGATE(12,6,A1:A4)") == "3");   // MEDIAN
  CHECK(t.Eval("AGGREGATE(9,2,A1:A4)") == "9");
  CHECK(t.Eval("AGGREGATE(9,4,A1:A4)") == "#DIV/0!");  // 4: ignore nothing
  CHECK(t.Eval("AGGREGATE(0,6,A1:A4)") == "#VALUE!");
  CHECK(t.Eval("AGGREGATE(9,8,A1:A4)") == "#VALUE!");
  // the array form: the last argument is k
  CHECK(t.Eval("AGGREGATE(14,6,A1:A4,1)") == "5");    // LARGE
  CHECK(t.Eval("AGGREGATE(14,6,A1:A4,2)") == "3");
  CHECK(t.Eval("AGGREGATE(15,6,A1:A4,1)") == "1");    // SMALL
  CHECK(t.Eval("AGGREGATE(15,6,A1:A4,4)") == "#NUM!");
  CHECK(t.Eval("AGGREGATE(16,6,A1:A4,0.5)") == "3");  // PERCENTILE.INC
  CHECK(t.Eval("AGGREGATE(17,6,A1:A4,2)") == "3");    // QUARTILE.INC
  CHECK(t.Eval("AGGREGATE(18,6,A1:A4,0.5)") == "3");  // PERCENTILE.EXC
  CHECK(t.Eval("AGGREGATE(19,6,A1:A4,0)") == "#NUM!");  // QUARTILE.EXC takes 1-3
  CHECK(t.Eval("AGGREGATE(14,6,A1:A4)") == "#VALUE!");  // k is missing
  CHECK(t.Eval("AGGREGATE(14,4,A1:A4,1)") == "#DIV/0!");
  t.Set("A5", "5");
  CHECK(t.Eval("AGGREGATE(13,6,A1:A5)") == "5");     // MODE.SNGL
  t.Set("B1", "=SUBTOTAL(9,A1)");
  t.Set("B2", "10");
  CHECK(t.Eval("AGGREGATE(9,0,B1:B2)") == "10");  // nested SUBTOTAL left out with options 0-3
  CHECK(t.Eval("AGGREGATE(9,4,B1:B2)") == "11");  // and counted with 4-7
}

// ---------------------------------------------------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("AVERAGEA, COUNTBLANK, and the IFS aggregates") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "TRUE");
  t.Set("A3", "text");
  CHECK(Near(N(t, "AVERAGEA(A1:A3)"), 2.0 / 3));  // TRUE is 1, text is 0
  CHECK(t.Eval("AVERAGEA(A10:A12)") == "#DIV/0!");
  t.Set("B1", "1");
  t.Set("B3", "=\"\"");  // a formula's empty text counts as blank
  CHECK(t.Eval("COUNTBLANK(B1:B4)") == "3");
  t.Set("C1", "TXF");
  t.Set("D1", "10");
  t.Set("C2", "MXF");
  t.Set("D2", "20");
  t.Set("C3", "TXF");
  t.Set("D3", "30");
  CHECK(t.Eval("AVERAGEIFS(D1:D3,C1:C3,\"TXF\")") == "20");
  CHECK(t.Eval("MAXIFS(D1:D3,C1:C3,\"TXF\")") == "30");
  CHECK(t.Eval("MINIFS(D1:D3,C1:C3,\"TXF\")") == "10");
  CHECK(t.Eval("MAXIFS(D1:D3,C1:C3,\"none\")") == "0");
  CHECK(t.Eval("AVERAGEIFS(D1:D3,C1:C3,\"none\")") == "#DIV/0!");
  CHECK(t.Eval("MINIFS(D1:D3,C1:C3,\"TXF\",D1:D3,\">15\")") == "30");
}

TEST_CASE("The normal distribution and STANDARDIZE") {
  yctest::Doc t;
  CHECK(Near(N(t, "NORM.DIST(1,0,1,TRUE)"), 0.841344746068543, 1e-9));
  CHECK(Near(N(t, "NORM.DIST(0,0,1,FALSE)"), 0.398942280401433, 1e-9));
  CHECK(Near(N(t, "NORMDIST(42,40,1.5,TRUE)"), 0.908788780274132, 1e-9));  // Microsoft's example
  CHECK(t.Eval("NORM.DIST(1,0,0,TRUE)") == "#NUM!");
  CHECK(Near(N(t, "NORM.S.DIST(0,TRUE)"), 0.5));
  CHECK(Near(N(t, "NORMSDIST(1.333333)"), 0.908788726, 1e-7));
  CHECK(Near(N(t, "NORM.INV(0.908789,40,1.5)"), 42.000002, 1e-6));  // Microsoft's example
  CHECK(Near(N(t, "NORMINV(0.5,10,2)"), 10));
  CHECK(t.Eval("NORM.INV(0,10,2)") == "#NUM!");
  CHECK(t.Eval("NORM.INV(0.5,10,0)") == "#NUM!");
  CHECK(Near(N(t, "NORM.S.INV(0.975)"), 1.95996398454005, 1e-8));
  CHECK(Near(N(t, "NORMSINV(0.908789)"), 1.3333347, 1e-6));
  CHECK(Near(N(t, "STANDARDIZE(42,40,1.5)"), 4.0 / 3));
  CHECK(t.Eval("STANDARDIZE(42,40,0)") == "#NUM!");
}

TEST_CASE("Percentiles, quartiles and GEOMEAN") {
  yctest::Doc t;
  for (int i = 1; i <= 4; ++i) t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
  CHECK(Near(N(t, "PERCENTILE.INC(A1:A4,0.3)"), 1.9));
  CHECK(Near(N(t, "PERCENTILE(A1:A4,1)"), 4));
  CHECK(t.Eval("PERCENTILE.INC(A1:A4,1.5)") == "#NUM!");
  CHECK(Near(N(t, "PERCENTILE.EXC(A1:A4,0.4)"), 2));
  CHECK(t.Eval("PERCENTILE.EXC(A1:A4,0.1)") == "#NUM!");  // outside 1/(n+1) .. n/(n+1)
  CHECK(Near(N(t, "QUARTILE.INC(A1:A4,1)"), 1.75));
  CHECK(Near(N(t, "QUARTILE(A1:A4,2)"), 2.5));
  CHECK(t.Eval("QUARTILE(A1:A4,5)") == "#NUM!");
  CHECK(t.Eval("PERCENTILE(B1:B4,0.5)") == "#NUM!");  // no numbers
  CHECK(Near(N(t, "GEOMEAN(4,9)"), 6));
  CHECK(Near(N(t, "GEOMEAN(A1:A4)"), std::pow(24.0, 0.25)));
  CHECK(t.Eval("GEOMEAN(0,4)") == "#NUM!");
}

TEST_CASE("COVARIANCE.S, SLOPE, INTERCEPT and FORECAST") {
  yctest::Doc t;
  const char* xs[] = {"2", "4", "8"};
  const char* ys[] = {"5", "11", "12"};
  for (int i = 0; i < 3; ++i) {
    t.Set(("A" + std::to_string(i + 1)).c_str(), xs[i]);
    t.Set(("B" + std::to_string(i + 1)).c_str(), ys[i]);
  }
  CHECK(Near(N(t, "COVARIANCE.S(A1:A3,B1:B3)"), 29.0 / 3));
  CHECK(t.Eval("COVARIANCE.S(A1,B1)") == "#DIV/0!");
  CHECK(t.Eval("COVARIANCE.S(A1:A3,B1:B2)") == "#N/A");
  const char* x2[] = {"1", "2", "3", "4"};
  const char* y2[] = {"2", "4", "6", "8.5"};
  for (int i = 0; i < 4; ++i) {
    t.Set(("D" + std::to_string(i + 1)).c_str(), x2[i]);
    t.Set(("E" + std::to_string(i + 1)).c_str(), y2[i]);
  }
  CHECK(Near(N(t, "SLOPE(E1:E4,D1:D4)"), 2.15));
  CHECK(Near(N(t, "INTERCEPT(E1:E4,D1:D4)"), -0.25));
  CHECK(Near(N(t, "FORECAST(5,E1:E4,D1:D4)"), 10.5));
  CHECK(Near(N(t, "FORECAST.LINEAR(5,E1:E4,D1:D4)"), 10.5));
  t.Set("F1", "3");
  t.Set("F2", "3");
  CHECK(t.Eval("SLOPE(E1:E2,F1:F2)") == "#DIV/0!");  // x doesn't vary
  CHECK(t.Eval("SLOPE(E1:E4,D1:D3)") == "#N/A");
}

// ---------------------------------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("LEFT, RIGHT and REPLACE count characters, not bytes") {
  yctest::Doc t;
  CHECK(t.Eval("LEFT(\"abc\",2)") == "ab");
  CHECK(t.Eval("LEFT(\"abc\")") == "a");
  CHECK(t.Eval("LEFT(\"abc\",10)") == "abc");
  CHECK(t.Eval("LEFT(\"abc\",-1)") == "#VALUE!");
  CHECK(t.Eval("LEFT(\"\xE5\x8F\xB0\xE7\xA9\x8D\xE9\x9B\xBB\",2)") == "\xE5\x8F\xB0\xE7\xA9\x8D");
  CHECK(t.Eval("RIGHT(\"abc\",2)") == "bc");
  CHECK(t.Eval("RIGHT(\"abc\")") == "c");
  CHECK(t.Eval("RIGHT(\"\xE5\x8F\xB0\xE7\xA9\x8D\xE9\x9B\xBB\",1)") == "\xE9\x9B\xBB");
  CHECK(t.Eval("RIGHT(\"abc\",-1)") == "#VALUE!");
  CHECK(t.Eval("RIGHT(12345,2)") == "45");
  CHECK(t.Eval("REPLACE(\"abcdef\",2,3,\"X\")") == "aXef");
  CHECK(t.Eval("REPLACE(\"2009\",3,2,\"10\")") == "2010");  // Microsoft's example
  CHECK(t.Eval("REPLACE(\"abc\",10,1,\"Z\")") == "abcZ");
  CHECK(t.Eval("REPLACE(\"abc\",0,1,\"Z\")") == "#VALUE!");
}

TEST_CASE("UPPER, LOWER, PROPER, TRIM, CLEAN and EXACT") {
  yctest::Doc t;
  CHECK(t.Eval("UPPER(\"total 2nd\")") == "TOTAL 2ND");
  CHECK(t.Eval("LOWER(\"E. E. Cummings\")") == "e. e. cummings");
  CHECK(t.Eval("PROPER(\"this is a TITLE\")") == "This Is A Title");
  CHECK(t.Eval("PROPER(\"2-way street\")") == "2-Way Street");  // Microsoft's example
  CHECK(t.Eval("PROPER(\"76BudGet\")") == "76Budget");
  CHECK(t.Eval("TRIM(\"  First   Quarter  Earnings \")") == "First Quarter Earnings");
  CHECK(t.Eval("TRIM(\"   \")").empty());
  CHECK(t.Eval("CLEAN(CHAR(9)&\"Monthly report\"&CHAR(10))") == "Monthly report");
  CHECK(t.Eval("EXACT(\"word\",\"word\")") == "TRUE");
  CHECK(t.Eval("EXACT(\"Word\",\"word\")") == "FALSE");
}

TEST_CASE("VALUE and NUMBERVALUE read typed numbers") {
  yctest::Doc t;
  CHECK(t.Eval("VALUE(\"$1,000\")") == "1000");  // Microsoft's example
  CHECK(t.Eval("VALUE(\"1,234.5\")") == "1234.5");
  CHECK(t.Eval("VALUE(\"50%\")") == "0.5");
  CHECK(t.Eval("VALUE(\"(5)\")") == "-5");
  CHECK(Near(N(t, "VALUE(\"16:48:00\")-VALUE(\"12:00:00\")"), 0.2));
  CHECK(t.Eval("VALUE(\"abc\")") == "#VALUE!");
  CHECK(t.Eval("VALUE(7)") == "7");
  CHECK(Near(N(t, "NUMBERVALUE(\"2.500,27\",\",\",\".\")"), 2500.27));  // Microsoft's examples
  CHECK(Near(N(t, "NUMBERVALUE(\"3.5%\")"), 0.035));
  CHECK(Near(N(t, "NUMBERVALUE(\" 1 000 \")"), 1000));
  CHECK(t.Eval("NUMBERVALUE(\"\")") == "0");
  CHECK(t.Eval("NUMBERVALUE(\"1.2.3\")") == "#VALUE!");
  // every % divides by 100, as =9%% does; a group separator after the decimal one is an error
  CHECK(Near(N(t, "NUMBERVALUE(\"9%%\")"), 0.0009));
  CHECK(Near(N(t, "NUMBERVALUE(\"1,234.5\")"), 1234.5));
  CHECK(t.Eval("NUMBERVALUE(\"1.234,5\")") == "#VALUE!");
  CHECK(t.Eval("NUMBERVALUE(\"%\")") == "0");  // measured in Excel 16
}

TEST_CASE("CONCAT, CONCATENATE, N, T and UNICODE") {
  yctest::Doc t;
  t.Set("A1", "a");
  t.Set("B1", "2");
  t.Set("C1", "TRUE");
  CHECK(t.Eval("CONCAT(A1:C1,\"-\",4)") == "a2TRUE-4");
  CHECK(t.Eval("CONCAT(\"x\",A1:A3)") == "xa");  // empty cells add nothing
  CHECK(t.Eval("CONCATENATE(\"Stream \",\"population for \",\"brook trout\")") == "Stream population for brook trout");
  CHECK(t.Eval("CONCATENATE(A1,B1)") == "a2");
  CHECK(t.Eval("CONCATENATE(A1,1/0)") == "#DIV/0!");
  CHECK(t.Eval("N(7)") == "7");
  CHECK(t.Eval("N(TRUE)") == "1");
  CHECK(t.Eval("N(\"seven\")") == "0");
  CHECK(t.Eval("N(A1)") == "0");
  CHECK(t.Eval("T(\"Rainfall\")") == "Rainfall");
  CHECK(t.Eval("T(19)").empty());
  CHECK(t.Eval("T(TRUE)").empty());
  CHECK(t.Eval("UNICODE(\"B\")") == "66");
  CHECK(t.Eval("UNICODE(\"\xE5\x8F\xB0\")") == "21488");
  CHECK(t.Eval("UNICODE(\"\")") == "#VALUE!");
}

TEST_CASE("FIXED and DOLLAR format numbers as text, as Excel's examples show") {
  yctest::Doc t;
  CHECK(t.Eval("FIXED(1234.567,1)") == "1,234.6");
  CHECK(t.Eval("FIXED(1234.567,-1)") == "1,230");
  CHECK(t.Eval("FIXED(-1234.567,-1,TRUE)") == "-1230");
  CHECK(t.Eval("FIXED(44.332)") == "44.33");
  CHECK(t.Eval("DOLLAR(1234.567,2)") == "$1,234.57");
  CHECK(t.Eval("DOLLAR(99.888)") == "$99.89");
  CHECK(t.Eval("DOLLAR(1234.567,-2)") == "$1,200");
  CHECK(t.Eval("DOLLAR(-1234.567,-2)") == "($1,200)");
  CHECK(t.Eval("DOLLAR(-0.123,4)") == "($0.1230)");
  CHECK(t.Eval("DOLLAR(-5)") == "($5.00)");
}

TEST_CASE("TEXTBEFORE and TEXTAFTER") {
  yctest::Doc t;
  CHECK(t.Eval("TEXTBEFORE(\"Red riding hood's, red hood\",\"hood\")") == "Red riding ");
  CHECK(t.Eval("TEXTAFTER(\"Red riding hood's, red hood\",\"hood\")") == "'s, red hood");
  CHECK(t.Eval("TEXTBEFORE(\"a-b-c\",\"-\",2)") == "a-b");
  CHECK(t.Eval("TEXTAFTER(\"a-b-c\",\"-\",-1)") == "c");
  CHECK(t.Eval("TEXTBEFORE(\"a-b-c\",\"-\",-2)") == "a");
  CHECK(t.Eval("TEXTAFTER(\"Red RIDING\",\"riding\",1,1)").empty());
  CHECK(t.Eval("TEXTAFTER(\"abc\",\"x\")") == "#N/A");
  CHECK(t.Eval("TEXTAFTER(\"abc\",\"x\",1,0,0,\"none\")") == "none");
  CHECK(t.Eval("TEXTBEFORE(\"abc\",\"b\",0)") == "#VALUE!");
  // match_end: the end of the text counts as a delimiter (the start, counting from the end)
  CHECK(t.Eval("TEXTBEFORE(\"a-b-c\",\"-\",3)") == "#N/A");
  CHECK(t.Eval("TEXTBEFORE(\"a-b-c\",\"-\",3,,1)") == "a-b-c");
  CHECK(t.Eval("TEXTAFTER(\"a-b-c\",\"-\",-3,,1)") == "a-b-c");
  CHECK(t.Eval("TEXTAFTER(\"abc\",\"x\",1,0,1)").empty());
  // a list of delimiters: any of them
  CHECK(t.Eval("TEXTBEFORE(\"a,b;c\",{\";\",\",\"})") == "a");
  CHECK(t.Eval("TEXTAFTER(\"a,b;c\",{\";\",\",\"},2)") == "c");
  // an instance past the text's length is #VALUE!, past the delimiters #N/A
  CHECK(t.Eval("TEXTBEFORE(\"ab\",\"a\",3)") == "#VALUE!");
  CHECK(t.Eval("TEXTBEFORE(\"abc\",\"a\",2)") == "#N/A");
  CHECK(t.Eval("TEXTBEFORE(\"abc\",\"\")").empty());
  CHECK(t.Eval("TEXTAFTER(\"abc\",\"\")") == "abc");
}

// ---------------------------------------------------------------------------------------------------------------------
// Information and lookup
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("ISEVEN, ISODD, ISREF, ISFORMULA, FORMULATEXT, CELL and ERROR.TYPE") {
  yctest::Doc t;
  t.Set("A1", "=1+1");
  t.Set("A2", "5");
  t.Set("A3", "text");
  CHECK(t.Eval("ISEVEN(-1)") == "FALSE");
  CHECK(t.Eval("ISEVEN(2.5)") == "TRUE");
  CHECK(t.Eval("ISODD(5)") == "TRUE");
  CHECK(t.Eval("ISODD(\"x\")") == "#VALUE!");
  CHECK(t.Eval("ISREF(A1)") == "TRUE");
  CHECK(t.Eval("ISREF(\"A1\")") == "FALSE");
  CHECK(t.Eval("ISFORMULA(A1)") == "TRUE");
  CHECK(t.Eval("ISFORMULA(A2)") == "FALSE");
  CHECK(t.Eval("FORMULATEXT(A1)") == "=1+1");
  CHECK(t.Eval("FORMULATEXT(A2)") == "#N/A");
  CHECK(t.Eval("CELL(\"address\",B3)") == "$B$3");
  CHECK(t.Eval("CELL(\"row\",B3)") == "3");
  CHECK(t.Eval("CELL(\"col\",B3)") == "2");
  CHECK(t.Eval("CELL(\"contents\",A2)") == "5");
  CHECK(t.Eval("CELL(\"type\",A3)") == "l");
  CHECK(t.Eval("CELL(\"type\",A2)") == "v");
  CHECK(t.Eval("CELL(\"type\",C9)") == "b");
  CHECK(t.Eval("CELL(\"sheetname\",A1)") == t.d.S(t.sheet())->name);
  CHECK(t.Eval("CELL(\"nonsense\",A1)") == "#VALUE!");
  CHECK(t.Eval("ERROR.TYPE(1/0)") == "2");
  CHECK(t.Eval("ERROR.TYPE(NA())") == "7");
  CHECK(t.Eval("ERROR.TYPE(#VALUE!)") == "3");
  CHECK(t.Eval("ERROR.TYPE(#NULL!)") == "1");
  CHECK(t.Eval("ERROR.TYPE(#REF!)") == "4");
  CHECK(t.Eval("ERROR.TYPE(#NAME?)") == "5");
  CHECK(t.Eval("ERROR.TYPE(#NUM!)") == "6");
  CHECK(t.Eval("ERROR.TYPE(5)") == "#N/A");
  CHECK(t.Eval("SHEET()") == "1");
  CHECK(t.Eval("SHEETS()") == "1");
}

TEST_CASE("XMATCH, LOOKUP, ADDRESS, TRANSPOSE and HYPERLINK") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "3");
  t.Set("A3", "5");
  t.Set("B1", "a");
  t.Set("B2", "b");
  t.Set("B3", "c");
  CHECK(t.Eval("XMATCH(3,A1:A3)") == "2");
  CHECK(t.Eval("XMATCH(4,A1:A3)") == "#N/A");
  CHECK(t.Eval("XMATCH(4,A1:A3,-1)") == "2");  // exact or the next smaller
  CHECK(t.Eval("XMATCH(4,A1:A3,1)") == "3");   // exact or the next larger
  CHECK(t.Eval("XMATCH(\"?\",B1:B3,2)") == "1");  // wildcards
  CHECK(t.Eval("XMATCH(5,A1:A3,0,-1)") == "3");   // from the end
  CHECK(t.Eval("XMATCH(\"b\",B1:B3)") == "2");
  CHECK(t.Eval("LOOKUP(4,A1:A3,B1:B3)") == "b");
  CHECK(t.Eval("LOOKUP(5,A1:A3,B1:B3)") == "c");
  CHECK(t.Eval("LOOKUP(0,A1:A3,B1:B3)") == "#N/A");
  CHECK(t.Eval("LOOKUP(9,A1:B3)") == "c");  // the array form: the last column
  CHECK(t.Eval("ADDRESS(2,3)") == "$C$2");
  CHECK(t.Eval("ADDRESS(2,3,2)") == "C$2");
  CHECK(t.Eval("ADDRESS(2,3,3)") == "$C2");
  CHECK(t.Eval("ADDRESS(2,3,4)") == "C2");
  CHECK(t.Eval("ADDRESS(2,3,1,FALSE)") == "R2C3");
  CHECK(t.Eval("ADDRESS(2,3,4,FALSE)") == "R[2]C[3]");
  CHECK(t.Eval("ADDRESS(2,3,1,FALSE,\"[Book1]Sheet1\")") == "[Book1]Sheet1!R2C3");  // Excel 16, measured (not its help page)
  CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"EXCEL SHEET\")") == "'EXCEL SHEET'!$C$2");
  CHECK(t.Eval("ADDRESS(0,1)") == "#VALUE!");
  t.Set("D1", "=TRANSPOSE(A1:B3)");
  CHECK(t.V("D1").AsDouble() == 1);
  CHECK(t.V("F1").AsDouble() == 5);
  CHECK(t.V("E2").str() == "b");
  CHECK(t.Eval("HYPERLINK(\"https://example.com\",\"site\")") == "site");
  CHECK(t.Eval("HYPERLINK(\"https://example.com\")") == "https://example.com");
}
