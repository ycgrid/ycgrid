// SPDX-License-Identifier: Apache-2.0
// fn_math2.cpp - more math & trig: ACOT ACOTH COT COTH CSC CSCH SEC SECH ARABIC ROMAN BASE CEILING.PRECISE ISO.CEILING
// FLOOR.PRECISE COMBINA FACTDOUBLE MULTINOMIAL SERIESSUM SQRTPI SUMX2MY2 SUMX2PY2 SUMXMY2 and the matrix functions
// MDETERM MINVERSE MMULT MUNIT
#include "formula/fn_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace yc::formula::fn {
namespace {

constexpr double kPi = 3.14159265358979323846;

EV Result(double v) { return std::isfinite(v) ? Num(v) : Er(Err::Num); }

// ---- trigonometry ----

// ACOT is the arctangent of 1 / x, taken into 0..pi: Excel's results to the last bit (30 values measured), where
// pi / 2 - atan(x) lost its digits as x grew (ACOT(1E+5) from the 12th digit, ACOT(1E+16) was 0; Excel's 1E-16)
EV FAcot(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x == 0) return Num(kPi / 2);
  const double r = std::atan(1 / x);
  return Result(x < 0 ? kPi + r : r);
}
// ACOTH: the logarithm's form is Excel's to the last bit near 1 (measured up to 3.6); from 4 on atanh(1 / x), which
// keeps its digits for a large x, where the logarithm of (x + 1) / (x - 1) lost them from 1E+5 and was 0 from 1E+16
// (Excel's 1E-16). Between 4 and 20 Excel's own results are up to 3 units in the last place away from both.
EV FAcoth(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  const double a = std::fabs(x);
  if (a <= 1) return Er(Err::Num);
  const double r = a < 4 ? 0.5 * std::log((a + 1) / (a - 1)) : std::atanh(1 / a);
  return Result(x < 0 ? -r : r);
}
// cot / csc / sec: |x| must be below 2^27 (Excel)
EV Recip(Evaluator& ev, const Node& call, double (*f)(double), bool zero_div, bool limit) {
  YC_NUM(x, 0);
  if (limit && std::fabs(x) >= 134217728.0) return Er(Err::Num);
  const double d = f(x);
  if (d == 0 || (zero_div && x == 0)) return Er(Err::Div0);
  return Result(1 / d);
}
EV FCot(Evaluator& ev, const Node& call) { return Recip(ev, call, [](double x) { return std::tan(x); }, true, true); }
EV FCoth(Evaluator& ev, const Node& call) { return Recip(ev, call, [](double x) { return std::tanh(x); }, true, false); }
EV FCsc(Evaluator& ev, const Node& call) { return Recip(ev, call, [](double x) { return std::sin(x); }, true, true); }
EV FCsch(Evaluator& ev, const Node& call) { return Recip(ev, call, [](double x) { return std::sinh(x); }, true, false); }
EV FSec(Evaluator& ev, const Node& call) { return Recip(ev, call, [](double x) { return std::cos(x); }, false, true); }
EV FSech(Evaluator& ev, const Node& call) { return Recip(ev, call, [](double x) { return std::cosh(x); }, false, false); }

// ---- Roman numerals ----

EV FRoman(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  int mode = 0;
  if (Has(call, 1)) {
    const Value f = ev.Scalar(call.kids[1]);
    if (f.IsError()) return EV::S(f);
    if (f.IsBool()) {
      mode = f.boolean() ? 0 : 4;
    } else {
      double m;
      Err e = Err::None;
      if (!Evaluator::ToNumber(f, &m, &e)) return Er(e);
      mode = static_cast<int>(std::trunc(m));
    }
  }
  n = std::trunc(n);
  if (n < 0 || n > 3999 || mode < 0 || mode > 4) return Er(Err::Value);
  static const char kChars[] = {'M', 'D', 'C', 'L', 'X', 'V', 'I'};
  static const int kValues[] = {1000, 500, 100, 50, 10, 5, 1};
  const int max_index = 6;
  std::string out;
  int val = static_cast<int>(n);
  for (int i = 0; i <= max_index / 2; ++i) {
    int index = 2 * i;
    const int digit = val / kValues[index];
    if (digit % 5 == 4) {
      const int index2 = digit == 4 ? index - 1 : index - 2;
      int steps = 0;
      while (steps < mode && index < max_index) {
        ++steps;
        if (kValues[index2] - kValues[index + 1] <= val) ++index;
        else steps = mode;
      }
      out += kChars[index];
      out += kChars[index2];
      val = val + kValues[index] - kValues[index2];
    } else {
      if (digit > 4) out += kChars[index - 1];
      out.append(static_cast<size_t>(digit % 5), kChars[index]);
      val %= kValues[index];
    }
  }
  return Str(out);
}

EV FArabic(Evaluator& ev, const Node& call) {
  YC_TEXT(s, 0);
  std::string t;
  for (char c : s) {
    if (c != ' ') t += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (t.size() > 255) return Er(Err::Value);
  bool neg = false;
  if (!t.empty() && t[0] == '-') {
    neg = true;
    t.erase(0, 1);
  }
  auto val = [](char c) {
    switch (c) {
      case 'I': return 1;
      case 'V': return 5;
      case 'X': return 10;
      case 'L': return 50;
      case 'C': return 100;
      case 'D': return 500;
      case 'M': return 1000;
      default: return 0;
    }
  };
  double total = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    const int v = val(t[i]);
    if (!v) return Er(Err::Value);
    const int next = i + 1 < t.size() ? val(t[i + 1]) : 0;
    total += v < next ? -v : v;
  }
  return Num(neg ? -total : total);
}

// ---- bases and rounding ----

EV FBase(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(radix, 1);
  YC_NUM_OPT(minlen, 2, 0);
  n = std::trunc(n);
  radix = std::trunc(radix);
  minlen = std::trunc(minlen);
  if (n < 0 || n >= 9007199254740992.0 || radix < 2 || radix > 36 || minlen < 0 || minlen > 255) return Er(Err::Num);
  static const char* kDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string s;
  auto u = static_cast<uint64_t>(n);
  const auto r = static_cast<uint64_t>(radix);
  do {
    s.insert(s.begin(), kDigits[u % r]);
    u /= r;
  } while (u);
  if (s.size() < static_cast<size_t>(minlen)) s.insert(0, static_cast<size_t>(minlen) - s.size(), '0');
  return Str(s);
}

// the quotient and the result at 15 significant digits (Round15), as CEILING / FLOOR read them
EV Precise(Evaluator& ev, const Node& call, bool up) {
  YC_NUM(x, 0);
  YC_NUM_OPT(sig, 1, 1);
  sig = std::fabs(sig);
  if (sig == 0 || x == 0) return Num(0);
  // the quotient at 15 digits first, as CEILING and FLOOR: FLOOR.PRECISE(0.3,0.1) is 0.3, not 0.2
  const double q = Round15(x / sig);
  return Result(Round15((up ? std::ceil(q) : std::floor(q)) * sig));
}
EV FCeilingPrecise(Evaluator& ev, const Node& call) { return Precise(ev, call, true); }
EV FFloorPrecise(Evaluator& ev, const Node& call) { return Precise(ev, call, false); }

// ---- combinatorics and series ----

double Combin(double n, double k) { return std::round(std::exp(std::lgamma(n + 1) - std::lgamma(k + 1) - std::lgamma(n - k + 1))); }

EV FCombinA(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(k, 1);
  n = std::trunc(n);
  k = std::trunc(k);
  if (n < 0 || k < 0 || n < k) return Er(Err::Num);
  if (n == 0 && k == 0) return Num(1);
  return Result(Combin(n + k - 1, k));
}

EV FFactDouble(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  n = std::trunc(n);
  if (n < -1) return Er(Err::Num);
  double r = 1;
  for (double k = n; k > 1; k -= 2) r *= k;
  return Result(r);
}

EV FMultinomial(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!CollectNumbers(ev, call, &v, &err)) return EV::S(err);
  double sum = 0, logden = 0;
  for (double x : v) {
    x = std::trunc(x);
    if (x < 0) return Er(Err::Num);
    sum += x;
    logden += std::lgamma(x + 1);
  }
  return Result(std::round(std::exp(std::lgamma(sum + 1) - logden)));
}

EV FSeriesSum(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(n, 1);
  YC_NUM(m, 2);
  const auto a = ev.ToArray(ev.Eval(call.kids[3]));
  if (!a) return Er(Err::Value);
  double total = 0;
  size_t i = 0;
  for (const Value& v : a->v) {
    if (v.IsError()) return EV::S(v);
    if (!v.IsNumeric()) return Er(Err::Value);
    total += v.AsDouble() * std::pow(x, n + static_cast<double>(i) * m);
    ++i;
  }
  return Result(total);
}

EV FSqrtPi(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x < 0) return Er(Err::Num);
  return Result(std::sqrt(x * kPi));
}

// SUMX2MY2 / SUMX2PY2 / SUMXMY2: two arrays of the same size; pairs where either value isn't a number are skipped,
// and without any pair of numbers the result is #DIV/0! (SUMX2MY2(C1:C3,D1:D3) of blank cells and
// SUMX2MY2({"a","b"},{1,1}), measured); an array of references is #NUM! (FuncInfo::ref_mode 5)
EV PairSum(Evaluator& ev, const Node& call, int kind) {
  const auto a = ev.ToArray(ev.Eval(call.kids[0]));
  const auto b = ev.ToArray(ev.Eval(call.kids[1]));
  if (!a || !b) return Er(Err::Value);
  if (a->v.size() != b->v.size()) return Er(Err::NA);
  double s = 0;
  bool pairs = false;
  for (size_t i = 0; i < a->v.size(); ++i) {
    if (a->v[i].IsError()) return EV::S(a->v[i]);
    if (b->v[i].IsError()) return EV::S(b->v[i]);
    if (!a->v[i].IsNumeric() || !b->v[i].IsNumeric()) continue;
    const double x = a->v[i].AsDouble(), y = b->v[i].AsDouble();
    s += kind == 0 ? x * x - y * y : kind == 1 ? x * x + y * y : (x - y) * (x - y);
    pairs = true;
  }
  if (!pairs) return Er(Err::Div0);
  return Result(s);
}
EV FSumX2MY2(Evaluator& ev, const Node& call) { return PairSum(ev, call, 0); }
EV FSumX2PY2(Evaluator& ev, const Node& call) { return PairSum(ev, call, 1); }
EV FSumXMY2(Evaluator& ev, const Node& call) { return PairSum(ev, call, 2); }

// ---- matrices ----

using Mat = std::vector<std::vector<double>>;

// a numeric matrix; false = #VALUE! (blank or text cells)
bool ToMatrix(Evaluator& ev, const Node& n, Mat* m, EV* err) {
  const auto a = ev.ToArray(ev.Eval(n));
  if (!a || a->v.empty()) {
    *err = Er(Err::Value);
    return false;
  }
  m->assign(a->rows, std::vector<double>(a->cols));
  for (uint32_t r = 0; r < a->rows; ++r) {
    for (uint32_t c = 0; c < a->cols; ++c) {
      const Value& v = a->At(r, c);
      if (v.IsError()) {
        *err = EV::S(v);
        return false;
      }
      if (!v.IsNumeric()) {
        *err = Er(Err::Value);
        return false;
      }
      (*m)[r][c] = v.AsDouble();
    }
  }
  return true;
}

EV FromMatrix(const Mat& m) {
  auto a = std::make_shared<Array>();
  a->rows = static_cast<uint32_t>(m.size());
  a->cols = m.empty() ? 0 : static_cast<uint32_t>(m[0].size());
  a->v.reserve(static_cast<size_t>(a->rows) * a->cols);
  for (const auto& row : m) {
    for (double x : row) a->v.push_back(std::isfinite(x) ? Value::Number(x) : Value::Error(Err::Num));
  }
  return EV::A(std::move(a));
}

// LU decomposition with partial pivoting; returns the determinant, inverts into *inv when given
bool Lu(Mat m, double* det, Mat* inv) {
  const size_t n = m.size();
  Mat id(n, std::vector<double>(n, 0));
  for (size_t i = 0; i < n; ++i) id[i][i] = 1;
  double d = 1;
  for (size_t c = 0; c < n; ++c) {
    size_t p = c;
    for (size_t r = c + 1; r < n; ++r) {
      if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
    }
    if (m[p][c] == 0) {
      *det = 0;
      return false;
    }
    if (p != c) {
      std::swap(m[p], m[c]);
      std::swap(id[p], id[c]);
      d = -d;
    }
    d *= m[c][c];
    const double piv = m[c][c];
    for (size_t k = 0; k < n; ++k) {
      m[c][k] /= piv;
      id[c][k] /= piv;
    }
    for (size_t r = 0; r < n; ++r) {
      if (r == c || m[r][c] == 0) continue;
      const double f = m[r][c];
      for (size_t k = 0; k < n; ++k) {
        m[r][k] -= f * m[c][k];
        id[r][k] -= f * id[c][k];
      }
    }
  }
  *det = d;
  if (inv) *inv = std::move(id);
  return true;
}

EV FMDeterm(Evaluator& ev, const Node& call) {
  Mat m;
  EV err;
  if (!ToMatrix(ev, call.kids[0], &m, &err)) return err;
  if (m.size() != m[0].size()) return Er(Err::Value);
  double d;
  Lu(m, &d, nullptr);
  return Result(d);
}

EV FMInverse(Evaluator& ev, const Node& call) {
  Mat m;
  EV err;
  if (!ToMatrix(ev, call.kids[0], &m, &err)) return err;
  if (m.size() != m[0].size()) return Er(Err::Value);
  double d;
  Mat inv;
  if (!Lu(m, &d, &inv) || std::fabs(d) < 1e-300) return Er(Err::Num);
  return FromMatrix(inv);
}

EV FMMult(Evaluator& ev, const Node& call) {
  Mat a, b;
  EV err;
  if (!ToMatrix(ev, call.kids[0], &a, &err)) return err;
  if (!ToMatrix(ev, call.kids[1], &b, &err)) return err;
  if (a[0].size() != b.size()) return Er(Err::Value);
  Mat c(a.size(), std::vector<double>(b[0].size(), 0));
  for (size_t i = 0; i < a.size(); ++i) {
    for (size_t k = 0; k < b.size(); ++k) {
      const double x = a[i][k];
      for (size_t j = 0; j < b[0].size(); ++j) c[i][j] += x * b[k][j];
    }
  }
  return FromMatrix(c);
}

EV FMUnit(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  n = std::trunc(n);
  if (n < 1 || n > 10000) return Er(Err::Value);
  const auto k = static_cast<size_t>(n);
  Mat m(k, std::vector<double>(k, 0));
  for (size_t i = 0; i < k; ++i) m[i][i] = 1;
  return FromMatrix(m);
}

}  // namespace

void RegisterMath2(std::vector<FuncInfo>& t) {
  t.push_back({"ACOT", 1, 1, FAcot});
  t.push_back({"ACOTH", 1, 1, FAcoth});
  t.push_back({"COT", 1, 1, FCot});
  t.push_back({"COTH", 1, 1, FCoth});
  t.push_back({"CSC", 1, 1, FCsc});
  t.push_back({"CSCH", 1, 1, FCsch});
  t.push_back({"SEC", 1, 1, FSec});
  t.push_back({"SECH", 1, 1, FSech});
  t.push_back({"ROMAN", 1, 2, FRoman});
  t.push_back({"ARABIC", 1, 1, FArabic});
  t.push_back({"BASE", 2, 3, FBase});
  t.push_back({"CEILING.PRECISE", 1, 2, FCeilingPrecise});
  t.push_back({"ISO.CEILING", 1, 2, FCeilingPrecise});
  t.push_back({"FLOOR.PRECISE", 1, 2, FFloorPrecise});
  t.push_back({"COMBINA", 2, 2, FCombinA});
  t.push_back({"FACTDOUBLE", 1, 1, FFactDouble});
  t.push_back({"MULTINOMIAL", 1, 255, FMultinomial});
  t.push_back({"SERIESSUM", 4, 4, FSeriesSum});
  t.push_back({"SQRTPI", 1, 1, FSqrtPi});
  t.push_back({"SUMX2MY2", 2, 2, FSumX2MY2});
  t.push_back({"SUMX2PY2", 2, 2, FSumX2PY2});
  t.push_back({"SUMXMY2", 2, 2, FSumXMY2});
  t.push_back({"MDETERM", 1, 1, FMDeterm});
  t.push_back({"MINVERSE", 1, 1, FMInverse});
  t.push_back({"MMULT", 2, 2, FMMult});
  t.push_back({"MUNIT", 1, 1, FMUnit});
}

}  // namespace yc::formula::fn
