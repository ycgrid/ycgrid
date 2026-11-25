// SPDX-License-Identifier: Apache-2.0
// fn_math.cpp - math functions
#include "formula/fn_util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>

namespace yc::formula::fn {
namespace {

double RoundDigits(double x, double digits, int mode) {  // mode 0 round, 1 round up, -1 round down
  // more digits than a number has keep it, fewer than 1E-308 make it 0 (ROUNDDOWN(2.5, 400) is 2.5, (2.5, -400) 0)
  if (digits > 400) return x;
  if (digits < -400 && mode <= 0) return 0;
  const int d = static_cast<int>(std::trunc(std::clamp(digits, -400.0, 400.0)));
  const double p = std::pow(10.0, d);
  if (!std::isfinite(x * p)) return x;
  if (p == 0 && mode <= 0) return 0;
  double y = Round15(x * p);
  double r;
  if (mode == 0) r = std::round(y);
  // a tolerance relative to the number: ROUNDUP(1E-20, 1) is 0.1, as Excel's (an absolute one took it for 0)
  else if (mode > 0) r = y >= 0 ? std::ceil(y - std::fabs(y) * 1e-12) : std::floor(y + std::fabs(y) * 1e-12);
  else r = std::trunc(y);
  return Round15(r / p);
}

std::mt19937_64& Rng() {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  return rng;
}

EV FSum(Evaluator& ev, const Node& call) { return SumValues(ev, call); }

EV FProduct(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!CollectNumbers(ev, call, &v, &err, 0, kNumsSkipBlankArgs)) return EV::S(err);
  if (v.empty()) return Num(0);
  double p = 1;
  for (double x : v) p *= x;
  return Num(p);
}

EV FSumSq(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!CollectNumbers(ev, call, &v, &err)) return EV::S(err);
  double s = 0;
  for (double x : v) s += x * x;
  return Num(s);
}

EV FSumIf(Evaluator& ev, const Node& call) {
  const EV range = ev.Eval(call.kids[0]);
  const Criteria cr = ParseCriteria(ev.Scalar(call.kids[1]));
  EV sum_range = Has(call, 2) ? ev.Eval(call.kids[2]) : range;
  // sum_range is aligned at its upper-left corner and has the size of range
  if (Has(call, 2) && sum_range.k == EV::K::Range && range.k == EV::K::Range) {
    core::Rect r = sum_range.r.rect;
    r.r1 = r.r0 + static_cast<uint32_t>(range.r.rect.Rows() - 1);
    r.c1 = r.c0 + static_cast<uint32_t>(range.r.rect.Cols() - 1);
    sum_range.r.rect = r;
  }
  auto a = ev.ToArray(range);
  auto b = ev.ToArray(sum_range);
  if (!a || !b) return Er(Err::Num);
  double s = 0;
  for (size_t i = 0; i < a->v.size() && i < b->v.size(); ++i) {
    if (!cr.Match(a->v[i])) continue;
    const Value& x = b->v[i];
    if (x.IsError()) return EV::S(x);
    if (x.IsNumeric()) s += x.AsDouble();
  }
  return Num(s);
}

// shared by SUMIFS / COUNTIFS / AVERAGEIFS / MAXIFS / MINIFS: returns the indexes that meet every criterion
bool MatchIfs(Evaluator& ev, const Node& call, int first, std::vector<char>* ok, size_t* n, Value* err) {
  *n = 0;
  for (int i = first; i + 1 < Argc(call); i += 2) {
    auto a = ev.ToArray(ev.Eval(call.kids[static_cast<size_t>(i)]));
    if (!a) {
      *err = Value::Error(Err::Num);
      return false;
    }
    const Criteria cr = ParseCriteria(ev.Scalar(call.kids[static_cast<size_t>(i + 1)]));
    if (ok->empty()) {
      ok->assign(a->v.size(), 1);
      *n = a->v.size();
    } else if (a->v.size() != *n) {
      *err = Value::Error(Err::Value);
      return false;
    }
    for (size_t k = 0; k < a->v.size(); ++k) {
      if ((*ok)[k] && !cr.Match(a->v[k])) (*ok)[k] = 0;
    }
  }
  return true;
}

EV FSumIfs(Evaluator& ev, const Node& call) {
  std::vector<char> ok;
  size_t n;
  Value err;
  if (!MatchIfs(ev, call, 1, &ok, &n, &err)) return EV::S(err);
  auto s = ev.ToArray(ev.Eval(call.kids[0]));
  if (!s || s->v.size() != n) return Er(Err::Value);
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!ok[i]) continue;
    if (s->v[i].IsError()) return EV::S(s->v[i]);
    if (s->v[i].IsNumeric()) sum += s->v[i].AsDouble();
  }
  return Num(sum);
}

EV FSumProduct(Evaluator& ev, const Node& call) {
  std::vector<std::shared_ptr<Array>> arrs;
  bool sizes_differ = false;
  for (const Node& k : call.kids) {
    auto a = ev.ToArray(ev.Eval(k));
    if (!a) return Er(Err::Num);
    if (!arrs.empty() && (a->rows != arrs[0]->rows || a->cols != arrs[0]->cols)) sizes_differ = true;
    arrs.push_back(a);
  }
  if (arrs.empty()) return Er(Err::Value);
  if (sizes_differ) {  // an error in the arrays comes before their sizes (Excel 16, measured)
    for (const auto& a : arrs) {
      for (const Value& v : a->v) {
        if (v.IsError()) return EV::S(v);
      }
    }
    return Er(Err::Value);
  }
  double s = 0;
  for (size_t i = 0; i < arrs[0]->v.size(); ++i) {
    double p = 1;
    for (const auto& a : arrs) {
      const Value& v = a->v[i];
      if (v.IsError()) return EV::S(v);
      p *= v.IsNumeric() ? v.AsDouble() : 0.0;
    }
    s += p;
  }
  return Num(s);
}

template <double (*F)(double)>
EV Unary1(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  const double r = F(x);
  if (!std::isfinite(r)) return Er(Err::Num);
  return Num(r);
}

double Sign(double x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); }
// INT reads its number at 15 digits first, as Excel (INT(4.35*100) is 435: the product is 434.99999999999994)
double IntF(double x) { return std::floor(Round15(x)); }
double Deg(double x) { return x * 180.0 / 3.14159265358979323846; }
double Rad(double x) { return x * 3.14159265358979323846 / 180.0; }

EV FAbs(Evaluator& ev, const Node& call) {
  const Value v = ev.Scalar(call.kids[0]);
  if (v.IsDecimal()) {
    core::Dec d = core::DecFromValue(v);
    d.m = d.m.Abs();
    return EV::S(core::DecToValue(d));
  }
  double x;
  Err e = Err::None;
  if (!Evaluator::ToNumber(v, &x, &e)) return Er(e);
  return Num(std::fabs(x));
}

EV FSqrt(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x < 0) return Er(Err::Num);
  return Num(std::sqrt(x));
}
EV FLn(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x <= 0) return Er(Err::Num);
  return Num(std::log(x));
}
EV FLog10(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x <= 0) return Er(Err::Num);
  return Num(std::log10(x));
}
EV FLog(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT_BLANK0(b, 1, 10);  // LOG(100,) is #NUM!: a base left empty is 0 (Excel 16, measured)
  if (x <= 0 || b <= 0) return Er(Err::Num);
  if (b == 1) return Er(Err::Div0);
  return Num(std::log(x) / std::log(b));
}
EV FExp(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  const double r = std::exp(x);
  if (!std::isfinite(r)) return Er(Err::Num);
  return Num(r);
}
// x^y; a negative x takes a y whose reciprocal is an odd whole number as that root ((-8)^(1/3) is -2, as in Excel)
double PowerOf(double x, double y) {
  if (x < 0 && y != std::trunc(y)) {
    const double inv = 1 / y, k = std::round(inv);
    if (std::fabs(inv - k) <= 1e-12 * std::fabs(inv) && std::fmod(k, 2) != 0) return -std::pow(-x, y);
  }
  return std::pow(x, y);
}

EV FPower(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(y, 1);
  if (x == 0 && y == 0) return Er(Err::Num);
  if (x == 0 && y < 0) return Er(Err::Div0);
  const double r = PowerOf(x, y);
  if (!std::isfinite(r)) return Er(Err::Num);
  return Num(r);
}
EV FPi(Evaluator&, const Node&) { return Num(3.14159265358979323846); }

EV FRound(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT(d, 1, 0);
  return Num(RoundDigits(x, d, 0));
}
EV FRoundUp(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT(d, 1, 0);
  return Num(RoundDigits(x, d, 1));
}
EV FRoundDown(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT(d, 1, 0);
  return Num(RoundDigits(x, d, -1));
}
EV FTrunc(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT(d, 1, 0);
  return Num(RoundDigits(x, d, -1));
}
EV FMRound(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(m, 1);
  if (m == 0) return Num(0);
  if ((x > 0 && m < 0) || (x < 0 && m > 0)) return Er(Err::Num);
  // Excel 16, measured on 1,268 cases around the halves: the quotient in doubles, up when its fraction reaches a
  // half within 5E-15, and the product as it comes. MROUND(6.05,0.1) is 6 (the quotient is 60.49999999999999) and
  // MROUND(1.15,0.1) is 1.2000000000000002; the quotient and the product at 15 digits gave 6.1 and 1.2.
  const double q = std::fabs(x) / std::fabs(m);
  double k = std::floor(q);
  if (q - k >= 0.5 - 5e-15) k += 1;
  const double r = k * std::fabs(m);
  if (!std::isfinite(r)) return Er(Err::Num);
  return Num(x < 0 ? -r : r);
}

EV FMod(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(d, 1);
  if (d == 0) return Er(Err::Div0);
  // x - d * q with one rounding (as Excel: MOD(1,0.1) is 0.09999999999999995); the result has the divisor's sign and
  // stays below it: a residue of the other sign gets d added (MOD(6.3,2.1) is 2.0999999999999996), and a result
  // that reaches d is 0 (MOD(-1E-20,1) is 0)
  double r = std::fma(-d, std::floor(x / d), x);
  if (r != 0 && (r < 0) != (d < 0)) r += d;
  if (std::fabs(r) >= std::fabs(d)) r = 0;
  return Num(r);
}
EV FQuotient(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(d, 1);
  if (d == 0) return Er(Err::Div0);
  return Num(std::trunc(x / d));
}

double CeilTo(double x, double s) { return s == 0 ? 0 : Round15(std::ceil(Round15(x / s)) * s); }
double FloorTo(double x, double s) { return s == 0 ? 0 : Round15(std::floor(Round15(x / s)) * s); }

EV FCeiling(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  // a significance left empty is 0: CEILING(2.5,) is 0 and FLOOR(2.5,) #DIV/0! (Excel 16, measured)
  YC_NUM_OPT_BLANK0(s, 1, x >= 0 ? 1 : -1);
  if (x > 0 && s < 0) return Er(Err::Num);
  if (x < 0 && s < 0) return Num(-CeilTo(-x, -s));
  return Num(CeilTo(x, s));
}
EV FFloor(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT_BLANK0(s, 1, 1);
  if (s == 0) return x == 0 ? Num(0) : Er(Err::Div0);  // FLOOR(0,0) is 0, as in Excel
  if (x > 0 && s < 0) return Er(Err::Num);
  if (x < 0 && s < 0) return Num(-FloorTo(-x, -s));
  return Num(FloorTo(x, s));
}
EV FCeilingMath(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT(s, 1, 1);
  YC_NUM_OPT(mode, 2, 0);
  s = std::fabs(s);
  if (s == 0) return Num(0);
  if (x < 0 && mode != 0) return Num(-CeilTo(-x, s));
  return Num(CeilTo(x, s));
}
EV FFloorMath(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM_OPT(s, 1, 1);
  YC_NUM_OPT(mode, 2, 0);
  s = std::fabs(s);
  if (s == 0) return Num(0);
  if (x < 0 && mode != 0) return Num(-FloorTo(-x, s));
  return Num(FloorTo(x, s));
}
EV FEven(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  double r = std::ceil(std::fabs(x) / 2) * 2;
  return Num(x < 0 ? -r : r);
}
EV FOdd(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  double a = std::ceil(std::fabs(x));
  if (std::fmod(a, 2) == 0) a += 1;
  return Num(x < 0 ? -a : a);
}

// GCD / LCM arguments: as CollectNumbers, but a logical or text that isn't a number inside a reference, range or
// array is #VALUE!, as in Excel (GCD(A1:A3) with TRUE in A3 is #VALUE!; a cell holding the text 5 counts as 5)
bool IntegerArgs(Evaluator& ev, const Node& call, std::vector<double>* out, Value* err) {
  for (const Node& k : call.kids) {
    if (k.kind == NK::Missing) continue;
    const EV e = ev.Eval(k);
    if (e.k == EV::K::Scalar) {
      if (e.s.IsError()) {
        *err = e.s;
        return false;
      }
      if (e.s.IsEmpty()) continue;
      double d;
      Err er = Err::None;
      if (!Evaluator::ToNumber(e.s, &d, &er)) {
        *err = Value::Error(er);
        return false;
      }
      out->push_back(d);
      continue;
    }
    bool ok = true;
    ev.ForEachValue(e, [&](const Value& v) {
      if (v.IsError() || v.IsPending()) {
        *err = v;
        ok = false;
      } else if (v.IsBool()) {
        *err = Value::Error(Err::Value);
        ok = false;
      } else if (v.IsString()) {
        double d;
        Err er = Err::None;
        if (Evaluator::ToNumber(v, &d, &er)) {
          out->push_back(d);
        } else {
          *err = Value::Error(Err::Value);
          ok = false;
        }
      } else if (v.IsNumeric()) {
        out->push_back(v.AsDouble());
      }
      return ok;
    });
    if (!ok) return false;
  }
  return true;
}

EV FGcd(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!IntegerArgs(ev, call, &v, &err)) return EV::S(err);
  long long g = 0;
  for (double x : v) {
    if (x < 0 || x > 9007199254740992.0) return Er(Err::Num);  // past 2^53: #NUM!, as Excel
    g = std::gcd(g, static_cast<long long>(std::floor(x)));
  }
  return Num(static_cast<double>(g));
}
EV FLcm(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!IntegerArgs(ev, call, &v, &err)) return EV::S(err);
  long long l = 1;
  for (double x : v) {
    if (x < 0 || x > 9007199254740992.0) return Er(Err::Num);
    const long long n = static_cast<long long>(std::floor(x));
    if (n == 0) return Num(0);
    // a multiple past 2^53 is #NUM! (LCM(2^52, 3): Excel 16, measured)
    const long long g = std::gcd(l, n);
    if (static_cast<double>(l / g) * static_cast<double>(n) > 9007199254740992.0) return Er(Err::Num);
    l = l / g * n;
  }
  return Num(static_cast<double>(l));
}
EV FFact(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x < 0 || x >= 171) return Er(Err::Num);  // 171! is past a double (FACT(1E+20) was 1)
  double r = 1;
  for (int i = 2; i <= static_cast<int>(std::floor(x)); ++i) r *= i;
  if (!std::isfinite(r)) return Er(Err::Num);
  return Num(r);
}
EV FCombin(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(k, 1);
  n = std::floor(n);
  k = std::floor(k);
  if (n < 0 || k < 0 || k > n) return Er(Err::Num);
  double r = 1;
  for (int i = 1; i <= static_cast<int>(k); ++i) r = r * (n - k + i) / i;
  return Num(std::round(r));
}

EV FRand(Evaluator&, const Node&) {
  std::uniform_real_distribution<double> d(0.0, 1.0);
  return Num(d(Rng()));
}
EV FRandBetween(Evaluator& ev, const Node& call) {
  YC_NUM(a, 0);
  YC_NUM(b, 1);
  // Excel 16, measured: a bottom above the top is #NUM! before any rounding (RANDBETWEEN(3,2.5), though 2.5 would
  // round up to 3); the integers from the bottom rounded up to the top rounded down are equally likely (600 draws of
  // RANDBETWEEN(1,2.5) gave 1 and 2 only, half each), and with no integer between the two the result is the bottom
  // rounded up (RANDBETWEEN(0.2,0.3) is 1, RANDBETWEEN(2.5,2.5) 3). In doubles, not integers: RANDBETWEEN(1E+19,
  // 1E+19) is 1E+19.
  if (a > b) return Er(Err::Num);
  const double lo = std::ceil(a);
  const double hi = std::max(lo, std::floor(b));
  std::uniform_real_distribution<double> d(0.0, 1.0);
  double r = lo + std::floor(d(Rng()) * (hi - lo + 1));
  if (r > hi) r = hi;
  if (r == 0) r = 0;  // never -0 (RANDBETWEEN(-0.9,-0.1))
  return Num(r);
}

double TrigSin(double x) { return std::sin(x); }
double TrigCos(double x) { return std::cos(x); }
double TrigTan(double x) { return std::tan(x); }
double TrigAtan(double x) { return std::atan(x); }
EV FAsin(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x < -1 || x > 1) return Er(Err::Num);
  return Num(std::asin(x));
}
EV FAcos(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x < -1 || x > 1) return Er(Err::Num);
  return Num(std::acos(x));
}
EV FAtan2(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(y, 1);
  if (x == 0 && y == 0) return Er(Err::Div0);
  return Num(std::atan2(y, x));
}

// SUBTOTAL / AGGREGATE. skip_hidden: skip every hidden row; skip_filtered: skip the rows an AutoFilter hides;
// skip_errors: error values are left out (else the first one is the result); skip_nested: cells holding SUBTOTAL
// or AGGREGATE are left out; with_k: the last argument is k of the array form (AGGREGATE 13-19)
struct AggOpts {
  bool skip_hidden = false, skip_filtered = false, skip_errors = false, skip_nested = true, with_k = false;
};

// the k-th percentile of sorted values, inclusive (Excel's PERCENTILE.INC) or exclusive (PERCENTILE.EXC)
EV PercentileOfSorted(const std::vector<double>& s, double k, bool exclusive) {
  const double n = static_cast<double>(s.size());
  if (s.empty() || k < 0 || k > 1) return Er(Err::Num);
  const double rank = exclusive ? k * (n + 1) - 1 : k * (n - 1);
  if (rank < 0 || rank > n - 1) return Er(Err::Num);
  const size_t lo = static_cast<size_t>(std::floor(rank));
  const double frac = rank - static_cast<double>(lo);
  return Num(lo + 1 < s.size() ? s[lo] + frac * (s[lo + 1] - s[lo]) : s[lo]);
}

EV Aggregate(Evaluator& ev, int fn_num, const Node& call, int first, AggOpts o) {
  std::vector<double> v;
  size_t counta = 0;
  bool err = false;
  Value errv;
  // An error value: COUNT (2) never sees it, COUNTA (3) counts it as any other value, and for every other function
  // the first one is the result; unless the option leaves errors out (Excel 16, measured: SUBTOTAL(3,G1:G8) counts
  // #DIV/0! and #N/A, SUBTOTAL(2,G1:G8) is 2, SUBTOTAL(9,G1:G8) is #DIV/0!).
  auto take = [&](const Value& x) {
    if (x.IsError()) {
      if (o.skip_errors || fn_num == 2) return true;
      if (fn_num == 3) {
        ++counta;
        return true;
      }
      errv = x;
      err = true;
      return false;
    }
    if (!x.IsEmpty()) ++counta;
    if (x.IsNumeric()) v.push_back(x.AsDouble());
    return true;
  };
  const size_t last = o.with_k ? call.kids.size() - 1 : call.kids.size();
  for (size_t i = static_cast<size_t>(first); i < last; ++i) {
    const EV e = ev.Eval(call.kids[i]);
    if (e.k == EV::K::Range) {
      ev.host().ForEachCell(e.r.sheet, ev.ClampRect(e.r.sheet, e.r.rect), [&](uint32_t r, uint32_t c, const Value& x) {
        if (o.skip_hidden && ev.host().RowHidden(e.r.sheet, r)) return true;
        if (o.skip_filtered && ev.host().RowFiltered(e.r.sheet, r)) return true;
        // nested SUBTOTALs are not counted twice
        if (o.skip_nested) {
          const std::string f = ev.host().FormulaText(e.r.sheet, r, c);
          if (!f.empty() && (f.find("SUBTOTAL(") != std::string::npos || f.find("AGGREGATE(") != std::string::npos)) return true;
        }
        return take(x);
      });
    } else {
      ev.ForEachValue(e, take);
    }
    if (err) return EV::S(errv);
  }
  if (fn_num == 13) {  // AGGREGATE's MODE.SNGL: the most frequent value, the first one met on a tie
    size_t best = 0;
    double mode = 0;
    for (size_t a = 0; a < v.size(); ++a) {
      const size_t c = static_cast<size_t>(std::count(v.begin(), v.end(), v[a]));
      if (c > best) best = c, mode = v[a];
    }
    return best > 1 ? Num(mode) : Er(Err::NA);
  }
  if (o.with_k) {  // AGGREGATE's array form: 14 LARGE, 15 SMALL, 16-19 percentiles and quartiles
    YC_NUM(k, static_cast<int>(call.kids.size() - 1));
    std::sort(v.begin(), v.end());
    if (fn_num == 14 || fn_num == 15) {
      // k is within 1..n as given, then LARGE takes it rounded up and SMALL rounded down, as the functions
      // themselves (Excel 16, measured: AGGREGATE(15,6,{1,2,3},1.5) is 1 and (14,6,{1,2,3},0.9) #NUM!)
      if (!(k >= 1 && k <= static_cast<double>(v.size()))) return Er(Err::Num);
      const double kk = fn_num == 14 ? std::ceil(k) : std::floor(k);
      const size_t at = static_cast<size_t>(kk) - 1;
      return Num(fn_num == 14 ? v[v.size() - 1 - at] : v[at]);
    }
    if (fn_num == 16) return PercentileOfSorted(v, k, false);
    if (fn_num == 18) return PercentileOfSorted(v, k, true);
    const double q = std::floor(k);
    if (fn_num == 17) return q < 0 || q > 4 ? Er(Err::Num) : PercentileOfSorted(v, q / 4, false);
    return q < 1 || q > 3 ? Er(Err::Num) : PercentileOfSorted(v, q / 4, true);  // 19
  }
  const size_t n = v.size();
  auto mean = [&] { return ExcelSum(v) / static_cast<double>(n); };  // the sums cancel as SUM's do
  auto var = [&](bool sample) -> double {
    const double m = mean();
    double s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return s / static_cast<double>(sample ? n - 1 : n);
  };
  switch (fn_num) {
    case 1: return n ? Num(mean()) : Er(Err::Div0);
    case 2: return Num(static_cast<double>(n));
    case 3: return Num(static_cast<double>(counta));
    case 4: return Num(n ? *std::max_element(v.begin(), v.end()) : 0);
    case 5: return Num(n ? *std::min_element(v.begin(), v.end()) : 0);
    case 6: {
      double p = n ? 1 : 0;
      for (double x : v) p *= x;
      return Num(p);
    }
    case 7: return n > 1 ? Num(std::sqrt(var(true))) : Er(Err::Div0);
    case 8: return n > 0 ? Num(std::sqrt(var(false))) : Er(Err::Div0);
    case 9: return Num(ExcelSum(v));
    case 10: return n > 1 ? Num(var(true)) : Er(Err::Div0);
    case 11: return n > 0 ? Num(var(false)) : Er(Err::Div0);
    case 12: {
      if (!n) return Er(Err::Num);
      std::sort(v.begin(), v.end());
      return Num(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2);
    }
    default: return Er(Err::Value);
  }
}

EV FSubtotal(Evaluator& ev, const Node& call) {
  YC_NUM(f, 0);
  int fn_num = static_cast<int>(f);
  const bool skip = fn_num > 100;
  if (skip) fn_num -= 100;
  if (fn_num < 1 || fn_num > 11) return Er(Err::Value);
  // like Excel: rows filtered out are never counted, whatever the function number
  AggOpts o;
  o.skip_hidden = skip;
  o.skip_filtered = true;
  return Aggregate(ev, fn_num, call, 1, o);
}
EV FAggregate(Evaluator& ev, const Node& call) {
  YC_NUM(f, 0);
  YC_NUM(opt, 1);
  const int fn_num = static_cast<int>(f);
  const int op = static_cast<int>(opt);
  if (fn_num < 1 || fn_num > 19 || op < 0 || op > 7) return Er(Err::Value);
  // Excel's options: 1 / 3 / 5 / 7 hidden rows, 2 / 3 / 6 / 7 error values, 0-3 nested SUBTOTAL and AGGREGATE
  AggOpts o;
  o.skip_hidden = op == 1 || op == 3 || op == 5 || op == 7;
  o.skip_errors = op == 2 || op == 3 || op == 6 || op == 7;
  o.skip_nested = op <= 3;
  o.with_k = fn_num >= 14;
  if (o.with_k && call.kids.size() < 4) return Er(Err::Value);  // k is missing
  return Aggregate(ev, fn_num, call, 2, o);
}

}  // namespace

void RegisterMath(std::vector<FuncInfo>& t) {
  t.push_back({"SUM", 1, 255, FSum});
  t.push_back({"SUMIF", 2, 3, FSumIf});
  t.push_back({"SUMIFS", 3, 255, FSumIfs});
  t.push_back({"SUMPRODUCT", 1, 255, FSumProduct});
  t.push_back({"SUMSQ", 1, 255, FSumSq});
  t.push_back({"PRODUCT", 1, 255, FProduct});
  t.push_back({"ABS", 1, 1, FAbs});
  t.push_back({"SIGN", 1, 1, Unary1<Sign>});
  t.push_back({"INT", 1, 1, Unary1<IntF>});
  t.push_back({"TRUNC", 1, 2, FTrunc});
  t.push_back({"ROUND", 2, 2, FRound});
  t.push_back({"ROUNDUP", 2, 2, FRoundUp});
  t.push_back({"ROUNDDOWN", 2, 2, FRoundDown});
  t.push_back({"MROUND", 2, 2, FMRound});
  t.push_back({"MOD", 2, 2, FMod});
  t.push_back({"QUOTIENT", 2, 2, FQuotient});
  t.push_back({"SQRT", 1, 1, FSqrt});
  t.push_back({"POWER", 2, 2, FPower});
  t.push_back({"EXP", 1, 1, FExp});
  t.push_back({"LN", 1, 1, FLn});
  t.push_back({"LOG", 1, 2, FLog});
  t.push_back({"LOG10", 1, 1, FLog10});
  t.push_back({"PI", 0, 0, FPi});
  t.push_back({"RAND", 0, 0, FRand, true});
  t.push_back({"RANDBETWEEN", 2, 2, FRandBetween, true});
  t.push_back({"CEILING", 1, 2, FCeiling});
  t.push_back({"FLOOR", 1, 2, FFloor});
  t.push_back({"CEILING.MATH", 1, 3, FCeilingMath});
  t.push_back({"FLOOR.MATH", 1, 3, FFloorMath});
  t.push_back({"EVEN", 1, 1, FEven});
  t.push_back({"ODD", 1, 1, FOdd});
  t.push_back({"GCD", 1, 255, FGcd});
  t.push_back({"LCM", 1, 255, FLcm});
  t.push_back({"FACT", 1, 1, FFact});
  t.push_back({"COMBIN", 2, 2, FCombin});
  t.push_back({"SIN", 1, 1, Unary1<TrigSin>});
  t.push_back({"COS", 1, 1, Unary1<TrigCos>});
  t.push_back({"TAN", 1, 1, Unary1<TrigTan>});
  t.push_back({"ASIN", 1, 1, FAsin});
  t.push_back({"ACOS", 1, 1, FAcos});
  t.push_back({"ATAN", 1, 1, Unary1<TrigAtan>});
  t.push_back({"ATAN2", 2, 2, FAtan2});
  t.push_back({"DEGREES", 1, 1, Unary1<Deg>});
  t.push_back({"RADIANS", 1, 1, Unary1<Rad>});
  t.push_back({"SUBTOTAL", 2, 255, FSubtotal});
  t.push_back({"AGGREGATE", 3, 255, FAggregate});
}

}  // namespace yc::formula::fn
