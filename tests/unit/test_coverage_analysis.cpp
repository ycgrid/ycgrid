// SPDX-License-Identifier: Apache-2.0
// Coverage of the Analysis ToolPak (engine/analysis.cpp) beyond test_analysis.cpp: every input each tool refuses with
// its message, the random number distributions (seeded), random sampling, the F-test's other tail, Regression through
// the origin, Exponential Smoothing's standard errors and the tool names.
#include <catch2/catch_test_macros.hpp>

#include "core/i18n.h"
#include "engine/analysis.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace an = yc::engine::analysis;
using yc::core::Value;
using an::Tool;

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

Value N(double x) { return Value::Number(x); }
Value S(const char* s) { return Value::String(s); }
const Value E;

std::string Ref(uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
  return "R" + std::to_string(r0 + 1) + "C" + std::to_string(c0 + 1) + ":R" + std::to_string(r1 + 1) + "C" + std::to_string(c1 + 1);
}

an::Block B(std::vector<std::vector<Value>> rows) {
  an::Block b;
  b.rows = static_cast<uint32_t>(rows.size());
  b.cols = rows.empty() ? 0 : static_cast<uint32_t>(rows[0].size());
  for (auto& r : rows) {
    r.resize(b.cols);
    for (auto& v : r) b.v.push_back(v);
  }
  b.ref = Ref;
  return b;
}

an::Block Column(std::initializer_list<double> xs) {
  std::vector<std::vector<Value>> rows;
  for (double x : xs) rows.push_back({N(x)});
  return B(rows);
}

an::Params P(Tool tool, an::Block in = {}) {
  an::Params p;
  p.tool = tool;
  p.in = std::move(in);
  p.out_ref = Ref;
  return p;
}

std::string Refused(const an::Params& p) {
  an::Result r;
  std::string err;
  if (an::Run(p, &r, &err)) return "(ran)";
  return err;
}

// the text in the first column of each output row
std::vector<std::string> Labels(const an::Result& r) {
  std::vector<std::string> out;
  for (const auto& row : r.rows) out.push_back(!row.empty() && row[0].v.IsString() ? std::string(row[0].v.str()) : std::string());
  return out;
}

}  // namespace

TEST_CASE("Analysis coverage: what every tool refuses, in Excel's words") {
  English en;
  auto p = P(Tool::Descriptive, Column({1, 2}));
  p.alpha = 1.5;
  CHECK(Refused(p) == "Alpha must be between 0 and 1.");
  CHECK(Refused(P(Tool::Descriptive)) == "Enter the input range.");
  CHECK(Refused(P(Tool::Count, Column({1}))) == "Unknown analysis tool.");
  CHECK(Refused(P(Tool::Descriptive, B({{N(1), E}, {N(2), E}}))) == "The input range is empty: Column2");
  CHECK(Refused(P(Tool::Anova1, Column({1, 2, 3}))) == "ANOVA needs at least two groups.");
  CHECK(Refused(P(Tool::Anova2, B({{S("x")}}))) == "The input range is too small.");
  CHECK(Refused(P(Tool::Anova2, B({{E, S("a"), S("b")}, {S("r1"), N(1), S("x")}, {S("r1"), N(1), N(2)}}))) ==
        "The input range contains non-numeric data.");
  auto a2 = P(Tool::Anova2, B({{E, S("a"), S("b")}, {S("r1"), N(1), N(2)}, {S("r1"), N(3), N(4)}, {S("r2"), N(5), N(6)}}));
  a2.rows_per_sample = 2;
  CHECK(Refused(a2) == "Rows per sample must divide the data rows into at least two samples of two or more rows.");
  CHECK(Refused(P(Tool::Anova3, B({{N(1), N(2)}}))) == "ANOVA needs at least two rows and two columns.");
  CHECK(Refused(P(Tool::Correlation, B({{N(1), N(1)}, {N(2), E}, {N(3), N(3)}}))) == "The variables must have the same number of values (at least two).");
  CHECK(Refused(P(Tool::ExpSmoothing, B({{N(1), N(2)}, {N(3), N(4)}}))) == "The input range must be a single row or column.");
  CHECK(Refused(P(Tool::ExpSmoothing, B({{N(1)}, {E}, {N(3)}}))) == "The input range contains blank cells.");
  auto es = P(Tool::ExpSmoothing, Column({1, 2, 3}));
  es.damping = 1.5;
  CHECK(Refused(es) == "The damping factor must be between 0 and 1.");
  auto ma = P(Tool::MovingAverage, Column({1, 2, 3}));
  ma.interval = 1;
  CHECK(Refused(ma) == "The interval must be at least 2 and no more than the number of values.");
  ma.interval = 4;
  CHECK(Refused(ma) == "The interval must be at least 2 and no more than the number of values.");
  CHECK(Refused(P(Tool::FTest, Column({1, 2}))) == "Enter both variable ranges.");
  auto ft = P(Tool::FTest, Column({1, 2}));
  ft.in2 = B({{N(1), N(2)}, {N(3), N(4)}});
  CHECK(Refused(ft) == "Each variable range must be a single row or column.");
  ft.in2 = Column({1});
  CHECK(Refused(ft) == "Each variable needs at least two values.");
  auto tp = P(Tool::TTestPaired, Column({1, 2, 3}));
  tp.in2 = Column({1, 2});
  CHECK(Refused(tp) == "Paired samples must have the same number of values.");
  CHECK(Refused(P(Tool::Histogram, B({{N(1)}, {S("x")}}))) == "The input range contains non-numeric data.");
  CHECK(Refused(P(Tool::Histogram, B({{E}, {E}}))) == "The input range is empty.");
  auto h = P(Tool::Histogram, Column({1, 2}));
  h.bins = B({{N(1)}, {S("x")}});
  CHECK(Refused(h) == "The bin range contains non-numeric data.");
  CHECK(Refused(P(Tool::Sampling, B({{N(1)}, {S("x")}}))) == "The input range contains non-numeric data.");
  CHECK(Refused(P(Tool::Sampling, B({{E}}))) == "The input range is empty.");
  auto sp = P(Tool::Sampling, Column({1, 2, 3}));
  sp.period = 4;
  CHECK(Refused(sp) == "The period must be between 1 and the number of values.");
  sp.periodic = false;
  sp.count = 0;
  CHECK(Refused(sp) == "Enter the number of samples.");
  auto rn = P(Tool::Random);
  rn.points = 0;
  CHECK(Refused(rn) == "Enter the number of random numbers.");
  rn.points = 3;
  rn.distribution = 6;
  rn.p3 = 0;
  CHECK(Refused(rn) == "Enter a range (from <= to) and a positive step.");
  rn.distribution = 7;
  rn.bins = Column({1, 2});
  CHECK(Refused(rn) == "The value and probability input range must have two columns.");
  rn.bins = B({{N(1), S("x")}});
  CHECK(Refused(rn) == "The value and probability input range contains non-numeric data.");
  rn.bins = B({{N(1), N(0.5)}, {N(2), N(0.4)}});
  CHECK(Refused(rn) == "The probabilities must add up to 1.");
  rn.distribution = 99;
  CHECK(Refused(rn) == "Unknown distribution.");
  auto rg = P(Tool::Regression, Column({1, 2, 3}));
  CHECK(Refused(rg) == "Enter the Y and X ranges.");
  rg.in = B({{N(1), N(2)}, {N(3), N(4)}});
  rg.in2 = Column({1, 2});
  CHECK(Refused(rg) == "The Y range must be a single column.");
  rg.in = Column({1, 2, 3});
  CHECK(Refused(rg) == "The Y and X ranges must have the same number of rows.");
  rg.in2 = B({{N(1)}, {S("x")}, {N(3)}});
  CHECK(Refused(rg) == "The Y and X ranges must hold numbers only; a blank or text is in this row of the ranges: 2");
  rg.in = Column({1, 2});
  rg.in2 = B({{N(1), N(2)}, {N(2), N(3)}});
  CHECK(Refused(rg) == "There are too few observations for the number of X variables.");
  rg.in = Column({1, 2, 3, 4});
  rg.in2 = B({{N(1), N(2)}, {N(2), N(4)}, {N(3), N(6)}, {N(4), N(8)}});
  CHECK(Refused(rg) == "The X variables are collinear: the regression can't be computed.");
}

TEST_CASE("Analysis coverage: random numbers by distribution, seeded; random and periodic sampling") {
  English en;
  auto rn = P(Tool::Random);
  rn.points = 50;
  rn.variables = 2;
  rn.seed = 7;
  auto values = [&](int dist, double p1, double p2) {
    rn.distribution = dist;
    rn.p1 = p1;
    rn.p2 = p2;
    an::Result r;
    std::string err;
    REQUIRE(an::Run(rn, &r, &err));
    REQUIRE(r.rows.size() == 50);
    std::vector<double> out;
    for (const auto& row : r.rows) {
      REQUIRE(row.size() == 2);
      for (const auto& c : row) out.push_back(c.v.AsDouble());
    }
    return out;
  };
  for (double x : values(1, 5, 6)) CHECK((x >= 5 && x <= 6));  // uniform
  for (double x : values(3, 0.5, 0)) CHECK((x == 0 || x == 1));  // Bernoulli
  for (double x : values(4, 0.5, 10)) CHECK((x >= 0 && x <= 10 && x == std::floor(x)));  // binomial
  for (double x : values(5, 3, 0)) CHECK((x >= 0 && x == std::floor(x)));  // Poisson
  rn.bins = B({{N(10), N(0.25)}, {N(20), N(0.75)}});
  std::set<double> seen;
  for (double x : values(7, 0, 0)) seen.insert(x);  // discrete
  CHECK(seen == std::set<double>{10, 20});
  CHECK(values(1, 0, 1) == values(1, 0, 1));  // one seed, the same numbers
  // random sampling picks values of the input
  auto sp = P(Tool::Sampling, Column({3, 5, 7}));
  sp.periodic = false;
  sp.count = 20;
  sp.seed = 11;
  an::Result r;
  std::string err;
  REQUIRE(an::Run(sp, &r, &err));
  REQUIRE(r.rows.size() == 20);
  for (const auto& row : r.rows) CHECK((row[0].v.AsDouble() == 3 || row[0].v.AsDouble() == 5 || row[0].v.AsDouble() == 7));
}

TEST_CASE("Analysis coverage: the F-test's upper tail, Regression through the origin, smoothing errors, a mode") {
  English en;
  auto ft = P(Tool::FTest, Column({1, 9, 2, 8}));  // the first variable varies more: F > 1
  ft.in2 = Column({4, 5, 4, 5});
  an::Result r;
  std::string err;
  REQUIRE(an::Run(ft, &r, &err));
  const auto labels = Labels(r);
  CHECK(std::find(labels.begin(), labels.end(), "P(F<=f) one-tail") != labels.end());
  CHECK(std::find(labels.begin(), labels.end(), "F Critical one-tail") != labels.end());
  // y = 2x through the origin: the intercept row says 0 and #N/A
  auto rg = P(Tool::Regression, Column({2, 4, 6, 8.5}));
  rg.in2 = Column({1, 2, 3, 4});
  rg.zero_constant = true;
  REQUIRE(an::Run(rg, &r, &err));
  bool intercept = false;
  for (const auto& row : r.rows) {
    if (!row.empty() && row[0].v.IsString() && row[0].v.str() == "Intercept") {
      intercept = true;
      CHECK(row[1].v.AsDouble() == 0);
      CHECK(row[2].v.IsError());
    }
  }
  CHECK(intercept);
  // exponential smoothing with standard errors: formulas from the fourth value on
  auto es = P(Tool::ExpSmoothing, Column({1, 2, 3, 4, 5, 6}));
  es.std_errors = true;
  REQUIRE(an::Run(es, &r, &err));
  bool sqrt_formula = false;
  for (const auto& row : r.rows) {
    if (row.size() > 1 && row[1].formula.find("SQRT(SUMXMY2(") != std::string::npos) sqrt_formula = true;
  }
  CHECK(sqrt_formula);
  REQUIRE(r.rows.size() >= 4);
  CHECK(r.rows[1][1].v.IsError());  // too early for a standard error
  // descriptive statistics: the mode of a column with a repeated value
  REQUIRE(an::Run(P(Tool::Descriptive, Column({4, 7, 7, 1})), &r, &err));
  bool mode = false;
  for (const auto& row : r.rows) {
    if (!row.empty() && row[0].v.IsString() && row[0].v.str() == "Mode") mode = row.size() > 1 && row[1].v.AsDouble() == 7;
  }
  CHECK(mode);
}

TEST_CASE("Analysis coverage: every tool's name and macro, found again by its macro") {
  for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
    const Tool t = static_cast<Tool>(i);
    INFO(i);
    CHECK(std::string(an::ToolName(t)).size() > 3);
    const std::string macro = an::ToolMacro(t);
    REQUIRE_FALSE(macro.empty());
    Tool back = Tool::Count;
    CHECK(an::ToolFromMacro("ATPVBAEN.XLAM!" + macro, &back));
    CHECK(back == t);
  }
  CHECK(std::string(an::ToolName(Tool::Count)).empty());
  CHECK(std::string(an::ToolMacro(Tool::Count)).empty());
  Tool none = Tool::Descriptive;
  CHECK_FALSE(an::ToolFromMacro("NoSuchMacro", &none));
}
