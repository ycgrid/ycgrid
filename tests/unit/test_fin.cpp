// SPDX-License-Identifier: Apache-2.0
// Financial functions: results checked against Excel's documented examples
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

using Catch::Approx;

namespace {

double Ev(yctest::Doc& t, const std::string& formula) {
  t.Set("Z1000", "=" + formula);
  return t.N("Z1000");
}

std::string EvText(yctest::Doc& t, const std::string& formula) { return t.Eval(formula); }

}  // namespace

TEST_CASE("PV FV PMT NPER") {
  yctest::Doc t;
  CHECK(Ev(t, "PMT(0.08/12,10,10000)") == Approx(-1037.0320893).epsilon(1e-9));
  CHECK(Ev(t, "PMT(0.06/12,18*12,0,50000)") == Approx(-129.0811609).epsilon(1e-9));
  CHECK(Ev(t, "PV(0.08/12,12*20,500,,0)") == Approx(-59777.14585).epsilon(1e-9));
  CHECK(Ev(t, "FV(0.06/12,10,-200,-500,1)") == Approx(2581.4033740).epsilon(1e-9));
  CHECK(Ev(t, "FV(0.12/12,12,-1000)") == Approx(12682.5030131).epsilon(1e-9));
  CHECK(Ev(t, "NPER(0.12/12,-100,-1000,10000,1)") == Approx(59.6738657).epsilon(1e-9));
  CHECK(Ev(t, "NPER(0.12/12,-100,-1000)") == Approx(-9.5785940).epsilon(1e-8));
  CHECK(Ev(t, "PMT(0,10,1000)") == Approx(-100));  // zero rate
  CHECK(EvText(t, "PMT(0.1,0,1000)") == "#NUM!");
}

TEST_CASE("IPMT PPMT") {
  yctest::Doc t;
  CHECK(Ev(t, "IPMT(0.1/12,1,3*12,8000)") == Approx(-66.6666667).epsilon(1e-9));
  CHECK(Ev(t, "IPMT(0.1,3,3,8000)") == Approx(-292.4471299).epsilon(1e-9));
  CHECK(Ev(t, "PPMT(0.1/12,1,2*12,2000)") == Approx(-75.6231860).epsilon(1e-9));
  CHECK(Ev(t, "PPMT(0.08,10,10,200000)") == Approx(-27598.0534624).epsilon(1e-9));
  // interest + principal = payment
  CHECK(Ev(t, "IPMT(0.05/12,7,60,30000)+PPMT(0.05/12,7,60,30000)") == Approx(Ev(t, "PMT(0.05/12,60,30000)")));
  CHECK(EvText(t, "IPMT(0.1,0,3,8000)") == "#NUM!");
}

TEST_CASE("IPMT and PPMT keep their digits as Excel's do (measured in Excel 16)") {
  yctest::Doc t;
  // PPMT where PMT and IPMT nearly cancel: Excel's is within a few units in the last place of the exact value; the
  // difference PMT - IPMT was 2E-4 away
  CHECK(Ev(t, "PPMT(0.08,10,360,8.172E3,0,1)") == Approx(-1.1226811053165241E-09).epsilon(1e-14));
  CHECK(Ev(t, "PPMT(0.08,3,360,1.548E5,-1E4,1)") == Approx(-1.1607276474096786E-08).epsilon(1e-14));
  CHECK(Ev(t, "PPMT(0.01,4,360,2.358E4,0,0)") == Approx(-6.951286667641822).epsilon(2e-15));
  CHECK(Ev(t, "PPMT(0.01,10,360,9.311E5,-1E4,0)") == Approx(-288.2414429448149).epsilon(2e-15));
  CHECK(Ev(t, "PPMT(0.003,11,12,1.699E7,-1E4,1)") == Approx(-1429836.8524079975).epsilon(2e-15));
  CHECK(Ev(t, "PPMT(0.05/12,12,24.5,1.565E3,0,1)") == Approx(-63.38706503671495).epsilon(2e-15));
  // IPMT over few periods at a small rate: (1 + r)^k - 1 keeps its digits (113 units in the last place before)
  CHECK(Ev(t, "IPMT(0.003,10,24.5,8.564E6,-1E4,0)") == Approx(-16483.25627720822).epsilon(2e-15));
  CHECK(Ev(t, "IPMT(0.003,8,36,9.001E6,-1E4,0)") == Approx(-21983.519125151095).epsilon(2e-15));
  CHECK(Ev(t, "IPMT(0.07/12,10,12,3.702E6,-1E4,1)") == Approx(-5551.846809959378).epsilon(2e-15));
  CHECK(Ev(t, "IPMT(0.07/12,5,12,2.915E7,0,1)") == Approx(-114009.52768531209).epsilon(2e-15));
  CHECK(Ev(t, "IPMT(0.08,1,36,1.774E6,0,0)") == -141920);
  CHECK(Ev(t, "IPMT(0.08,1,36,1.774E6,0,1)") == 0);
  // interest + principal is still the payment
  CHECK(Ev(t, "IPMT(0.08,10,360,8.172E3,0,1)+PPMT(0.08,10,360,8.172E3,0,1)") ==
        Approx(Ev(t, "PMT(0.08,360,8.172E3,0,1)")).epsilon(1e-15));
}

TEST_CASE("RATE") {
  yctest::Doc t;
  CHECK(Ev(t, "RATE(4*12,-200,8000)") == Approx(0.0077014725).epsilon(1e-7));
  CHECK(Ev(t, "RATE(4*12,-200,8000)*12") == Approx(0.0924176700).epsilon(1e-7));
  CHECK(Ev(t, "RATE(10,0,-100,200)") == Approx(0.0717734625).epsilon(1e-8));
  CHECK(EvText(t, "RATE(10,100,100)") == "#NUM!");  // no solution
}

TEST_CASE("RATE searches as Excel's does (measured in Excel 16 to the last bit)") {
  yctest::Doc t;
  // no finite root: the search creeps toward -1 and stops where Excel's stops
  CHECK(Ev(t, "RATE(10,0,-100,0)") == -0.99999874110137132);
  CHECK(Ev(t, "RATE(2,0,-100,0)") == -0.99999988072370694);
  CHECK(Ev(t, "RATE(2,0,100,0)") == -0.99999988085766234);  // the second point goes toward the root
  CHECK(Ev(t, "RATE(10,0,-1E50,0)") == -0.99999874110137132);
  CHECK(Ev(t, "RATE(10,0,-100,0,0,-0.5)") == -0.99999872133611512);
  CHECK(Ev(t, "RATE(10,0,-100,0,0,-1+1E-5)") == -0.99999);
  CHECK(Ev(t, "RATE(20,0,-100,0,0,-1+1E-3)") == -0.99900000190733318);
  // still creeping after 200 steps: returned where it is, or #NUM!
  CHECK(Ev(t, "RATE(20,0,-100,0)") == -0.99911207191253926);
  CHECK(Ev(t, "RATE(24,0,-100,0)") == -0.9970141220397587);
  CHECK(Ev(t, "RATE(13,0,-100,0,0,9)") == -0.99985107586580324);
  CHECK(Ev(t, "RATE(20,0,100,0)") == -0.99911307159991847);
  CHECK(EvText(t, "RATE(11,0,-100,0)") == "#NUM!");
  CHECK(EvText(t, "RATE(25,0,-100,0)") == "#NUM!");
  CHECK(EvText(t, "RATE(16,0,-100,0,0,-0.5)") == "#NUM!");
  CHECK(EvText(t, "RATE(2,0,1E50,0)") == "#NUM!");
  CHECK(EvText(t, "RATE(58,0,-195.372,-19.0121,0,5)") == "#NUM!");
  // the rate must stay above -1 + 1E-7; a guess at or below -1 is #VALUE!
  CHECK(EvText(t, "RATE(1,0,-100,0)") == "#NUM!");
  CHECK(EvText(t, "RATE(1,0,-1,1E-7)") == "#NUM!");
  CHECK(Ev(t, "RATE(1,0,-1,1.5E-7)") == -0.99999985);
  CHECK(EvText(t, "RATE(3,0,1,8)") == "#NUM!");
  CHECK(EvText(t, "RATE(10,-100,1000,0,0,-1)") == "#VALUE!");
  CHECK(EvText(t, "RATE(3,0,1,8,0,-2.5)") == "#VALUE!");
  // ordinary rates, to the last bit
  CHECK(Ev(t, "RATE(360,-1073.64,200000)") == 0.0041666445363455892);
  CHECK(Ev(t, "RATE(4*12,-200,8000)") == 0.0077014724882013682);
  CHECK(Ev(t, "RATE(10,0,-100,200)") == 0.071773462536441887);
  CHECK(Ev(t, "RATE(12,-100,1000,0,1)") == 0.03503153036227754);
  CHECK(Ev(t, "RATE(12,-100,1000,0,2)") == 0.03503153036227754);
  CHECK(Ev(t, "RATE(100,-100,-100,500)") == -0.19999999996183102);
  CHECK(Ev(t, "RATE(12,-1E14,1E15)") == 0.029228540769133924);
  CHECK(Ev(t, "RATE(12,-100,1000,0,0,200)") == 0.029228540774119474);
  CHECK(Ev(t, "RATE(10,0,-100,200,0,0.0717834625362931)") == 0.071773462536293312);
  CHECK(Ev(t, "RATE(3,-100,270,0,1,0)") == 0.11554353198640598);
  CHECK(Ev(t, "RATE(2.5,-10,100,-50)") == -0.11920151591921034);
  CHECK(Ev(t, "RATE(10.5,0,-100,250)") == 0.091186661124439669);
  // a root at 0 or a double root: Excel's answer carries the search's rounding
  CHECK(Ev(t, "RATE(10,-100,1000)") == -1.9633517072925619E-10);
  CHECK(Ev(t, "RATE(5,-10,100,-50)") == 1.9756829119077782E-09);
  CHECK(Ev(t, "RATE(2,200,-100,-300,0,0.1)") == -2.1549976505804635e-06);
  CHECK(Ev(t, "RATE(1,-1,1,0,1)") == 0.1008);  // f is 0 for every rate
  CHECK(Ev(t, "RATE(5,-100,500,0,0,0)") == 0);
  CHECK(Ev(t, "RATE(12,-100,1000,,,)") == 0.02922854076912796);  // an empty guess is 0
}

TEST_CASE("RATE of large amounts ends where Excel's search ends (measured in Excel 16 to the last bit)") {
  yctest::Doc t;
  // a mortgage of 1E9: |f| stays one unit in the last place above 1E-7 and the points step back and forth; Excel
  // returns the point in its first slot (the last one numbered a multiple of 3) when |f| there is below 1E-4
  CHECK(Ev(t, "RATE(360,-1E7,1E9)") == 0.0096892458225819218);  // #NUM! before
  CHECK(Ev(t, "RATE(360,-1E7,1E9,0,0,0.0096)") == 0.0096892458225819218);
  CHECK(Ev(t, "RATE(360,-1E7,1E9,0,0,0.2)") == 0.0096892458225819236);
  CHECK(Ev(t, "RATE(360,-1E7,1E9,0,0,0.00968924582258192)") == 0.0096892458225819236);
  CHECK(Ev(t, "RATE(360,1E7,-1E9)") == 0.0096892458225819236);
  CHECK(Ev(t, "RATE(360,-3E5,100*3E5)") == 0.0096892458225819236);
  CHECK(Ev(t, "RATE(360,-8E7,100*8E7)") == 0.0096892458225819218);
  CHECK(Ev(t, "RATE(360,-1E7,1E9,0,1)") == 0.0097958647477931638);
  CHECK(Ev(t, "RATE(360,-1E7,1E9,-1E8)") == 0.0097227955955259543);
  CHECK(Ev(t, "RATE(360,-2.5E7,4E9)") == 0.0053268934354218228);
  CHECK(Ev(t, "RATE(60,-2E7,1E9)") == 0.0061834131612539308);
  CHECK(Ev(t, "RATE(240,-3.4E8,80*3.4E8)") == 0.011740948230747989);
  CHECK(Ev(t, "RATE(120,-1E9,50*1E9,0,1)") == 0.017952015939710124);
  CHECK(Ev(t, "RATE(480,-1E5,1E7,0,1)") == 0.010015660553027874);
  CHECK(Ev(t, "RATE(60,-1E8,4E9)") == 0.01439478100091383);
  // |f| stays at 1E-4 or more: #NUM!, as in Excel
  CHECK(EvText(t, "RATE(360,-1E10,1E12)") == "#NUM!");
  CHECK(EvText(t, "RATE(360,-7E8,7E10)") == "#NUM!");
  CHECK(EvText(t, "RATE(120,-1.9E9,50*1.9E9,0,1)") == "#NUM!");
  CHECK(EvText(t, "RATE(360,-1E7,1E9,0,0,0.005)") == "#NUM!");
  // a function that is flat from the start is no answer
  CHECK(EvText(t, "RATE(12,0,0,0)") == "#NUM!");
  CHECK(EvText(t, "RATE(351,0,-153.8936,0,0,-0.9)") == "#NUM!");
}

TEST_CASE("FV PV PMT compute (1 + r)^n as Excel does (measured in Excel 16)") {
  yctest::Doc t;
  // whole periods by repeated squaring, a negative number of them as 1 over that, others as exp(n ln(1 + r))
  CHECK(Ev(t, "FV(0.05/12,360,-100)") == 83225.86353614775);
  CHECK(Ev(t, "FV(0.07,30,-1000)") == 94460.786323743276);
  CHECK(Ev(t, "FV(0.003,600,-50,-1000)") == 89922.833860369457);
  CHECK(Ev(t, "FV(0.05/12,36.25,-9.752E6,0,0)") == 380749783.531307);
  CHECK(Ev(t, "FV(0.01,-12,-100)") == -1125.507747348463);
  CHECK(Ev(t, "FV(0.05,-10,100,1000)") == 158.26023937772197);
  CHECK(Ev(t, "FV(-1.5,-2,-100)") == -200);
  CHECK(Ev(t, "PV(0.08/12,240,500)") == -59777.145851187823);
  CHECK(Ev(t, "PV(0.1,20,-100,1000,1)") == 787.84838114929869);
  CHECK(Ev(t, "PV(0.12/12,480,-5.652E6,0,1)") == 566040681.45590854);
  CHECK(Ev(t, "PV(0.05,-10,100)") == 1257.7892535548831);
  // PMT: within a few units in the last place of Excel's, often to the last bit
  CHECK(Ev(t, "PMT(0.05/12,360,2E5)") == -1073.6432460242781);
  CHECK(Ev(t, "PMT(0.01,480,1E7,0,1)") == -99851.48038233821);
  CHECK(Ev(t, "PMT(0.003,360,8.458E6,0,0)") == -38453.903722044219);
  CHECK(Ev(t, "PMT(0.07/12,360,1E6)") == Approx(-6653.0249517918319).epsilon(1e-15));  // 62 units away before
  CHECK(Ev(t, "PMT(0.0425/12,360,350000)") == Approx(-1721.7896187781928).epsilon(1e-15));  // 298 before
  CHECK(Ev(t, "PMT(0.05/12,1E6,1000)") == -4.166666666666667);  // (1 + r)^n overflows: the interest alone
}

TEST_CASE("NPV IRR") {
  yctest::Doc t;
  CHECK(Ev(t, "NPV(0.1,-10000,3000,4200,6800)") == Approx(1188.4434123).epsilon(1e-9));
  t.Set("A1", "-70000");
  t.Set("A2", "12000");
  t.Set("A3", "15000");
  t.Set("A4", "18000");
  t.Set("A5", "21000");
  t.Set("A6", "26000");
  t.Set("A7", "text is skipped");
  CHECK(Ev(t, "IRR(A1:A6)") == Approx(0.0866309480).epsilon(1e-8));
  CHECK(Ev(t, "IRR(A1:A7)") == Approx(0.0866309480).epsilon(1e-8));
  CHECK(Ev(t, "IRR(A1:A4)") == Approx(-0.1821374641).epsilon(1e-8));
  CHECK(Ev(t, "IRR(A1:A3,-0.1)") == Approx(-0.4435069413).epsilon(1e-8));
  CHECK(Ev(t, "IRR({-100,110})") == Approx(0.1));
  CHECK(EvText(t, "IRR({100,110})") == "#NUM!");  // no sign change
}

TEST_CASE("XNPV XIRR") {
  yctest::Doc t;
  const char* values[] = {"-10000", "2750", "4250", "3250", "2750"};
  const char* dates[] = {"39448", "39508", "39751", "39859", "39904"};  // 2008/1/1, 3/1, 10/30, 2009/2/15, 4/1
  for (int i = 0; i < 5; ++i) {
    t.Set(("B" + std::to_string(i + 1)).c_str(), values[i]);
    t.Set(("C" + std::to_string(i + 1)).c_str(), dates[i]);
  }
  CHECK(Ev(t, "XNPV(0.09,B1:B5,C1:C5)") == Approx(2086.6476020).epsilon(1e-8));
  CHECK(Ev(t, "XIRR(B1:B5,C1:C5)") == Approx(0.3733625335).epsilon(1e-7));
  CHECK(EvText(t, "XNPV(0.09,B1:B5,C1:C4)") == "#NUM!");  // different sizes
}
