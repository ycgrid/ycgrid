// SPDX-License-Identifier: Apache-2.0
// Round 8 (math, date and financial functions measured against Excel 16 itself): date and time parts rounded to the
// second, dates stored as text, ACCRINT past the first coupon, the coupon days of PRICE / YIELD / DURATION, far tails
// and inverses of the distributions, MOD and the PRECISE roundings, odd roots of negative numbers, collinear LINEST,
// PMT near a zero rate, CONVERT's decimal byte prefixes, SMALL / LARGE / QUARTILE's k, loose typed times and the
// argument rules of many functions
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "formula/numfmt.h"
#include "test_helpers.h"

#include <string>

namespace formula = yc::formula;
using Catch::Approx;

namespace {

double Ev(yctest::Doc& t, const std::string& f) {
  t.Set("Z999", "=" + f);
  return t.N("Z999");
}

// Excel's value to the given relative tolerance
Approx Xl(double v, double eps = 1e-12) { return Approx(v).epsilon(eps); }

// D1:D10 = 1 2 2 3 4 5 5 5 8 13, E1:E10 = 2 4 5 4 5 7 8 9 10 15 (the bug hunter's sheet)
void FillData(yctest::Doc& t) {
  const char* d[] = {"1", "2", "2", "3", "4", "5", "5", "5", "8", "13"};
  const char* e[] = {"2", "4", "5", "4", "5", "7", "8", "9", "10", "15"};
  for (int i = 0; i < 10; ++i) {
    t.Set(("D" + std::to_string(i + 1)).c_str(), d[i]);
    t.Set(("E" + std::to_string(i + 1)).c_str(), e[i]);
  }
}

}  // namespace

TEST_CASE("Round8: date and time parts round to the nearest second, carrying into the next day") {
  yctest::Doc t;
  CHECK(t.Eval("SECOND(45292.5+30.6/86400)") == "31");  // truncated: 30
  CHECK(t.Eval("DAY(45292+86399.6/86400)") == "2");
  CHECK(t.Eval("HOUR(45292+86399.6/86400)") == "0");
  CHECK(t.Eval("SECOND(45292+86399.6/86400)") == "0");
  CHECK(t.Eval("WEEKDAY(45292+86399.6/86400)") == "3");
  CHECK(t.Eval("SECOND(0.5/86400)") == "1");
  CHECK(t.Eval("SECOND(0.49/86400)") == "0");
  CHECK(t.Eval("MINUTE(TIME(9,30,59)+0.7/86400)") == "31");
  CHECK(t.Eval("MINUTE(TIME(0,0,59)+0.6/86400)") == "1");
  CHECK(t.Eval("SECOND(TIME(0,0,59)+0.4/86400)") == "59");
  CHECK(t.Eval("HOUR(0.99999999)") == "0");
  CHECK(t.Eval("HOUR(0.999994)") == "23");
  CHECK(t.Eval("HOUR(0.999995)") == "0");
  CHECK(t.Eval("HOUR(TIME(23,59,59)+0.5/86400)") == "0");
  CHECK(t.Eval("DAY(TIME(23,59,59)+0.5/86400+45292)") == "2");
  CHECK(t.Eval("SECOND(\"12:00:30.6\")") == "31");
  CHECK(t.Eval("SECOND(TIME(1,2,3)-1E-9)") == "3");
  CHECK(t.Eval("DAY(2958465.99)") == "31");
  CHECK(t.Eval("HOUR(2958465.9999999)") == "#NUM!");  // rounds past 9999-12-31
  CHECK(t.Eval("HOUR(1.5)") == "12");
}

TEST_CASE("Round8: date functions read numbers stored as text") {
  yctest::Doc t;
  t.Set("B1", "'45292");
  t.Set("B2", "'2024/3/5");
  CHECK(t.Eval("YEAR(B1)") == "2024");  // #VALUE! before
  CHECK(t.Eval("MONTH(\"5\")") == "1");
  CHECK(t.Eval("YEAR(\"100\")") == "1900");
  CHECK(t.Eval("EOMONTH(B1,0)") == "45322");
  CHECK(t.Eval("WORKDAY(B1,1)") == "45293");
  CHECK(t.Eval("HOUR(\"0.5\")") == "12");
  CHECK(t.Eval("DAYS(B2,B1)") == "64");
  CHECK(t.Eval("DATEDIF(B1,B2,\"d\")") == "64");
  CHECK(Ev(t, "YEARFRAC(B2,B1)") == Xl(0.17777777777777778));
  CHECK(t.Eval("YEAR(B2)") == "2024");  // dates as text still read
  CHECK(t.Eval("DAY(\"abc\")") == "#VALUE!");
}

TEST_CASE("Round8: ACCRINT keeps accruing from the issue past the first interest date") {
  yctest::Doc t;
  CHECK(Ev(t, "ACCRINT(DATE(2024,1,1),DATE(2024,7,1),DATE(2024,9,1),0.06,1000,2,0)") == Xl(40));  // 30 before
  CHECK(Ev(t, "ACCRINT(DATE(2024,1,1),DATE(2024,7,1),DATE(2024,9,1),0.06,1000,2,0,FALSE)") == Xl(40));  // 10 before
  CHECK(Ev(t, "ACCRINT(DATE(2024,1,1),DATE(2024,7,1),DATE(2025,9,1),0.06,1000,2,0)") == Xl(99.999999999999986));
  CHECK(Ev(t, "ACCRINT(DATE(2024,1,1),DATE(2024,7,1),DATE(2025,9,1),0.06,1000,2,0,FALSE)") == Xl(100));
  CHECK(Ev(t, "ACCRINT(DATE(2024,1,1),DATE(2024,7,1),DATE(2024,9,1),0.06,1000,2,1)") == Xl(40.219780219780226));
  CHECK(Ev(t, "ACCRINT(DATE(2024,1,1),DATE(2024,7,1),DATE(2024,9,1),0.06,1000,4,0)") == Xl(40));
  CHECK(Ev(t, "ACCRINT(DATE(2024,1,1),DATE(2024,7,1),DATE(2024,7,2),0.06,1000,2,0)") == Xl(30.166666666666664));
  CHECK(Ev(t, "ACCRINT(DATE(2023,11,1),DATE(2024,7,1),DATE(2024,9,1),0.06,1000,2,0)") == Xl(49.999999999999993));
  CHECK(Ev(t, "ACCRINT(DATE(2020,1,1),DATE(2020,7,1),DATE(2023,3,15),0.05,1000,2,0)") == Xl(160.27777777777777));
  CHECK(Ev(t, "ACCRINT(DATE(2020,1,1),DATE(2020,7,1),DATE(2023,3,15),0.05,1000,2,1)") == Xl(160.02747252747253));
  CHECK(Ev(t, "ACCRINT(DATE(2020,1,1),DATE(2020,7,1),DATE(2023,3,15),0.05,1000,2,2)") == Xl(160.13888888888889));
  CHECK(Ev(t, "ACCRINT(DATE(2020,1,1),DATE(2020,7,1),DATE(2023,3,15),0.05,1000,2,3)") == Xl(160));
  CHECK(Ev(t, "ACCRINT(DATE(2020,1,1),DATE(2020,7,1),DATE(2023,3,15),0.05,1000,2,4)") == Xl(160.27777777777777));
  CHECK(Ev(t, "ACCRINT(DATE(2008,3,1),DATE(2008,8,31),DATE(2008,10,1),0.1,1000,2,0,TRUE)") == Xl(58.611111111111114));
  CHECK(Ev(t, "ACCRINT(DATE(2008,3,1),DATE(2008,8,31),DATE(2008,10,1),0.1,1000,2,0,FALSE)") == Xl(58.333333333333336));
  // up to the first interest date nothing changes
  CHECK(Ev(t, "ACCRINT(DATE(2024,1,1),DATE(2024,7,1),DATE(2024,7,1),0.06,1000,2,0)") == Xl(30));
  CHECK(Ev(t, "ACCRINT(DATE(2008,3,1),DATE(2008,8,31),DATE(2008,5,1),0.1,1000,2,0)") == Xl(16.666666666666664));
}

TEST_CASE("Round8: PRICE, YIELD and DURATION count the days to the next coupon as COUPDAYS - COUPDAYBS") {
  yctest::Doc t;
  CHECK(Ev(t, "PRICE(DATE(2024,1,15),DATE(2027,3,15),0.05,0.06,100,2,2)") == Xl(97.151008319803481));  // 97.118550
  CHECK(Ev(t, "PRICE(DATE(2024,1,15),DATE(2027,3,15),0.05,0.06,100,2,3)") == Xl(97.1470962208154));
  CHECK(Ev(t, "PRICE(DATE(2024,1,15),DATE(2027,3,15),0.05,0.06,100,2,4)") == Xl(97.146327503737936));
  CHECK(Ev(t, "PRICE(DATE(2024,1,15),DATE(2027,3,15),0.05,0.06,100,1,3)") == Xl(97.175933490302128));
  CHECK(Ev(t, "PRICE(DATE(2008,2,15),DATE(2017,11,15),0.0575,0.065,100,2,2)") == Xl(94.63656403002507));
  CHECK(Ev(t, "PRICE(DATE(2008,2,15),DATE(2017,11,15),0.0575,0.065,100,2,3)") == Xl(94.63517479678454));
  CHECK(Ev(t, "PRICE(DATE(2024,2,29),DATE(2029,8,31),0.05,0.06,100,2,4)") == Xl(95.373687943312717));
  CHECK(Ev(t, "PRICE(DATE(2024,1,15),DATE(2024,3,15),0.05,0.06,100,2,2)") == Xl(99.8242085763545));  // one coupon
  CHECK(Ev(t, "PRICE(DATE(2024,1,15),DATE(2024,3,15),0.05,0.06,100,2,3)") == Xl(99.819421709186827));
  CHECK(Ev(t, "YIELD(DATE(2024,1,15),DATE(2027,3,15),0.05,97,100,2,3)") == Xl(0.060526434103778691, 1e-9));
  CHECK(Ev(t, "YIELD(DATE(2024,1,15),DATE(2027,3,15),0.05,97,100,2,2)") == Xl(0.060541161600772234, 1e-9));
  CHECK(Ev(t, "DURATION(DATE(2024,1,15),DATE(2027,3,15),0.05,0.06,2,2)") == Xl(2.9104662713625236));
  CHECK(Ev(t, "MDURATION(DATE(2024,1,15),DATE(2027,3,15),0.05,0.06,2,3)") == Xl(2.8302025096203365));
  // bases 0 and 1 as before
  CHECK(Ev(t, "PRICE(DATE(2024,1,15),DATE(2027,3,15),0.05,0.06,100,2,1)") == Xl(97.14786945202475));
  CHECK(Ev(t, "PRICE(DATE(2008,2,15),DATE(2017,11,15),0.0575,0.065,100,2,0)") == Xl(94.634361621322128));
}

TEST_CASE("Round8: far right tails of the chi-square, Poisson, F and t distributions keep their digits") {
  yctest::Doc t;
  CHECK(Ev(t, "CHISQ.DIST.RT(100,3)") == Xl(1.5541594313896074E-21));  // 1 - CDF gave 0
  CHECK(Ev(t, "CHIDIST(100,3)") == Xl(1.5541594313896074E-21));
  CHECK(Ev(t, "CHISQ.DIST.RT(200,3)") == Xl(4.2185411071920649E-43));
  CHECK(Ev(t, "CHISQ.DIST.RT(150,10)") == Xl(3.7274850550625306E-27));
  CHECK(Ev(t, "POISSON.DIST(0,50,TRUE)") == Xl(1.9287498479639178E-22));
  CHECK(Ev(t, "CHISQ.TEST({100,0;0,100},{50,50;50,50})") == Xl(2.0884875837625446E-45));
  CHECK(Ev(t, "F.DIST.RT(100,10,20)") == Xl(7.825181710807465E-15));  // 7.772E-15 before
  CHECK(Ev(t, "F.DIST.RT(1000,5,5)") == Xl(1.7117826911287113E-07));
  CHECK(Ev(t, "T.DIST.2T(30,5)") == Xl(7.7186486204960435E-07));
  CHECK(Ev(t, "T.DIST.RT(40,3)") == Xl(1.719034039457927E-05));
  CHECK(Ev(t, "T.DIST.RT(-2,5)") == Xl(1 - 0.050969739414929181));
}

TEST_CASE("Round8: MOD stays within zero and the divisor, with the divisor's sign") {
  yctest::Doc t;
  CHECK(Ev(t, "MOD(6.3,2.1)") == 2.0999999999999996);  // -8.88E-16 before
  CHECK(Ev(t, "MOD(-1E-20,1)") == 0);                  // 1 before
  CHECK(Ev(t, "MOD(-1E-17,1)") == 0);
  CHECK(Ev(t, "MOD(1E-20,-1)") == 0);  // -1 before
  CHECK(Ev(t, "MOD(1,0.1)") == Xl(0.09999999999999995));
  CHECK(Ev(t, "MOD(3,0.2)") == Xl(0.19999999999999984));
  CHECK(Ev(t, "MOD(0.3,0.1)") == Xl(0.099999999999999978));
  CHECK(t.Eval("MOD(-3,2)") == "1");
  CHECK(t.Eval("MOD(3,-2)") == "-1");
  CHECK(t.Eval("MOD(-3,-2)") == "-1");
  CHECK(t.Eval("MOD(5.5,2)") == "1.5");
  CHECK(t.Eval("MOD(-0.1,1)") == "0.9");
  CHECK(t.Eval("MOD(2^40,3)") == "1");
}

TEST_CASE("Round8: FLOOR.PRECISE, CEILING.PRECISE and ISO.CEILING read the quotient at 15 digits") {
  yctest::Doc t;
  CHECK(Ev(t, "FLOOR.PRECISE(0.3,0.1)") == Xl(0.30000000000000004));  // 0.2 before
  CHECK(Ev(t, "FLOOR.PRECISE(1.15,0.05)") == Xl(1.1500000000000001));
  CHECK(Ev(t, "FLOOR.PRECISE(2.3,0.1)") == Xl(2.3000000000000003));
  CHECK(Ev(t, "FLOOR.PRECISE(6.3,2.1)") == Xl(6.3000000000000007));
  CHECK(Ev(t, "CEILING.PRECISE(-0.3,0.1)") == Xl(-0.30000000000000004));  // -0.2 before
  CHECK(Ev(t, "CEILING.PRECISE(0.7,0.1)") == Xl(0.70000000000000007));
  CHECK(Ev(t, "ISO.CEILING(0.7,0.1)") == Xl(0.70000000000000007));
  CHECK(t.Eval("ISO.CEILING(-4.1)") == "-4");
  CHECK(t.Eval("CEILING.PRECISE(-4.1,-2)") == "-4");
  CHECK(t.Eval("FLOOR.PRECISE(-3.2,-1)") == "-4");
  CHECK(t.Eval("CEILING.PRECISE(4.1,0)") == "0");
}

TEST_CASE("Round8: a negative base takes an exponent whose reciprocal is odd as that root") {
  yctest::Doc t;
  CHECK(Ev(t, "(-8)^(1/3)") == Xl(-1.9999999999999998));  // #NUM! before
  CHECK(Ev(t, "POWER(-8,1/3)") == Xl(-1.9999999999999998));
  CHECK(Ev(t, "POWER(-27,1/3)") == Xl(-2.9999999999999996));
  CHECK(Ev(t, "(-27)^(1/3)") == Xl(-2.9999999999999996));
  CHECK(Ev(t, "(-32)^0.2") == Xl(-2));
  CHECK(Ev(t, "(-2)^(1/3)") == Xl(-1.2599210498948732));
  CHECK(Ev(t, "(-8)^(1/5)") == Xl(-1.515716566510398));
  CHECK(Ev(t, "POWER(-1,1/3)") == Xl(-1));
  CHECK(t.Eval("(-8)^(2/3)") == "#NUM!");
  CHECK(t.Eval("(-8)^(1/4)") == "#NUM!");
  CHECK(t.Eval("(-8)^0.3333333333") == "#NUM!");
  CHECK(t.Eval("(-2)^0.5") == "#NUM!");
  CHECK(t.Eval("(-2)^3") == "-8");
  CHECK(t.Eval("(-2)^-3") == "-0.125");
}

TEST_CASE("Round8: LINEST and TREND leave out a collinear x column, keeping the later one") {
  yctest::Doc t;
  FillData(t);
  CHECK(Ev(t, "INDEX(LINEST({1,2,3},{1,1,1}),1)") == 0);  // #NUM! before
  CHECK(Ev(t, "INDEX(LINEST({1,2,3},{1,1,1}),2)") == Xl(2));
  CHECK(Ev(t, "INDEX(TREND({1,2,3},{1,1,1},2),1)") == Xl(2));
  CHECK(Ev(t, "INDEX(LINEST(E1:E10,D1:D10^{1,1}),1)") == Xl(1.0376344086021505));
  CHECK(Ev(t, "INDEX(LINEST(E1:E10,D1:D10^{1,1}),2)") == 0);
  CHECK(Ev(t, "INDEX(LINEST(E1:E10,D1:D10^{1,1}),3)") == Xl(1.9193548387096779));
  CHECK(Ev(t, "INDEX(LINEST({1,2,3,5},{1,2,3,4;2,4,6,8}),1)") == Xl(0.65));
  CHECK(Ev(t, "INDEX(LINEST({1,2,3,5},{1,2,3,4}*{1;1}),1)") == Xl(1.3));
  // independent columns as before
  CHECK(Ev(t, "INDEX(LINEST(E1:E10,D1:D10^{1,2}),1)") == Xl(-0.023728419911744143, 1e-9));
  CHECK(Ev(t, "INDEX(LINEST(E1:E10,D1:D10^{1,2}),3)") == Xl(1.1485639080281831, 1e-9));
  CHECK(Ev(t, "INDEX(LINEST(E1:E10,D1:D10,TRUE,TRUE),4,2)") == Xl(8));
  CHECK(Ev(t, "INDEX(LINEST(E1:E10,D1:D10,TRUE,TRUE),2,1)") == Xl(0.0989524855876664, 1e-9));
}

TEST_CASE("Round8: the incomplete gamma converges for large shapes") {
  yctest::Doc t;
  CHECK(Ev(t, "GAMMA.DIST(100000,100000,1,TRUE)") == Xl(0.50042052211036514, 1e-8));  // 0.4996293 before
  CHECK(Ev(t, "POISSON.DIST(100000,100000,TRUE)") == Xl(0.50084104309934019, 1e-8));
  CHECK(Ev(t, "CHISQ.DIST(100000,100000,TRUE)") == Xl(0.50059470810479334, 1e-8));
  CHECK(Ev(t, "GAMMA.DIST(10000,10000,1,TRUE)") == Xl(0.50132980833995522, 1e-9));
  CHECK(Ev(t, "POISSON.DIST(5000,5100,TRUE)") == Xl(0.08143182864539486, 1e-9));
  CHECK(Ev(t, "POISSON.DIST(3,2.5,TRUE)") == Xl(0.75757613313306593));
}

TEST_CASE("Round8: inverse distributions solve far tails to their last digits") {
  yctest::Doc t;
  CHECK(Ev(t, "T.INV(1E-10,3)") == Xl(-2225.7692846830932));  // -2225.768811 before
  CHECK(Ev(t, "T.INV(1E-8,10)") == Xl(-15.895687652513546));
  CHECK(Ev(t, "T.INV(0.975,5)") == Xl(2.570581835636315));
  CHECK(Ev(t, "T.INV.2T(1E-10,30)") == Xl(9.667351762492558));
  CHECK(Ev(t, "CHIINV(1E-10,3)") == Xl(49.542155927523666));
  CHECK(Ev(t, "F.INV.RT(1E-10,3,4)") == Xl(182572.63027878967));
  CHECK(Ev(t, "F.INV.RT(0.05,3,4)") == Xl(6.5913821164255788));
  // Excel's 50.844171332449172 is itself off by 8.5E-7 (the exact quantile is 50.8441279118); 50.844062 before
  CHECK(Ev(t, "CHISQ.INV.RT(1E-12,1)") == Xl(50.844171332449172, 1.5e-6));
  CHECK(t.Eval("T.INV.2T(1,5)") == "0");
  CHECK(t.Eval("T.INV(0.5,10)") == "0");
}

TEST_CASE("Round8: PMT keeps its digits at a rate near zero") {
  yctest::Doc t;
  CHECK(Ev(t, "PMT(1E-12,12,1200)") == Xl(-100.00000000065002, 1e-14));  // -99.99111 before
  CHECK(Ev(t, "PMT(1E-10,12,1200)") == Xl(-100.000000065, 1e-14));
  CHECK(Ev(t, "PMT(1E-8,360,100000)") == Xl(-277.77827916696663));
  CHECK(Ev(t, "PMT(0.05/12,360,-200000)") == Xl(1073.6432460242781));
  CHECK(Ev(t, "IPMT(1E-10,1,12,1200)") == Xl(-1.2000000000000002E-07));
  // FV and PV keep Excel's own (1 + r)^n - 1
  CHECK(Ev(t, "FV(1E-10,12,-100)") == Xl(1200.0000992884452));
  CHECK(Ev(t, "PV(1E-10,12,-100)") == Xl(1200.000097848445));
}

TEST_CASE("Round8: CONVERT takes decimal prefixes on bits and bytes") {
  yctest::Doc t;
  CHECK(t.Eval("CONVERT(1,\"kbyte\",\"byte\")") == "1000");  // #N/A before
  CHECK(t.Eval("CONVERT(1,\"Mbyte\",\"byte\")") == "1000000");
  CHECK(t.Eval("CONVERT(1,\"kbit\",\"bit\")") == "1000");
  CHECK(t.Eval("CONVERT(1,\"Gbit\",\"Mbit\")") == "1000");
  CHECK(t.Eval("CONVERT(1,\"kibyte\",\"byte\")") == "1024");
  CHECK(t.Eval("CONVERT(1,\"Gibyte\",\"Mibyte\")") == "1024");
  CHECK(t.Eval("CONVERT(1,\"Kibyte\",\"byte\")") == "#N/A");
  CHECK(t.Eval("CONVERT(1,\"byte\",\"bit\")") == "8");
}

TEST_CASE("Round8: SMALL takes k down, LARGE up, and QUARTILE a whole quart") {
  yctest::Doc t;
  FillData(t);
  CHECK(t.Eval("SMALL(D1:D10,1.9)") == "1");  // 2 before
  CHECK(t.Eval("SMALL(D1:D10,1.1)") == "1");
  CHECK(t.Eval("SMALL(D1:D10,0.5)") == "#NUM!");
  CHECK(t.Eval("SMALL(D1:D10,10.5)") == "#NUM!");
  CHECK(t.Eval("LARGE(D1:D10,0.9)") == "#NUM!");  // 13 before
  CHECK(t.Eval("LARGE(D1:D10,1.1)") == "8");
  CHECK(t.Eval("LARGE(D1:D10,2.5)") == "5");
  CHECK(t.Eval("LARGE(D1:D10,0)") == "#NUM!");
  CHECK(t.Eval("LARGE(D1:D10,11)") == "#NUM!");
  CHECK(t.Eval("QUARTILE(D1:D10,3.5)") == "5");  // 7.625 (the 0.875 quantile) before
  CHECK(t.Eval("QUARTILE.INC(D1:D10,0.5)") == "1");
  CHECK(Ev(t, "QUARTILE(D1:D10,1)") == Xl(2.25));
  CHECK(t.Eval("QUARTILE(D1:D10,5)") == "#NUM!");
  CHECK(t.Eval("QUARTILE({1,2,3,4,5},3.5)") == "4");
}

TEST_CASE("Round8: TIMEVALUE and typed text take Excel's loose times") {
  yctest::Doc t;
  CHECK(Ev(t, "TIMEVALUE(\"0:90\")") == Xl(0.0625));  // #VALUE! before
  CHECK(Ev(t, "TIMEVALUE(\"0:60\")") == Xl(0.041666666666666664));
  CHECK(Ev(t, "TIMEVALUE(\"10:61\")") == Xl(0.45902777777777776));
  CHECK(Ev(t, "TIMEVALUE(\"6:00:60\")") == Xl(0.25069444444444444));
  CHECK(Ev(t, "TIMEVALUE(\"0:59:60\")") == Xl(0.041666666666666664));
  CHECK(Ev(t, "TIMEVALUE(\"0:0:90\")") == Xl(0.0010416666666666667));
  CHECK(Ev(t, "TIMEVALUE(\"6:\")") == Xl(0.25));
  CHECK(Ev(t, "TIMEVALUE(\"1:30.5\")") == Xl(0.0010474537037037037));  // mm:ss.0
  CHECK(Ev(t, "TIMEVALUE(\"1:30.25\")") == Xl(0.0010445601851851853));
  CHECK(Ev(t, "TIMEVALUE(\"61:30.5\")") == Xl(0.042714120370370368));
  CHECK(Ev(t, "\"1:30.5\"*1") == Xl(0.0010474537037037037));
  CHECK(Ev(t, "\"0:90\"*1") == Xl(0.0625));
  CHECK(Ev(t, "TIMEVALUE(\"6:30:15.75\")") == Xl(0.271015625));
  CHECK(t.Eval("TIMEVALUE(\"10000:00\")") == "#VALUE!");
  CHECK(t.Eval("TIMEVALUE(\"13:00 PM\")") == "#VALUE!");
  CHECK(t.Eval("TIMEVALUE(\"abc\")") == "#VALUE!");
  // typed into a cell: a time in mm:ss.0, as Excel formats 1:30.5
  const auto p = formula::ParseInput("1:30.5");
  CHECK(p.value.AsDouble() == Xl(0.0010474537037037037));
  CHECK(p.format == "mm:ss.0");
}

TEST_CASE("Round8: EDATE and EOMONTH stay within 1900 and 9999") {
  yctest::Doc t;
  CHECK(t.Eval("EDATE(1,-1)") == "#NUM!");  // a serial before 1900 before
  CHECK(t.Eval("EDATE(15,-1)") == "#NUM!");
  CHECK(t.Eval("EDATE(31,-1)") == "#NUM!");
  CHECK(t.Eval("EOMONTH(1,-1)") == "#NUM!");
  CHECK(t.Eval("EDATE(DATE(9999,12,1),1)") == "#NUM!");
  CHECK(t.Eval("EOMONTH(DATE(9999,12,1),1)") == "#NUM!");
  CHECK(t.Eval("EOMONTH(DATE(9999,12,1),0)") == "2958465");
  CHECK(t.Eval("EOMONTH(0,0)") == "31");
  CHECK(t.Eval("EDATE(DATE(1900,1,31),1)") == "59");
  CHECK(t.Eval("EDATE(60,12)") == "425");
  CHECK(t.Eval("EDATE(DATE(2024,1,31),-1.5)") == "45291");
}

TEST_CASE("Round8: argument rules of date, math, engineering and financial functions") {
  yctest::Doc t;
  t.Set("A1", "5");
  t.Set("A2", "'5");
  t.Set("A3", "TRUE");
  t.Set("A6", "abc");
  FillData(t);
  // WORKDAY takes a fraction of a day down
  CHECK(t.Eval("WORKDAY(DATE(2024,1,1),-0.5)") == "45289");  // 45292 before
  CHECK(t.Eval("WORKDAY(DATE(2024,1,1),-2.9)") == "45287");
  CHECK(t.Eval("WORKDAY(DATE(2024,1,1),-1.1)") == "45288");
  CHECK(t.Eval("WORKDAY(DATE(2024,1,1),2.9)") == "45294");
  CHECK(t.Eval("WORKDAY.INTL(DATE(2024,1,1),-2.5,1)") == "45287");
  // a holiday that is text
  CHECK(t.Eval("WORKDAY(DATE(2024,1,1),1,A6)") == "#VALUE!");
  CHECK(t.Eval("NETWORKDAYS(DATE(2024,1,1),DATE(2024,1,15),A6)") == "#VALUE!");
  CHECK(t.Eval("NETWORKDAYS(DATE(2024,1,1),DATE(2024,1,15),A4)") == "11");
  // TIME's parts are at most 32767
  CHECK(t.Eval("TIME(0,0,86400)") == "#NUM!");
  CHECK(t.Eval("TIME(32768,0,0)") == "#NUM!");
  CHECK(t.Eval("TIME(0,32768,0)") == "#NUM!");
  CHECK(t.Eval("TIME(0,0,32768)") == "#NUM!");
  CHECK(Ev(t, "TIME(0,0,32767)") == Xl(0.3792476851851852));
  CHECK(Ev(t, "TIME(0,32767,32767)") == Xl(0.13410879629629591, 1e-9));
  // GCD / LCM refuse text and logicals in ranges
  CHECK(t.Eval("GCD(A1:A3)") == "#VALUE!");  // 5 before
  CHECK(t.Eval("LCM(A1:A3)") == "#VALUE!");
  CHECK(t.Eval("GCD(D1:D3,A6)") == "#VALUE!");
  CHECK(t.Eval("GCD(A1,A2)") == "5");
  CHECK(t.Eval("GCD(\"6\",4)") == "2");
  CHECK(t.Eval("GCD(D1:D10)") == "1");
  CHECK(t.Eval("LCM(D1:D10)") == "1560");
  CHECK(t.Eval("GCD(A4,6)") == "6");
  // base conversions refuse logicals
  CHECK(t.Eval("DEC2BIN(TRUE)") == "#VALUE!");  // 1 before
  CHECK(t.Eval("DEC2HEX(TRUE)") == "#VALUE!");
  CHECK(t.Eval("DEC2HEX(A3)") == "#VALUE!");
  CHECK(t.Eval("BIN2DEC(TRUE)") == "#VALUE!");  // #NUM! before
  CHECK(t.Eval("DEC2BIN(\"5\")") == "101");
  CHECK(t.Eval("DEC2BIN(A2)") == "101");
  CHECK(t.Eval("BIN2DEC(101)") == "5");
  CHECK(t.Eval("DEC2BIN(-1)") == "1111111111");
  // ISO weeks of the first days of 1900 belong to 1899's week 52
  CHECK(t.Eval("ISOWEEKNUM(0)") == "52");  // 0 before
  CHECK(t.Eval("ISOWEEKNUM(1)") == "52");
  CHECK(t.Eval("ISOWEEKNUM(2)") == "1");
  CHECK(t.Eval("ISOWEEKNUM(60)") == "9");
  CHECK(t.Eval("WEEKNUM(1,21)") == "52");
  CHECK(t.Eval("WEEKNUM(0,21)") == "52");
  CHECK(t.Eval("WEEKNUM(0)") == "0");
  CHECK(t.Eval("ISOWEEKNUM(-1)") == "#NUM!");
  // DATEDIF "yd" from a February 29th
  CHECK(t.Eval("DATEDIF(DATE(2020,2,29),DATE(2024,2,28),\"yd\")") == "365");  // 364 before
  CHECK(t.Eval("DATEDIF(DATE(2020,2,29),DATE(2023,2,28),\"yd\")") == "365");
  CHECK(t.Eval("DATEDIF(DATE(2020,2,29),DATE(2021,3,1),\"yd\")") == "0");
  CHECK(t.Eval("DATEDIF(DATE(2024,2,29),DATE(2025,2,28),\"yd\")") == "365");
  CHECK(t.Eval("DATEDIF(DATE(2020,3,1),DATE(2024,2,28),\"yd\")") == "364");
  CHECK(t.Eval("DATEDIF(DATE(2023,2,28),DATE(2024,2,29),\"yd\")") == "1");
  // DAYS360 and Excel's February 29th, 1900
  CHECK(t.Eval("DAYS360(60,90)") == "30");  // 31 before
  CHECK(t.Eval("DAYS360(59,90)") == "32");  // 30 before
  CHECK(t.Eval("DAYS360(60,91,TRUE)") == "31");
  // DATE with a year past 9999 that the months bring back
  CHECK(t.Eval("DATE(10000,-1,1)") == "2958405");  // #NUM! before
  CHECK(t.Eval("DATE(10000,1,1)") == "#NUM!");
  CHECK(t.Eval("DATE(9999,12,31)") == "2958465");
  CHECK(t.Eval("DATE(-1,1,1)") == "#NUM!");
  // FLOOR(0,0), SKEW.P of two values
  CHECK(t.Eval("FLOOR(0,0)") == "0");  // #DIV/0! before
  CHECK(t.Eval("FLOOR(5,0)") == "#DIV/0!");
  CHECK(t.Eval("SKEW.P(1,2)") == "#DIV/0!");  // 0 before
  CHECK(Ev(t, "SKEW.P(D1:D10)") == Xl(1.284918218084024, 1e-9));
  // financial edge rates
  CHECK(t.Eval("PMT(-1,10,100)") == "#NUM!");  // 0 before
  CHECK(t.Eval("PMT(-2,10,100)") == "#NUM!");
  CHECK(Ev(t, "PMT(-0.5,10,100)") == Xl(-0.048875855327468229, 1e-9));
  CHECK(t.Eval("NPER(0,0,100)") == "#DIV/0!");  // #NUM! before
  CHECK(t.Eval("PV(-1,10,100)") == "#DIV/0!");  // #NUM! before
  CHECK(t.Eval("FV(-1,10,100)") == "-100");
  CHECK(t.Eval("XNPV(0,{-100,110},{45292,45658})") == "#NUM!");  // 10 before
  CHECK(t.Eval("XNPV(-1,{-100,110},{45292,45658})") == "#NUM!");
  CHECK(Ev(t, "XNPV(0.09,{-10000,2750,4250,3250,2750},{39448,39508,39673,39751,39859})") == Xl(2262.7380360566813, 1e-9));
}
