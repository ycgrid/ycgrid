// SPDX-License-Identifier: Apache-2.0
// analysis.h - Excel's Analysis ToolPak (Data > Data Analysis): the tools' calculations and output layouts
//
// Each tool reads blocks of sheet values and returns a block of output cells laid out like Excel's (values, and
// formulas for Moving Average and Exponential Smoothing, which Excel writes as formulas too). Document::RunAnalysis
// reads the inputs from the sheet and writes the output (document_data.cpp).
#pragma once

#include "core/value.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace yc::engine::analysis {

enum class Tool : int {
  Anova1,        // ANOVA: Single Factor
  Anova2,        // ANOVA: Two-Factor With Replication
  Anova3,        // ANOVA: Two-Factor Without Replication
  Correlation,
  Covariance,
  Descriptive,
  ExpSmoothing,
  FTest,         // F-Test Two-Sample for Variances
  Fourier,
  Histogram,
  MovingAverage,
  Random,        // Random Number Generation
  RankPercentile,
  Regression,
  Sampling,
  TTestPaired,
  TTestEqual,    // Two-Sample Assuming Equal Variances
  TTestUnequal,  // Two-Sample Assuming Unequal Variances
  ZTest,         // z-Test: Two Sample for Means
  Count
};

const char* ToolName(Tool t);                     // English name, as in Excel's Data Analysis list
const char* ToolMacro(Tool t);                    // Excel's ATPVBAEN.XLAM procedure name (Descr, Regress...)
bool ToolFromMacro(std::string_view name, Tool* out);  // case-insensitive; "ATPVBAEN.XLAM!" prefix allowed

// a rectangular block of sheet values; ref(r0, c0, r1, c1) gives the address text of a part of it for formulas
struct Block {
  uint32_t rows = 0, cols = 0;
  std::vector<core::Value> v;  // row-major
  std::function<std::string(uint32_t, uint32_t, uint32_t, uint32_t)> ref;
  const core::Value& At(uint32_t r, uint32_t c) const { return v[static_cast<size_t>(r) * cols + c]; }
  bool empty() const { return v.empty(); }
};

struct Params {
  Tool tool = Tool::Descriptive;
  Block in, in2, bins;  // in2: variable 2 (two-sample tests), X range (regression); bins: histogram bins, discrete values
  bool by_rows = false, labels = false;
  double alpha = 0.05;
  // Descriptive Statistics
  bool summary = true;
  double conf = 0;  // confidence level for the mean in percent (0 = not shown)
  int kth_largest = 0, kth_smallest = 0;
  // ANOVA with replication
  int rows_per_sample = 1;
  // Exponential Smoothing, Moving Average
  double damping = 0.3;
  int interval = 3;
  bool std_errors = false;
  // two-sample tests
  double hyp_diff = 0, var1 = 0, var2 = 0;
  // Fourier
  bool inverse = false;
  // Histogram
  bool pareto = false, cumulative = false;
  // Random Number Generation: distribution 1 uniform (p1..p2), 2 normal (mean p1, sd p2), 3 Bernoulli (p1), 4 binomial
  // (p1, trials p2), 5 Poisson (lambda p1), 6 patterned (p1 to p2 step p3, each p4 times, sequence p5 times),
  // 7 discrete (bins: values and probabilities)
  int variables = 1, points = 0, distribution = 1;
  double p1 = 0, p2 = 1, p3 = 1, p4 = 1, p5 = 1;
  uint64_t seed = 0;  // 0 = random
  // Regression
  bool zero_constant = false, residuals = false, std_residuals = false, normal_probability = false;
  double confidence = 0;  // a second confidence level in percent (0 = 95 only)
  // Sampling: periodic (every `period`-th value) or random (`count` values)
  bool periodic = true;
  int period = 1, count = 1;
  // the output's own cells, for formulas (Moving Average / Exponential Smoothing)
  std::function<std::string(uint32_t, uint32_t, uint32_t, uint32_t)> out_ref;
};

struct Cell {
  core::Value v;
  std::string formula;  // "=..." (written as a formula), else v
  std::string fmt;      // number format ("" = General)
};

struct Result {
  std::vector<std::vector<Cell>> rows;  // ragged: missing cells are empty
  uint32_t Cols() const {
    size_t c = 0;
    for (const auto& r : rows) c = std::max(c, r.size());
    return static_cast<uint32_t>(c);
  }
};

// runs a tool; false with a message (Excel's wording) when the input doesn't fit
bool Run(const Params& p, Result* out, std::string* error);

}  // namespace yc::engine::analysis
