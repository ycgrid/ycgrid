// SPDX-License-Identifier: Apache-2.0
// Analysis ToolPak (engine/analysis.cpp, Document::RunAnalysis): output layouts and numbers, checked against the
// worksheet functions (which are checked against Excel elsewhere)
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

using Catch::Approx;
using yc::core::Rect;
using yc::engine::Document;
using yc::engine::analysis::Tool;

namespace {

Rect R(const char* a, const char* b) {
  const auto p = yctest::Doc::P(a), q = yctest::Doc::P(b);
  return Rect{p.row, p.col, q.row, q.col};
}

// runs a tool with the input at in (and in2), output at out on the same sheet
bool Run(yctest::Doc& t, Document::AnalysisRequest req, const char* out, std::string* err = nullptr) {
  req.in_sheet = req.in2_sheet = req.bins_sheet = req.out_sheet = t.sheet();
  const auto o = yctest::Doc::P(out);
  req.out_r = o.row;
  req.out_c = o.col;
  std::string e;
  uint32_t sh = 0;
  Rect rect{};
  const bool ok = t.d.RunAnalysis(req, &sh, &rect, err ? err : &e);
  t.d.Recalc();
  return ok;
}

double EvalN(yctest::Doc& t, const std::string& f) {
  t.Set("Z1000", "=" + f);
  return t.N("Z1000");
}

Document::AnalysisRequest Req(Tool tool, Rect in) {
  Document::AnalysisRequest r;
  r.p.tool = tool;
  r.in = in;
  r.has_in = true;
  return r;
}

}  // namespace

TEST_CASE("analysis: descriptive statistics") {
  yctest::Doc t;
  t.Set("A1", "Price");
  for (int i = 1; i <= 10; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string(i));
  auto req = Req(Tool::Descriptive, R("A1", "A11"));
  req.p.labels = true;
  req.p.conf = 95;
  req.p.kth_largest = 2;
  req.p.kth_smallest = 1;
  REQUIRE(Run(t, req, "C1"));
  CHECK(t.T("C1") == "Price");
  CHECK(t.T("C3") == "Mean");
  CHECK(t.N("D3") == 5.5);
  CHECK(t.N("D4") == Approx(0.957427107756338));
  CHECK(t.N("D5") == 5.5);
  CHECK(t.T("D6") == "#N/A");  // no mode
  CHECK(t.N("D7") == Approx(3.02765035409749));
  CHECK(t.N("D8") == Approx(9.16666666666667));
  CHECK(t.N("D9") == Approx(-1.2));
  CHECK(t.N("D10") == Approx(0).margin(1e-12));
  CHECK(t.N("D11") == 9);
  CHECK(t.N("D14") == 55);
  CHECK(t.N("D15") == 10);
  CHECK(t.T("C16") == "Largest(2)");
  CHECK(t.N("D16") == 9);
  CHECK(t.N("D17") == 1);
  CHECK(t.T("C18") == "Confidence Level(95.0%)");
  CHECK(t.N("D18") == Approx(EvalN(t, "T.INV(0.975,9)*D4")));
  // text in the data is refused
  t.Set("A5", "x");
  std::string err;
  CHECK_FALSE(Run(t, req, "C1", &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("analysis: t-tests, z-test and F-test") {
  yctest::Doc t;
  for (int i = 1; i <= 5; ++i) {
    t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
    t.Set(("B" + std::to_string(i)).c_str(), std::to_string(i * 2 + (i % 2)));
  }
  auto req = Req(Tool::TTestEqual, R("A1", "A5"));
  req.in2 = R("B1", "B5");
  req.has_in2 = true;
  REQUIRE(Run(t, req, "D1"));
  CHECK(t.T("D1") == "t-Test: Two-Sample Assuming Equal Variances");
  CHECK(t.T("E3") == "Variable 1");
  CHECK(t.N("E4") == 3);
  CHECK(t.T("D7") == "Pooled Variance");
  CHECK(t.N("E9") == 8);  // df
  CHECK(t.N("E13") == Approx(EvalN(t, "T.TEST(A1:A5,B1:B5,2,2)")));
  CHECK(t.N("E11") * 2 == Approx(t.N("E13")));
  CHECK(t.N("E14") == Approx(EvalN(t, "T.INV.2T(0.05,8)")));
  // paired
  req.p.tool = Tool::TTestPaired;
  REQUIRE(Run(t, req, "D20"));
  CHECK(t.T("D26") == "Pearson Correlation");
  CHECK(t.N("E26") == Approx(EvalN(t, "CORREL(A1:A5,B1:B5)")));
  CHECK(t.N("E32") == Approx(EvalN(t, "T.TEST(A1:A5,B1:B5,2,1)")));
  // unequal variances: Excel rounds df
  req.p.tool = Tool::TTestUnequal;
  REQUIRE(Run(t, req, "D40"));
  CHECK(t.T("D46") == "Hypothesized Mean Difference");
  CHECK(t.N("E47") == std::round(t.N("E47")));
  // F-test
  req.p.tool = Tool::FTest;
  REQUIRE(Run(t, req, "D60"));
  CHECK(t.T("D67") == "F");
  CHECK(t.N("E67") == Approx(EvalN(t, "VAR.S(A1:A5)/VAR.S(B1:B5)")));
  CHECK(t.N("E68") == Approx(EvalN(t, "F.DIST(E67,4,4,TRUE)")));  // F < 1: the left tail
  // z-test needs the variances
  req.p.tool = Tool::ZTest;
  std::string err;
  CHECK_FALSE(Run(t, req, "D80", &err));
  req.p.var1 = 2.5;
  req.p.var2 = 10;
  REQUIRE(Run(t, req, "D80"));
  CHECK(t.T("D87") == "z");
  CHECK(t.N("E87") == Approx((3.0 - EvalN(t, "AVERAGE(B1:B5)")) / std::sqrt(2.5 / 5 + 10.0 / 5)));
}

TEST_CASE("analysis: ANOVA") {
  yctest::Doc t;
  const char* data[][3] = {{"1", "4", "7"}, {"2", "5", "8"}, {"3", "6", "10"}};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), data[r][c]);
  }
  REQUIRE(Run(t, Req(Tool::Anova1, R("A1", "C3")), "E1"));
  CHECK(t.T("E1") == "Anova: Single Factor");
  CHECK(t.T("E4") == "Groups");
  CHECK(t.T("E5") == "Column 1");
  CHECK(t.N("H7") == Approx(25.0 / 3));  // average of 7, 8, 10
  CHECK(t.T("E10") == "ANOVA");
  CHECK(t.T("E12") == "Between Groups");
  const double f = t.N("I12");
  CHECK(t.N("J12") == Approx(EvalN(t, "F.DIST.RT(I12,2,6)")));
  CHECK(t.N("K12") == Approx(EvalN(t, "F.INV.RT(0.05,2,6)")));
  CHECK(f > 10);
  CHECK(t.T("E15") == "Total");
  CHECK(t.N("F15") == Approx(t.N("F12") + t.N("F13")));
  // two-factor without replication: rows and columns
  REQUIRE(Run(t, Req(Tool::Anova3, R("A1", "C3")), "E20"));
  CHECK(t.T("E23") == "Row 1");
  CHECK(t.T("E27") == "Column 1");
  CHECK(t.T("E34") == "Rows");
  CHECK(t.T("E36") == "Error");
  CHECK(t.N("F34") + t.N("F35") + t.N("F36") == Approx(t.N("F38")));
  // with replication: labels and two rows per sample
  yctest::Doc u;
  const char* g[][3] = {{"", "Hot", "Cold"}, {"A", "10", "6"}, {"", "12", "7"}, {"B", "14", "5"}, {"", "15", "4"}};
  for (int r = 0; r < 5; ++r) {
    for (int c = 0; c < 3; ++c) u.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), g[r][c]);
  }
  auto req = Req(Tool::Anova2, R("A1", "C5"));
  req.p.rows_per_sample = 2;
  REQUIRE(Run(u, req, "E1"));
  CHECK(u.T("E1") == "Anova: Two-Factor With Replication");
  CHECK(u.T("F3") == "Hot");
  CHECK(u.T("H3") == "Total");
  CHECK(u.T("E4") == "A");
  CHECK(u.N("F5") == 2);    // Count
  CHECK(u.N("F7") == 11);   // Average of 10, 12
  CHECK(u.N("H5") == 4);
  CHECK(u.T("E16") == "Total");
  bool found = false;
  for (int r = 20; r < 40; ++r) {
    if (u.T(("E" + std::to_string(r)).c_str()) == "Interaction") {
      found = true;
      // within: (10-11)^2+(12-11)^2+(6-6.5)^2*2+(14-14.5)^2*2+(5-4.5)^2*2 = 2 + 0.5*3 = 3.5
      CHECK(u.N(("F" + std::to_string(r + 1)).c_str()) == Approx(3.5));
      CHECK(u.N(("G" + std::to_string(r + 1)).c_str()) == 4);
    }
  }
  CHECK(found);
}

TEST_CASE("analysis: regression matches LINEST") {
  yctest::Doc t;
  const double x1[] = {1, 2, 3, 4, 5, 6, 7, 8};
  const double x2[] = {3, 1, 4, 1, 5, 9, 2, 6};
  for (int i = 0; i < 8; ++i) {
    const std::string r = std::to_string(i + 2);
    t.Set(("B" + r).c_str(), std::to_string(x1[i]));
    t.Set(("C" + r).c_str(), std::to_string(x2[i]));
    t.Set(("A" + r).c_str(), std::to_string(2 + 1.5 * x1[i] - 0.7 * x2[i] + ((i * 7) % 5 - 2) * 0.3));
  }
  t.Set("A1", "Y");
  t.Set("B1", "Size");
  t.Set("C1", "Age");
  auto req = Req(Tool::Regression, R("A1", "A9"));
  req.in2 = R("B1", "C9");
  req.has_in2 = true;
  req.p.labels = true;
  req.p.residuals = true;
  req.p.std_residuals = true;
  req.p.normal_probability = true;
  req.p.confidence = 99;
  REQUIRE(Run(t, req, "E1"));
  CHECK(t.T("E1") == "SUMMARY OUTPUT");
  CHECK(t.N("F5") == Approx(EvalN(t, "INDEX(LINEST(A2:A9,B2:C9,TRUE,TRUE),3,1)")));  // R Square
  CHECK(t.N("F8") == 8);
  CHECK(t.N("I12") == Approx(EvalN(t, "INDEX(LINEST(A2:A9,B2:C9,TRUE,TRUE),4,1)")));  // F
  CHECK(t.T("E17") == "Intercept");
  CHECK(t.N("F17") == Approx(EvalN(t, "INDEX(LINEST(A2:A9,B2:C9),1,3)")));
  CHECK(t.T("E18") == "Size");
  CHECK(t.N("F18") == Approx(EvalN(t, "INDEX(LINEST(A2:A9,B2:C9),1,2)")));
  CHECK(t.N("G19") == Approx(EvalN(t, "INDEX(LINEST(A2:A9,B2:C9,TRUE,TRUE),2,1)")));  // se of Age
  CHECK(t.N("I18") == Approx(EvalN(t, "T.DIST.2T(ABS(H18),5)")));
  CHECK(t.T("L16") == "Lower 99.0%");
  CHECK(t.N("L18") == Approx(t.N("F18") - EvalN(t, "T.INV(0.995,5)") * t.N("G18")));
  CHECK(t.T("E23") == "RESIDUAL OUTPUT");
  CHECK(t.T("F25") == "Predicted Y");
  CHECK(t.N("G26") == Approx(t.N("A2") - t.N("F26")));
  CHECK(t.T("J23") == "PROBABILITY OUTPUT");
  CHECK(t.N("J26") == Approx(6.25));
}

TEST_CASE("analysis: moving average and exponential smoothing write formulas") {
  yctest::Doc t;
  const int v[] = {3, 5, 9, 20, 12, 17, 22, 23};
  for (int i = 0; i < 8; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string(v[i]));
  auto req = Req(Tool::MovingAverage, R("A1", "A8"));
  req.p.interval = 3;
  req.p.std_errors = true;
  REQUIRE(Run(t, req, "B1"));
  CHECK(t.T("B2") == "#N/A");
  CHECK(t.F("B3") == "=AVERAGE(A1:A3)");
  CHECK(t.N("B3") == Approx(17.0 / 3));
  CHECK(t.T("C4") == "#N/A");
  CHECK(t.F("C5") == "=SQRT(SUMXMY2(A3:A5,B3:B5)/3)");
  auto es = Req(Tool::ExpSmoothing, R("A1", "A8"));
  es.p.damping = 0.3;
  REQUIRE(Run(t, es, "E1"));
  CHECK(t.T("E1") == "#N/A");
  CHECK(t.F("E2") == "=A1");
  CHECK(t.F("E3") == "=0.7*A2+0.3*E2");
  CHECK(t.N("E3") == Approx(0.7 * 5 + 0.3 * 3));
}

TEST_CASE("analysis: correlation, covariance, histogram, rank, Fourier, sampling, random numbers") {
  yctest::Doc t;
  const int a[] = {1, 3, 2, 5, 4, 3};
  const int b[] = {2, 6, 5, 9, 8, 5};
  for (int i = 0; i < 6; ++i) {
    t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string(a[i]));
    t.Set(("B" + std::to_string(i + 1)).c_str(), std::to_string(b[i]));
  }
  REQUIRE(Run(t, Req(Tool::Correlation, R("A1", "B6")), "D1"));
  CHECK(t.T("E1") == "Column 1");
  CHECK(t.N("E2") == 1);
  CHECK(t.N("E3") == Approx(EvalN(t, "CORREL(A1:A6,B1:B6)")));
  CHECK(t.V("F2").IsEmpty());  // above the diagonal
  REQUIRE(Run(t, Req(Tool::Covariance, R("A1", "B6")), "D10"));
  CHECK(t.N("E12") == Approx(EvalN(t, "COVARIANCE.P(A1:A6,B1:B6)")));
  // histogram with bins and cumulative %
  t.Set("H1", "2");
  t.Set("H2", "4");
  auto h = Req(Tool::Histogram, R("A1", "A6"));
  h.bins = R("H1", "H2");
  h.has_bins = true;
  h.p.cumulative = true;
  h.p.pareto = true;
  REQUIRE(Run(t, h, "D20"));
  CHECK(t.T("D20") == "Bin");
  CHECK(t.N("E21") == 2);  // 1, 2
  CHECK(t.N("E22") == 3);  // 3, 3, 4
  CHECK(t.T("D23") == "More");
  CHECK(t.N("E23") == 1);
  CHECK(t.T("F23") == "100.00%");
  CHECK(t.N("G21") == 4);  // pareto: the biggest bin first
  // rank and percentile
  REQUIRE(Run(t, Req(Tool::RankPercentile, R("A1", "A6")), "D30"));
  CHECK(t.T("D30") == "Point");
  CHECK(t.N("D31") == 4);  // the 5 is the 4th value
  CHECK(t.N("F31") == 1);
  CHECK(t.T("G31") == "100.00%");
  CHECK(t.N("F33") == 3);  // two 3s share rank 3
  CHECK(t.N("F34") == 3);
  // Fourier
  t.Set("J1", "1");
  t.Set("J2", "2");
  t.Set("J3", "3");
  t.Set("J4", "4");
  REQUIRE(Run(t, Req(Tool::Fourier, R("J1", "J4")), "K1"));
  CHECK(t.T("K1") == "10");
  CHECK(t.T("K2") == "-2+2i");
  CHECK(t.T("K3") == "-2");
  CHECK(t.T("K4") == "-2-2i");
  auto inv = Req(Tool::Fourier, R("K1", "K4"));
  inv.p.inverse = true;
  REQUIRE(Run(t, inv, "L1"));
  CHECK(t.T("L2") == "2");
  CHECK(t.T("L4") == "4");
  std::string err;
  CHECK_FALSE(Run(t, Req(Tool::Fourier, R("J1", "J3")), "M1", &err));  // not a power of 2
  // sampling: every 2nd value
  auto s = Req(Tool::Sampling, R("A1", "A6"));
  s.p.period = 2;
  REQUIRE(Run(t, s, "N1"));
  CHECK(t.N("N1") == 3);
  CHECK(t.N("N3") == 3);
  CHECK(t.V("N4").IsEmpty());
  // random numbers with a seed are repeatable
  Document::AnalysisRequest rn;
  rn.p.tool = Tool::Random;
  rn.p.variables = 2;
  rn.p.points = 5;
  rn.p.distribution = 2;
  rn.p.p1 = 100;
  rn.p.p2 = 15;
  rn.p.seed = 42;
  REQUIRE(Run(t, rn, "P1"));
  const double first = t.N("P1");
  REQUIRE(Run(t, rn, "S1"));
  CHECK(t.N("S1") == first);
  CHECK(t.V("Q5").IsNumeric());
  rn.p.distribution = 6;
  rn.p.p1 = 1;
  rn.p.p2 = 3;
  rn.p.p3 = 1;
  rn.p.p4 = 2;
  rn.p.p5 = 1;
  REQUIRE(Run(t, rn, "U1"));
  CHECK(t.N("U1") == 1);
  CHECK(t.N("U2") == 1);
  CHECK(t.N("U3") == 2);
  CHECK(t.N("U6") == 3);
}

TEST_CASE("analysis: tools by Excel's macro names; output on a new sheet is one undo step") {
  Tool tool;
  REQUIRE(yc::engine::analysis::ToolFromMacro("ATPVBAEN.XLAM!Descr", &tool));
  CHECK(tool == Tool::Descriptive);
  REQUIRE(yc::engine::analysis::ToolFromMacro("regress", &tool));
  CHECK(tool == Tool::Regression);
  CHECK_FALSE(yc::engine::analysis::ToolFromMacro("Nope", &tool));
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "4");
  auto req = Req(Tool::Descriptive, R("A1", "A3"));
  req.new_sheet = true;
  req.sheet_name = "Stats";
  req.in_sheet = t.sheet();
  uint32_t sh = 0;
  Rect rect{};
  std::string err;
  const size_t sheets = t.d.wb().sheets.size();
  REQUIRE(t.d.RunAnalysis(req, &sh, &rect, &err));
  CHECK(t.d.wb().sheets.size() == sheets + 1);
  CHECK(t.d.wb().FindSheet("Stats") == 0);  // before the input's sheet
  CHECK(t.d.GetValue(sh, 2, 1).AsDouble() == Approx(7.0 / 3));
  REQUIRE(t.d.Undo());
  CHECK(t.d.wb().sheets.size() == sheets);
}
