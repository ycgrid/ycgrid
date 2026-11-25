// SPDX-License-Identifier: Apache-2.0
// fn_dist.cpp - probability distributions, more descriptive statistics, hyperbolic functions (PLAN §6.5)
//
// The special functions (regularized incomplete beta / gamma) follow the classic continued-fraction / series forms;
// inverses are found by bisection on the CDF. Arguments that Excel truncates to integers (degrees of freedom, trials)
// are truncated here too; out-of-domain arguments return #NUM!.
#include "formula/fn_util.h"
#include "formula/stats_api.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace yc::formula::fn {
namespace {

constexpr double kPi = 3.14159265358979323846;

EV Result(double v) { return std::isfinite(v) ? Num(v) : Er(Err::Num); }

// ---- special functions ----

// continued fraction of the incomplete beta function (Lentz)
double BetaCf(double a, double b, double x) {
  const double tiny = 1e-300;
  double qab = a + b, qap = a + 1, qam = a - 1, c = 1, d = 1 - qab * x / qap;
  if (std::fabs(d) < tiny) d = tiny;
  d = 1 / d;
  double h = d;
  for (int m = 1; m <= 300; ++m) {
    const int m2 = 2 * m;
    double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
    d = 1 + aa * d;
    if (std::fabs(d) < tiny) d = tiny;
    c = 1 + aa / c;
    if (std::fabs(c) < tiny) c = tiny;
    d = 1 / d;
    h *= d * c;
    aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
    d = 1 + aa * d;
    if (std::fabs(d) < tiny) d = tiny;
    c = 1 + aa / c;
    if (std::fabs(c) < tiny) c = tiny;
    d = 1 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1) < 1e-15) break;
  }
  return h;
}

// regularized incomplete beta I_x(a, b)
double BetaReg(double a, double b, double x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  const double lbt = std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) + a * std::log(x) + b * std::log(1 - x);
  const double bt = std::exp(lbt);
  if (x < (a + 1) / (a + b + 2)) return bt * BetaCf(a, b, x) / a;
  return 1 - bt * BetaCf(b, a, 1 - x) / b;
}

// regularized incomplete gamma: the lower P(a, x) (upper = false) or the upper Q(a, x) = 1 - P (upper = true), each
// computed directly where it is small so that a far tail keeps its digits (CHISQ.DIST.RT(100,3) is 1.55E-21);
// the series and the continued fraction run until they converge (about 9 sqrt(a) terms near x = a)
double GammaPQ(double a, double x, bool upper) {
  if (x <= 0) return upper ? 1 : 0;
  const double gln = std::lgamma(a);
  const int cap = static_cast<int>(std::min(1e8, 1000 + 50 * std::sqrt(a)));
  if (x < a + 1) {  // series for P
    double ap = a, sum = 1 / a, del = sum;
    for (int n = 0; n < cap; ++n) {
      ap += 1;
      del *= x / ap;
      sum += del;
      if (std::fabs(del) < std::fabs(sum) * 1e-16) break;
    }
    const double p = sum * std::exp(-x + a * std::log(x) - gln);
    return upper ? 1 - p : p;
  }
  // continued fraction for Q
  const double tiny = 1e-300;
  double b = x + 1 - a, c = 1 / tiny, d = 1 / b, h = d;
  for (int i = 1; i <= cap; ++i) {
    const double an = -i * (i - a);
    b += 2;
    d = an * d + b;
    if (std::fabs(d) < tiny) d = tiny;
    c = b + an / c;
    if (std::fabs(c) < tiny) c = tiny;
    d = 1 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1) < 1e-16) break;
  }
  const double q = std::exp(-x + a * std::log(x) - gln) * h;
  return upper ? q : 1 - q;
}
double GammaP(double a, double x) { return GammaPQ(a, x, false); }
double GammaQ(double a, double x) { return GammaPQ(a, x, true); }

// x with cdf(x) = p on [lo, hi] (cdf increasing); grows hi when needed
double InvertCdf(const std::function<double(double)>& cdf, double p, double lo, double hi) {
  for (int i = 0; i < 200 && cdf(hi) < p; ++i) hi *= 2;
  for (int i = 0; i < 300; ++i) {
    const double mid = (lo + hi) / 2;
    if (cdf(mid) < p) lo = mid;
    else hi = mid;
    if (hi - lo <= 1e-15 * std::max(1.0, std::fabs(mid))) break;
  }
  return (lo + hi) / 2;
}

// x >= lo with tail(x) = p, where tail falls from tail(lo) toward 0 (a right tail), found to the last digits of x
// rather than of 1 - p, so that far tails keep their precision (T.INV(1E-10,3) is -2225.7692846830932)
double InvertTail(const std::function<double(double)>& tail, double p, double lo, double hi) {
  if (tail(lo) <= p) return lo;
  for (int i = 0; i < 1100 && tail(hi) > p; ++i) {
    lo = hi;
    hi *= 2;
  }
  for (int i = 0; i < 1100; ++i) {
    const double mid = lo + (hi - lo) / 2;
    if (mid <= lo || mid >= hi) break;
    if (tail(mid) > p) lo = mid;
    else hi = mid;
    if (hi - lo <= 2e-16 * hi) break;
  }
  return lo + (hi - lo) / 2;
}

// ---- distributions ----

double TCdf(double t, double df) {
  const double x = df / (df + t * t);
  const double tail = 0.5 * BetaReg(df / 2, 0.5, x);
  return t >= 0 ? 1 - tail : tail;
}
double TPdf(double t, double df) {
  return std::exp(std::lgamma((df + 1) / 2) - std::lgamma(df / 2)) / std::sqrt(df * kPi) * std::pow(1 + t * t / df, -(df + 1) / 2);
}
// P(T > t) for t >= 0, computed directly (not as 1 - cdf)
double TRightTail(double t, double df) { return 0.5 * BetaReg(df / 2, 0.5, df / (df + t * t)); }
// t with P(T > t) = q, q at most 0.5
double TTailInv(double q, double df) {
  return InvertTail([df](double t) { return TRightTail(t, df); }, q, 0, 10);
}
double TInv(double p, double df) {  // left-tailed
  if (p == 0.5) return 0;
  if (p < 0.5) return -TTailInv(p, df);
  return TTailInv(1 - p, df);
}
double ChiCdf(double x, double df) { return GammaP(df / 2, x / 2); }
double ChiTail(double x, double df) { return GammaQ(df / 2, x / 2); }
double ChiPdf(double x, double df) {
  if (x < 0) return 0;
  if (x == 0) return df == 2 ? 0.5 : (df < 2 ? INFINITY : 0);
  return std::exp((df / 2 - 1) * std::log(x) - x / 2 - (df / 2) * std::log(2.0) - std::lgamma(df / 2));
}
double FCdf(double x, double d1, double d2) { return x <= 0 ? 0 : BetaReg(d1 / 2, d2 / 2, d1 * x / (d1 * x + d2)); }
double FTailOf(double x, double d1, double d2) { return x <= 0 ? 1 : BetaReg(d2 / 2, d1 / 2, d2 / (d2 + d1 * x)); }
double FPdf(double x, double d1, double d2) {
  if (x <= 0) return 0;
  const double l = 0.5 * (d1 * std::log(d1 * x) + d2 * std::log(d2) - (d1 + d2) * std::log(d1 * x + d2)) - std::log(x) -
                   (std::lgamma(d1 / 2) + std::lgamma(d2 / 2) - std::lgamma((d1 + d2) / 2));
  return std::exp(l);
}
double GammaCdf(double x, double a, double b) { return GammaP(a, x / b); }
double GammaPdf(double x, double a, double b) {
  if (x < 0) return 0;
  if (x == 0) return a == 1 ? 1 / b : 0;
  return std::exp((a - 1) * std::log(x) - x / b - a * std::log(b) - std::lgamma(a));
}
double BinomPmf(double k, double n, double p) {
  if (p == 0) return k == 0 ? 1 : 0;
  if (p == 1) return k == n ? 1 : 0;
  return std::exp(std::lgamma(n + 1) - std::lgamma(k + 1) - std::lgamma(n - k + 1) + k * std::log(p) + (n - k) * std::log(1 - p));
}
double PoissonPmf(double k, double m) { return m == 0 ? (k == 0 ? 1 : 0) : std::exp(k * std::log(m) - m - std::lgamma(k + 1)); }

#define YC_CUM(var, i)                          \
  bool var = false;                             \
  {                                             \
    Value _e;                                   \
    if (!ev.ArgBool(call.kids[(i)], &var, &_e)) \
      return EV::S(_e);                         \
  }

EV FTDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(df_in, 1);
  YC_CUM(cum, 2);
  const double df = std::trunc(df_in);
  if (df < 1) return Er(Err::Num);
  return Result(cum ? TCdf(x, df) : TPdf(x, df));
}
EV FTDist2T(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(df_in, 1);
  const double df = std::trunc(df_in);
  if (df < 1 || x < 0) return Er(Err::Num);
  return Result(2 * TRightTail(x, df));
}
EV FTDistRT(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(df_in, 1);
  const double df = std::trunc(df_in);
  if (df < 1) return Er(Err::Num);
  return Result(x >= 0 ? TRightTail(x, df) : 1 - TRightTail(-x, df));
}
// TDIST(x, df, tails): legacy, x >= 0
EV FTDistLegacy(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(df_in, 1);
  YC_NUM(tails, 2);
  const double df = std::trunc(df_in);
  if (df < 1 || x < 0 || (tails != 1 && tails != 2)) return Er(Err::Num);
  return Result(tails * TRightTail(x, df));
}
EV FTInv(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  YC_NUM(df_in, 1);
  const double df = std::trunc(df_in);
  if (p <= 0 || p >= 1 || df < 1) return Er(Err::Num);
  return Result(TInv(p, df));
}
EV FTInv2T(Evaluator& ev, const Node& call) {  // also TINV
  YC_NUM(p, 0);
  YC_NUM(df_in, 1);
  const double df = std::trunc(df_in);
  if (p <= 0 || p > 1 || df < 1) return Er(Err::Num);
  return Result(TTailInv(p / 2, df));
}

EV FChiDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(df_in, 1);
  YC_CUM(cum, 2);
  const double df = std::trunc(df_in);
  if (x < 0 || df < 1) return Er(Err::Num);
  return Result(cum ? ChiCdf(x, df) : ChiPdf(x, df));
}
EV FChiDistRT(Evaluator& ev, const Node& call) {  // also CHIDIST
  YC_NUM(x, 0);
  YC_NUM(df_in, 1);
  const double df = std::trunc(df_in);
  if (x < 0 || df < 1) return Er(Err::Num);
  return Result(ChiTail(x, df));
}
EV FChiInv(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  YC_NUM(df_in, 1);
  const double df = std::trunc(df_in);
  if (p < 0 || p >= 1 || df < 1) return Er(Err::Num);
  return Result(InvertCdf([df](double x) { return ChiCdf(x, df); }, p, 0, std::max(1.0, df * 2)));
}
EV FChiInvRT(Evaluator& ev, const Node& call) {  // also CHIINV
  YC_NUM(p, 0);
  YC_NUM(df_in, 1);
  const double df = std::trunc(df_in);
  if (p <= 0 || p > 1 || df < 1) return Er(Err::Num);
  return Result(InvertTail([df](double x) { return ChiTail(x, df); }, p, 0, std::max(1.0, df * 2)));
}

EV FFDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  YC_CUM(cum, 3);
  const double d1 = std::trunc(a), d2 = std::trunc(b);
  if (x < 0 || d1 < 1 || d2 < 1) return Er(Err::Num);
  return Result(cum ? FCdf(x, d1, d2) : FPdf(x, d1, d2));
}
EV FFDistRT(Evaluator& ev, const Node& call) {  // also FDIST
  YC_NUM(x, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  const double d1 = std::trunc(a), d2 = std::trunc(b);
  if (x < 0 || d1 < 1 || d2 < 1) return Er(Err::Num);
  return Result(FTailOf(x, d1, d2));
}
EV FFInv(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  const double d1 = std::trunc(a), d2 = std::trunc(b);
  if (p < 0 || p >= 1 || d1 < 1 || d2 < 1) return Er(Err::Num);
  return Result(InvertCdf([=](double x) { return FCdf(x, d1, d2); }, p, 0, 10));
}
EV FFInvRT(Evaluator& ev, const Node& call) {  // also FINV
  YC_NUM(p, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  const double d1 = std::trunc(a), d2 = std::trunc(b);
  if (p <= 0 || p > 1 || d1 < 1 || d2 < 1) return Er(Err::Num);
  return Result(InvertTail([=](double x) { return FTailOf(x, d1, d2); }, p, 0, 10));
}

EV FGammaDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  YC_CUM(cum, 3);
  if (x < 0 || a <= 0 || b <= 0) return Er(Err::Num);
  return Result(cum ? GammaCdf(x, a, b) : GammaPdf(x, a, b));
}
EV FGammaInv(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  if (p < 0 || p >= 1 || a <= 0 || b <= 0) return Er(Err::Num);
  return Result(InvertCdf([=](double x) { return GammaCdf(x, a, b); }, p, 0, std::max(1.0, a * b * 2)));
}
EV FGammaLn(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x <= 0) return Er(Err::Num);
  return Result(std::lgamma(x));
}
EV FGamma(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x == 0 || (x < 0 && x == std::floor(x))) return Er(Err::Num);
  return Result(std::tgamma(x));
}

EV FBinomDist(Evaluator& ev, const Node& call) {
  YC_NUM(k_in, 0);
  YC_NUM(n_in, 1);
  YC_NUM(p, 2);
  YC_CUM(cum, 3);
  const double k = std::trunc(k_in), n = std::trunc(n_in);
  if (k < 0 || k > n || p < 0 || p > 1) return Er(Err::Num);
  if (!cum) return Result(BinomPmf(k, n, p));
  double s = 0;
  for (double i = 0; i <= k; ++i) s += BinomPmf(i, n, p);
  return Result(std::min(1.0, s));
}
EV FPoissonDist(Evaluator& ev, const Node& call) {
  YC_NUM(x_in, 0);
  YC_NUM(m, 1);
  YC_CUM(cum, 2);
  const double x = std::trunc(x_in);
  if (x < 0 || m < 0) return Er(Err::Num);
  if (!cum) return Result(PoissonPmf(x, m));
  return Result(GammaQ(x + 1, m));  // P(X <= x) = Q(x + 1, m)
}
EV FExponDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(l, 1);
  YC_CUM(cum, 2);
  if (x < 0 || l <= 0) return Er(Err::Num);
  return Result(cum ? 1 - std::exp(-l * x) : l * std::exp(-l * x));
}
EV FWeibullDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  YC_CUM(cum, 3);
  if (x < 0 || a <= 0 || b <= 0) return Er(Err::Num);
  const double z = std::pow(x / b, a);
  return Result(cum ? 1 - std::exp(-z) : a / std::pow(b, a) * std::pow(x, a - 1) * std::exp(-z));
}
EV FLognormDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(m, 1);
  YC_NUM(s, 2);
  YC_BOOL_OPT_BLANK0(cum, 3, true);  // LOGNORM.DIST(1,0,1,) is the density (Excel 16, measured)
  if (x <= 0 || s <= 0) return Er(Err::Num);
  const double z = (std::log(x) - m) / s;
  return Result(cum ? StdNormCdf(z) : StdNormPdf(z) / (x * s));
}
EV FLognormInv(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  YC_NUM(m, 1);
  YC_NUM(s, 2);
  if (p <= 0 || p >= 1 || s <= 0) return Er(Err::Num);
  return Result(std::exp(m + s * StdNormInv(p)));
}
EV FConfidenceNorm(Evaluator& ev, const Node& call) {
  YC_NUM(alpha, 0);
  YC_NUM(sd, 1);
  YC_NUM(n_in, 2);
  const double n = std::trunc(n_in);
  if (alpha <= 0 || alpha >= 1 || sd <= 0 || n < 1) return Er(Err::Num);
  return Result(StdNormInv(1 - alpha / 2) * sd / std::sqrt(n));
}
EV FConfidenceT(Evaluator& ev, const Node& call) {
  YC_NUM(alpha, 0);
  YC_NUM(sd, 1);
  YC_NUM(n_in, 2);
  const double n = std::trunc(n_in);
  if (alpha <= 0 || alpha >= 1 || sd <= 0 || n < 1) return Er(Err::Num);
  if (n == 1) return Er(Err::Div0);
  return Result(TInv(1 - alpha / 2, n - 1) * sd / std::sqrt(n));
}

// ---- descriptive statistics ----

bool Values(Evaluator& ev, const Node& call, std::vector<double>* v, Value* err, int first = 0) {
  return CollectNumbers(ev, call, v, err, first);
}
double MeanOf(const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size()); }

EV FDevSq(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Values(ev, call, &v, &err)) return EV::S(err);
  if (v.empty()) return Er(Err::Num);
  const double m = MeanOf(v);
  double s = 0;
  for (double x : v) s += (x - m) * (x - m);
  return Result(s);
}
EV FAveDev(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Values(ev, call, &v, &err)) return EV::S(err);
  if (v.empty()) return Er(Err::Num);
  const double m = MeanOf(v);
  double s = 0;
  for (double x : v) s += std::fabs(x - m);
  return Result(s / static_cast<double>(v.size()));
}
EV FSkew(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Values(ev, call, &v, &err)) return EV::S(err);
  const double n = static_cast<double>(v.size());
  if (n < 3) return Er(Err::Div0);
  const double m = MeanOf(v);
  double s2 = 0;
  for (double x : v) s2 += (x - m) * (x - m);
  const double sd = std::sqrt(s2 / (n - 1));
  if (sd == 0) return Er(Err::Div0);
  double s3 = 0;
  for (double x : v) s3 += std::pow((x - m) / sd, 3);
  return Result(n / ((n - 1) * (n - 2)) * s3);
}
EV FKurt(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Values(ev, call, &v, &err)) return EV::S(err);
  const double n = static_cast<double>(v.size());
  if (n < 4) return Er(Err::Div0);
  const double m = MeanOf(v);
  double s2 = 0;
  for (double x : v) s2 += (x - m) * (x - m);
  const double sd = std::sqrt(s2 / (n - 1));
  if (sd == 0) return Er(Err::Div0);
  double s4 = 0;
  for (double x : v) s4 += std::pow((x - m) / sd, 4);
  return Result(n * (n + 1) / ((n - 1) * (n - 2) * (n - 3)) * s4 - 3 * (n - 1) * (n - 1) / ((n - 2) * (n - 3)));
}
EV FHarMean(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Values(ev, call, &v, &err)) return EV::S(err);
  if (v.empty()) return Er(Err::Num);
  double s = 0;
  for (double x : v) {
    if (x <= 0) return Er(Err::Num);
    s += 1 / x;
  }
  return Result(static_cast<double>(v.size()) / s);
}
EV FTrimMean(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  {
    const EV data = BlankArgAsZero(ev.Eval(call.kids[0]));  // TRIMMEAN(,0.1) is 0 (Excel 16, measured)
    if (data.k == EV::K::Scalar && (data.s.IsBool() || data.s.IsString())) {
      // a logical or a number as text typed as the argument counts, as in SUM: TRIMMEAN(TRUE,0.1) is 1 and
      // TRIMMEAN("5",0.1) 5, while those in a range or an array are skipped (measured)
      double x;
      core::Err e;
      if (!Evaluator::ToNumber(data.s, &x, &e)) return Er(e);
      v.push_back(x);
    } else {
      const auto a = ev.ToArray(data);
      if (!a) return Er(Err::Value);
      for (const Value& x : a->v) {
        if (x.IsError()) return EV::S(x);
        if (x.IsNumeric()) v.push_back(x.AsDouble());
      }
    }
  }
  YC_NUM(pct, 1);
  if (pct < 0 || pct >= 1 || v.empty()) return Er(Err::Num);
  std::sort(v.begin(), v.end());
  // Excel rounds the excluded count down to a multiple of 2 and trims half from each end
  const size_t k = static_cast<size_t>(std::floor(static_cast<double>(v.size()) * pct / 2));
  if (2 * k >= v.size()) return Er(Err::Num);
  const double s = std::accumulate(v.begin() + static_cast<std::ptrdiff_t>(k), v.end() - static_cast<std::ptrdiff_t>(k), 0.0);
  return Result(s / static_cast<double>(v.size() - 2 * k));
}

// PERCENTRANK.INC / .EXC (array, x, [significance]): interpolates between neighbors; truncated to significance digits
EV PercentRank(Evaluator& ev, const Node& call, bool exc) {
  std::vector<double> v;
  {
    const auto a = ev.ToArray(ev.Eval(call.kids[0]));
    if (!a) return Er(Err::Value);
    for (const Value& x : a->v) {
      if (x.IsError()) return EV::S(x);
      if (x.IsNumeric()) v.push_back(x.AsDouble());
    }
  }
  YC_NUM(x, 1);
  YC_NUM_OPT(sig_in, 2, 3);
  const int sig = static_cast<int>(std::trunc(sig_in));
  if (v.empty() || sig < 1) return Er(Err::Num);
  std::sort(v.begin(), v.end());
  const double n = static_cast<double>(v.size());
  if (x < v.front() || x > v.back()) return Er(Err::NA);
  auto rank_at = [&](size_t i) {  // rank of the i-th smallest value
    return exc ? (static_cast<double>(i) + 1) / (n + 1) : (n == 1 ? 1.0 : static_cast<double>(i) / (n - 1));
  };
  double r = 0;
  const auto lo = std::lower_bound(v.begin(), v.end(), x);
  const size_t i = static_cast<size_t>(lo - v.begin());
  if (*lo == x) {
    r = rank_at(i);
  } else {  // between v[i-1] and v[i]
    const double f = (x - v[i - 1]) / (v[i] - v[i - 1]);
    r = rank_at(i - 1) + f * (rank_at(i) - rank_at(i - 1));
  }
  const double scale = std::pow(10.0, sig);
  return Result(std::floor(r * scale + 1e-9) / scale);
}
EV FPercentRankInc(Evaluator& ev, const Node& call) { return PercentRank(ev, call, false); }
EV FPercentRankExc(Evaluator& ev, const Node& call) { return PercentRank(ev, call, true); }

EV FCovarP(Evaluator& ev, const Node& call) {
  std::vector<double> x, y;
  Value err;
  if (!CollectPairs(ev, call, &x, &y, &err)) return EV::S(err);
  if (x.empty()) return Er(Err::Div0);
  const double mx = MeanOf(x), my = MeanOf(y);
  double s = 0;
  for (size_t i = 0; i < x.size(); ++i) s += (x[i] - mx) * (y[i] - my);
  return Result(s / static_cast<double>(x.size()));
}
// r of the pairs (PEARSON / RSQ)
bool Pearson(Evaluator& ev, const Node& call, double* r, Value* err) {
  std::vector<double> x, y;
  if (!CollectPairs(ev, call, &x, &y, err)) return false;
  if (x.size() < 2) {
    *err = Value::Error(Err::Div0);
    return false;
  }
  const double mx = MeanOf(x), my = MeanOf(y);
  double sxy = 0, sxx = 0, syy = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    sxy += (x[i] - mx) * (y[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
    syy += (y[i] - my) * (y[i] - my);
  }
  if (sxx == 0 || syy == 0) {
    *err = Value::Error(Err::Div0);
    return false;
  }
  *r = sxy / std::sqrt(sxx * syy);
  return true;
}
EV FPearson(Evaluator& ev, const Node& call) {
  double r = 0;
  Value err;
  if (!Pearson(ev, call, &r, &err)) return EV::S(err);
  return Result(r);
}
EV FRsq(Evaluator& ev, const Node& call) {
  double r = 0;
  Value err;
  if (!Pearson(ev, call, &r, &err)) return EV::S(err);
  return Result(r * r);
}
// STEYX(known_y, known_x)
EV FSteyx(Evaluator& ev, const Node& call) {
  std::vector<double> y, x;
  Value err;
  if (!CollectPairs(ev, call, &y, &x, &err)) return EV::S(err);
  const double n = static_cast<double>(x.size());
  if (n < 3) return Er(Err::Div0);
  const double mx = MeanOf(x), my = MeanOf(y);
  double sxy = 0, sxx = 0, syy = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    sxy += (x[i] - mx) * (y[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
    syy += (y[i] - my) * (y[i] - my);
  }
  if (sxx == 0) return Er(Err::Div0);
  return Result(std::sqrt((syy - sxy * sxy / sxx) / (n - 2)));
}

// ---- hyperbolic functions ----

template <double (*F)(double)>
EV Hyp(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  return Result(F(x));
}
double Sinh(double x) { return std::sinh(x); }
double Cosh(double x) { return std::cosh(x); }
double Tanh(double x) { return std::tanh(x); }
double Asinh(double x) { return std::asinh(x); }
EV FAcosh(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x < 1) return Er(Err::Num);
  return Result(std::acosh(x));
}
EV FAtanh(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x <= -1 || x >= 1) return Er(Err::Num);
  return Result(std::atanh(x));
}


// ---- more distributions and tests ----

// STDEVA, VARA...: every argument's numbers; in a reference text counts as 0 and a logical as 1 / 0 (kNumsA)
bool ValuesA(Evaluator& ev, const Node& call, std::vector<double>* out, Value* err) {
  return CollectNumbers(ev, call, out, err, 0, kNumsA);
}

EV VarA(Evaluator& ev, const Node& call, bool sample, bool root) {
  std::vector<double> v;
  Value err;
  if (!ValuesA(ev, call, &v, &err)) return EV::S(err);
  const size_t n = v.size();
  if (n < (sample ? 2u : 1u)) return Er(Err::Div0);
  const double m = MeanOf(v);
  double s = 0;
  for (double x : v) s += (x - m) * (x - m);
  s /= static_cast<double>(sample ? n - 1 : n);
  return Result(root ? std::sqrt(s) : s);
}
EV FStdevA(Evaluator& ev, const Node& call) { return VarA(ev, call, true, true); }
EV FStdevPA(Evaluator& ev, const Node& call) { return VarA(ev, call, false, true); }
EV FVarA(Evaluator& ev, const Node& call) { return VarA(ev, call, true, false); }
EV FVarPA(Evaluator& ev, const Node& call) { return VarA(ev, call, false, false); }

EV FSkewP(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!Values(ev, call, &v, &err)) return EV::S(err);
  const size_t n = v.size();
  if (n < 3) return Er(Err::Div0);  // SKEW.P(1,2) is #DIV/0!, as in Excel
  const double m = MeanOf(v);
  double s2 = 0, s3 = 0;
  for (double x : v) {
    s2 += (x - m) * (x - m);
    s3 += (x - m) * (x - m) * (x - m);
  }
  const double sd = std::sqrt(s2 / static_cast<double>(n));
  if (sd == 0) return Er(Err::Div0);
  return Result(s3 / static_cast<double>(n) / (sd * sd * sd));
}

double BetaPdf(double x, double a, double b) {
  if (x < 0 || x > 1) return 0;
  return std::exp((a - 1) * std::log(x) + (b - 1) * std::log(1 - x) - (std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b)));
}

EV FBetaDist(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  YC_CUM(cum, 3);
  YC_NUM_OPT(lo, 4, 0);
  YC_NUM_OPT(hi, 5, 1);
  if (a <= 0 || b <= 0 || lo >= hi || x < lo || x > hi) return Er(Err::Num);
  const double z = (x - lo) / (hi - lo);
  return Result(cum ? BetaReg(a, b, z) : BetaPdf(z, a, b) / (hi - lo));
}
EV FBetaDistLegacy(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  YC_NUM_OPT(lo, 3, 0);
  YC_NUM_OPT(hi, 4, 1);
  if (a <= 0 || b <= 0 || lo >= hi || x < lo || x > hi) return Er(Err::Num);
  return Result(BetaReg(a, b, (x - lo) / (hi - lo)));
}
EV FBetaInv(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  YC_NUM(a, 1);
  YC_NUM(b, 2);
  YC_NUM_OPT(lo, 3, 0);
  YC_NUM_OPT(hi, 4, 1);
  if (p <= 0 || p > 1 || a <= 0 || b <= 0 || lo >= hi) return Er(Err::Num);
  double l = 0, h = 1;
  for (int i = 0; i < 200; ++i) {
    const double mid = (l + h) / 2;
    if (BetaReg(a, b, mid) < p) l = mid;
    else h = mid;
  }
  return Result(lo + (hi - lo) * (l + h) / 2);
}

EV FBinomDistRange(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(p, 1);
  YC_NUM(s, 2);
  n = std::trunc(n);
  s = std::trunc(s);
  double s2 = s;
  if (Has(call, 3)) {
    YC_NUM(t, 3);
    s2 = std::trunc(t);
  }
  if (n < 0 || p < 0 || p > 1 || s < 0 || s > n || s2 < s || s2 > n) return Er(Err::Num);
  double total = 0;
  for (double k = s; k <= s2; ++k) total += BinomPmf(k, n, p);
  return Result(total);
}

EV FBinomInv(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(p, 1);
  YC_NUM(alpha, 2);
  n = std::trunc(n);
  if (n < 0 || p < 0 || p > 1 || alpha <= 0 || alpha >= 1) return Er(Err::Num);
  double cdf = 0;
  for (double k = 0; k <= n; ++k) {
    cdf += BinomPmf(k, n, p);
    if (cdf >= alpha - 1e-12) return Num(k);
  }
  return Num(n);
}

// two ranges as flat numeric vectors of the same shape; false: #N/A (shapes differ) or the first error
bool SameShape(Evaluator& ev, const Node& a, const Node& b, std::shared_ptr<Array>* x, std::shared_ptr<Array>* y, Value* err) {
  *x = ev.ToArray(ev.Eval(a));
  *y = ev.ToArray(ev.Eval(b));
  if (!*x || !*y) {
    *err = Value::Error(Err::Value);
    return false;
  }
  if ((*x)->rows != (*y)->rows || (*x)->cols != (*y)->cols) {
    *err = Value::Error(Err::NA);
    return false;
  }
  return true;
}

EV FChiTest(Evaluator& ev, const Node& call) {
  std::shared_ptr<Array> a, e;
  Value err;
  if (!SameShape(ev, call.kids[0], call.kids[1], &a, &e, &err)) return EV::S(err);
  double chi = 0;
  for (size_t i = 0; i < a->v.size(); ++i) {
    if (a->v[i].IsError()) return EV::S(a->v[i]);
    if (e->v[i].IsError()) return EV::S(e->v[i]);
    if (!a->v[i].IsNumeric() || !e->v[i].IsNumeric()) continue;
    const double ex = e->v[i].AsDouble();
    if (ex == 0) return Er(Err::Div0);
    const double d = a->v[i].AsDouble() - ex;
    chi += d * d / ex;
  }
  const double df = a->rows > 1 && a->cols > 1 ? static_cast<double>(a->rows - 1) * (a->cols - 1) : static_cast<double>(a->v.size()) - 1;
  if (df < 1) return Er(Err::NA);
  return Result(ChiTail(chi, df));
}

std::vector<double> NumsOf(const Array& a) {
  std::vector<double> v;
  for (const Value& x : a.v) {
    if (x.IsNumeric()) v.push_back(x.AsDouble());
  }
  return v;
}
double SampleVar(const std::vector<double>& v) {
  const double m = MeanOf(v);
  double s = 0;
  for (double x : v) s += (x - m) * (x - m);
  return s / static_cast<double>(v.size() - 1);
}

EV FFTest(Evaluator& ev, const Node& call) {
  const auto a = ev.ToArray(ev.Eval(call.kids[0]));
  const auto b = ev.ToArray(ev.Eval(call.kids[1]));
  if (!a || !b) return Er(Err::Value);
  const auto x = NumsOf(*a), y = NumsOf(*b);
  if (x.size() < 2 || y.size() < 2) return Er(Err::Div0);
  const double v1 = SampleVar(x), v2 = SampleVar(y);
  if (v1 == 0 || v2 == 0) return Er(Err::Div0);
  const double f = v1 / v2;
  const double c = FCdf(f, static_cast<double>(x.size() - 1), static_cast<double>(y.size() - 1));
  return Result(2 * std::min(c, 1 - c));
}

EV FTTest(Evaluator& ev, const Node& call) {
  YC_NUM(tails, 2);
  YC_NUM(type, 3);
  tails = std::trunc(tails);
  type = std::trunc(type);
  if ((tails != 1 && tails != 2) || type < 1 || type > 3) return Er(Err::Num);
  const auto a = ev.ToArray(ev.Eval(call.kids[0]));
  const auto b = ev.ToArray(ev.Eval(call.kids[1]));
  if (!a || !b) return Er(Err::Value);
  double t, df;
  if (type == 1) {
    if (a->v.size() != b->v.size()) return Er(Err::NA);
    std::vector<double> d;
    for (size_t i = 0; i < a->v.size(); ++i) {
      if (a->v[i].IsNumeric() && b->v[i].IsNumeric()) d.push_back(a->v[i].AsDouble() - b->v[i].AsDouble());
    }
    if (d.size() < 2) return Er(Err::Div0);
    const double m = MeanOf(d), s = std::sqrt(SampleVar(d));
    if (s == 0) return Er(Err::Div0);
    t = m / (s / std::sqrt(static_cast<double>(d.size())));
    df = static_cast<double>(d.size()) - 1;
  } else {
    const auto x = NumsOf(*a), y = NumsOf(*b);
    if (x.size() < 2 || y.size() < 2) return Er(Err::Div0);
    const double n1 = static_cast<double>(x.size()), n2 = static_cast<double>(y.size());
    const double v1 = SampleVar(x), v2 = SampleVar(y);
    if (type == 2) {
      const double sp = ((n1 - 1) * v1 + (n2 - 1) * v2) / (n1 + n2 - 2);
      t = (MeanOf(x) - MeanOf(y)) / std::sqrt(sp * (1 / n1 + 1 / n2));
      df = n1 + n2 - 2;
    } else {
      const double s1 = v1 / n1, s2 = v2 / n2;
      t = (MeanOf(x) - MeanOf(y)) / std::sqrt(s1 + s2);
      df = (s1 + s2) * (s1 + s2) / (s1 * s1 / (n1 - 1) + s2 * s2 / (n2 - 1));
    }
  }
  if (!std::isfinite(t)) return Er(Err::Div0);
  return Result(tails * TRightTail(std::fabs(t), df));
}

// Z.TEST as Excel 16 answers (measured): an error in the data comes back, the first one; data without a number is
// #N/A and with one number #DIV/0!, with a sigma or without; a sigma that is given, also one left empty (0), must be
// positive (#NUM!); without one the sample's own deviation is used, and a deviation of 0 is #DIV/0!. The upper tail is
// taken directly: 1 - cdf lost the digits of a small probability.
EV FZTest(Evaluator& ev, const Node& call) {
  const auto a = ev.ToArray(ev.Eval(call.kids[0]));
  if (!a) return Er(Err::Value);
  for (const Value& e : a->v) {
    if (e.IsError()) return EV::S(e);
  }
  YC_NUM(x, 1);
  const bool given = Argc(call) > 2;
  double sigma = 0;
  if (given) {
    YC_NUM(s, 2);
    sigma = s;
  }
  const auto v = NumsOf(*a);
  if (v.empty()) return Er(Err::NA);
  if (v.size() < 2) return Er(Err::Div0);
  if (given) {
    if (sigma <= 0) return Er(Err::Num);
  } else {
    sigma = std::sqrt(SampleVar(v));
    if (sigma <= 0) return Er(Err::Div0);
  }
  return Result(StdNormCdf((x - MeanOf(v)) / (sigma / std::sqrt(static_cast<double>(v.size())))));
}

EV FFisher(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  if (x <= -1 || x >= 1) return Er(Err::Num);
  return Result(0.5 * std::log((1 + x) / (1 - x)));
}
EV FFisherInv(Evaluator& ev, const Node& call) {
  YC_NUM(y, 0);
  return Result(std::tanh(y));
}
EV FGauss(Evaluator& ev, const Node& call) {
  YC_NUM(z, 0);
  return Result(StdNormCdf(z) - 0.5);
}
EV FPhi(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  return Result(StdNormPdf(x));
}

double LogComb(double n, double k) { return std::lgamma(n + 1) - std::lgamma(k + 1) - std::lgamma(n - k + 1); }

EV Hypgeom(Evaluator& ev, const Node& call, bool has_cum) {
  YC_NUM(k, 0);
  YC_NUM(n, 1);
  YC_NUM(kk, 2);
  YC_NUM(nn, 3);
  bool cum = false;
  if (has_cum) {
    YC_CUM(c, 4);
    cum = c;
  }
  k = std::trunc(k);
  n = std::trunc(n);
  kk = std::trunc(kk);
  nn = std::trunc(nn);
  if (k < 0 || k > n || k > kk || n <= 0 || n > nn || kk <= 0 || kk > nn || nn <= 0 || k < n - nn + kk) return Er(Err::Num);
  auto pmf = [&](double x) { return std::exp(LogComb(kk, x) + LogComb(nn - kk, n - x) - LogComb(nn, n)); };
  if (!cum) return Result(pmf(k));
  double s = 0;
  for (double x = std::max(0.0, n - nn + kk); x <= k; ++x) s += pmf(x);
  return Result(s);
}
EV FHypgeomDist(Evaluator& ev, const Node& call) { return Hypgeom(ev, call, true); }
EV FHypgeomDistLegacy(Evaluator& ev, const Node& call) { return Hypgeom(ev, call, false); }

EV NegBinom(Evaluator& ev, const Node& call, bool has_cum) {
  YC_NUM(f, 0);
  YC_NUM(s, 1);
  YC_NUM(p, 2);
  bool cum = false;
  if (has_cum) {
    YC_CUM(c, 3);
    cum = c;
  }
  f = std::trunc(f);
  s = std::trunc(s);
  if (p < 0 || p > 1 || f < 0 || s < 1) return Er(Err::Num);
  auto pmf = [&](double x) { return std::exp(LogComb(x + s - 1, s - 1) + s * std::log(p) + x * std::log1p(-p)); };
  if (!cum) return Result(pmf(f));
  double t = 0;
  for (double x = 0; x <= f; ++x) t += pmf(x);
  return Result(t);
}
EV FNegBinomDist(Evaluator& ev, const Node& call) { return NegBinom(ev, call, true); }
EV FNegBinomDistLegacy(Evaluator& ev, const Node& call) { return NegBinom(ev, call, false); }

EV FPermut(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(k, 1);
  n = std::trunc(n);
  k = std::trunc(k);
  if (n < 0 || k < 0 || n < k) return Er(Err::Num);
  return Result(std::round(std::exp(std::lgamma(n + 1) - std::lgamma(n - k + 1))));
}
EV FPermutationA(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(k, 1);
  n = std::trunc(n);
  k = std::trunc(k);
  if (n < 0 || k < 0) return Er(Err::Num);
  return Result(std::pow(n, k));
}

EV FProb(Evaluator& ev, const Node& call) {
  std::shared_ptr<Array> x, p;
  Value err;
  if (!SameShape(ev, call.kids[0], call.kids[1], &x, &p, &err)) return EV::S(err);
  YC_NUM(lo, 2);
  double hi = lo;
  if (Has(call, 3)) {
    YC_NUM(u, 3);
    hi = u;
  }
  double total = 0, in = 0;
  for (size_t i = 0; i < x->v.size(); ++i) {
    if (!p->v[i].IsNumeric()) continue;
    const double pr = p->v[i].AsDouble();
    if (pr < 0 || pr > 1) return Er(Err::Num);
    total += pr;
    if (x->v[i].IsNumeric()) {
      const double xv = x->v[i].AsDouble();
      if (xv >= lo && xv <= hi) in += pr;
    }
  }
  if (std::fabs(total - 1) > 1e-7) return Er(Err::Num);
  return Result(in);
}

EV FQuartileExc(Evaluator& ev, const Node& call) {
  const auto a = ev.ToArray(ev.Eval(call.kids[0]));
  if (!a) return Er(Err::Value);
  YC_NUM(q, 1);
  q = std::trunc(q);
  if (q < 1 || q > 3) return Er(Err::Num);
  auto v = NumsOf(*a);
  std::sort(v.begin(), v.end());
  const double n = static_cast<double>(v.size());
  const double rank = q / 4 * (n + 1);
  if (rank < 1 || rank > n) return Er(Err::Num);
  const size_t i = static_cast<size_t>(rank) - 1;
  const double frac = rank - std::floor(rank);
  return Result(i + 1 < v.size() ? v[i] + frac * (v[i + 1] - v[i]) : v[i]);
}

EV FFrequency(Evaluator& ev, const Node& call) {
  const auto d = ev.ToArray(ev.Eval(call.kids[0]));
  const auto b = ev.ToArray(ev.Eval(call.kids[1]));
  if (!d || !b) return Er(Err::Value);
  std::vector<std::pair<double, size_t>> bins;
  for (const Value& v : b->v) {
    if (v.IsNumeric()) bins.emplace_back(v.AsDouble(), bins.size());
  }
  std::vector<double> counts(bins.size() + 1, 0);
  auto sorted = bins;
  std::sort(sorted.begin(), sorted.end());
  for (const Value& v : d->v) {
    if (v.IsError()) return EV::S(v);
    if (!v.IsNumeric()) continue;
    const double x = v.AsDouble();
    auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(x, size_t{0}),
                               [](const auto& p, const auto& q) { return p.first < q.first; });
    if (it == sorted.end()) counts.back() += 1;
    else counts[it->second] += 1;
  }
  auto out = std::make_shared<Array>();
  out->rows = static_cast<uint32_t>(counts.size());
  out->cols = 1;
  for (double c : counts) out->v.push_back(Value::Number(c));
  return EV::A(std::move(out));
}

EV FModeMult(Evaluator& ev, const Node& call) {
  std::vector<double> v;
  Value err;
  if (!CollectModeNumbers(ev, call, &v, &err)) return EV::S(err);
  std::vector<std::pair<double, int>> seen;  // in order of first appearance
  for (double x : v) {
    auto it = std::find_if(seen.begin(), seen.end(), [x](const auto& p) { return p.first == x; });
    if (it == seen.end()) seen.emplace_back(x, 1);
    else ++it->second;
  }
  int best = 1;
  for (const auto& p : seen) best = std::max(best, p.second);
  if (best < 2) return Er(Err::NA);
  auto out = std::make_shared<Array>();
  for (const auto& p : seen) {
    if (p.second == best) out->v.push_back(Value::Number(p.first));
  }
  out->rows = static_cast<uint32_t>(out->v.size());
  out->cols = 1;
  return EV::A(std::move(out));
}

// ---- regression: LINEST, LOGEST, TREND, GROWTH ----

struct Regression {
  // observations: y[i], x[i][j] (j = variable)
  std::vector<double> y;
  std::vector<std::vector<double>> x;
  bool by_rows = false;  // observations run along a row (y is a row)
  uint32_t y_rows = 0, y_cols = 0;
};

// y and x as observations; x defaults to 1..n
bool ReadRegression(Evaluator& ev, const Node& call, int yi, int xi, bool log_y, Regression* r, EV* err) {
  const auto ya = ev.ToArray(ev.Eval(call.kids[static_cast<size_t>(yi)]));
  if (!ya || ya->v.empty()) {
    *err = Er(Err::Value);
    return false;
  }
  r->y_rows = ya->rows;
  r->y_cols = ya->cols;
  r->by_rows = ya->rows == 1 && ya->cols > 1;
  for (const Value& v : ya->v) {
    if (v.IsError()) {
      *err = EV::S(v);
      return false;
    }
    if (!v.IsNumeric()) {
      *err = Er(Err::Value);
      return false;
    }
    double y = v.AsDouble();
    if (log_y) {
      if (y <= 0) {
        *err = Er(Err::Num);
        return false;
      }
      y = std::log(y);
    }
    r->y.push_back(y);
  }
  const size_t n = r->y.size();
  if (xi < Argc(call) && Has(call, xi)) {
    const auto xa = ev.ToArray(ev.Eval(call.kids[static_cast<size_t>(xi)]));
    if (!xa) {
      *err = Er(Err::Value);
      return false;
    }
    for (const Value& v : xa->v) {
      if (v.IsError()) {
        *err = EV::S(v);
        return false;
      }
      if (!v.IsNumeric()) {
        *err = Er(Err::Value);
        return false;
      }
    }
    size_t k;
    // one variable of the same shape, or several variables stacked across the other dimension
    if (xa->rows == ya->rows && xa->cols == ya->cols) {
      r->x.assign(n, std::vector<double>(1));
      for (size_t i = 0; i < n; ++i) r->x[i][0] = xa->v[i].AsDouble();
      return true;
    }
    if (!r->by_rows && xa->rows == n) {
      k = xa->cols;
      r->x.assign(n, std::vector<double>(k));
      for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < k; ++j) r->x[i][j] = xa->At(static_cast<uint32_t>(i), static_cast<uint32_t>(j)).AsDouble();
      }
      return true;
    }
    if (r->by_rows && xa->cols == n) {
      k = xa->rows;
      r->x.assign(n, std::vector<double>(k));
      for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < k; ++j) r->x[i][j] = xa->At(static_cast<uint32_t>(j), static_cast<uint32_t>(i)).AsDouble();
      }
      return true;
    }
    *err = Er(Err::Ref);
    return false;
  }
  r->x.assign(n, std::vector<double>(1));
  for (size_t i = 0; i < n; ++i) r->x[i][0] = static_cast<double>(i + 1);
  return true;
}

// solves (X'X) b = X'y (with an intercept column first when use_const); false when singular
struct Fit {
  std::vector<double> b;  // [b0 (intercept, 0 without const), b1..bk]
  std::vector<double> se;
  double r2 = 0, sey = 0, f = 0, df = 0, ssreg = 0, ssresid = 0;
};

bool LeastSquares(const Regression& r, bool use_const, Fit* out) {
  const size_t n = r.y.size(), k = r.x.empty() ? 0 : r.x[0].size();
  const size_t p_all = k + (use_const ? 1 : 0);
  if (n < p_all || p_all == 0) return false;
  // design column j: the intercept's 1s first (with use_const), then the variables
  auto column = [&](size_t i, size_t j) { return use_const ? (j == 0 ? 1.0 : r.x[i][j - 1]) : r.x[i][j]; };
  // a column that the others already explain (collinear x) is left out with a coefficient and error of 0, as Excel
  // does: the intercept is tried first, then the variables from the last to the first (Excel keeps the later one of
  // two equal columns)
  std::vector<size_t> order;
  if (use_const) order.push_back(0);
  for (size_t j = p_all; j-- > (use_const ? 1u : 0u);) order.push_back(j);
  std::vector<size_t> keep;
  std::vector<std::vector<double>> ortho;  // orthonormal columns kept so far
  for (size_t j : order) {
    std::vector<double> v(n);
    double norm0 = 0;
    for (size_t i = 0; i < n; ++i) {
      v[i] = column(i, j);
      norm0 += v[i] * v[i];
    }
    for (int pass = 0; pass < 2; ++pass) {  // Gram-Schmidt, twice for accuracy
      for (const auto& q : ortho) {
        double dot = 0;
        for (size_t i = 0; i < n; ++i) dot += q[i] * v[i];
        for (size_t i = 0; i < n; ++i) v[i] -= dot * q[i];
      }
    }
    double norm = 0;
    for (double x : v) norm += x * x;
    if (norm0 == 0 || norm <= 1e-28 * norm0) continue;  // what is left is rounding (1E-14 of its length)
    const double len = std::sqrt(norm);
    for (double& x : v) x /= len;
    ortho.push_back(std::move(v));
    keep.push_back(j);
  }
  std::sort(keep.begin(), keep.end());
  const size_t p = keep.size();
  if (p == 0) return false;
  auto row = [&](size_t i, size_t j) { return column(i, keep[j]); };
  std::vector<std::vector<double>> a(p, std::vector<double>(p + p + 1, 0));
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < p; ++j) {
      for (size_t l = 0; l < p; ++l) a[j][l] += row(i, j) * row(i, l);
      a[j][p + p] += row(i, j) * r.y[i];
    }
  }
  for (size_t j = 0; j < p; ++j) a[j][p + j] = 1;  // augmented identity for the inverse
  for (size_t c = 0; c < p; ++c) {
    size_t piv = c;
    for (size_t i = c + 1; i < p; ++i) {
      if (std::fabs(a[i][c]) > std::fabs(a[piv][c])) piv = i;
    }
    if (std::fabs(a[piv][c]) < 1e-12) return false;
    std::swap(a[piv], a[c]);
    const double d = a[c][c];
    for (double& v : a[c]) v /= d;
    for (size_t i = 0; i < p; ++i) {
      if (i == c) continue;
      const double f = a[i][c];
      if (f == 0) continue;
      for (size_t l = 0; l < a[i].size(); ++l) a[i][l] -= f * a[c][l];
    }
  }
  std::vector<double> coef(p);
  for (size_t j = 0; j < p; ++j) coef[j] = a[j][p + p];
  out->b.assign(k + 1, 0);
  for (size_t j = 0; j < p; ++j) out->b[use_const ? keep[j] : keep[j] + 1] = coef[j];
  // statistics
  double ymean = 0;
  for (double y : r.y) ymean += y;
  ymean /= static_cast<double>(n);
  double ssres = 0, sstot = 0;
  for (size_t i = 0; i < n; ++i) {
    double pred = out->b[0];
    for (size_t j = 0; j < k; ++j) pred += out->b[j + 1] * r.x[i][j];
    ssres += (r.y[i] - pred) * (r.y[i] - pred);
    const double t = use_const ? r.y[i] - ymean : r.y[i];
    sstot += t * t;
  }
  out->df = static_cast<double>(n) - static_cast<double>(p);
  out->ssresid = ssres;
  out->ssreg = sstot - ssres;
  out->r2 = sstot == 0 ? 1 : out->ssreg / sstot;
  out->sey = out->df > 0 ? std::sqrt(ssres / out->df) : 0;
  const double kk = static_cast<double>(p - (use_const ? 1 : 0));  // the variables kept
  out->f = out->df > 0 && ssres > 0 ? (out->ssreg / kk) / (ssres / out->df) : INFINITY;
  out->se.assign(k + 1, 0);
  const double s2 = out->df > 0 ? ssres / out->df : 0;
  for (size_t j = 0; j < p; ++j) out->se[use_const ? keep[j] : keep[j] + 1] = std::sqrt(std::max(0.0, s2 * a[j][p + j]));
  return true;
}

// LINEST / LOGEST output: m_k ... m_1, b (+ statistics rows)
EV RegressionOut(const Fit& fit, bool stats, bool exp_coef, bool use_const) {
  const size_t k = fit.b.size() - 1;
  const uint32_t cols = static_cast<uint32_t>(k + 1), rows = stats ? 5 : 1;
  auto out = std::make_shared<Array>();
  out->rows = rows;
  out->cols = cols;
  out->v.assign(static_cast<size_t>(rows) * cols, Value::Error(Err::NA));
  auto coef = [&](double v) { return Value::Number(exp_coef ? std::exp(v) : v); };
  for (size_t j = 0; j < k; ++j) out->At(0, static_cast<uint32_t>(j)) = coef(fit.b[k - j]);
  out->At(0, static_cast<uint32_t>(k)) = exp_coef ? Value::Number(use_const ? std::exp(fit.b[0]) : 1) : Value::Number(fit.b[0]);
  if (stats) {
    for (size_t j = 0; j < k; ++j) out->At(1, static_cast<uint32_t>(j)) = Value::Number(fit.se[k - j]);
    out->At(1, static_cast<uint32_t>(k)) = use_const ? Value::Number(fit.se[0]) : Value::Error(Err::NA);
    out->At(2, 0) = Value::Number(fit.r2);
    if (cols > 1) out->At(2, 1) = Value::Number(fit.sey);
    out->At(3, 0) = std::isfinite(fit.f) ? Value::Number(fit.f) : Value::Error(Err::Num);
    if (cols > 1) out->At(3, 1) = Value::Number(fit.df);
    out->At(4, 0) = Value::Number(fit.ssreg);
    if (cols > 1) out->At(4, 1) = Value::Number(fit.ssresid);
  }
  return EV::A(std::move(out));
}

EV Linest(Evaluator& ev, const Node& call, bool log_y) {
  Regression r;
  EV err;
  if (!ReadRegression(ev, call, 0, 1, log_y, &r, &err)) return err;
  YC_BOOL_OPT(use_const, 2, true);
  YC_BOOL_OPT(stats, 3, false);
  Fit fit;
  if (!LeastSquares(r, use_const, &fit)) return Er(Err::Num);
  return RegressionOut(fit, stats, log_y, use_const);
}
EV FLinest(Evaluator& ev, const Node& call) { return Linest(ev, call, false); }
EV FLogest(Evaluator& ev, const Node& call) { return Linest(ev, call, true); }

// TREND / GROWTH: predictions for new_x (or the known x)
EV Trend(Evaluator& ev, const Node& call, bool log_y) {
  Regression r;
  EV err;
  if (!ReadRegression(ev, call, 0, 1, log_y, &r, &err)) return err;
  YC_BOOL_OPT(use_const, 3, true);
  Fit fit;
  if (!LeastSquares(r, use_const, &fit)) return Er(Err::Num);
  const size_t k = fit.b.size() - 1;
  auto predict = [&](const std::vector<double>& xs) {
    double v = fit.b[0];
    for (size_t j = 0; j < k; ++j) v += fit.b[j + 1] * xs[j];
    return log_y ? std::exp(v) : v;
  };
  auto out = std::make_shared<Array>();
  if (Has(call, 2)) {
    const auto nx = ev.ToArray(ev.Eval(call.kids[2]));
    if (!nx) return Er(Err::Value);
    for (const Value& v : nx->v) {
      if (!v.IsNumeric()) return Er(Err::Value);
    }
    if (k == 1) {
      out->rows = nx->rows;
      out->cols = nx->cols;
      for (const Value& v : nx->v) out->v.push_back(Value::Number(predict({v.AsDouble()})));
    } else if (!r.by_rows && nx->cols == k) {
      out->rows = nx->rows;
      out->cols = 1;
      for (uint32_t i = 0; i < nx->rows; ++i) {
        std::vector<double> xs(k);
        for (size_t j = 0; j < k; ++j) xs[j] = nx->At(i, static_cast<uint32_t>(j)).AsDouble();
        out->v.push_back(Value::Number(predict(xs)));
      }
    } else if (r.by_rows && nx->rows == k) {
      out->rows = 1;
      out->cols = nx->cols;
      for (uint32_t i = 0; i < nx->cols; ++i) {
        std::vector<double> xs(k);
        for (size_t j = 0; j < k; ++j) xs[j] = nx->At(static_cast<uint32_t>(j), i).AsDouble();
        out->v.push_back(Value::Number(predict(xs)));
      }
    } else {
      return Er(Err::Ref);
    }
  } else {
    out->rows = r.y_rows;
    out->cols = r.y_cols;
    for (const auto& xs : r.x) out->v.push_back(Value::Number(predict(xs)));
  }
  return EV::A(std::move(out));
}
EV FTrend(Evaluator& ev, const Node& call) { return Trend(ev, call, false); }
EV FGrowth(Evaluator& ev, const Node& call) { return Trend(ev, call, true); }

// ---- FORECAST.ETS: additive Holt-Winters (triple exponential smoothing) ----
// Excel's AAA version of ETS; the smoothing parameters are fitted by minimizing the squared one-step errors, so the
// results are close to Excel's but not always identical (Excel's optimizer isn't documented).

struct EtsSeries {
  std::vector<double> y;
  double t0 = 0, step = 1;
};

// values + timeline -> an evenly spaced series (duplicates aggregated, gaps filled)
bool BuildSeries(Evaluator& ev, const Node& vals, const Node& times, int completion, int aggregation, EtsSeries* s, EV* err) {
  std::shared_ptr<Array> v, t;
  Value e;
  if (!SameShape(ev, vals, times, &v, &t, &e)) {
    *err = EV::S(e);
    return false;
  }
  std::vector<std::pair<double, double>> pts;
  for (size_t i = 0; i < v->v.size(); ++i) {
    if (!t->v[i].IsNumeric()) {
      *err = Er(Err::Value);
      return false;
    }
    if (v->v[i].IsNumeric()) pts.emplace_back(t->v[i].AsDouble(), v->v[i].AsDouble());
  }
  if (pts.size() < 3) {
    *err = Er(Err::NA);
    return false;
  }
  std::sort(pts.begin(), pts.end());
  // aggregate duplicate times
  std::vector<std::pair<double, std::vector<double>>> groups;
  for (const auto& p : pts) {
    if (!groups.empty() && groups.back().first == p.first) groups.back().second.push_back(p.second);
    else groups.push_back({p.first, {p.second}});
  }
  std::vector<std::pair<double, double>> agg;
  for (auto& g : groups) {
    auto& xs = g.second;
    double r = 0;
    switch (aggregation) {
      case 2: r = static_cast<double>(xs.size()); break;                               // COUNT
      case 3: r = static_cast<double>(xs.size()); break;                               // COUNTA
      case 4: r = *std::max_element(xs.begin(), xs.end()); break;                      // MAX
      case 5: {                                                                        // MEDIAN
        std::sort(xs.begin(), xs.end());
        r = xs.size() % 2 ? xs[xs.size() / 2] : (xs[xs.size() / 2 - 1] + xs[xs.size() / 2]) / 2;
        break;
      }
      case 6: r = *std::min_element(xs.begin(), xs.end()); break;                      // MIN
      case 7: r = std::accumulate(xs.begin(), xs.end(), 0.0); break;                   // SUM
      default: r = std::accumulate(xs.begin(), xs.end(), 0.0) / static_cast<double>(xs.size());  // AVERAGE
    }
    agg.emplace_back(g.first, r);
  }
  // the step: the smallest gap
  double step = INFINITY;
  for (size_t i = 1; i < agg.size(); ++i) step = std::min(step, agg[i].first - agg[i - 1].first);
  if (!(step > 0) || agg.size() < 3) {
    *err = Er(Err::Num);
    return false;
  }
  s->t0 = agg[0].first;
  s->step = step;
  const size_t n = static_cast<size_t>(std::llround((agg.back().first - agg[0].first) / step)) + 1;
  if (n > 100000) {
    *err = Er(Err::Num);
    return false;
  }
  s->y.assign(n, NAN);
  for (const auto& p : agg) {
    const double pos = (p.first - s->t0) / step;
    if (std::fabs(pos - std::round(pos)) > 1e-6) {
      *err = Er(Err::Num);  // the timeline isn't evenly spaced
      return false;
    }
    s->y[static_cast<size_t>(std::llround(pos))] = p.second;
  }
  for (size_t i = 0; i < n; ++i) {
    if (!std::isnan(s->y[i])) continue;
    if (completion == 0) {
      s->y[i] = 0;
      continue;
    }
    size_t j = i;
    while (j < n && std::isnan(s->y[j])) ++j;
    const double a = s->y[i - 1], b = j < n ? s->y[j] : a;
    for (size_t k = i; k < j; ++k) s->y[k] = a + (b - a) * static_cast<double>(k - i + 1) / static_cast<double>(j - i + 1);
    i = j;
  }
  return true;
}

// seasonality: the lag with the highest autocorrelation (that is also a local peak), 1 = none
int DetectSeason(const std::vector<double>& y) {
  const size_t n = y.size();
  if (n < 8) return 1;
  // detrend with first differences
  std::vector<double> d(n - 1);
  for (size_t i = 1; i < n; ++i) d[i - 1] = y[i] - y[i - 1];
  const double m = std::accumulate(d.begin(), d.end(), 0.0) / static_cast<double>(d.size());
  double var = 0;
  for (double x : d) var += (x - m) * (x - m);
  if (var == 0) return 1;
  const size_t max_lag = std::min<size_t>(d.size() / 2, 8760);
  std::vector<double> ac(max_lag + 2, 0);
  for (size_t lag = 1; lag <= max_lag + 1 && lag < d.size(); ++lag) {
    double s = 0;
    for (size_t i = lag; i < d.size(); ++i) s += (d[i] - m) * (d[i - lag] - m);
    ac[lag] = s / var;
  }
  int best = 1;
  double best_v = 0.3;  // needs a clear correlation
  for (size_t lag = 2; lag <= max_lag; ++lag) {
    if (ac[lag] > best_v && ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1]) {
      best_v = ac[lag];
      best = static_cast<int>(lag);
    }
  }
  return best;
}

struct EtsModel {
  double alpha = 0.5, beta = 0.1, gamma = 0.1;
  int season = 1;
  double level = 0, trend = 0;
  std::vector<double> seas;
  double sse = 0, mae = 0, smape = 0, mase = 0;
  size_t n = 0;
};

// runs the smoothing; returns the sum of squared one-step errors
double RunEts(const std::vector<double>& y, EtsModel* m) {
  const size_t n = y.size();
  const int L = m->season;
  // initial level / trend / seasonal indexes from the first seasons
  m->seas.assign(static_cast<size_t>(std::max(1, L)), 0);
  if (L > 1 && n >= static_cast<size_t>(2 * L)) {
    double a1 = 0, a2 = 0;
    for (int i = 0; i < L; ++i) {
      a1 += y[static_cast<size_t>(i)];
      a2 += y[static_cast<size_t>(i + L)];
    }
    a1 /= L;
    a2 /= L;
    m->level = a1;
    m->trend = (a2 - a1) / L;
    for (int i = 0; i < L; ++i) m->seas[static_cast<size_t>(i)] = y[static_cast<size_t>(i)] - a1;
  } else {
    m->level = y[0];
    m->trend = n > 1 ? y[1] - y[0] : 0;
  }
  double sse = 0, sae = 0, sape = 0, naive = 0;
  size_t cnt = 0;
  for (size_t t = 1; t < n; ++t) {
    const size_t si = L > 1 ? t % static_cast<size_t>(L) : 0;
    const double s = L > 1 ? m->seas[si] : 0;
    const double f = m->level + m->trend + s;
    const double e = y[t] - f;
    sse += e * e;
    sae += std::fabs(e);
    if (std::fabs(y[t]) + std::fabs(f) > 0) sape += 2 * std::fabs(e) / (std::fabs(y[t]) + std::fabs(f));
    naive += std::fabs(y[t] - y[t - 1]);
    ++cnt;
    const double prev = m->level;
    m->level = m->alpha * (y[t] - s) + (1 - m->alpha) * (m->level + m->trend);
    m->trend = m->beta * (m->level - prev) + (1 - m->beta) * m->trend;
    if (L > 1) m->seas[si] = m->gamma * (y[t] - m->level) + (1 - m->gamma) * s;
  }
  m->n = n;
  m->sse = sse;
  m->mae = cnt ? sae / static_cast<double>(cnt) : 0;
  m->smape = cnt ? sape / static_cast<double>(cnt) : 0;
  m->mase = naive > 0 ? sae / naive : 0;
  return sse;
}

EtsModel FitEts(const std::vector<double>& y, int season) {
  EtsModel best;
  best.season = season;
  double best_sse = INFINITY;
  // coarse grid, then a finer pass around the best point
  auto search = [&](double a0, double a1, double b0, double b1, double g0, double g1, int steps) {
    for (int i = 0; i <= steps; ++i) {
      for (int j = 0; j <= steps; ++j) {
        for (int k = 0; k <= (season > 1 ? steps : 0); ++k) {
          EtsModel m;
          m.season = season;
          m.alpha = a0 + (a1 - a0) * i / steps;
          m.beta = b0 + (b1 - b0) * j / steps;
          m.gamma = season > 1 ? g0 + (g1 - g0) * k / steps : 0;
          const double sse = RunEts(y, &m);
          if (sse < best_sse) {
            best_sse = sse;
            best = m;
          }
        }
      }
    }
  };
  search(0.01, 0.99, 0.0, 0.5, 0.0, 0.99, 8);
  const double a = best.alpha, b = best.beta, g = best.gamma;
  search(std::max(0.001, a - 0.12), std::min(0.999, a + 0.12), std::max(0.0, b - 0.06), std::min(0.999, b + 0.06), std::max(0.0, g - 0.12),
         std::min(0.999, g + 0.12), 8);
  RunEts(y, &best);  // leaves the final state in best
  return best;
}

struct EtsArgs {
  EtsSeries s;
  int season = 1;
  EtsModel model;
};

// values at vi, timeline at ti, then seasonality / completion / aggregation from si
bool EtsPrepare(Evaluator& ev, const Node& call, int vi, int ti, int si, EtsArgs* a, EV* err) {
  int season = 1, completion = 1, aggregation = 1;  // seasonality 1 = detect
  auto opt = [&](int i, int* out, int def) {
    *out = def;
    if (!Has(call, i)) return true;
    double d;
    Value e;
    if (!ev.ArgNumber(call.kids[static_cast<size_t>(i)], &d, &e)) {
      *err = EV::S(e);
      return false;
    }
    *out = static_cast<int>(std::trunc(d));
    return true;
  };
  if (si >= 0 && !opt(si, &season, 1)) return false;
  if (!opt(si >= 0 ? si + 1 : ti + 1, &completion, 1)) return false;
  if (!opt(si >= 0 ? si + 2 : ti + 2, &aggregation, 1)) return false;
  if (season < 0 || season > 8760 || completion < 0 || completion > 1 || aggregation < 1 || aggregation > 7) {
    *err = Er(Err::Num);
    return false;
  }
  if (!BuildSeries(ev, call.kids[static_cast<size_t>(vi)], call.kids[static_cast<size_t>(ti)], completion, aggregation, &a->s, err)) return false;
  a->season = season == 1 ? DetectSeason(a->s.y) : season == 0 ? 1 : season;
  if (a->season > 1 && a->s.y.size() < static_cast<size_t>(2 * a->season)) a->season = 1;
  a->model = FitEts(a->s.y, a->season);
  return true;
}

double EtsForecast(const EtsArgs& a, double target, double* h_out) {
  const double h = (target - a.s.t0) / a.s.step - static_cast<double>(a.s.y.size() - 1);
  *h_out = h;
  const int L = a.model.season;
  double s = 0;
  if (L > 1) {
    const auto idx = static_cast<size_t>(((static_cast<long long>(a.s.y.size()) - 1 + static_cast<long long>(std::ceil(h))) % L + L) % L);
    s = a.model.seas[idx];
  }
  return a.model.level + h * a.model.trend + s;
}

EV FForecastEts(Evaluator& ev, const Node& call) {
  YC_NUM(target, 0);
  EtsArgs a;
  EV err;
  if (!EtsPrepare(ev, call, 1, 2, 3, &a, &err)) return err;
  double h;
  const double f = EtsForecast(a, target, &h);
  if (h <= 0) return Er(Err::Num);  // the target must be after the last time
  return Result(f);
}

EV FForecastEtsConfint(Evaluator& ev, const Node& call) {
  YC_NUM(target, 0);
  YC_NUM_OPT(conf, 3, 0.95);
  if (conf <= 0 || conf >= 1) return Er(Err::Num);
  EtsArgs a;
  EV err;
  if (!EtsPrepare(ev, call, 1, 2, 4, &a, &err)) return err;
  double h;
  EtsForecast(a, target, &h);
  if (h <= 0) return Er(Err::Num);
  const double n = static_cast<double>(a.s.y.size() - 1);
  const double sigma = std::sqrt(a.model.sse / std::max(1.0, n));
  // variance grows with the horizon (approximation for the additive model)
  const double al = a.model.alpha, be = a.model.beta;
  double var = 1;
  for (int j = 1; j < static_cast<int>(std::ceil(h)); ++j) var += (al + al * be * j) * (al + al * be * j);
  return Result(StdNormInv(0.5 + conf / 2) * sigma * std::sqrt(var));
}

EV FForecastEtsSeasonality(Evaluator& ev, const Node& call) {
  EtsArgs a;
  EV err;
  if (!EtsPrepare(ev, call, 0, 1, -1, &a, &err)) return err;
  return Num(a.season == 1 ? 0 : a.season);
}

EV FForecastEtsStat(Evaluator& ev, const Node& call) {
  YC_NUM(kind, 2);
  EtsArgs a;
  EV err;
  if (!EtsPrepare(ev, call, 0, 1, 3, &a, &err)) return err;
  switch (static_cast<int>(kind)) {
    case 1: return Result(a.model.alpha);
    case 2: return Result(a.model.beta);
    case 3: return Result(a.model.gamma);
    case 4: return Result(a.model.mase);
    case 5: return Result(a.model.smape);
    case 6: return Result(a.model.mae);
    case 7: return Result(std::sqrt(a.model.sse / std::max<double>(1, static_cast<double>(a.s.y.size() - 1))));
    case 8: return Result(a.s.step);
    default: return Er(Err::Num);
  }
}

}  // namespace

}  // namespace yc::formula::fn

namespace yc::formula::stats {
double TCdf(double t, double df) { return fn::TCdf(t, df); }
double TInv(double p, double df) { return fn::TInv(p, df); }
double FCdf(double x, double d1, double d2) { return fn::FCdf(x, d1, d2); }
double FInv(double p, double d1, double d2) {
  return fn::InvertCdf([d1, d2](double x) { return fn::FCdf(x, d1, d2); }, p, 0, 10);
}
double NormSCdf(double z) { return fn::StdNormCdf(z); }
double TTail(double t, double df) {
  const double tail = 0.5 * fn::BetaReg(df / 2, 0.5, df / (df + t * t));
  return t >= 0 ? tail : 1 - tail;
}
double FTail(double x, double d1, double d2) { return x <= 0 ? 1 : fn::BetaReg(d2 / 2, d1 / 2, d2 / (d2 + d1 * x)); }
double NormSTail(double z) { return 0.5 * std::erfc(z / std::sqrt(2.0)); }
double NormSInv(double p) { return fn::StdNormInv(p); }
}  // namespace yc::formula::stats

namespace yc::formula::fn {

void RegisterDist(std::vector<FuncInfo>& t) {
  t.push_back({"T.DIST", 3, 3, FTDist});
  t.push_back({"T.DIST.2T", 2, 2, FTDist2T});
  t.push_back({"T.DIST.RT", 2, 2, FTDistRT});
  t.push_back({"TDIST", 3, 3, FTDistLegacy});
  t.push_back({"T.INV", 2, 2, FTInv});
  t.push_back({"T.INV.2T", 2, 2, FTInv2T});
  t.push_back({"TINV", 2, 2, FTInv2T});
  t.push_back({"CHISQ.DIST", 3, 3, FChiDist});
  t.push_back({"CHISQ.DIST.RT", 2, 2, FChiDistRT});
  t.push_back({"CHIDIST", 2, 2, FChiDistRT});
  t.push_back({"CHISQ.INV", 2, 2, FChiInv});
  t.push_back({"CHISQ.INV.RT", 2, 2, FChiInvRT});
  t.push_back({"CHIINV", 2, 2, FChiInvRT});
  t.push_back({"F.DIST", 4, 4, FFDist});
  t.push_back({"F.DIST.RT", 3, 3, FFDistRT});
  t.push_back({"FDIST", 3, 3, FFDistRT});
  t.push_back({"F.INV", 3, 3, FFInv});
  t.push_back({"F.INV.RT", 3, 3, FFInvRT});
  t.push_back({"FINV", 3, 3, FFInvRT});
  t.push_back({"GAMMA.DIST", 4, 4, FGammaDist});
  t.push_back({"GAMMADIST", 4, 4, FGammaDist});
  t.push_back({"GAMMA.INV", 3, 3, FGammaInv});
  t.push_back({"GAMMAINV", 3, 3, FGammaInv});
  t.push_back({"GAMMALN", 1, 1, FGammaLn});
  t.push_back({"GAMMALN.PRECISE", 1, 1, FGammaLn});
  t.push_back({"GAMMA", 1, 1, FGamma});
  t.push_back({"BINOM.DIST", 4, 4, FBinomDist});
  t.push_back({"BINOMDIST", 4, 4, FBinomDist});
  t.push_back({"POISSON.DIST", 3, 3, FPoissonDist});
  t.push_back({"POISSON", 3, 3, FPoissonDist});
  t.push_back({"EXPON.DIST", 3, 3, FExponDist});
  t.push_back({"EXPONDIST", 3, 3, FExponDist});
  t.push_back({"WEIBULL.DIST", 4, 4, FWeibullDist});
  t.push_back({"WEIBULL", 4, 4, FWeibullDist});
  t.push_back({"LOGNORM.DIST", 4, 4, FLognormDist});
  t.push_back({"LOGNORMDIST", 3, 3, FLognormDist});
  t.push_back({"LOGNORM.INV", 3, 3, FLognormInv});
  t.push_back({"LOGINV", 3, 3, FLognormInv});
  t.push_back({"CONFIDENCE.NORM", 3, 3, FConfidenceNorm});
  t.push_back({"CONFIDENCE", 3, 3, FConfidenceNorm});
  t.push_back({"CONFIDENCE.T", 3, 3, FConfidenceT});
  t.push_back({"DEVSQ", 1, 255, FDevSq});
  t.push_back({"AVEDEV", 1, 255, FAveDev});
  t.push_back({"SKEW", 1, 255, FSkew});
  t.push_back({"KURT", 1, 255, FKurt});
  t.push_back({"HARMEAN", 1, 255, FHarMean});
  t.push_back({"TRIMMEAN", 2, 2, FTrimMean});
  t.push_back({"PERCENTRANK", 2, 3, FPercentRankInc});
  t.push_back({"PERCENTRANK.INC", 2, 3, FPercentRankInc});
  t.push_back({"PERCENTRANK.EXC", 2, 3, FPercentRankExc});
  t.push_back({"COVARIANCE.P", 2, 2, FCovarP});
  t.push_back({"COVAR", 2, 2, FCovarP});
  t.push_back({"PEARSON", 2, 2, FPearson});
  t.push_back({"RSQ", 2, 2, FRsq});
  t.push_back({"STEYX", 2, 2, FSteyx});
  t.push_back({"SINH", 1, 1, Hyp<Sinh>});
  t.push_back({"COSH", 1, 1, Hyp<Cosh>});
  t.push_back({"TANH", 1, 1, Hyp<Tanh>});
  t.push_back({"ASINH", 1, 1, Hyp<Asinh>});
  t.push_back({"ACOSH", 1, 1, FAcosh});
  t.push_back({"ATANH", 1, 1, FAtanh});
  t.push_back({"STDEVA", 1, 255, FStdevA});
  t.push_back({"STDEVPA", 1, 255, FStdevPA});
  t.push_back({"VARA", 1, 255, FVarA});
  t.push_back({"VARPA", 1, 255, FVarPA});
  t.push_back({"SKEW.P", 1, 255, FSkewP});
  t.push_back({"BETA.DIST", 4, 6, FBetaDist});
  t.push_back({"BETADIST", 3, 5, FBetaDistLegacy});
  t.push_back({"BETA.INV", 3, 5, FBetaInv});
  t.push_back({"BETAINV", 3, 5, FBetaInv});
  t.push_back({"BINOM.DIST.RANGE", 3, 4, FBinomDistRange});
  t.push_back({"BINOM.INV", 3, 3, FBinomInv});
  t.push_back({"CRITBINOM", 3, 3, FBinomInv});
  t.push_back({"CHISQ.TEST", 2, 2, FChiTest});
  t.push_back({"CHITEST", 2, 2, FChiTest});
  t.push_back({"F.TEST", 2, 2, FFTest});
  t.push_back({"FTEST", 2, 2, FFTest});
  t.push_back({"T.TEST", 4, 4, FTTest});
  t.push_back({"TTEST", 4, 4, FTTest});
  t.push_back({"Z.TEST", 2, 3, FZTest});
  t.push_back({"ZTEST", 2, 3, FZTest});
  t.push_back({"FISHER", 1, 1, FFisher});
  t.push_back({"FISHERINV", 1, 1, FFisherInv});
  t.push_back({"GAUSS", 1, 1, FGauss});
  t.push_back({"PHI", 1, 1, FPhi});
  t.push_back({"HYPGEOM.DIST", 5, 5, FHypgeomDist});
  t.push_back({"HYPGEOMDIST", 4, 4, FHypgeomDistLegacy});
  t.push_back({"NEGBINOM.DIST", 4, 4, FNegBinomDist});
  t.push_back({"NEGBINOMDIST", 3, 3, FNegBinomDistLegacy});
  t.push_back({"PERMUT", 2, 2, FPermut});
  t.push_back({"PERMUTATIONA", 2, 2, FPermutationA});
  t.push_back({"PROB", 3, 4, FProb});
  t.push_back({"QUARTILE.EXC", 2, 2, FQuartileExc});
  t.push_back({"FREQUENCY", 2, 2, FFrequency});
  t.push_back({"MODE.MULT", 1, 255, FModeMult});
  t.push_back({"LINEST", 1, 4, FLinest});
  t.push_back({"LOGEST", 1, 4, FLogest});
  t.push_back({"TREND", 1, 4, FTrend});
  t.push_back({"GROWTH", 1, 4, FGrowth});
  t.push_back({"FORECAST.ETS", 3, 6, FForecastEts});
  t.push_back({"FORECAST.ETS.CONFINT", 3, 7, FForecastEtsConfint});
  t.push_back({"FORECAST.ETS.SEASONALITY", 2, 4, FForecastEtsSeasonality});
  t.push_back({"FORECAST.ETS.STAT", 3, 6, FForecastEtsStat});
}

}  // namespace yc::formula::fn
