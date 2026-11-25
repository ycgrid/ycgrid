// SPDX-License-Identifier: Apache-2.0
// fn_stat.cpp - statistical functions
#include "formula/fn_util.h"

#include <algorithm>
#include <map>
#include <numeric>

namespace yc::formula::fn {
namespace {

bool Nums(Evaluator& ev, const Node& call, std::vector<double>* v, Value* err, int mode = kNumsPlain) {
  return CollectNumbers(ev, call, v, err, 0, mode);
}

double Mean(const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size()); }
double Var(const std::vector<double>& v, bool sample) {
  const double m = Mean(v);
  double s = 0;
  for (double x : v) s += (x - m) * (x - m);
  return s / static_cast<double>(sample ? v.size() - 1 : v.size());
}

EV FAverage(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Nums(ev, call, &v, &err)) return EV::S(err);
  if (v.empty()) return Er(Err::Div0);
  return Num(ExcelSum(v) / static_cast<double>(v.size()));  // AVERAGE(0.1,0.2,-0.3) is 0, as SUM (Excel)
}
EV FAverageA(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Nums(ev, call, &v, &err, kNumsAverageA)) return EV::S(err);
  if (v.empty()) return Er(Err::Div0);
  return Num(ExcelSum(v) / static_cast<double>(v.size()));
}

EV FCount(Evaluator& ev, const Node& call) {
  size_t n = 0;
  for (const Node& k : call.kids) {
    const EV e = BlankArgAsZero(ev.Eval(k));  // COUNT(,,) is 3 and COUNT(IF(TRUE,,1)) 1 (Excel 16, measured)
    if (e.k == EV::K::Scalar) {
      double d;
      Err er;
      if (e.s.IsNumeric() || (!e.s.IsError() && !e.s.IsEmpty() && Evaluator::ToNumber(e.s, &d, &er) && !e.s.IsString())) ++n;
      else if (e.s.IsString() && Evaluator::ToNumber(e.s, &d, &er)) ++n;
      continue;
    }
    ev.ForEachValue(e, [&](const Value& v) {
      if (v.IsNumeric()) ++n;
      return true;
    });
  }
  return Num(static_cast<double>(n));
}

EV FCountA(Evaluator& ev, const Node& call) {
  size_t n = 0;
  for (const Node& k : call.kids) {
    const EV e = ev.Eval(k);
    if (e.k == EV::K::Scalar) {
      // a value given directly counts, a blank too: COUNTA(,) is 2 and COUNTA(A8,IF(TRUE,,),"") 2 with A8 empty
      // (Excel 16, measured); only an empty cell of a reference isn't counted
      ++n;
      continue;
    }
    if (e.k == EV::K::Array) {
      // every element of an array counts, an empty one too: COUNTA(UNIQUE(R1:R3)) and COUNTA(FILTER(R1:R3,...)) are
      // 3 with R2 empty, where COUNTA(R1:R3) is 2 (Excel 16, measured)
      n += e.a->v.size();
      continue;
    }
    ev.ForEachValue(e, [&](const Value& v) {
      if (!v.IsEmpty()) ++n;
      return true;
    });
  }
  return Num(static_cast<double>(n));
}

EV FCountBlank(Evaluator& ev, const Node& call) {
  const EV e = ev.Eval(call.kids[0]);
  if (e.k != EV::K::Range) return Er(Err::Value);
  const core::Rect r = ev.ClampRect(e.r.sheet, e.r.rect);
  size_t nonblank = 0;
  ev.host().ForEachCell(e.r.sheet, r, [&](uint32_t, uint32_t, const Value& v) {
    if (!(v.IsString() && v.str().empty())) ++nonblank;
    return true;
  });
  return Num(static_cast<double>(r.Count() - nonblank));
}

EV FCountIf(Evaluator& ev, const Node& call) {
  const EV range = ev.Eval(call.kids[0]);
  const Criteria cr = ParseCriteria(ev.Scalar(call.kids[1]));
  size_t n = 0;
  // when the criterion doesn't match blanks only non-blank cells need visiting
  const bool needs_blank = cr.Match(Value());
  if (!needs_blank && range.k == EV::K::Range) {
    ev.host().ForEachCell(range.r.sheet, ev.ClampRect(range.r.sheet, range.r.rect), [&](uint32_t, uint32_t, const Value& v) {
      if (cr.Match(v)) ++n;
      return true;
    });
    return Num(static_cast<double>(n));
  }
  auto a = ev.ToArray(range);
  if (!a) return Er(Err::Num);
  for (const Value& v : a->v) {
    if (cr.Match(v)) ++n;
  }
  return Num(static_cast<double>(n));
}

bool IfsMask(Evaluator& ev, const Node& call, int first, std::vector<char>* ok, Value* err) {
  size_t n = 0;
  for (int i = first; i + 1 < Argc(call); i += 2) {
    auto a = ev.ToArray(ev.Eval(call.kids[static_cast<size_t>(i)]));
    if (!a) {
      *err = Value::Error(Err::Num);
      return false;
    }
    const Criteria cr = ParseCriteria(ev.Scalar(call.kids[static_cast<size_t>(i + 1)]));
    if (ok->empty()) {
      ok->assign(a->v.size(), 1);
      n = a->v.size();
    } else if (a->v.size() != n) {
      *err = Value::Error(Err::Value);
      return false;
    }
    for (size_t k = 0; k < n; ++k) {
      if ((*ok)[k] && !cr.Match(a->v[k])) (*ok)[k] = 0;
    }
  }
  return true;
}

EV FCountIfs(Evaluator& ev, const Node& call) {
  std::vector<char> ok;
  Value err;
  if (!IfsMask(ev, call, 0, &ok, &err)) return EV::S(err);
  return Num(static_cast<double>(std::count(ok.begin(), ok.end(), 1)));
}

// values by mask: kind 0 average, 1 max, 2 min
EV IfsAgg(Evaluator& ev, const Node& call, int kind, bool ifs) {
  std::vector<char> ok;
  Value err;
  std::shared_ptr<Array> vals;
  if (ifs) {
    if (!IfsMask(ev, call, 1, &ok, &err)) return EV::S(err);
    vals = ev.ToArray(ev.Eval(call.kids[0]));
  } else {
    // AVERAGEIF(range, criteria, [average_range])
    auto a = ev.ToArray(ev.Eval(call.kids[0]));
    if (!a) return Er(Err::Num);
    const Criteria cr = ParseCriteria(ev.Scalar(call.kids[1]));
    ok.resize(a->v.size());
    for (size_t i = 0; i < a->v.size(); ++i) ok[i] = cr.Match(a->v[i]) ? 1 : 0;
    vals = Has(call, 2) ? ev.ToArray(ev.Eval(call.kids[2])) : a;
  }
  if (!vals || vals->v.size() < ok.size()) return Er(Err::Value);
  double sum = 0, best = 0;
  size_t n = 0;
  for (size_t i = 0; i < ok.size(); ++i) {
    if (!ok[i]) continue;
    const Value& v = vals->v[i];
    if (v.IsError()) return EV::S(v);
    if (!v.IsNumeric()) continue;
    const double x = v.AsDouble();
    if (n == 0) best = x;
    else if (kind == 1) best = std::max(best, x);
    else if (kind == 2) best = std::min(best, x);
    sum += x;
    ++n;
  }
  if (kind == 0) return n ? Num(sum / static_cast<double>(n)) : Er(Err::Div0);
  return Num(n ? best : 0);
}
EV FAverageIf(Evaluator& ev, const Node& call) { return IfsAgg(ev, call, 0, false); }
EV FAverageIfs(Evaluator& ev, const Node& call) { return IfsAgg(ev, call, 0, true); }
EV FMaxIfs(Evaluator& ev, const Node& call) { return IfsAgg(ev, call, 1, true); }
EV FMinIfs(Evaluator& ev, const Node& call) { return IfsAgg(ev, call, 2, true); }

EV MaxMin(Evaluator& ev, const Node& call, bool is_max, bool a_variant) {
  // MAX must keep exact Decimal values too
  Value best;
  bool any = false;
  bool failed = false;
  Value err;
  auto consider = [&](const Value& v) {
    if (!any) {
      best = v;
      any = true;
      return;
    }
    bool ok;
    const int c = Evaluator::Compare(v, best, &ok);
    if (is_max ? c > 0 : c < 0) best = v;
  };
  for (const Node& k : call.kids) {
    const EV e = BlankArgAsZero(ev.Eval(k));  // MIN(,5) is 0 and MAX(-5,IF(TRUE,,1)) 0 (Excel 16, measured)
    if (e.k == EV::K::Scalar) {
      if (e.s.IsError()) return EV::S(e.s);
      if (e.s.IsNumeric()) {
        consider(e.s);
        continue;
      }
      double d;
      Err er = Err::None;
      if (!Evaluator::ToNumber(e.s, &d, &er)) return Er(er);
      consider(Value::Number(d));
      continue;
    }
    // MAXA / MINA read logicals and text in references only: MAXA({-1,TRUE}) and MAXA({-1,"a"}) are -1 (kNumsA)
    const bool in_ref = a_variant && e.k == EV::K::Range;
    ev.ForEachValue(e, [&](const Value& v) {
      if (v.IsError()) {
        err = v;
        failed = true;
        return false;
      }
      if (v.IsNumeric()) consider(v);
      else if (in_ref && v.IsBool()) consider(Value::Number(v.boolean() ? 1 : 0));
      else if (in_ref && v.IsString()) consider(Value::Number(0));
      return true;
    });
    if (failed) return EV::S(err);
  }
  return any ? EV::S(best) : Num(0);
}
EV FMax(Evaluator& ev, const Node& call) { return MaxMin(ev, call, true, false); }
EV FMin(Evaluator& ev, const Node& call) { return MaxMin(ev, call, false, false); }
EV FMaxA(Evaluator& ev, const Node& call) { return MaxMin(ev, call, true, true); }
EV FMinA(Evaluator& ev, const Node& call) { return MaxMin(ev, call, false, true); }

EV FMedian(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Nums(ev, call, &v, &err)) return EV::S(err);
  if (v.empty()) return Er(Err::Num);
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return Num(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2);
}

EV FMode(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!CollectModeNumbers(ev, call, &v, &err)) return EV::S(err);
  // several values equally frequent: the one that appears first in the data (Excel), not the one whose count reaches
  // the maximum first (MODE(2,1,1,2) is 2)
  std::map<double, int> cnt;
  int bc = 1;
  for (double x : v) bc = std::max(bc, ++cnt[x]);
  if (bc < 2) return Er(Err::NA);
  for (double x : v) {
    if (cnt[x] == bc) return Num(x);
  }
  return Er(Err::NA);
}

EV LargeSmall(Evaluator& ev, const Node& call, bool large) {
  std::vector<double> v;
  Value err;
  ev.ForEachValue(BlankArgAsZero(ev.Eval(call.kids[0])), [&](const Value& x) {
    if (x.IsError()) {
      err = x;
      return false;
    }
    if (x.IsNumeric()) v.push_back(x.AsDouble());
    return true;
  });
  if (err.IsError()) return EV::S(err);
  YC_NUM(kf, 1);
  // k from 1 to the count, as Excel: SMALL takes a fraction down (1.9 is the smallest), LARGE up (1.1 is the second
  // largest); 0.5 and 10.5 of ten values are #NUM! in both
  if (!(kf >= 1 && kf <= static_cast<double>(v.size()))) return Er(Err::Num);
  const size_t k = static_cast<size_t>(large ? std::ceil(kf) : std::floor(kf));
  std::sort(v.begin(), v.end());
  return Num(large ? v[v.size() - k] : v[k - 1]);
}
EV FLarge(Evaluator& ev, const Node& call) { return LargeSmall(ev, call, true); }
EV FSmall(Evaluator& ev, const Node& call) { return LargeSmall(ev, call, false); }

EV RankImpl(Evaluator& ev, const Node& call, bool avg) {
  YC_NUM(x, 0);
  std::vector<double> v;
  Value err;  // an error among the numbers is the answer (RANK(1, A1:A10) with #N/A in A10 is #N/A: Excel 16, measured)
  ev.ForEachValue(ev.Eval(call.kids[1]), [&](const Value& y) {
    if (y.IsError()) {
      err = y;
      return false;
    }
    if (y.IsNumeric()) v.push_back(y.AsDouble());
    return true;
  });
  if (err.IsError()) return EV::S(err);
  YC_NUM_OPT(order, 2, 0);
  size_t better = 0, equal = 0;
  for (double y : v) {
    if (y == x) ++equal;
    else if (order == 0 ? y > x : y < x) ++better;
  }
  if (!equal) return Er(Err::NA);
  if (avg) return Num(static_cast<double>(better) + (static_cast<double>(equal) + 1) / 2.0);
  return Num(static_cast<double>(better + 1));
}
EV FRank(Evaluator& ev, const Node& call) { return RankImpl(ev, call, false); }
EV FRankAvg(Evaluator& ev, const Node& call) { return RankImpl(ev, call, true); }

EV StdVar(Evaluator& ev, const Node& call, bool sample, bool is_sqrt) {
  std::vector<double> v;
  Value err;
  if (!Nums(ev, call, &v, &err)) return EV::S(err);
  if (v.size() < (sample ? 2u : 1u)) return Er(Err::Div0);
  const double r = Var(v, sample);
  return Num(is_sqrt ? std::sqrt(r) : r);
}
EV FStdevS(Evaluator& ev, const Node& call) { return StdVar(ev, call, true, true); }
EV FStdevP(Evaluator& ev, const Node& call) { return StdVar(ev, call, false, true); }
EV FVarS(Evaluator& ev, const Node& call) { return StdVar(ev, call, true, false); }
EV FVarP(Evaluator& ev, const Node& call) { return StdVar(ev, call, false, false); }

double NormCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }
double NormPdf(double z) { return std::exp(-0.5 * z * z) / std::sqrt(2 * 3.14159265358979323846); }

// Acklam's inverse normal approximation and one Halley step. The step's error is taken where no digits cancel: the
// upper half as the lower half of 1 - p (exact above 0.5), the tail through erfc and the middle through erf against
// p - 0.5. Taken as NormCdf(x) - p it lost the digits near 0.5 and near 1: NORM.S.INV(1-1E-12) was 7.03448690268,
// Excel's 7.0344869100478356 (Excel 16 gives the inverse of the double it is handed, measured).
double NormInv(double p) {
  static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
  static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01};
  static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
  static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
  if (p > 0.5) return -NormInv(1 - p);
  const double sqrt2 = std::sqrt(2.0);
  double x;
  if (p < 0.02425) {
    const double q = std::sqrt(-2 * std::log(p));
    x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
  } else {
    const double q = p - 0.5, r = q * q;
    x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
  }
  const double e = p < 0.25 ? 0.5 * std::erfc(-x / sqrt2) - p : 0.5 * std::erf(x / sqrt2) - (p - 0.5);
  const double u = e * std::sqrt(2 * 3.14159265358979323846) * std::exp(x * x / 2);
  const double step = u / (1 + x * u / 2);
  return std::isfinite(step) ? x - step : x;  // exp(x * x / 2) is past a double's range below 1E-308
}

EV FNormDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(mu, 1);
  YC_NUM(sd, 2);
  // cumulative left empty is FALSE, the density, as for every other distribution: NORM.DIST(1,0,1,) and
  // NORM.S.DIST(1,) are 0.2419707245 (Excel 16, measured)
  YC_BOOL_OPT_BLANK0(cum, 3, true);
  if (sd <= 0) return Er(Err::Num);
  const double z = (x - mu) / sd;
  return Num(cum ? NormCdf(z) : NormPdf(z) / sd);
}
EV FNormSDist(Evaluator& ev, const Node& call) {
  YC_NUM(z, 0);
  YC_BOOL_OPT_BLANK0(cum, 1, true);
  return Num(cum ? NormCdf(z) : NormPdf(z));
}
EV FNormInv(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  YC_NUM(mu, 1);
  YC_NUM(sd, 2);
  if (p <= 0 || p >= 1 || sd <= 0) return Er(Err::Num);
  return Num(mu + sd * NormInv(p));
}
EV FNormSInv(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  if (p <= 0 || p >= 1) return Er(Err::Num);
  return Num(NormInv(p));
}
EV FStandardize(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(mu, 1);
  YC_NUM(sd, 2);
  if (sd <= 0) return Er(Err::Num);
  return Num((x - mu) / sd);
}

EV Percentile(Evaluator& ev, const Node& call, bool exc, double k_scale) {
  std::vector<double> v;
  ev.ForEachValue(BlankArgAsZero(ev.Eval(call.kids[0])), [&](const Value& x) {
    if (x.IsNumeric()) v.push_back(x.AsDouble());
    return true;
  });
  YC_NUM(k, 1);
  if (k_scale != 1) k = std::floor(k);  // QUARTILE takes a whole quart (3.5 is the third quartile, as in Excel)
  k *= k_scale;
  if (v.empty() || k < 0 || k > 1) return Er(Err::Num);
  std::sort(v.begin(), v.end());
  const double n = static_cast<double>(v.size());
  double pos;
  if (exc) {
    pos = k * (n + 1) - 1;
    if (pos < 0 || pos > n - 1) return Er(Err::Num);
  } else {
    pos = k * (n - 1);
  }
  const size_t lo = static_cast<size_t>(std::floor(pos));
  const double frac = pos - std::floor(pos);
  if (lo + 1 >= v.size()) return Num(v.back());
  return Num(v[lo] + frac * (v[lo + 1] - v[lo]));
}
EV FPercentileInc(Evaluator& ev, const Node& call) { return Percentile(ev, call, false, 1); }
EV FPercentileExc(Evaluator& ev, const Node& call) { return Percentile(ev, call, true, 1); }
EV FQuartileInc(Evaluator& ev, const Node& call) { return Percentile(ev, call, false, 0.25); }

bool Pairs(Evaluator& ev, const Node& call, std::vector<double>* xs, std::vector<double>* ys, Value* err) {
  auto a = ev.ToArray(ev.Eval(call.kids[0]));
  auto b = ev.ToArray(ev.Eval(call.kids[1]));
  if (!a || !b || a->v.size() != b->v.size()) {
    *err = Value::Error(Err::NA);
    return false;
  }
  for (size_t i = 0; i < a->v.size(); ++i) {
    if (a->v[i].IsNumeric() && b->v[i].IsNumeric()) {
      xs->push_back(a->v[i].AsDouble());
      ys->push_back(b->v[i].AsDouble());
    }
  }
  return true;
}

EV FCorrel(Evaluator& ev, const Node& call) {
  std::vector<double> x, y;
  Value err;
  if (!Pairs(ev, call, &x, &y, &err)) return EV::S(err);
  if (x.size() < 2) return Er(Err::Div0);
  const double mx = Mean(x), my = Mean(y);
  double sxy = 0, sxx = 0, syy = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    sxy += (x[i] - mx) * (y[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
    syy += (y[i] - my) * (y[i] - my);
  }
  if (sxx == 0 || syy == 0) return Er(Err::Div0);
  return Num(sxy / std::sqrt(sxx * syy));
}
EV FCovarS(Evaluator& ev, const Node& call) {
  std::vector<double> x, y;
  Value err;
  if (!Pairs(ev, call, &x, &y, &err)) return EV::S(err);
  if (x.size() < 2) return Er(Err::Div0);
  const double mx = Mean(x), my = Mean(y);
  double s = 0;
  for (size_t i = 0; i < x.size(); ++i) s += (x[i] - mx) * (y[i] - my);
  return Num(s / static_cast<double>(x.size() - 1));
}
// SLOPE(known_y, known_x)
EV FSlope(Evaluator& ev, const Node& call) {
  std::vector<double> y, x;
  Value err;
  if (!Pairs(ev, call, &y, &x, &err)) return EV::S(err);
  if (x.size() < 2) return Er(Err::Div0);
  const double mx = Mean(x), my = Mean(y);
  double sxy = 0, sxx = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    sxy += (x[i] - mx) * (y[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
  }
  if (sxx == 0) return Er(Err::Div0);
  return Num(sxy / sxx);
}
EV FIntercept(Evaluator& ev, const Node& call) {
  std::vector<double> y, x;
  Value err;
  if (!Pairs(ev, call, &y, &x, &err)) return EV::S(err);
  if (x.size() < 2) return Er(Err::Div0);
  const double mx = Mean(x), my = Mean(y);
  double sxy = 0, sxx = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    sxy += (x[i] - mx) * (y[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
  }
  if (sxx == 0) return Er(Err::Div0);
  return Num(my - sxy / sxx * mx);
}
// FORECAST.LINEAR(x, known_y, known_x)
EV FForecast(Evaluator& ev, const Node& call) {
  YC_NUM(x0, 0);
  auto ay = ev.ToArray(ev.Eval(call.kids[1]));
  auto ax = ev.ToArray(ev.Eval(call.kids[2]));
  if (!ay || !ax || ay->v.size() != ax->v.size()) return Er(Err::NA);
  std::vector<double> x, y;
  for (size_t i = 0; i < ax->v.size(); ++i) {
    if (ax->v[i].IsNumeric() && ay->v[i].IsNumeric()) {
      x.push_back(ax->v[i].AsDouble());
      y.push_back(ay->v[i].AsDouble());
    }
  }
  if (x.size() < 2) return Er(Err::Div0);
  const double mx = Mean(x), my = Mean(y);
  double sxy = 0, sxx = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    sxy += (x[i] - mx) * (y[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
  }
  if (sxx == 0) return Er(Err::Div0);
  const double b = sxy / sxx;
  return Num(my - b * mx + b * x0);
}
EV FGeoMean(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Nums(ev, call, &v, &err)) return EV::S(err);
  if (v.empty()) return Er(Err::Num);
  double s = 0;
  for (double x : v) {
    if (x <= 0) return Er(Err::Num);
    s += std::log(x);
  }
  return Num(std::exp(s / static_cast<double>(v.size())));
}

}  // namespace

// shared with fn_dist.cpp
double StdNormCdf(double z) { return NormCdf(z); }
double StdNormPdf(double z) { return NormPdf(z); }
double StdNormInv(double p) { return NormInv(p); }
bool CollectPairs(Evaluator& ev, const Node& call, std::vector<double>* xs, std::vector<double>* ys, Value* err) { return Pairs(ev, call, xs, ys, err); }

void RegisterStat(std::vector<FuncInfo>& t) {
  t.push_back({"AVERAGE", 1, 255, FAverage});
  t.push_back({"AVERAGEA", 1, 255, FAverageA});
  t.push_back({"AVERAGEIF", 2, 3, FAverageIf});
  t.push_back({"AVERAGEIFS", 3, 255, FAverageIfs});
  t.push_back({"COUNT", 1, 255, FCount});
  t.push_back({"COUNTA", 1, 255, FCountA});
  t.push_back({"COUNTBLANK", 1, 1, FCountBlank});
  t.push_back({"COUNTIF", 2, 2, FCountIf});
  t.push_back({"COUNTIFS", 2, 254, FCountIfs});
  t.push_back({"MAX", 1, 255, FMax});
  t.push_back({"MIN", 1, 255, FMin});
  t.push_back({"MAXA", 1, 255, FMaxA});
  t.push_back({"MINA", 1, 255, FMinA});
  t.push_back({"MAXIFS", 3, 255, FMaxIfs});
  t.push_back({"MINIFS", 3, 255, FMinIfs});
  t.push_back({"MEDIAN", 1, 255, FMedian});
  t.push_back({"MODE", 1, 255, FMode});
  t.push_back({"MODE.SNGL", 1, 255, FMode});
  t.push_back({"LARGE", 2, 2, FLarge});
  t.push_back({"SMALL", 2, 2, FSmall});
  t.push_back({"RANK", 2, 3, FRank});
  t.push_back({"RANK.EQ", 2, 3, FRank});
  t.push_back({"RANK.AVG", 2, 3, FRankAvg});
  t.push_back({"STDEV", 1, 255, FStdevS});
  t.push_back({"STDEV.S", 1, 255, FStdevS});
  t.push_back({"STDEVP", 1, 255, FStdevP});
  t.push_back({"STDEV.P", 1, 255, FStdevP});
  t.push_back({"VAR", 1, 255, FVarS});
  t.push_back({"VAR.S", 1, 255, FVarS});
  t.push_back({"VARP", 1, 255, FVarP});
  t.push_back({"VAR.P", 1, 255, FVarP});
  t.push_back({"NORM.DIST", 4, 4, FNormDist});
  t.push_back({"NORMDIST", 4, 4, FNormDist});
  t.push_back({"NORM.S.DIST", 2, 2, FNormSDist});
  t.push_back({"NORMSDIST", 1, 1, FNormSDist});
  t.push_back({"NORM.INV", 3, 3, FNormInv});
  t.push_back({"NORMINV", 3, 3, FNormInv});
  t.push_back({"NORM.S.INV", 1, 1, FNormSInv});
  t.push_back({"NORMSINV", 1, 1, FNormSInv});
  t.push_back({"STANDARDIZE", 3, 3, FStandardize});
  t.push_back({"PERCENTILE", 2, 2, FPercentileInc});
  t.push_back({"PERCENTILE.INC", 2, 2, FPercentileInc});
  t.push_back({"PERCENTILE.EXC", 2, 2, FPercentileExc});
  t.push_back({"QUARTILE", 2, 2, FQuartileInc});
  t.push_back({"QUARTILE.INC", 2, 2, FQuartileInc});
  t.push_back({"CORREL", 2, 2, FCorrel});
  t.push_back({"COVARIANCE.S", 2, 2, FCovarS});
  t.push_back({"SLOPE", 2, 2, FSlope});
  t.push_back({"INTERCEPT", 2, 2, FIntercept});
  t.push_back({"FORECAST", 3, 3, FForecast});
  t.push_back({"FORECAST.LINEAR", 3, 3, FForecast});
  t.push_back({"GEOMEAN", 1, 255, FGeoMean});
}

}  // namespace yc::formula::fn
