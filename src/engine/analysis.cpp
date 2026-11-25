// SPDX-License-Identifier: Apache-2.0
// analysis.cpp - Excel's Analysis ToolPak: calculations and output layouts (see analysis.h)
#include "engine/analysis.h"

#include "core/i18n.h"
#include "formula/stats_api.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <map>
#include <numeric>
#include <random>

namespace yc::engine::analysis {

using core::Err;
using core::Value;
namespace st = formula::stats;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct ToolInfo {
  Tool tool;
  const char* name;
  const char* macro;
};
constexpr ToolInfo kTools[] = {
    {Tool::Anova1, "Anova: Single Factor", "Anova1"},
    {Tool::Anova2, "Anova: Two-Factor With Replication", "Anova2"},
    {Tool::Anova3, "Anova: Two-Factor Without Replication", "Anova3"},
    {Tool::Correlation, "Correlation", "Mcorrel"},
    {Tool::Covariance, "Covariance", "Mcovar"},
    {Tool::Descriptive, "Descriptive Statistics", "Descr"},
    {Tool::ExpSmoothing, "Exponential Smoothing", "Expon"},
    {Tool::FTest, "F-Test Two-Sample for Variances", "Ftestv"},
    {Tool::Fourier, "Fourier Analysis", "Fourier"},
    {Tool::Histogram, "Histogram", "Histogram"},
    {Tool::MovingAverage, "Moving Average", "Moveavg"},
    {Tool::Random, "Random Number Generation", "Random"},
    {Tool::RankPercentile, "Rank and Percentile", "RankPerc"},
    {Tool::Regression, "Regression", "Regress"},
    {Tool::Sampling, "Sampling", "Sample"},
    {Tool::TTestPaired, "t-Test: Paired Two Sample for Means", "Ttestm"},
    {Tool::TTestEqual, "t-Test: Two-Sample Assuming Equal Variances", "Ttest1"},
    {Tool::TTestUnequal, "t-Test: Two-Sample Assuming Unequal Variances", "Ttest2"},
    {Tool::ZTest, "z-Test: Two Sample for Means", "Ztestm"},
};

std::string Lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

// ---- output ----

struct Out {
  Result* r;
  Cell& at(uint32_t row, uint32_t col) {
    if (r->rows.size() <= row) r->rows.resize(row + 1);
    auto& line = r->rows[row];
    if (line.size() <= col) line.resize(col + 1);
    return line[col];
  }
  void text(uint32_t row, uint32_t col, std::string_view s) { at(row, col).v = Value::String(s); }
  void num(uint32_t row, uint32_t col, double x, const char* fmt = nullptr) {
    Cell& c = at(row, col);
    c.v = std::isfinite(x) ? Value::Number(x) : Value::Error(std::isnan(x) ? Err::Div0 : Err::Num);
    if (fmt) c.fmt = fmt;
  }
  void err(uint32_t row, uint32_t col, Err e) { at(row, col).v = Value::Error(e); }
  void formula(uint32_t row, uint32_t col, std::string f) { at(row, col).formula = std::move(f); }
};

std::string Num(double x) {
  char b[48];
  std::snprintf(b, sizeof(b), "%.15g", x);
  return b;
}

// ---- inputs ----

struct Var {
  std::string label;
  std::vector<double> x;
  std::vector<uint32_t> idx;  // position of each value along the variable (for formulas)
};

std::string DefaultLabel(const char* stem, size_t i) { return std::string(stem) + std::to_string(i + 1); }

std::string LabelText(const Value& v) {
  if (v.IsString()) return std::string(v.str());
  if (v.IsNumeric()) return Num(v.AsDouble());
  if (v.IsBool()) return v.boolean() ? "TRUE" : "FALSE";
  return {};
}

// the variables of a block (columns, or rows when by_rows); blank cells are skipped, text is an error
bool Vars(const Block& b, bool by_rows, bool labels, const char* stem, std::vector<Var>* out, std::string* error) {
  const uint32_t nv = by_rows ? b.rows : b.cols, len = by_rows ? b.cols : b.rows;
  out->assign(nv, Var{});
  for (uint32_t i = 0; i < nv; ++i) {
    Var& v = (*out)[i];
    v.label = DefaultLabel(stem, i);
    for (uint32_t j = 0; j < len; ++j) {
      const Value& x = by_rows ? b.At(i, j) : b.At(j, i);
      if (labels && j == 0) {
        const std::string t = LabelText(x);
        if (!t.empty()) v.label = t;
        continue;
      }
      if (x.IsEmpty()) continue;
      if (!x.IsNumeric()) {
        *error = TR("The input range contains non-numeric data.");
        return false;
      }
      v.x.push_back(x.AsDouble());
      v.idx.push_back(j);
    }
  }
  return true;
}

double Mean(const std::vector<double>& x) { return x.empty() ? kNaN : std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size()); }
double SumSq(const std::vector<double>& x, double m) {
  double s = 0;
  for (double v : x) s += (v - m) * (v - m);
  return s;
}
double VarS(const std::vector<double>& x) { return x.size() < 2 ? kNaN : SumSq(x, Mean(x)) / static_cast<double>(x.size() - 1); }
double Sum(const std::vector<double>& x) { return std::accumulate(x.begin(), x.end(), 0.0); }

// ---- the tools ----

bool Descriptive(const Params& p, Out& o, std::string* error) {
  std::vector<Var> vars;
  if (!Vars(p.in, p.by_rows, p.labels, p.by_rows ? "Row" : "Column", &vars, error)) return false;
  for (size_t i = 0; i < vars.size(); ++i) {
    const auto& x = vars[i].x;
    const uint32_t c = static_cast<uint32_t>(i * 2);
    uint32_t r = 0;
    o.text(r++, c, vars[i].label);
    ++r;
    const double n = static_cast<double>(x.size());
    if (x.empty()) {
      *error = TR("The input range is empty: ") + vars[i].label;
      return false;
    }
    std::vector<double> s = x;
    std::sort(s.begin(), s.end());
    const double m = Mean(x), var = VarS(x), sd = std::sqrt(var);
    if (p.summary) {
      o.text(r, c, "Mean");
      o.num(r++, c + 1, m);
      o.text(r, c, "Standard Error");
      o.num(r++, c + 1, sd / std::sqrt(n));
      o.text(r, c, "Median");
      const size_t h = s.size() / 2;
      o.num(r++, c + 1, s.size() % 2 ? s[h] : (s[h - 1] + s[h]) / 2);
      // mode: the most frequent value, the first one in the data on ties (MODE.SNGL)
      o.text(r, c, "Mode");
      std::map<double, int> cnt;
      for (double v : x) ++cnt[v];
      int best = 1;
      double mode = kNaN;
      for (double v : x) {
        if (cnt[v] > best) {
          best = cnt[v];
          mode = v;
        }
      }
      if (std::isnan(mode)) o.err(r++, c + 1, Err::NA);
      else o.num(r++, c + 1, mode);
      o.text(r, c, "Standard Deviation");
      o.num(r++, c + 1, sd);
      o.text(r, c, "Sample Variance");
      o.num(r++, c + 1, var);
      double m4 = 0, m3 = 0;
      for (double v : x) {
        const double z = (v - m) / sd;
        m3 += z * z * z;
        m4 += z * z * z * z;
      }
      o.text(r, c, "Kurtosis");
      if (n < 4 || sd == 0) o.err(r++, c + 1, Err::Div0);
      else o.num(r++, c + 1, n * (n + 1) / ((n - 1) * (n - 2) * (n - 3)) * m4 - 3 * (n - 1) * (n - 1) / ((n - 2) * (n - 3)));
      o.text(r, c, "Skewness");
      if (n < 3 || sd == 0) o.err(r++, c + 1, Err::Div0);
      else o.num(r++, c + 1, n / ((n - 1) * (n - 2)) * m3);
      o.text(r, c, "Range");
      o.num(r++, c + 1, s.back() - s.front());
      o.text(r, c, "Minimum");
      o.num(r++, c + 1, s.front());
      o.text(r, c, "Maximum");
      o.num(r++, c + 1, s.back());
      o.text(r, c, "Sum");
      o.num(r++, c + 1, Sum(x));
      o.text(r, c, "Count");
      o.num(r++, c + 1, n);
    }
    if (p.kth_largest > 0) {
      o.text(r, c, "Largest(" + std::to_string(p.kth_largest) + ")");
      if (static_cast<size_t>(p.kth_largest) > s.size()) o.err(r++, c + 1, Err::Num);
      else o.num(r++, c + 1, s[s.size() - static_cast<size_t>(p.kth_largest)]);
    }
    if (p.kth_smallest > 0) {
      o.text(r, c, "Smallest(" + std::to_string(p.kth_smallest) + ")");
      if (static_cast<size_t>(p.kth_smallest) > s.size()) o.err(r++, c + 1, Err::Num);
      else o.num(r++, c + 1, s[static_cast<size_t>(p.kth_smallest) - 1]);
    }
    if (p.conf > 0) {
      char lab[64];
      std::snprintf(lab, sizeof(lab), "Confidence Level(%.1f%%)", p.conf);
      o.text(r, c, lab);
      const double a = 1 - p.conf / 100;
      o.num(r++, c + 1, n < 2 ? kNaN : st::TInv(1 - a / 2, n - 1) * sd / std::sqrt(n));
    }
  }
  return true;
}

// the ANOVA table: rows of (name, ss, df) with F against the last (error) row; then the total
void AnovaTable(Out& o, uint32_t r, double alpha, const std::vector<std::tuple<std::string, double, double>>& effects,
                const std::tuple<std::string, double, double>& error_row) {
  o.text(r, 0, "ANOVA");
  ++r;
  const char* head[] = {"Source of Variation", "SS", "df", "MS", "F", "P-value", "F crit"};
  for (uint32_t c = 0; c < 7; ++c) o.text(r, c, head[c]);
  ++r;
  const double ss_e = std::get<1>(error_row), df_e = std::get<2>(error_row), ms_e = ss_e / df_e;
  double ss_t = ss_e, df_t = df_e;
  for (const auto& [name, ss, df] : effects) {
    const double ms = ss / df, f = ms / ms_e;
    o.text(r, 0, name);
    o.num(r, 1, ss);
    o.num(r, 2, df);
    o.num(r, 3, ms);
    o.num(r, 4, f);
    o.num(r, 5, st::FTail(f, df, df_e));
    o.num(r, 6, st::FInv(1 - alpha, df, df_e));
    ++r;
    ss_t += ss;
    df_t += df;
  }
  o.text(r, 0, std::get<0>(error_row));
  o.num(r, 1, ss_e);
  o.num(r, 2, df_e);
  o.num(r, 3, ms_e);
  r += 2;
  o.text(r, 0, "Total");
  o.num(r, 1, ss_t);
  o.num(r, 2, df_t);
}

// a SUMMARY row: count, sum, average, variance
void SummaryRow(Out& o, uint32_t r, uint32_t c, const std::string& label, const std::vector<double>& x) {
  o.text(r, c, label);
  o.num(r, c + 1, static_cast<double>(x.size()));
  o.num(r, c + 2, Sum(x));
  o.num(r, c + 3, Mean(x));
  o.num(r, c + 4, VarS(x));
}

bool Anova1(const Params& p, Out& o, std::string* error) {
  std::vector<Var> g;
  if (!Vars(p.in, p.by_rows, p.labels, p.by_rows ? "Row " : "Column ", &g, error)) return false;
  if (g.size() < 2) {
    *error = TR("ANOVA needs at least two groups.");
    return false;
  }
  o.text(0, 0, "Anova: Single Factor");
  o.text(2, 0, "SUMMARY");
  const char* head[] = {"Groups", "Count", "Sum", "Average", "Variance"};
  for (uint32_t c = 0; c < 5; ++c) o.text(3, c, head[c]);
  uint32_t r = 4;
  std::vector<double> all;
  for (const auto& v : g) {
    SummaryRow(o, r++, 0, v.label, v.x);
    all.insert(all.end(), v.x.begin(), v.x.end());
  }
  const double gm = Mean(all);
  double ssb = 0, ssw = 0;
  for (const auto& v : g) {
    const double m = Mean(v.x);
    ssb += static_cast<double>(v.x.size()) * (m - gm) * (m - gm);
    ssw += SumSq(v.x, m);
  }
  AnovaTable(o, r + 2, p.alpha, {{"Between Groups", ssb, static_cast<double>(g.size() - 1)}},
             {"Within Groups", ssw, static_cast<double>(all.size() - g.size())});
  return true;
}

// a grid of numbers with optional row / column labels (two-factor ANOVA); blank or text inside is an error
bool Grid(const Block& b, bool row_labels, bool col_labels, std::vector<std::vector<double>>* x, std::vector<std::string>* rl,
          std::vector<std::string>* cl, std::string* error) {
  const uint32_t r0 = col_labels ? 1 : 0, c0 = row_labels ? 1 : 0;
  if (b.rows <= r0 || b.cols <= c0) {
    *error = TR("The input range is too small.");
    return false;
  }
  for (uint32_t c = c0; c < b.cols; ++c) {
    const std::string t = col_labels ? LabelText(b.At(0, c)) : std::string();
    cl->push_back(t.empty() ? DefaultLabel("Column ", c - c0) : t);
  }
  for (uint32_t r = r0; r < b.rows; ++r) {
    const std::string t = row_labels ? LabelText(b.At(r, 0)) : std::string();
    rl->push_back(t);
    std::vector<double> row;
    for (uint32_t c = c0; c < b.cols; ++c) {
      const Value& v = b.At(r, c);
      if (!v.IsNumeric()) {
        *error = TR("The input range contains non-numeric data.");
        return false;
      }
      row.push_back(v.AsDouble());
    }
    x->push_back(std::move(row));
  }
  return true;
}

bool Anova3(const Params& p, Out& o, std::string* error) {
  std::vector<std::vector<double>> x;
  std::vector<std::string> rl, cl;
  if (!Grid(p.in, p.labels, p.labels, &x, &rl, &cl, error)) return false;
  const size_t a = x.size(), b = x[0].size();
  if (a < 2 || b < 2) {
    *error = TR("ANOVA needs at least two rows and two columns.");
    return false;
  }
  o.text(0, 0, "Anova: Two-Factor Without Replication");
  const char* head[] = {"SUMMARY", "Count", "Sum", "Average", "Variance"};
  for (uint32_t c = 0; c < 5; ++c) o.text(2, c, head[c]);
  uint32_t r = 3;
  std::vector<double> all;
  for (size_t i = 0; i < a; ++i) {
    SummaryRow(o, r++, 0, rl[i].empty() ? DefaultLabel("Row ", i) : rl[i], x[i]);
    all.insert(all.end(), x[i].begin(), x[i].end());
  }
  ++r;
  std::vector<std::vector<double>> cols(b);
  for (size_t j = 0; j < b; ++j) {
    for (size_t i = 0; i < a; ++i) cols[j].push_back(x[i][j]);
    SummaryRow(o, r++, 0, cl[j], cols[j]);
  }
  const double gm = Mean(all);
  double ssr = 0, ssc = 0;
  for (size_t i = 0; i < a; ++i) ssr += static_cast<double>(b) * (Mean(x[i]) - gm) * (Mean(x[i]) - gm);
  for (size_t j = 0; j < b; ++j) ssc += static_cast<double>(a) * (Mean(cols[j]) - gm) * (Mean(cols[j]) - gm);
  const double sst = SumSq(all, gm);
  AnovaTable(o, r + 2, p.alpha, {{"Rows", ssr, static_cast<double>(a - 1)}, {"Columns", ssc, static_cast<double>(b - 1)}},
             {"Error", sst - ssr - ssc, static_cast<double>((a - 1) * (b - 1))});
  return true;
}

bool Anova2(const Params& p, Out& o, std::string* error) {
  // the input includes the labels: the first row names the columns, the first column names each sample block
  std::vector<std::vector<double>> x;
  std::vector<std::string> rl, cl;
  if (!Grid(p.in, true, true, &x, &rl, &cl, error)) return false;
  const size_t rep = static_cast<size_t>(std::max(1, p.rows_per_sample));
  if (rep < 2 || x.size() % rep != 0 || x.size() / rep < 2) {
    *error = TR("Rows per sample must divide the data rows into at least two samples of two or more rows.");
    return false;
  }
  const size_t a = x.size() / rep, b = x[0].size();
  o.text(0, 0, "Anova: Two-Factor With Replication");
  o.text(2, 0, "SUMMARY");
  for (size_t j = 0; j < b; ++j) o.text(2, static_cast<uint32_t>(j + 1), cl[j]);
  o.text(2, static_cast<uint32_t>(b + 1), "Total");
  uint32_t r = 3;
  std::vector<double> all;
  std::vector<std::vector<double>> cols(b);
  double ss_cells = 0;  // within
  std::vector<double> row_means(a), col_means(b), cell_means(a * b);
  auto block = [&](const std::string& label, const std::vector<std::vector<double>>& cells, const std::vector<double>& total) {
    o.text(r++, 0, label);
    const char* stat[] = {"Count", "Sum", "Average", "Variance"};
    for (int k = 0; k < 4; ++k) {
      o.text(r, 0, stat[k]);
      for (size_t j = 0; j <= cells.size(); ++j) {
        const auto& v = j < cells.size() ? cells[j] : total;
        if (j == cells.size() && total.empty()) continue;
        const double val = k == 0 ? static_cast<double>(v.size()) : k == 1 ? Sum(v) : k == 2 ? Mean(v) : VarS(v);
        o.num(r, static_cast<uint32_t>(j + 1), val);
      }
      ++r;
    }
    ++r;
  };
  for (size_t i = 0; i < a; ++i) {
    std::vector<std::vector<double>> cells(b);
    std::vector<double> rowall;
    for (size_t k = 0; k < rep; ++k) {
      for (size_t j = 0; j < b; ++j) {
        const double v = x[i * rep + k][j];
        cells[j].push_back(v);
        cols[j].push_back(v);
        rowall.push_back(v);
        all.push_back(v);
      }
    }
    for (size_t j = 0; j < b; ++j) {
      cell_means[i * b + j] = Mean(cells[j]);
      ss_cells += SumSq(cells[j], cell_means[i * b + j]);
    }
    row_means[i] = Mean(rowall);
    const std::string label = rl[i * rep].empty() ? DefaultLabel("Sample ", i) : rl[i * rep];
    block(label, cells, rowall);
  }
  block("Total", cols, {});
  const double gm = Mean(all), n_r = static_cast<double>(rep), na = static_cast<double>(a), nb = static_cast<double>(b);
  for (size_t j = 0; j < b; ++j) col_means[j] = Mean(cols[j]);
  double ss_a = 0, ss_b = 0, ss_ab = 0;
  for (size_t i = 0; i < a; ++i) ss_a += nb * n_r * (row_means[i] - gm) * (row_means[i] - gm);
  for (size_t j = 0; j < b; ++j) ss_b += na * n_r * (col_means[j] - gm) * (col_means[j] - gm);
  for (size_t i = 0; i < a; ++i) {
    for (size_t j = 0; j < b; ++j) {
      const double d = cell_means[i * b + j] - row_means[i] - col_means[j] + gm;
      ss_ab += n_r * d * d;
    }
  }
  AnovaTable(o, r + 1, p.alpha, {{"Sample", ss_a, na - 1}, {"Columns", ss_b, nb - 1}, {"Interaction", ss_ab, (na - 1) * (nb - 1)}},
             {"Within", ss_cells, na * nb * (n_r - 1)});
  return true;
}

bool Matrix(const Params& p, Out& o, std::string* error, bool covariance) {
  std::vector<Var> v;
  if (!Vars(p.in, p.by_rows, p.labels, p.by_rows ? "Row " : "Column ", &v, error)) return false;
  for (size_t i = 0; i < v.size(); ++i) {
    o.text(0, static_cast<uint32_t>(i + 1), v[i].label);
    o.text(static_cast<uint32_t>(i + 1), 0, v[i].label);
  }
  o.text(0, 0, "");
  for (size_t i = 0; i < v.size(); ++i) {
    for (size_t j = 0; j <= i; ++j) {
      const auto& x = v[i].x;
      const auto& y = v[j].x;
      if (x.size() != y.size() || x.size() < 2) {
        *error = TR("The variables must have the same number of values (at least two).");
        return false;
      }
      const double mx = Mean(x), my = Mean(y);
      double sxy = 0;
      for (size_t k = 0; k < x.size(); ++k) sxy += (x[k] - mx) * (y[k] - my);
      const double val = covariance ? sxy / static_cast<double>(x.size()) : sxy / std::sqrt(SumSq(x, mx) * SumSq(y, my));
      o.num(static_cast<uint32_t>(i + 1), static_cast<uint32_t>(j + 1), val);
    }
  }
  return true;
}

// one variable for a time series tool (a column, or a row)
bool Series(const Params& p, Var* out, std::string* error) {
  std::vector<Var> v;
  const bool rows = p.in.rows == 1 && p.in.cols > 1;
  if (!Vars(p.in, rows, p.labels, "Column ", &v, error)) return false;
  if (v.size() != 1) {
    *error = TR("The input range must be a single row or column.");
    return false;
  }
  *out = std::move(v[0]);
  if (out->x.size() != (rows ? p.in.cols : p.in.rows) - (p.labels ? 1u : 0u)) {
    *error = TR("The input range contains blank cells.");
    return false;
  }
  return true;
}

std::string InRef(const Params& p, uint32_t i0, uint32_t i1) {
  const bool rows = p.in.rows == 1 && p.in.cols > 1;
  return rows ? p.in.ref(0, i0, 0, i1) : p.in.ref(i0, 0, i1, 0);
}

bool MovingAverage(const Params& p, Out& o, std::string* error) {
  Var v;
  if (!Series(p, &v, error)) return false;
  const size_t k = static_cast<size_t>(p.interval), n = v.x.size();
  if (k < 2 || k > n) {
    *error = TR("The interval must be at least 2 and no more than the number of values.");
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const uint32_t r = static_cast<uint32_t>(i);
    if (i + 1 < k) {
      o.err(r, 0, Err::NA);
    } else {
      o.formula(r, 0, "=AVERAGE(" + InRef(p, v.idx[i + 1 - k], v.idx[i]) + ")");
    }
    if (p.std_errors) {
      if (i + 2 < 2 * k) {
        o.err(r, 1, Err::NA);
      } else {
        o.formula(r, 1, "=SQRT(SUMXMY2(" + InRef(p, v.idx[i + 1 - k], v.idx[i]) + "," + p.out_ref(r + 1 - static_cast<uint32_t>(k), 0, r, 0) + ")/" +
                            std::to_string(k) + ")");
      }
    }
  }
  return true;
}

bool ExpSmoothing(const Params& p, Out& o, std::string* error) {
  Var v;
  if (!Series(p, &v, error)) return false;
  if (p.damping < 0 || p.damping > 1) {
    *error = TR("The damping factor must be between 0 and 1.");
    return false;
  }
  const size_t n = v.x.size();
  const std::string a = Num(1 - p.damping), d = Num(p.damping);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t r = static_cast<uint32_t>(i);
    if (i == 0) {
      o.err(r, 0, Err::NA);
    } else if (i == 1) {
      o.formula(r, 0, "=" + InRef(p, v.idx[0], v.idx[0]));
    } else {
      o.formula(r, 0, "=" + a + "*" + InRef(p, v.idx[i - 1], v.idx[i - 1]) + "+" + d + "*" + p.out_ref(r - 1, 0, r - 1, 0));
    }
    if (p.std_errors) {
      if (i < 4) {
        o.err(r, 1, Err::NA);
      } else {
        o.formula(r, 1, "=SQRT(SUMXMY2(" + InRef(p, v.idx[i - 3], v.idx[i - 1]) + "," + p.out_ref(r - 3, 0, r - 1, 0) + ")/3)");
      }
    }
  }
  return true;
}

// two samples: in (variable 1) and in2 (variable 2), each a single row or column
bool TwoSamples(const Params& p, Var* a, Var* b, std::string* error) {
  for (int k = 0; k < 2; ++k) {
    const Block& blk = k == 0 ? p.in : p.in2;
    if (blk.empty()) {
      *error = TR("Enter both variable ranges.");
      return false;
    }
    std::vector<Var> v;
    const bool rows = blk.rows == 1 && blk.cols > 1;
    if (!Vars(blk, rows, p.labels, "Variable ", &v, error)) return false;
    if (v.size() != 1) {
      *error = TR("Each variable range must be a single row or column.");
      return false;
    }
    if (!p.labels) v[0].label = DefaultLabel("Variable ", static_cast<size_t>(k));
    (k == 0 ? *a : *b) = std::move(v[0]);
  }
  if (a->x.size() < 2 || b->x.size() < 2) {
    *error = TR("Each variable needs at least two values.");
    return false;
  }
  return true;
}

void TestTail(Out& o, uint32_t& r, const char* stat, double t, double df, double alpha, bool z) {
  const double p1 = z ? st::NormSTail(std::fabs(t)) : st::TTail(std::fabs(t), df);
  o.text(r, 0, z ? "z" : "t Stat");
  o.num(r++, 1, t);
  o.text(r, 0, z ? "P(Z<=z) one-tail" : "P(T<=t) one-tail");
  o.num(r++, 1, p1);
  o.text(r, 0, z ? "z Critical one-tail" : "t Critical one-tail");
  o.num(r++, 1, z ? st::NormSInv(1 - alpha) : st::TInv(1 - alpha, df));
  o.text(r, 0, z ? "P(Z<=z) two-tail" : "P(T<=t) two-tail");
  o.num(r++, 1, 2 * p1);
  o.text(r, 0, z ? "z Critical two-tail" : "t Critical two-tail");
  o.num(r++, 1, z ? st::NormSInv(1 - alpha / 2) : st::TInv(1 - alpha / 2, df));
  (void)stat;
}

bool TwoSampleTest(const Params& p, Out& o, std::string* error) {
  Var a, b;
  if (!TwoSamples(p, &a, &b, error)) return false;
  const double n1 = static_cast<double>(a.x.size()), n2 = static_cast<double>(b.x.size());
  const double m1 = Mean(a.x), m2 = Mean(b.x), v1 = VarS(a.x), v2 = VarS(b.x);
  const char* title = p.tool == Tool::TTestPaired    ? "t-Test: Paired Two Sample for Means"
                      : p.tool == Tool::TTestEqual   ? "t-Test: Two-Sample Assuming Equal Variances"
                      : p.tool == Tool::TTestUnequal ? "t-Test: Two-Sample Assuming Unequal Variances"
                      : p.tool == Tool::ZTest        ? "z-Test: Two Sample for Means"
                                                     : "F-Test Two-Sample for Variances";
  o.text(0, 0, title);
  o.text(2, 1, a.label);
  o.text(2, 2, b.label);
  uint32_t r = 3;
  auto pair = [&](const char* name, double x, double y) {
    o.text(r, 0, name);
    o.num(r, 1, x);
    o.num(r, 2, y);
    ++r;
  };
  auto one = [&](const char* name, double x) {
    o.text(r, 0, name);
    o.num(r, 1, x);
    ++r;
  };
  pair("Mean", m1, m2);
  if (p.tool == Tool::ZTest) {
    if (p.var1 <= 0 || p.var2 <= 0) {
      *error = TR("Enter the known variances of both variables.");
      return false;
    }
    pair("Known Variance", p.var1, p.var2);
    pair("Observations", n1, n2);
    one("Hypothesized Mean Difference", p.hyp_diff);
    TestTail(o, r, "z", (m1 - m2 - p.hyp_diff) / std::sqrt(p.var1 / n1 + p.var2 / n2), 0, p.alpha, true);
    return true;
  }
  pair("Variance", v1, v2);
  pair("Observations", n1, n2);
  if (p.tool == Tool::FTest) {
    const double f = v1 / v2, d1 = n1 - 1, d2 = n2 - 1;
    pair("df", d1, d2);
    one("F", f);
    if (f < 1) {
      one("P(F<=f) one-tail", st::FCdf(f, d1, d2));
      one("F Critical one-tail", st::FInv(p.alpha, d1, d2));
    } else {
      one("P(F<=f) one-tail", st::FTail(f, d1, d2));
      one("F Critical one-tail", st::FInv(1 - p.alpha, d1, d2));
    }
    return true;
  }
  if (p.tool == Tool::TTestPaired) {
    if (a.x.size() != b.x.size()) {
      *error = TR("Paired samples must have the same number of values.");
      return false;
    }
    double sxy = 0;
    std::vector<double> d(a.x.size());
    for (size_t i = 0; i < d.size(); ++i) {
      sxy += (a.x[i] - m1) * (b.x[i] - m2);
      d[i] = a.x[i] - b.x[i];
    }
    one("Pearson Correlation", sxy / std::sqrt(SumSq(a.x, m1) * SumSq(b.x, m2)));
    one("Hypothesized Mean Difference", p.hyp_diff);
    const double df = n1 - 1;
    one("df", df);
    TestTail(o, r, "t", (Mean(d) - p.hyp_diff) / std::sqrt(VarS(d) / n1), df, p.alpha, false);
    return true;
  }
  if (p.tool == Tool::TTestEqual) {
    const double pooled = ((n1 - 1) * v1 + (n2 - 1) * v2) / (n1 + n2 - 2), df = n1 + n2 - 2;
    one("Pooled Variance", pooled);
    one("Hypothesized Mean Difference", p.hyp_diff);
    one("df", df);
    TestTail(o, r, "t", (m1 - m2 - p.hyp_diff) / std::sqrt(pooled * (1 / n1 + 1 / n2)), df, p.alpha, false);
    return true;
  }
  // unequal variances (Welch); Excel rounds the degrees of freedom
  const double s1 = v1 / n1, s2 = v2 / n2;
  const double df = std::round((s1 + s2) * (s1 + s2) / (s1 * s1 / (n1 - 1) + s2 * s2 / (n2 - 1)));
  one("Hypothesized Mean Difference", p.hyp_diff);
  one("df", df);
  TestTail(o, r, "t", (m1 - m2 - p.hyp_diff) / std::sqrt(s1 + s2), df, p.alpha, false);
  return true;
}

bool Histogram(const Params& p, Out& o, std::string* error) {
  std::vector<double> x;
  {
    const Block& b = p.in;
    for (uint32_t r = 0; r < b.rows; ++r) {
      for (uint32_t c = 0; c < b.cols; ++c) {
        if (p.labels && r == 0) continue;
        const Value& v = b.At(r, c);
        if (v.IsEmpty()) continue;
        if (!v.IsNumeric()) {
          *error = TR("The input range contains non-numeric data.");
          return false;
        }
        x.push_back(v.AsDouble());
      }
    }
  }
  if (x.empty()) {
    *error = TR("The input range is empty.");
    return false;
  }
  std::vector<double> bins;
  std::string bin_label = "Bin";
  if (!p.bins.empty()) {
    for (uint32_t r = 0; r < p.bins.rows; ++r) {
      for (uint32_t c = 0; c < p.bins.cols; ++c) {
        const Value& v = p.bins.At(r, c);
        if (p.labels && r == 0 && c == 0 && !v.IsNumeric()) {
          if (!LabelText(v).empty()) bin_label = LabelText(v);
          continue;
        }
        if (v.IsEmpty()) continue;
        if (!v.IsNumeric()) {
          *error = TR("The bin range contains non-numeric data.");
          return false;
        }
        bins.push_back(v.AsDouble());
      }
    }
  } else {
    // like Excel: about sqrt(n) bins of equal width from the minimum to the maximum
    const auto [mn, mx] = std::minmax_element(x.begin(), x.end());
    const int k = std::max(1, static_cast<int>(std::floor(std::sqrt(static_cast<double>(x.size())))));
    const double w = (*mx - *mn) / k;
    for (int i = 0; i <= k; ++i) bins.push_back(*mn + w * i);
    if (w == 0) bins.resize(1);
  }
  std::sort(bins.begin(), bins.end());
  std::vector<double> freq(bins.size() + 1, 0);
  for (double v : x) {
    const size_t i = static_cast<size_t>(std::lower_bound(bins.begin(), bins.end(), v) - bins.begin());
    freq[i] += 1;
  }
  const double n = static_cast<double>(x.size());
  o.text(0, 0, bin_label);
  o.text(0, 1, "Frequency");
  if (p.cumulative) o.text(0, 2, "Cumulative %");
  double cum = 0;
  for (size_t i = 0; i <= bins.size(); ++i) {
    const uint32_t r = static_cast<uint32_t>(i + 1);
    if (i < bins.size()) o.num(r, 0, bins[i]);
    else o.text(r, 0, "More");
    o.num(r, 1, freq[i]);
    cum += freq[i];
    if (p.cumulative) o.num(r, 2, cum / n, "0.00%");
  }
  if (p.pareto) {
    // sorted by frequency (descending), next to the first table
    const uint32_t c0 = p.cumulative ? 3 : 2;
    std::vector<size_t> ord(bins.size() + 1);
    std::iota(ord.begin(), ord.end(), 0);
    std::stable_sort(ord.begin(), ord.end(), [&](size_t i, size_t j) { return freq[i] > freq[j]; });
    o.text(0, c0, bin_label);
    o.text(0, c0 + 1, "Frequency");
    if (p.cumulative) o.text(0, c0 + 2, "Cumulative %");
    cum = 0;
    for (size_t k = 0; k < ord.size(); ++k) {
      const uint32_t r = static_cast<uint32_t>(k + 1);
      const size_t i = ord[k];
      if (i < bins.size()) o.num(r, c0, bins[i]);
      else o.text(r, c0, "More");
      o.num(r, c0 + 1, freq[i]);
      cum += freq[i];
      if (p.cumulative) o.num(r, c0 + 2, cum / n, "0.00%");
    }
  }
  return true;
}

bool RankPercentile(const Params& p, Out& o, std::string* error) {
  std::vector<Var> v;
  if (!Vars(p.in, p.by_rows, p.labels, p.by_rows ? "Row" : "Column", &v, error)) return false;
  for (size_t k = 0; k < v.size(); ++k) {
    const uint32_t c = static_cast<uint32_t>(k * 4);
    const auto& x = v[k].x;
    o.text(0, c, "Point");
    o.text(0, c + 1, v[k].label);
    o.text(0, c + 2, "Rank");
    o.text(0, c + 3, "Percent");
    std::vector<size_t> ord(x.size());
    std::iota(ord.begin(), ord.end(), 0);
    std::stable_sort(ord.begin(), ord.end(), [&](size_t i, size_t j) { return x[i] > x[j]; });
    std::vector<double> s = x;
    std::sort(s.begin(), s.end());
    const double n = static_cast<double>(x.size());
    for (size_t i = 0; i < ord.size(); ++i) {
      const double val = x[ord[i]];
      const uint32_t r = static_cast<uint32_t>(i + 1);
      o.num(r, c, static_cast<double>(ord[i] + 1));
      o.num(r, c + 1, val);
      // RANK (ties share the best rank) and PERCENTRANK.INC truncated to 3 digits
      const double greater = static_cast<double>(s.end() - std::upper_bound(s.begin(), s.end(), val));
      o.num(r, c + 2, greater + 1);
      const double below = static_cast<double>(std::lower_bound(s.begin(), s.end(), val) - s.begin());
      const double pr = n > 1 ? std::floor(below / (n - 1) * 1000 + 1e-9) / 1000 : 1;
      o.num(r, c + 3, pr, "0.00%");
    }
  }
  return true;
}

bool Sampling(const Params& p, Out& o, std::string* error) {
  std::vector<double> x;
  for (uint32_t c = 0; c < p.in.cols; ++c) {
    for (uint32_t r = 0; r < p.in.rows; ++r) {
      if (p.labels && r == 0) continue;
      const Value& v = p.in.At(r, c);
      if (v.IsEmpty()) continue;
      if (!v.IsNumeric()) {
        *error = TR("The input range contains non-numeric data.");
        return false;
      }
      x.push_back(v.AsDouble());
    }
  }
  if (x.empty()) {
    *error = TR("The input range is empty.");
    return false;
  }
  if (p.periodic) {
    if (p.period < 1 || static_cast<size_t>(p.period) > x.size()) {
      *error = TR("The period must be between 1 and the number of values.");
      return false;
    }
    uint32_t r = 0;
    for (size_t i = static_cast<size_t>(p.period) - 1; i < x.size(); i += static_cast<size_t>(p.period)) o.num(r++, 0, x[i]);
  } else {
    if (p.count < 1) {
      *error = TR("Enter the number of samples.");
      return false;
    }
    std::mt19937_64 rng(p.seed ? p.seed : std::random_device{}());
    std::uniform_int_distribution<size_t> pick(0, x.size() - 1);
    for (int i = 0; i < p.count; ++i) o.num(static_cast<uint32_t>(i), 0, x[pick(rng)]);
  }
  return true;
}

bool RandomNumbers(const Params& p, Out& o, std::string* error) {
  std::mt19937_64 rng(p.seed ? p.seed : std::random_device{}());
  const int vars = std::max(1, p.variables);
  if (p.distribution == 6) {
    // patterned: from p1 to p2 in steps of p3, each number p4 times, the sequence p5 times
    if (p.p3 <= 0 || p.p2 < p.p1) {
      *error = TR("Enter a range (from <= to) and a positive step.");
      return false;
    }
    std::vector<double> seq;
    for (int s = 0; s < std::max(1, static_cast<int>(p.p5)); ++s) {
      for (double v = p.p1; v <= p.p2 + 1e-12 * std::fabs(p.p2); v += p.p3) {
        for (int k = 0; k < std::max(1, static_cast<int>(p.p4)); ++k) seq.push_back(v);
      }
    }
    for (int c = 0; c < vars; ++c) {
      for (size_t i = 0; i < seq.size(); ++i) o.num(static_cast<uint32_t>(i), static_cast<uint32_t>(c), seq[i]);
    }
    return true;
  }
  if (p.points < 1) {
    *error = TR("Enter the number of random numbers.");
    return false;
  }
  std::vector<double> values, weights;
  if (p.distribution == 7) {
    if (p.bins.cols != 2 || p.bins.rows < 1) {
      *error = TR("The value and probability input range must have two columns.");
      return false;
    }
    double total = 0;
    for (uint32_t r = 0; r < p.bins.rows; ++r) {
      if (!p.bins.At(r, 0).IsNumeric() || !p.bins.At(r, 1).IsNumeric()) {
        *error = TR("The value and probability input range contains non-numeric data.");
        return false;
      }
      values.push_back(p.bins.At(r, 0).AsDouble());
      weights.push_back(p.bins.At(r, 1).AsDouble());
      total += weights.back();
    }
    if (std::fabs(total - 1) > 1e-9) {
      *error = TR("The probabilities must add up to 1.");
      return false;
    }
  }
  for (int c = 0; c < vars; ++c) {
    for (int i = 0; i < p.points; ++i) {
      double v = 0;
      switch (p.distribution) {
        case 1: v = std::uniform_real_distribution<double>(p.p1, p.p2)(rng); break;
        case 2: v = std::normal_distribution<double>(p.p1, std::max(0.0, p.p2))(rng); break;
        case 3: v = std::bernoulli_distribution(std::clamp(p.p1, 0.0, 1.0))(rng) ? 1 : 0; break;
        case 4: v = static_cast<double>(std::binomial_distribution<int>(std::max(0, static_cast<int>(p.p2)), std::clamp(p.p1, 0.0, 1.0))(rng)); break;
        case 5: v = static_cast<double>(std::poisson_distribution<int>(std::max(1e-12, p.p1))(rng)); break;
        case 7: v = values[std::discrete_distribution<size_t>(weights.begin(), weights.end())(rng)]; break;
        default:
          *error = TR("Unknown distribution.");
          return false;
      }
      o.num(static_cast<uint32_t>(i), static_cast<uint32_t>(c), v);
    }
  }
  return true;
}

// complex numbers as Excel writes them (COMPLEX): "3", "2i", "-1.5+2i"
std::string ComplexText(std::complex<double> z) {
  auto clean = [](double v) { return std::fabs(v) < 1e-15 ? 0.0 : v; };
  const double re = clean(z.real()), im = clean(z.imag());
  if (im == 0) return Num(re);
  std::string s = re != 0 ? Num(re) : std::string();
  if (im == 1) s += re != 0 ? "+i" : "i";
  else if (im == -1) s += "-i";
  else s += (im > 0 && re != 0 ? "+" : "") + Num(im) + "i";
  return s;
}

bool ParseComplex(const Value& v, std::complex<double>* out) {
  if (v.IsNumeric()) {
    *out = {v.AsDouble(), 0};
    return true;
  }
  if (!v.IsString()) return false;
  std::string s(v.str());
  s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
  if (s.empty()) return false;
  const char unit = s.back();
  if (unit != 'i' && unit != 'j') {
    char* end = nullptr;
    const double re = std::strtod(s.c_str(), &end);
    if (!end || *end) return false;
    *out = {re, 0};
    return true;
  }
  s.pop_back();
  // the imaginary part starts at the last sign that isn't an exponent's
  size_t k = s.size();
  while (k > 0) {
    --k;
    if ((s[k] == '+' || s[k] == '-') && (k == 0 || (s[k - 1] != 'e' && s[k - 1] != 'E'))) break;
  }
  const std::string re_s = s.substr(0, k), im_s = s.substr(k);
  auto num = [](const std::string& t, double* d) {
    if (t.empty() || t == "+") return *d = 1, true;
    if (t == "-") return *d = -1, true;
    char* end = nullptr;
    *d = std::strtod(t.c_str(), &end);
    return end && !*end;
  };
  double re = 0, im = 0;
  if (!re_s.empty()) {
    char* end = nullptr;
    re = std::strtod(re_s.c_str(), &end);
    if (!end || *end) return false;
  }
  if (!num(im_s, &im)) return false;
  *out = {re, im};
  return true;
}

bool Fourier(const Params& p, Out& o, std::string* error) {
  const bool rows = p.in.rows == 1 && p.in.cols > 1;
  const uint32_t len = rows ? p.in.cols : p.in.rows;
  std::vector<std::complex<double>> a;
  for (uint32_t i = p.labels ? 1 : 0; i < len; ++i) {
    std::complex<double> z;
    if (!ParseComplex(rows ? p.in.At(0, i) : p.in.At(i, 0), &z)) {
      *error = TR("The input range contains non-numeric data.");
      return false;
    }
    a.push_back(z);
  }
  const size_t n = a.size();
  if (n < 2 || n > 4096 || (n & (n - 1)) != 0) {
    *error = TR("The number of values must be a power of 2 (2 to 4096).");
    return false;
  }
  // iterative radix-2 FFT; the inverse divides by n
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  const double sign = p.inverse ? 1 : -1;
  for (size_t m = 2; m <= n; m <<= 1) {
    const std::complex<double> w = std::polar(1.0, sign * 2 * 3.14159265358979323846 / static_cast<double>(m));
    for (size_t k = 0; k < n; k += m) {
      std::complex<double> t(1, 0);
      for (size_t j = 0; j < m / 2; ++j) {
        const auto u = a[k + j], v = a[k + j + m / 2] * t;
        a[k + j] = u + v;
        a[k + j + m / 2] = u - v;
        t *= w;
      }
    }
  }
  for (size_t i = 0; i < n; ++i) {
    const auto z = p.inverse ? a[i] / static_cast<double>(n) : a[i];
    o.text(static_cast<uint32_t>(i), 0, ComplexText(z));
  }
  return true;
}

bool Regression(const Params& p, Out& o, std::string* error) {
  // y: one column; x: one or more columns, same rows
  if (p.in.empty() || p.in2.empty()) {
    *error = TR("Enter the Y and X ranges.");
    return false;
  }
  if (p.in.cols != 1) {
    *error = TR("The Y range must be a single column.");
    return false;
  }
  if (p.in2.rows != p.in.rows) {
    *error = TR("The Y and X ranges must have the same number of rows.");
    return false;
  }
  const uint32_t r0 = p.labels ? 1 : 0, k = p.in2.cols;
  std::vector<double> y;
  std::vector<std::vector<double>> x;
  for (uint32_t r = r0; r < p.in.rows; ++r) {
    std::vector<double> row;
    auto bad = [&] {
      *error = TR("The Y and X ranges must hold numbers only; a blank or text is in this row of the ranges: ") + std::to_string(r + 1);
      return false;
    };
    if (!p.in.At(r, 0).IsNumeric()) return bad();
    for (uint32_t c = 0; c < k; ++c) {
      if (!p.in2.At(r, c).IsNumeric()) return bad();
      row.push_back(p.in2.At(r, c).AsDouble());
    }
    y.push_back(p.in.At(r, 0).AsDouble());
    x.push_back(std::move(row));
  }
  const bool cst = !p.zero_constant;
  const size_t n = y.size(), q = k + (cst ? 1 : 0);
  if (n <= q) {
    *error = TR("There are too few observations for the number of X variables.");
    return false;
  }
  // normal equations with the inverse (Gauss-Jordan)
  auto col = [&](size_t i, size_t j) { return cst ? (j == 0 ? 1.0 : x[i][j - 1]) : x[i][j]; };
  std::vector<std::vector<double>> m(q, std::vector<double>(2 * q + 1, 0));
  for (size_t i = 0; i < n; ++i) {
    for (size_t a = 0; a < q; ++a) {
      for (size_t b = 0; b < q; ++b) m[a][b] += col(i, a) * col(i, b);
      m[a][2 * q] += col(i, a) * y[i];
    }
  }
  for (size_t a = 0; a < q; ++a) m[a][q + a] = 1;
  for (size_t c = 0; c < q; ++c) {
    size_t piv = c;
    for (size_t i = c + 1; i < q; ++i) {
      if (std::fabs(m[i][c]) > std::fabs(m[piv][c])) piv = i;
    }
    if (std::fabs(m[piv][c]) < 1e-12) {
      *error = TR("The X variables are collinear: the regression can't be computed.");
      return false;
    }
    std::swap(m[piv], m[c]);
    const double d = m[c][c];
    for (double& v : m[c]) v /= d;
    for (size_t i = 0; i < q; ++i) {
      if (i == c || m[i][c] == 0) continue;
      const double f = m[i][c];
      for (size_t l = 0; l < m[i].size(); ++l) m[i][l] -= f * m[c][l];
    }
  }
  std::vector<double> b(q), pred(n), res(n);
  for (size_t j = 0; j < q; ++j) b[j] = m[j][2 * q];
  const double ym = Mean(y);
  double sse = 0, sst = 0;
  for (size_t i = 0; i < n; ++i) {
    pred[i] = 0;
    for (size_t j = 0; j < q; ++j) pred[i] += b[j] * col(i, j);
    res[i] = y[i] - pred[i];
    sse += res[i] * res[i];
    sst += cst ? (y[i] - ym) * (y[i] - ym) : y[i] * y[i];
  }
  const double dfr = static_cast<double>(k), dfe = static_cast<double>(n - q), dft = cst ? static_cast<double>(n - 1) : static_cast<double>(n);
  const double ssr = sst - sse, r2 = sst == 0 ? 1 : ssr / sst, mse = sse / dfe;
  const double adj = 1 - (1 - r2) * dft / dfe;
  o.text(0, 0, "SUMMARY OUTPUT");
  o.text(2, 0, "Regression Statistics");
  o.text(3, 0, "Multiple R");
  o.num(3, 1, std::sqrt(r2));
  o.text(4, 0, "R Square");
  o.num(4, 1, r2);
  o.text(5, 0, "Adjusted R Square");
  o.num(5, 1, adj);
  o.text(6, 0, "Standard Error");
  o.num(6, 1, std::sqrt(mse));
  o.text(7, 0, "Observations");
  o.num(7, 1, static_cast<double>(n));
  o.text(9, 0, "ANOVA");
  const char* ah[] = {"", "df", "SS", "MS", "F", "Significance F"};
  for (uint32_t c = 0; c < 6; ++c) o.text(10, c, ah[c]);
  const double f = (ssr / dfr) / mse;
  o.text(11, 0, "Regression");
  o.num(11, 1, dfr);
  o.num(11, 2, ssr);
  o.num(11, 3, ssr / dfr);
  o.num(11, 4, f);
  o.num(11, 5, st::FTail(f, dfr, dfe));
  o.text(12, 0, "Residual");
  o.num(12, 1, dfe);
  o.num(12, 2, sse);
  o.num(12, 3, mse);
  o.text(13, 0, "Total");
  o.num(13, 1, dft);
  o.num(13, 2, sst);
  const double conf2 = p.confidence > 0 && p.confidence < 100 ? p.confidence : 95;
  char l2[48], u2[48];
  std::snprintf(l2, sizeof(l2), "Lower %.1f%%", conf2);
  std::snprintf(u2, sizeof(u2), "Upper %.1f%%", conf2);
  const char* ch[] = {"", "Coefficients", "Standard Error", "t Stat", "P-value", "Lower 95%", "Upper 95%", l2, u2};
  for (uint32_t c = 0; c < 9; ++c) o.text(15, c, ch[c]);
  const double t95 = st::TInv(0.975, dfe), t2 = st::TInv(1 - (1 - conf2 / 100) / 2, dfe);
  uint32_t r = 16;
  auto coef_row = [&](const std::string& name, size_t j) {
    o.text(r, 0, name);
    const double se = std::sqrt(std::max(0.0, mse * m[j][q + j]));
    const double t = b[j] / se;
    o.num(r, 1, b[j]);
    o.num(r, 2, se);
    o.num(r, 3, t);
    o.num(r, 4, 2 * st::TTail(std::fabs(t), dfe));
    o.num(r, 5, b[j] - t95 * se);
    o.num(r, 6, b[j] + t95 * se);
    o.num(r, 7, b[j] - t2 * se);
    o.num(r, 8, b[j] + t2 * se);
    ++r;
  };
  if (cst) {
    coef_row("Intercept", 0);
  } else {
    o.text(r, 0, "Intercept");
    o.num(r, 1, 0);
    for (uint32_t c = 2; c < 9; ++c) o.err(r, c, Err::NA);
    ++r;
  }
  for (uint32_t c = 0; c < k; ++c) {
    std::string name = k == 1 ? "X Variable" : "X Variable " + std::to_string(c + 1);
    if (p.labels && !LabelText(p.in2.At(0, c)).empty()) name = LabelText(p.in2.At(0, c));
    coef_row(name, cst ? c + 1 : c);
  }
  if (p.residuals || p.std_residuals || p.normal_probability) {
    r += 3;
    const uint32_t top = r;
    if (p.residuals || p.std_residuals) {
      o.text(r, 0, "RESIDUAL OUTPUT");
      r += 2;
      std::string yname = p.labels && !LabelText(p.in.At(0, 0)).empty() ? LabelText(p.in.At(0, 0)) : "Y";
      o.text(r, 0, "Observation");
      o.text(r, 1, "Predicted " + yname);
      o.text(r, 2, "Residuals");
      if (p.std_residuals) o.text(r, 3, "Standard Residuals");
      ++r;
      const double sres = std::sqrt(sse / static_cast<double>(n - 1));
      for (size_t i = 0; i < n; ++i, ++r) {
        o.num(r, 0, static_cast<double>(i + 1));
        o.num(r, 1, pred[i]);
        o.num(r, 2, res[i]);
        if (p.std_residuals) o.num(r, 3, res[i] / sres);
      }
    }
    if (p.normal_probability) {
      const uint32_t c0 = p.residuals || p.std_residuals ? (p.std_residuals ? 5u : 4u) : 0u;
      uint32_t pr = top;
      o.text(pr, c0, "PROBABILITY OUTPUT");
      pr += 2;
      std::string yname = p.labels && !LabelText(p.in.At(0, 0)).empty() ? LabelText(p.in.At(0, 0)) : "Y";
      o.text(pr, c0, "Percentile");
      o.text(pr, c0 + 1, yname);
      ++pr;
      std::vector<double> ys = y;
      std::sort(ys.begin(), ys.end());
      for (size_t i = 0; i < n; ++i, ++pr) {
        o.num(pr, c0, 100.0 * (2.0 * static_cast<double>(i) + 1) / (2.0 * static_cast<double>(n)));
        o.num(pr, c0 + 1, ys[i]);
      }
    }
  }
  return true;
}

}  // namespace

const char* ToolName(Tool t) {
  for (const auto& i : kTools) {
    if (i.tool == t) return i.name;
  }
  return "";
}

const char* ToolMacro(Tool t) {
  for (const auto& i : kTools) {
    if (i.tool == t) return i.macro;
  }
  return "";
}

bool ToolFromMacro(std::string_view name, Tool* out) {
  std::string n = Lower(name);
  if (const size_t bang = n.find('!'); bang != std::string::npos) n = n.substr(bang + 1);
  for (const auto& i : kTools) {
    if (Lower(i.macro) == n || Lower(i.name) == n) {
      *out = i.tool;
      return true;
    }
  }
  return false;
}

bool Run(const Params& p, Result* out, std::string* error) {
  *out = Result{};
  Out o{out};
  if (p.alpha <= 0 || p.alpha >= 1) {
    *error = TR("Alpha must be between 0 and 1.");
    return false;
  }
  const bool needs_input = p.tool != Tool::Random;
  if (needs_input && p.in.empty()) {
    *error = TR("Enter the input range.");
    return false;
  }
  switch (p.tool) {
    case Tool::Anova1: return Anova1(p, o, error);
    case Tool::Anova2: return Anova2(p, o, error);
    case Tool::Anova3: return Anova3(p, o, error);
    case Tool::Correlation: return Matrix(p, o, error, false);
    case Tool::Covariance: return Matrix(p, o, error, true);
    case Tool::Descriptive: return Descriptive(p, o, error);
    case Tool::ExpSmoothing: return ExpSmoothing(p, o, error);
    case Tool::FTest:
    case Tool::TTestPaired:
    case Tool::TTestEqual:
    case Tool::TTestUnequal:
    case Tool::ZTest: return TwoSampleTest(p, o, error);
    case Tool::Fourier: return Fourier(p, o, error);
    case Tool::Histogram: return Histogram(p, o, error);
    case Tool::MovingAverage: return MovingAverage(p, o, error);
    case Tool::Random: return RandomNumbers(p, o, error);
    case Tool::RankPercentile: return RankPercentile(p, o, error);
    case Tool::Regression: return Regression(p, o, error);
    case Tool::Sampling: return Sampling(p, o, error);
    default: break;
  }
  *error = TR("Unknown analysis tool.");
  return false;
}

}  // namespace yc::engine::analysis
