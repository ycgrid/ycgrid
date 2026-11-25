// SPDX-License-Identifier: Apache-2.0
// Distributions, descriptive statistics, hyperbolic and DBCS text functions: Excel's documented examples
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

using Catch::Approx;

namespace {

double Ev(yctest::Doc& t, const std::string& formula) {
  t.Set("Z1000", "=" + formula);
  return t.N("Z1000");
}

// relative tolerance of Excel's documentation, which prints 6-8 significant digits
Approx Near(double v) { return Approx(v).epsilon(1e-5); }

}  // namespace

TEST_CASE("T distribution") {
  yctest::Doc t;
  CHECK(Ev(t, "T.DIST(60,1,TRUE)") == Near(0.99469533));
  CHECK(Ev(t, "T.DIST(8,3,FALSE)") == Near(0.00073691));
  CHECK(Ev(t, "T.DIST.2T(1.959999998,60)") == Near(0.054644930));
  CHECK(Ev(t, "T.DIST.RT(1.959999998,60)") == Near(0.027322465));
  CHECK(Ev(t, "TDIST(1.959999998,60,2)") == Near(0.054644930));
  CHECK(Ev(t, "T.INV(0.75,2)") == Near(0.8164966));
  CHECK(Ev(t, "T.INV.2T(0.546449,60)") == Near(0.606533));
  CHECK(Ev(t, "TINV(0.546449,60)") == Near(0.606533));
  CHECK(t.Eval("T.DIST(1,0,TRUE)") == "#NUM!");
}

TEST_CASE("Chi-squared, F and gamma distributions") {
  yctest::Doc t;
  CHECK(Ev(t, "CHISQ.DIST(0.5,1,TRUE)") == Near(0.52049988));
  CHECK(Ev(t, "CHISQ.DIST(2,3,FALSE)") == Near(0.20755375));
  CHECK(Ev(t, "CHISQ.DIST.RT(18.307,10)") == Near(0.0500006));
  CHECK(Ev(t, "CHISQ.INV(0.93,1)") == Near(3.283020));
  CHECK(Ev(t, "CHISQ.INV.RT(0.050001,10)") == Near(18.306973));
  CHECK(Ev(t, "F.DIST(15.2068649,6,4,TRUE)") == Near(0.99));
  CHECK(Ev(t, "F.DIST(15.2068649,6,4,FALSE)") == Near(0.0012238));
  CHECK(Ev(t, "F.DIST.RT(15.2068649,6,4)") == Near(0.01));
  CHECK(Ev(t, "F.INV(0.01,6,4)") == Near(0.10930991));
  CHECK(Ev(t, "F.INV.RT(0.01,6,4)") == Near(15.206865));
  CHECK(Ev(t, "GAMMA.DIST(10.00001131,9,2,FALSE)") == Near(0.032639));
  CHECK(Ev(t, "GAMMA.DIST(10.00001131,9,2,TRUE)") == Near(0.068094));
  CHECK(Ev(t, "GAMMA.INV(0.068094,9,2)") == Near(10.0000112));
  CHECK(Ev(t, "GAMMALN(4)") == Near(1.7917595));
  CHECK(Ev(t, "GAMMA(2.5)") == Near(1.329340));
  CHECK(t.Eval("GAMMA(-1)") == "#NUM!");
}

TEST_CASE("Discrete and other continuous distributions") {
  yctest::Doc t;
  CHECK(Ev(t, "BINOM.DIST(6,10,0.5,FALSE)") == Near(0.2050781));
  CHECK(Ev(t, "BINOM.DIST(6,10,0.5,TRUE)") == Near(0.828125));
  CHECK(Ev(t, "POISSON.DIST(2,5,TRUE)") == Near(0.124652));
  CHECK(Ev(t, "POISSON.DIST(2,5,FALSE)") == Near(0.084224));
  CHECK(Ev(t, "EXPON.DIST(0.2,10,TRUE)") == Near(0.86466472));
  CHECK(Ev(t, "EXPON.DIST(0.2,10,FALSE)") == Near(1.35335283));
  CHECK(Ev(t, "WEIBULL.DIST(105,20,100,TRUE)") == Near(0.929581));
  CHECK(Ev(t, "WEIBULL.DIST(105,20,100,FALSE)") == Near(0.035589));
  CHECK(Ev(t, "LOGNORM.DIST(4,3.5,1.2,TRUE)") == Near(0.0390836));
  CHECK(Ev(t, "LOGNORM.DIST(4,3.5,1.2,FALSE)") == Near(0.0176176));
  CHECK(Ev(t, "LOGNORM.INV(0.039084,3.5,1.2)") == Near(4.0000252));
  CHECK(Ev(t, "CONFIDENCE.NORM(0.05,2.5,50)") == Near(0.692952));
  CHECK(Ev(t, "CONFIDENCE.T(0.05,1,50)") == Near(0.284196));
}

TEST_CASE("Descriptive statistics") {
  yctest::Doc t;
  CHECK(Ev(t, "DEVSQ(4,5,8,7,11,4,3)") == Near(48));
  CHECK(Ev(t, "AVEDEV(4,5,6,7,5,4,3)") == Near(1.020408));
  CHECK(Ev(t, "SKEW(3,4,5,2,3,4,5,6,4,7)") == Near(0.359543));
  CHECK(Ev(t, "KURT(3,4,5,2,3,4,5,6,4,7)") == Near(-0.151799637));
  CHECK(Ev(t, "HARMEAN(4,5,8,7,11,4,3)") == Near(5.028376));
  CHECK(Ev(t, "TRIMMEAN({4,5,6,7,2,3,4,5,1,2,3},0.2)") == Near(3.777778));
  CHECK(Ev(t, "PERCENTRANK.INC({13,12,11,8,4,3,2,1,1,1},2)") == Near(0.333));
  CHECK(Ev(t, "PERCENTRANK.INC({13,12,11,8,4,3,2,1,1,1},4)") == Near(0.555));
  CHECK(Ev(t, "PERCENTRANK.INC({13,12,11,8,4,3,2,1,1,1},8)") == Near(0.666));
  CHECK(Ev(t, "PERCENTRANK.INC({13,12,11,8,4,3,2,1,1,1},5)") == Near(0.583));
  CHECK(Ev(t, "PERCENTRANK.EXC({1,2,3,6,6,6,7,8,9},7)") == Near(0.7));
  CHECK(Ev(t, "PERCENTRANK.EXC({1,2,3,6,6,6,7,8,9},5.43)") == Near(0.381));
  CHECK(Ev(t, "PERCENTRANK.EXC({1,2,3,6,6,6,7,8,9},5.43,1)") == Near(0.3));
  CHECK(Ev(t, "PEARSON({9,7,5,3,1},{10,6,1,5,3})") == Near(0.699379));
  CHECK(Ev(t, "RSQ({2,3,9,1,8,7,5},{6,5,11,7,5,4,4})") == Near(0.05795));
  CHECK(Ev(t, "STEYX({2,3,9,1,8,7,5},{6,5,11,7,5,4,4})") == Near(3.305719));
  CHECK(Ev(t, "COVARIANCE.P({3,2,4,5,6},{9,7,12,15,17})") == Near(5.2));
  CHECK(t.Eval("PERCENTRANK.INC({1,2,3},5)") == "#N/A");
}

TEST_CASE("Hyperbolic functions") {
  yctest::Doc t;
  CHECK(Ev(t, "SINH(1)") == Near(1.175201194));
  CHECK(Ev(t, "COSH(4)") == Near(27.308233));
  CHECK(Ev(t, "TANH(-2)") == Near(-0.96402758));
  CHECK(Ev(t, "ASINH(10)") == Near(2.99822295));
  CHECK(Ev(t, "ACOSH(10)") == Near(2.9932228));
  CHECK(Ev(t, "ATANH(0.76159416)") == Near(1.00000001));
  CHECK(t.Eval("ACOSH(0.5)") == "#NUM!");
}

TEST_CASE("DBCS byte functions and width conversion") {
  yctest::Doc t;
  CHECK(Ev(t, "LENB(\"ABC中文\")") == 7);
  CHECK(Ev(t, "LEN(\"ABC中文\")") == 5);
  CHECK(t.Eval("LEFTB(\"中文ABC\",4)") == "中文");
  CHECK(t.Eval("LEFTB(\"中文ABC\",3)") == "中 ");  // half of a double-byte character becomes a space
  CHECK(t.Eval("RIGHTB(\"中文ABC\",4)") == " ABC");  // the last 4 bytes: half of 文 (a space) + ABC
  CHECK(t.Eval("MIDB(\"中文ABC\",3,3)") == "文A");
  CHECK(Ev(t, "FINDB(\"A\",\"中文ABC\")") == 5);
  CHECK(Ev(t, "SEARCHB(\"b\",\"中文ABC\")") == 6);
  CHECK(t.Eval("REPLACEB(\"中文ABC\",3,2,\"字\")") == "中字ABC");
  CHECK(t.Eval("ASC(\"ＡＢＣ　１２３\")") == "ABC 123");
  CHECK(t.Eval("DBCS(\"AB 1\")") == "ＡＢ　１");
  CHECK(t.Eval("INFO(\"system\")") == "pcdos");
}
