// SPDX-License-Identifier: Apache-2.0
// fn_fin2.cpp - more financial functions: coupons (COUP*), bonds (PRICE, YIELD, DURATION, MDURATION, ODDF*, ODDL*,
// ACCRINT, ACCRINTM), discounted securities (PRICEDISC, YIELDDISC, DISC, INTRATE, RECEIVED, PRICEMAT, YIELDMAT),
// Treasury bills (TBILL*), depreciation (SLN, SYD, DB, DDB, VDB, AMORLINC, AMORDEGRC) and rates (EFFECT, NOMINAL,
// FVSCHEDULE, ISPMT, MIRR, PDURATION, RRI, CUMIPMT, CUMPRINC, DOLLARDE, DOLLARFR)
//
// Day-count bases: 0 US 30/360, 1 actual/actual, 2 actual/360, 3 actual/365, 4 European 30/360. The bond formulas
// are the ones in Excel's documentation; the depreciation ones (DB, DDB, VDB) follow Excel's measured results.
#include "formula/datetime.h"
#include "formula/fn_util.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace yc::formula::fn {
namespace {

EV Result(double v) { return std::isfinite(v) ? Num(v) : Er(Err::Num); }

// ---- dates ----

struct Ymd {
  int y, m, d;
};
Ymd ToYmd(double serial) {
  Ymd r{1900, 1, 1};
  SerialToYmd(serial, &r.y, &r.m, &r.d);
  return r;
}
double ToSerial(int y, int m, int d) { return DateSerial(y, m, d); }

// months added with Excel's coupon rule: the day is kept (clamped to the month); end of month stays end of month
double AddMonths(double serial, int months, bool eom) {
  Ymd a = ToYmd(serial);
  int y = a.y, m = a.m + months;
  while (m > 12) {
    m -= 12;
    ++y;
  }
  while (m < 1) {
    m += 12;
    --y;
  }
  const int dim = DaysInMonth(y, m);
  return ToSerial(y, m, eom ? dim : std::min(a.d, dim));
}

bool IsEom(double serial) {
  const Ymd a = ToYmd(serial);
  return a.d == DaysInMonth(a.y, a.m);
}

// days between two dates by basis (30/360 for 0 and 4)
double DayCount(double s, double e, int basis) {
  if (basis == 0 || basis == 4) {
    Ymd a = ToYmd(s), b = ToYmd(e);
    if (basis == 4) {
      if (a.d == 31) a.d = 30;
      if (b.d == 31) b.d = 30;
    } else {
      const bool last_feb_a = a.m == 2 && a.d == DaysInMonth(a.y, 2);
      const bool last_feb_b = b.m == 2 && b.d == DaysInMonth(b.y, 2);
      if (last_feb_a && last_feb_b) b.d = 30;
      if (a.d == 31 || last_feb_a) a.d = 30;
      if (b.d == 31 && a.d >= 30) b.d = 30;
    }
    return (b.y - a.y) * 360.0 + (b.m - a.m) * 30.0 + (b.d - a.d);
  }
  return e - s;
}

double YearDays(int basis, double s, double e) {
  switch (basis) {
    case 1: {
      // average length of the years the period touches
      const Ymd a = ToYmd(s), b = ToYmd(e);
      if (a.y == b.y) return IsLeap(a.y) ? 366 : 365;
      double total = 0;
      for (int y = a.y; y <= b.y; ++y) total += IsLeap(y) ? 366 : 365;
      return total / (b.y - a.y + 1);
    }
    case 3: return 365;
    default: return 360;
  }
}

// ---- argument helpers ----

bool IntArg(Evaluator& ev, const Node& call, int i, int def, int* out, Value* err) {
  *out = def;
  if (!Has(call, i)) return true;
  double d;
  if (!ev.ArgNumber(call.kids[static_cast<size_t>(i)], &d, err)) return false;
  *out = static_cast<int>(std::trunc(d));
  return true;
}

#define YC_DATEARG(var, i) \
  YC_NUM(var##_raw, i);    \
  const double var = std::floor(var##_raw)
#define YC_BASIS(var, i)                                       \
  int var;                                                     \
  {                                                            \
    Value _e;                                                  \
    if (!IntArg(ev, call, (i), 0, &var, &_e)) return EV::S(_e); \
    if (var < 0 || var > 4) return Er(Err::Num);               \
  }
#define YC_FREQ(var, i)                                        \
  int var;                                                     \
  {                                                            \
    Value _e;                                                  \
    if (!IntArg(ev, call, (i), 0, &var, &_e)) return EV::S(_e); \
    if (var != 1 && var != 2 && var != 4) return Er(Err::Num); \
  }

// ---- coupons ----

struct Coupon {
  double pcd, ncd;  // previous / next coupon date around the settlement
  int num;          // coupons from the settlement to the maturity
};

Coupon Coupons(double settle, double mat, int freq) {
  const int step = 12 / freq;
  const bool eom = IsEom(mat);
  // walk back from the maturity in whole coupon periods
  int k = 0;
  double d = mat;
  while (d > settle) {
    ++k;
    d = AddMonths(mat, -step * k, eom);
  }
  Coupon c;
  c.pcd = d;
  c.ncd = AddMonths(mat, -step * (k - 1), eom);
  c.num = k;
  return c;
}

double CoupDays(double settle, double mat, int freq, int basis) {
  if (basis == 1) {
    const Coupon c = Coupons(settle, mat, freq);
    return c.ncd - c.pcd;
  }
  return (basis == 3 ? 365.0 : 360.0) / freq;
}
double CoupDayBs(double settle, double mat, int freq, int basis) { return DayCount(Coupons(settle, mat, freq).pcd, settle, basis); }
double CoupDaysNc(double settle, double mat, int freq, int basis) {
  if (basis == 0) return CoupDays(settle, mat, freq, basis) - CoupDayBs(settle, mat, freq, basis);
  return DayCount(settle, Coupons(settle, mat, freq).ncd, basis);
}

// common checks of the coupon functions: settlement < maturity
#define YC_COUPON_ARGS                \
  YC_DATEARG(settle, 0);              \
  YC_DATEARG(mat, 1);                 \
  YC_FREQ(freq, 2);                   \
  YC_BASIS(basis, 3);                 \
  if (settle >= mat) return Er(Err::Num)

EV FCoupDayBs(Evaluator& ev, const Node& call) {
  YC_COUPON_ARGS;
  return Num(CoupDayBs(settle, mat, freq, basis));
}
EV FCoupDays(Evaluator& ev, const Node& call) {
  YC_COUPON_ARGS;
  return Num(CoupDays(settle, mat, freq, basis));
}
EV FCoupDaysNc(Evaluator& ev, const Node& call) {
  YC_COUPON_ARGS;
  return Num(CoupDaysNc(settle, mat, freq, basis));
}
EV FCoupNcd(Evaluator& ev, const Node& call) {
  YC_COUPON_ARGS;
  return Num(Coupons(settle, mat, freq).ncd);
}
EV FCoupPcd(Evaluator& ev, const Node& call) {
  YC_COUPON_ARGS;
  return Num(Coupons(settle, mat, freq).pcd);
}
EV FCoupNum(Evaluator& ev, const Node& call) {
  YC_COUPON_ARGS;
  return Num(Coupons(settle, mat, freq).num);
}

// ---- bonds ----

// the days from the settlement to the next coupon, as PRICE (and YIELD through it) and DURATION count them: the
// period's length less the days accrued (COUPDAYS - COUPDAYBS), not COUPDAYSNC, in every basis (measured in Excel;
// they differ for bases 2 to 4)
double DaysToNextCoupon(double settle, double mat, int freq, int basis) {
  return CoupDays(settle, mat, freq, basis) - CoupDayBs(settle, mat, freq, basis);
}

double Price(double settle, double mat, double rate, double yld, double redemption, int freq, int basis) {
  const double e = CoupDays(settle, mat, freq, basis);
  const double dsc = DaysToNextCoupon(settle, mat, freq, basis);
  const double a = CoupDayBs(settle, mat, freq, basis);
  const int n = Coupons(settle, mat, freq).num;
  const double c = 100 * rate / freq;
  if (n == 1) {
    const double t1 = c + redemption;
    const double t2 = yld / freq * dsc / e + 1;
    return t1 / t2 - c * a / e;
  }
  const double base = 1 + yld / freq;
  const double f = dsc / e;
  double p = redemption / std::pow(base, n - 1 + f);
  for (int k = 1; k <= n; ++k) p += c / std::pow(base, k - 1 + f);
  return p - c * a / e;
}

// Newton's method on a price function: finds y with f(y) = target
bool SolveYield(const std::function<double(double)>& f, double target, double guess, double* out) {
  double y = guess;
  for (int i = 0; i < 200; ++i) {
    const double v = f(y) - target;
    if (!std::isfinite(v)) return false;
    if (std::fabs(v) < 1e-10) {
      *out = y;
      return true;
    }
    const double h = 1e-7;
    const double d = (f(y + h) - f(y - h)) / (2 * h);
    if (!std::isfinite(d) || d == 0) return false;
    double ny = y - v / d;
    if (ny <= -0.99) ny = (y - 0.99) / 2;
    if (std::fabs(ny - y) < 1e-12) {
      *out = ny;
      return true;
    }
    y = ny;
  }
  return false;
}

EV FPrice(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_NUM(rate, 2);
  YC_NUM(yld, 3);
  YC_NUM(red, 4);
  YC_FREQ(freq, 5);
  YC_BASIS(basis, 6);
  if (settle >= mat || rate < 0 || yld < 0 || red <= 0) return Er(Err::Num);
  return Result(Price(settle, mat, rate, yld, red, freq, basis));
}

EV FYield(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_NUM(rate, 2);
  YC_NUM(pr, 3);
  YC_NUM(red, 4);
  YC_FREQ(freq, 5);
  YC_BASIS(basis, 6);
  if (settle >= mat || rate < 0 || pr <= 0 || red <= 0) return Er(Err::Num);
  const Coupon c = Coupons(settle, mat, freq);
  if (c.num == 1) {
    // one coupon left: Excel counts actual days for bases 1 to 3 (the period, the days accrued and the days to the
    // redemption: COUPDAYS of 2 and 3 doesn't apply), and for basis 0 the days to the redemption as 30/360 that
    // moves only the settlement's day (31 or the end of February to 30): Feb 29 to Aug 31 is 181, Nov 30 to Feb 29
    // 89 (measured on 70 cases)
    double a = CoupDayBs(settle, mat, freq, basis), e = CoupDays(settle, mat, freq, basis), dsr = CoupDaysNc(settle, mat, freq, basis);
    if (basis >= 1 && basis <= 3) {
      a = settle - c.pcd;
      e = c.ncd - c.pcd;
      dsr = c.ncd - settle;
    } else if (basis == 0) {
      Ymd s = ToYmd(settle);
      const Ymd r = ToYmd(c.ncd);
      if (s.d == 31 || (s.m == 2 && s.d == DaysInMonth(s.y, 2))) s.d = 30;
      dsr = (r.y - s.y) * 360.0 + (r.m - s.m) * 30.0 + (r.d - s.d);
    }
    const double num = (red / 100 + rate / freq) - (pr / 100 + a / e * rate / freq);
    const double den = pr / 100 + a / e * rate / freq;
    return Result(num / den * freq * e / dsr);
  }
  double y;
  if (!SolveYield([&](double v) { return Price(settle, mat, rate, v, red, freq, basis); }, pr, rate > 0 ? rate : 0.05, &y)) return Er(Err::Num);
  return Result(y);
}

double Duration(double settle, double mat, double cpn, double yld, int freq, int basis) {
  const double e = CoupDays(settle, mat, freq, basis);
  const double dsc = DaysToNextCoupon(settle, mat, freq, basis);
  const int n = Coupons(settle, mat, freq).num;
  const double base = 1 + yld / freq;
  const double f = dsc / e;
  double num = 0, den = 0;
  for (int k = 1; k <= n; ++k) {
    const double t = k - 1 + f;
    const double cf = 100 * cpn / freq + (k == n ? 100 : 0);
    const double pv = cf / std::pow(base, t);
    num += t * pv;
    den += pv;
  }
  return num / den / freq;
}

EV FDuration(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_NUM(cpn, 2);
  YC_NUM(yld, 3);
  YC_FREQ(freq, 4);
  YC_BASIS(basis, 5);
  if (settle >= mat || cpn < 0 || yld < 0) return Er(Err::Num);
  return Result(Duration(settle, mat, cpn, yld, freq, basis));
}
EV FMDuration(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_NUM(cpn, 2);
  YC_NUM(yld, 3);
  YC_FREQ(freq, 4);
  YC_BASIS(basis, 5);
  if (settle >= mat || cpn < 0 || yld < 0) return Er(Err::Num);
  return Result(Duration(settle, mat, cpn, yld, freq, basis) / (1 + yld / freq));
}

// quasi-coupon periods of an odd period from start to end (coupon dates anchored at anchor, stepping 12/freq months);
// for each: the days it counts (DC), its normal length (NL), the accrued days up to settle (A) and the days from settle
// to its end (DSC)
struct Quasi {
  double dc, nl, a, dsc;
};
std::vector<Quasi> QuasiPeriods(double start, double end, double settle, double anchor, int freq, int basis, bool forward) {
  std::vector<Quasi> out;
  const int step = 12 / freq;
  const bool eom = IsEom(anchor);
  // boundaries: coupon dates from anchor going backwards (odd first period) or forwards (odd last period)
  std::vector<double> marks;
  if (forward) {
    for (int k = 0; k < 600; ++k) {
      const double d = AddMonths(anchor, step * k, eom);
      marks.push_back(d);
      if (d >= end) break;
    }
  } else {
    for (int k = 0; k < 600; ++k) {
      const double d = AddMonths(anchor, -step * k, eom);
      marks.insert(marks.begin(), d);
      if (d <= start) break;
    }
  }
  for (size_t i = 0; i + 1 < marks.size(); ++i) {
    const double p0 = marks[i], p1 = marks[i + 1];
    const double lo = std::max(p0, start), hi = std::min(p1, end);
    if (hi <= lo) continue;
    Quasi q;
    q.nl = basis == 1 ? p1 - p0 : (basis == 3 ? 365.0 : 360.0) / freq;
    q.dc = DayCount(lo, hi, basis);
    q.a = settle > lo ? DayCount(lo, std::min(settle, hi), basis) : 0;
    q.dsc = settle < hi ? DayCount(std::max(settle, lo), hi, basis) : 0;
    out.push_back(q);
  }
  return out;
}

double OddLPrice(double settle, double mat, double last, double rate, double yld, double red, int freq, int basis) {
  const auto qs = QuasiPeriods(last, mat, settle, last, freq, basis, true);
  double sdc = 0, sa = 0, sdsc = 0;
  for (const Quasi& q : qs) {
    sdc += q.dc / q.nl;
    sa += q.a / q.nl;
    sdsc += q.dsc / q.nl;
  }
  const double c = 100 * rate / freq;
  return (red + sdc * c) / (1 + sdsc * yld / freq) - sa * c;
}

EV FOddLPrice(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_DATEARG(last, 2);
  YC_NUM(rate, 3);
  YC_NUM(yld, 4);
  YC_NUM(red, 5);
  YC_FREQ(freq, 6);
  YC_BASIS(basis, 7);
  if (!(last < settle && settle < mat) || rate < 0 || yld < 0 || red <= 0) return Er(Err::Num);
  return Result(OddLPrice(settle, mat, last, rate, yld, red, freq, basis));
}

EV FOddLYield(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_DATEARG(last, 2);
  YC_NUM(rate, 3);
  YC_NUM(pr, 4);
  YC_NUM(red, 5);
  YC_FREQ(freq, 6);
  YC_BASIS(basis, 7);
  if (!(last < settle && settle < mat) || rate < 0 || pr <= 0 || red <= 0) return Er(Err::Num);
  const auto qs = QuasiPeriods(last, mat, settle, last, freq, basis, true);
  double sdc = 0, sa = 0, sdsc = 0;
  for (const Quasi& q : qs) {
    sdc += q.dc / q.nl;
    sa += q.a / q.nl;
    sdsc += q.dsc / q.nl;
  }
  const double c = 100 * rate / freq;
  return Result(((red + sdc * c) - (pr + sa * c)) / (pr + sa * c) * freq / sdsc);
}

double OddFPrice(double settle, double mat, double issue, double first, double rate, double yld, double red, int freq, int basis) {
  const double c = 100 * rate / freq;
  const double base = 1 + yld / freq;
  // coupons after the first coupon
  const int step = 12 / freq;
  const bool eom = IsEom(mat);
  int n = 0;
  while (AddMonths(mat, -step * n, eom) > first + 0.5) ++n;
  // the odd first period, in quasi-coupon periods ending at the first coupon
  const auto qs = QuasiPeriods(issue, first, settle, first, freq, basis, false);
  double sdc = 0, sa = 0;
  for (const Quasi& q : qs) {
    sdc += q.dc / q.nl;
    sa += q.a / q.nl;
  }
  // whole quasi periods between the settlement and the first coupon, and the fraction to the next quasi coupon
  double nq = 0, frac = 0;
  // the quasi period that contains the settlement: the earliest one still ahead of it
  int idx = -1;
  for (size_t i = 0; i < qs.size(); ++i) {
    if (qs[i].dsc > 0) {
      idx = static_cast<int>(i);
      break;
    }
  }
  if (idx >= 0) {
    frac = qs[static_cast<size_t>(idx)].dsc / qs[static_cast<size_t>(idx)].nl;
    nq = static_cast<double>(qs.size()) - idx - 1;
  }
  double p = red / std::pow(base, n + nq + frac);
  p += c * sdc / std::pow(base, nq + frac);
  for (int k = 1; k <= n; ++k) p += c / std::pow(base, k + nq + frac);
  return p - c * sa;
}

EV FOddFPrice(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_DATEARG(issue, 2);
  YC_DATEARG(first, 3);
  YC_NUM(rate, 4);
  YC_NUM(yld, 5);
  YC_NUM(red, 6);
  YC_FREQ(freq, 7);
  YC_BASIS(basis, 8);
  if (!(issue < settle && settle < first && first < mat) || rate < 0 || yld < 0 || red <= 0) return Er(Err::Num);
  return Result(OddFPrice(settle, mat, issue, first, rate, yld, red, freq, basis));
}

EV FOddFYield(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_DATEARG(issue, 2);
  YC_DATEARG(first, 3);
  YC_NUM(rate, 4);
  YC_NUM(pr, 5);
  YC_NUM(red, 6);
  YC_FREQ(freq, 7);
  YC_BASIS(basis, 8);
  if (!(issue < settle && settle < first && first < mat) || rate < 0 || pr <= 0 || red <= 0) return Er(Err::Num);
  double y;
  if (!SolveYield([&](double v) { return OddFPrice(settle, mat, issue, first, rate, v, red, freq, basis); }, pr, rate > 0 ? rate : 0.05, &y))
    return Er(Err::Num);
  return Result(y);
}

EV FAccrInt(Evaluator& ev, const Node& call) {
  YC_DATEARG(issue, 0);
  YC_DATEARG(first, 1);
  YC_DATEARG(settle, 2);
  YC_NUM(rate, 3);
  YC_NUM_OPT(par, 4, 1000);
  YC_FREQ(freq, 5);
  YC_BASIS(basis, 6);
  YC_BOOL_OPT(from_issue, 7, true);
  if (issue >= settle || rate <= 0 || par <= 0) return Er(Err::Num);
  if (settle > first && issue < first) {
    // past the first coupon the interest keeps accruing from the issue (measured in Excel 16): calc_method FALSE
    // counts the days by the basis, TRUE the coupon periods, each whole one as 1 and the parts by their days over
    // the period's length (in basis 1 the length of the period that ends at the first coupon)
    if (!from_issue) return Result(par * rate * YearFraction(issue, settle, basis));
    const int step = 12 / freq;
    const bool eom = IsEom(first);
    const double fixed_nl = (basis == 3 ? 365.0 : 360.0) / freq;
    double s = 0;
    for (int k = 1; k < 1200; ++k) {  // before the first coupon, going back from it
      const double p0 = AddMonths(first, -step * k, eom), p1 = AddMonths(first, -step * (k - 1), eom);
      if (p1 <= issue) break;
      s += p0 >= issue ? 1 : DayCount(issue, p1, basis) / (basis == 1 ? p1 - p0 : fixed_nl);
    }
    double pcd = first;
    for (int k = 1; k < 1200; ++k) {  // whole periods after it
      const double nd = AddMonths(first, step * k, eom);
      if (nd > settle) break;
      s += 1;
      pcd = nd;
    }
    s += DayCount(pcd, settle, basis) / (basis == 1 ? first - AddMonths(first, -step, eom) : fixed_nl);
    return Result(par * rate / freq * s);
  }
  double start = issue;
  if (!from_issue && settle > first) {
    // accrue from the coupon date before the settlement
    const int step = 12 / freq;
    const bool eom = IsEom(first);
    double d = first;
    for (int k = 1; k < 1200; ++k) {
      const double nd = AddMonths(first, step * k, eom);
      if (nd > settle) break;
      d = nd;
    }
    start = d;
  }
  const auto qs = QuasiPeriods(start, settle, settle, first, freq, basis, start >= first);
  double s = 0;
  for (const Quasi& q : qs) s += q.dc / q.nl;
  return Result(par * rate / freq * s);
}

EV FAccrIntM(Evaluator& ev, const Node& call) {
  YC_DATEARG(issue, 0);
  YC_DATEARG(settle, 1);
  YC_NUM(rate, 2);
  YC_NUM_OPT(par, 3, 1000);
  YC_BASIS(basis, 4);
  if (issue >= settle || rate <= 0 || par <= 0) return Er(Err::Num);
  return Result(par * rate * YearFraction(issue, settle, basis));
}

// ---- discounted securities ----

#define YC_SEC_ARGS(a, b)       \
  YC_DATEARG(settle, 0);        \
  YC_DATEARG(mat, 1);           \
  YC_NUM(a, 2);                 \
  YC_NUM(b, 3);                 \
  YC_BASIS(basis, 4);           \
  if (settle >= mat) return Er(Err::Num); \
  const double yf = YearFraction(settle, mat, basis)

EV FPriceDisc(Evaluator& ev, const Node& call) {
  YC_SEC_ARGS(disc, red);
  if (disc <= 0 || red <= 0) return Er(Err::Num);
  return Result(red - disc * red * yf);
}
EV FYieldDisc(Evaluator& ev, const Node& call) {
  YC_SEC_ARGS(pr, red);
  if (pr <= 0 || red <= 0) return Er(Err::Num);
  return Result((red - pr) / pr / yf);
}
EV FDisc(Evaluator& ev, const Node& call) {
  YC_SEC_ARGS(pr, red);
  if (pr <= 0 || red <= 0) return Er(Err::Num);
  return Result((red - pr) / red / yf);
}
EV FIntRate(Evaluator& ev, const Node& call) {
  YC_SEC_ARGS(inv, red);
  if (inv <= 0 || red <= 0) return Er(Err::Num);
  return Result((red - inv) / inv / yf);
}
EV FReceived(Evaluator& ev, const Node& call) {
  YC_SEC_ARGS(inv, disc);
  if (inv <= 0 || disc <= 0) return Er(Err::Num);
  const double d = 1 - disc * yf;
  if (d <= 0) return Er(Err::Num);
  return Result(inv / d);
}

EV FPriceMat(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_DATEARG(issue, 2);
  YC_NUM(rate, 3);
  YC_NUM(yld, 4);
  YC_BASIS(basis, 5);
  if (settle >= mat || rate < 0 || yld < 0) return Er(Err::Num);
  const double b = YearDays(basis, issue, mat);
  const double dim = DayCount(issue, mat, basis), a = DayCount(issue, settle, basis), dsm = DayCount(settle, mat, basis);
  return Result((100 + dim / b * rate * 100) / (1 + dsm / b * yld) - a / b * rate * 100);
}
EV FYieldMat(Evaluator& ev, const Node& call) {
  YC_DATEARG(settle, 0);
  YC_DATEARG(mat, 1);
  YC_DATEARG(issue, 2);
  YC_NUM(rate, 3);
  YC_NUM(pr, 4);
  YC_BASIS(basis, 5);
  if (settle >= mat || rate < 0 || pr <= 0) return Er(Err::Num);
  const double b = YearDays(basis, issue, mat);
  const double dim = DayCount(issue, mat, basis), a = DayCount(issue, settle, basis), dsm = DayCount(settle, mat, basis);
  const double t = pr / 100 + a / b * rate;
  return Result(((1 + dim / b * rate) - t) / t * b / dsm);
}

// ---- Treasury bills (maturity at most one year after the settlement) ----

#define YC_TBILL(arg)                                                \
  YC_DATEARG(settle, 0);                                             \
  YC_DATEARG(mat, 1);                                                \
  YC_NUM(arg, 2);                                                    \
  const double dsm = mat - settle;                                   \
  if (dsm <= 0 || mat > AddMonths(settle, 12, false) || arg <= 0) return Er(Err::Num)

EV FTBillPrice(Evaluator& ev, const Node& call) {
  YC_TBILL(disc);
  const double p = 100 * (1 - disc * dsm / 360);
  return p <= 0 ? Er(Err::Num) : Result(p);
}
EV FTBillYield(Evaluator& ev, const Node& call) {
  YC_TBILL(pr);
  return Result((100 - pr) / pr * 360 / dsm);
}
EV FTBillEq(Evaluator& ev, const Node& call) {
  YC_TBILL(disc);
  if (dsm <= 182) return Result(365 * disc / (360 - disc * dsm));
  const double price = 100 * (1 - disc * dsm / 360);
  if (price <= 0) return Er(Err::Num);
  const double t = dsm / 365;
  return Result((-t + std::sqrt(t * t - (2 * t - 1) * (1 - 100 / price))) / (t - 0.5));
}

// ---- depreciation ----

EV FSln(Evaluator& ev, const Node& call) {
  YC_NUM(cost, 0);
  YC_NUM(salvage, 1);
  YC_NUM(life, 2);
  if (life == 0) return Er(Err::Div0);
  return Result((cost - salvage) / life);
}

EV FSyd(Evaluator& ev, const Node& call) {
  YC_NUM(cost, 0);
  YC_NUM(salvage, 1);
  YC_NUM(life, 2);
  YC_NUM(per, 3);
  if (life <= 0 || per <= 0 || per > life || salvage < 0) return Er(Err::Num);
  return Result((cost - salvage) * (life - per + 1) * 2 / (life * (life + 1)));
}

// The depreciation functions walk the periods one by one, as Excel does (its results are matched to the last bit
// that way); past this many periods the walk is replaced by its closed form (DB) or refused (VDB), so a life of
// 1E+15 periods can't keep the engine busy for hours.
constexpr double kLongWalk = 1e6;

// DB as Excel computes it (Excel 16, measured): the life is used as given (10.9 years is a rate of 1 - (s/c)^(1/10.9)),
// the period is cut to a whole number and one below 1 is the first; the period after the life is the rest of the last
// year, and a first year of 12 months leaves none of it (#NUM!). A life under one period has only that rest.
EV FDb(Evaluator& ev, const Node& call) {
  YC_NUM(cost, 0);
  YC_NUM(salvage, 1);
  YC_NUM(life, 2);
  YC_NUM(period, 3);
  YC_NUM_OPT_BLANK0(month, 4, 12);
  month = std::trunc(month);
  if (cost < 0 || salvage < 0 || life <= 0 || period <= 0 || month < 1 || month > 12) return Er(Err::Num);
  period = std::max(1.0, std::trunc(period));
  if (period > life + (month < 12 ? 1 : 0)) return Er(Err::Num);
  if (cost == 0) return Num(0);
  const double rate = std::round((1 - std::pow(salvage / cost, 1 / life)) * 1000) / 1000;
  double total = cost * rate * month / 12, dep = total;
  const double whole = std::min(period, std::floor(life));  // the last period depreciated at the whole rate
  if (whole > kLongWalk) {
    const double left = (cost - total) * std::pow(1 - rate, whole - 2);  // the value before that period
    dep = left * rate;
    total = cost - left * (1 - rate);
  } else {
    for (double p = 2; p <= whole; ++p) {
      dep = (cost - total) * rate;
      total += dep;
    }
  }
  if (period > life) dep = (cost - total) * rate * (12 - month) / 12;
  return Result(dep);
}

double Ddb(double cost, double salvage, double life, double period, double factor) {
  double rate = factor / life, old_value;
  if (rate >= 1) {
    rate = 1;
    old_value = period == 1 ? cost : 0;
  } else {
    old_value = cost * std::pow(1 - rate, period - 1);
  }
  const double new_value = cost * std::pow(1 - rate, period);
  double d = new_value < salvage ? old_value - salvage : old_value - new_value;
  return d < 0 ? 0 : d;
}

// DDB (Excel 16, measured): a period below 1 is the first, a period past the life is #NUM! (but the first period of a
// life under one period isn't), and a period with a fraction is the balance's fall over the year ending there
EV FDdb(Evaluator& ev, const Node& call) {
  YC_NUM(cost, 0);
  YC_NUM(salvage, 1);
  YC_NUM(life, 2);
  YC_NUM(period, 3);
  YC_NUM_OPT_BLANK0(factor, 4, 2);
  if (cost < 0 || salvage < 0 || life <= 0 || period <= 0 || (period > life && period > 1) || factor <= 0) return Er(Err::Num);
  return Result(Ddb(cost, salvage, life, std::max(period, 1.0), factor));
}

// VDB with the switch to the straight line, as Excel computes it (Excel 16, 1,713 cases measured; the two-part
// formula LibreOffice uses differs over periods with a fraction). One walk over the periods, on a grid that starts at
// start's fraction (0.3, 1.3, 2.3 ...: the part before it is a short period of its own, not counted):
//  - a period takes rate * book, or what is left above the salvage value when that is less;
//  - a period whose declining balance isn't cut is compared with the straight line over the life left, and from the
//    first period the line is more, every period takes that line's amount;
//  - a period cut by `end` takes its share, by length.
// A life under one period is the straight line over that life. false: too many periods to walk.
bool VdbSwitch(double cost, double salvage, double life, double start, double end, double factor, double* out) {
  if (life < 1) {
    *out = (cost - salvage) * (end - start) / life;
    return true;
  }
  if (end > kLongWalk) return false;
  const double rate = factor / life, int_start = std::floor(start), frac = start - int_start;
  double book = cost, total = 0, line = 0;
  bool straight = false;
  auto period = [&](double t, double length, bool counted) {
    double dep = line;
    if (!straight) {
      const double declining = rate * book;
      if (book - declining < salvage) {
        dep = book - salvage;
      } else {
        line = (book - salvage) / (life - t);
        straight = line > declining;
        dep = straight ? line : declining;
      }
    }
    dep *= length;
    book -= dep;
    if (counted) total += dep;
  };
  if (frac > 0) period(0, frac, false);
  for (double k = 0; frac + k < end; ++k) period(frac + k, std::min(1.0, end - (frac + k)), k >= int_start);
  *out = total;
  return true;
}

// VDB (Excel 16, measured): a factor of 0 is taken (the straight line from the start), a life of 0 is #DIV/0! when
// nothing else is wrong, and the factor or no_switch left empty is 0 / FALSE
EV FVdb(Evaluator& ev, const Node& call) {
  YC_NUM(cost, 0);
  YC_NUM(salvage, 1);
  YC_NUM(life, 2);
  YC_NUM(start, 3);
  YC_NUM(end, 4);
  YC_NUM_OPT_BLANK0(factor, 5, 2);
  YC_BOOL_OPT(no_switch, 6, false);
  if (cost < 0 || salvage < 0 || life < 0 || start < 0 || end < start || end > life || factor < 0) return Er(Err::Num);
  if (life == 0) return Er(Err::Div0);
  double vdb = 0;
  if (!no_switch) return VdbSwitch(cost, salvage, life, start, end, factor, &vdb) ? Result(vdb) : Er(Err::Num);
  // without the switch: the declining balance of each period, the first and the last by their share
  const double int_start = std::floor(start), int_end = std::ceil(end);
  if (int_end - int_start > kLongWalk) return Er(Err::Num);
  for (double i = int_start + 1; i <= int_end; ++i) {
    double term = Ddb(cost, salvage, life, i, factor);
    if (i == int_start + 1) term *= std::min(end, int_start + 1) - start;
    else if (i == int_end) term *= end + 1 - int_end;
    vdb += term;
  }
  return Result(vdb);
}

EV FAmorLinc(Evaluator& ev, const Node& call) {
  YC_NUM(cost, 0);
  YC_DATEARG(bought, 1);
  YC_DATEARG(first, 2);
  YC_NUM(salvage, 3);
  YC_NUM(per, 4);
  YC_NUM(rate, 5);
  YC_BASIS(basis, 6);
  if (cost < 0 || salvage < 0 || salvage > cost || per < 0 || rate <= 0 || bought > first || basis == 2) return Er(Err::Num);
  const double one = cost * rate, delta = cost - salvage;
  const double r0 = YearFraction(bought, first, basis) * rate * cost;
  const double full = std::floor((cost - salvage - r0) / one);
  const double p = std::trunc(per);
  if (p == 0) return Result(r0);
  if (p <= full) return Result(one);
  if (p == full + 1) return Result(delta - one * full - r0);
  return Num(0);
}

EV FAmorDegrc(Evaluator& ev, const Node& call) {
  YC_NUM(cost, 0);
  YC_DATEARG(bought, 1);
  YC_DATEARG(first, 2);
  YC_NUM(salvage, 3);
  YC_NUM(per, 4);
  YC_NUM(rate, 5);
  YC_BASIS(basis, 6);
  if (cost < 0 || salvage < 0 || salvage > cost || per < 0 || rate <= 0 || bought > first || basis == 2) return Er(Err::Num);
  const double use = 1 / rate;
  if (use >= 3 && use <= 4) return Er(Err::Num);  // Excel: life between 3 and 4 years is not allowed
  const double coeff = use < 3 ? 1 : use < 5 ? 1.5 : use <= 6 ? 2 : 2.5;
  rate *= coeff;
  double n_rate = std::round(YearFraction(bought, first, basis) * rate * cost);
  cost -= n_rate;
  double rest = cost - salvage;
  const int periods = static_cast<int>(per);
  for (int n = 0; n < periods; ++n) {
    n_rate = std::round(rate * cost);
    rest -= n_rate;
    if (rest < 0) {
      switch (periods - n) {
        case 0:
        case 1: return Num(std::round(cost * 0.5));
        default: return Num(0);
      }
    }
    cost -= n_rate;
  }
  return Result(n_rate);
}

// ---- rates ----

EV FEffect(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(n, 1);
  n = std::trunc(n);
  if (r <= 0 || n < 1) return Er(Err::Num);
  return Result(std::pow(1 + r / n, n) - 1);
}
EV FNominal(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(n, 1);
  n = std::trunc(n);
  if (r <= 0 || n < 1) return Er(Err::Num);
  return Result(n * (std::pow(1 + r, 1 / n) - 1));
}
EV FFvSchedule(Evaluator& ev, const Node& call) {
  YC_NUM(p, 0);
  const auto a = ev.ToArray(ev.Eval(call.kids[1]));
  if (!a) return Er(Err::Value);
  for (const Value& v : a->v) {
    if (v.IsError()) return EV::S(v);
    if (v.IsEmpty()) continue;
    double r;
    Err e = Err::None;
    if (!Evaluator::ToNumber(v, &r, &e)) return Er(Err::Value);
    p *= 1 + r;
  }
  return Result(p);
}
EV FIsPmt(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(per, 1);
  YC_NUM(n, 2);
  YC_NUM(pv, 3);
  if (n == 0) return Er(Err::Div0);
  return Result(pv * r * (per / n - 1));
}
EV FMirr(Evaluator& ev, const Node& call) {
  const auto a = ev.ToArray(ev.Eval(call.kids[0]));
  if (!a) return Er(Err::Value);
  YC_NUM(fr, 1);
  YC_NUM(rr, 2);
  std::vector<double> v;
  for (const Value& x : a->v) {
    if (x.IsError()) return EV::S(x);
    if (x.IsNumeric()) v.push_back(x.AsDouble());
  }
  const double n = static_cast<double>(v.size());
  double pos = 0, neg = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] > 0) pos += v[i] / std::pow(1 + rr, static_cast<double>(i));
    else neg += v[i] / std::pow(1 + fr, static_cast<double>(i));
  }
  if (pos == 0 || neg == 0 || n < 2) return Er(Err::Div0);
  return Result(std::pow(-pos * std::pow(1 + rr, n) / (neg * (1 + rr)), 1 / (n - 1)) - 1);
}
EV FPDuration(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(pv, 1);
  YC_NUM(fv, 2);
  if (r <= 0 || pv <= 0 || fv <= 0) return Er(Err::Num);
  return Result((std::log(fv) - std::log(pv)) / std::log(1 + r));
}
EV FRri(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(pv, 1);
  YC_NUM(fv, 2);
  if (n <= 0 || pv == 0) return Er(Err::Num);
  return Result(std::pow(fv / pv, 1 / n) - 1);
}

// IPMT / PPMT of one period (same formulas as fn_fin.cpp)
double PmtOf(double r, double n, double pv, double fv, int type) {
  if (r == 0) return -(pv + fv) / n;
  const double g = std::pow(1 + r, n);
  return -(r * (fv + pv * g)) / ((1 + r * type) * (g - 1));
}
double FvOf(double r, double n, double pmt, double pv, int type) {
  if (r == 0) return -(pv + pmt * n);
  const double g = std::pow(1 + r, n);
  return -(pv * g + pmt * (1 + r * type) * (g - 1) / r);
}
double IpmtOf(double r, double per, double n, double pv, int type) {
  const double pmt = PmtOf(r, n, pv, 0, type);
  if (type && per == 1) return 0;
  double ip = FvOf(r, per - 1, pmt, pv, type) * r;
  if (type) ip /= 1 + r;
  return ip;
}

EV Cum(Evaluator& ev, const Node& call, bool principal) {
  YC_NUM(r, 0);
  YC_NUM(n, 1);
  YC_NUM(pv, 2);
  YC_NUM(s, 3);
  YC_NUM(e, 4);
  YC_NUM(typef, 5);
  s = std::ceil(s);
  e = std::floor(e);
  n = std::ceil(n);
  if (r <= 0 || n <= 0 || pv <= 0 || s < 1 || e < s || e > n || (typef != 0 && typef != 1)) return Er(Err::Num);
  const int type = static_cast<int>(typef);
  const double pmt = PmtOf(r, n, pv, 0, type);
  double total = 0;
  for (double p = s; p <= e; ++p) {
    const double ip = IpmtOf(r, p, n, pv, type);
    total += principal ? pmt - ip : ip;
  }
  return Result(total);
}
EV FCumIPmt(Evaluator& ev, const Node& call) { return Cum(ev, call, false); }
EV FCumPrinc(Evaluator& ev, const Node& call) { return Cum(ev, call, true); }

EV FDollarDe(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(f, 1);
  f = std::trunc(f);
  if (f < 0) return Er(Err::Num);
  if (f == 0) return Er(Err::Div0);
  const double ip = std::trunc(x);
  return Result(ip + (x - ip) * std::pow(10.0, std::ceil(std::log10(f))) / f);
}
EV FDollarFr(Evaluator& ev, const Node& call) {
  YC_NUM(x, 0);
  YC_NUM(f, 1);
  f = std::trunc(f);
  if (f < 0) return Er(Err::Num);
  if (f == 0) return Er(Err::Div0);
  const double ip = std::trunc(x);
  return Result(ip + (x - ip) * f / std::pow(10.0, std::ceil(std::log10(f))));
}

}  // namespace

void RegisterFinancial2(std::vector<FuncInfo>& t) {
  t.push_back({"COUPDAYBS", 3, 4, FCoupDayBs});
  t.push_back({"COUPDAYS", 3, 4, FCoupDays});
  t.push_back({"COUPDAYSNC", 3, 4, FCoupDaysNc});
  t.push_back({"COUPNCD", 3, 4, FCoupNcd});
  t.push_back({"COUPNUM", 3, 4, FCoupNum});
  t.push_back({"COUPPCD", 3, 4, FCoupPcd});
  t.push_back({"PRICE", 6, 7, FPrice});
  t.push_back({"YIELD", 6, 7, FYield});
  t.push_back({"DURATION", 5, 6, FDuration});
  t.push_back({"MDURATION", 5, 6, FMDuration});
  t.push_back({"ODDLPRICE", 7, 8, FOddLPrice});
  t.push_back({"ODDLYIELD", 7, 8, FOddLYield});
  t.push_back({"ODDFPRICE", 8, 9, FOddFPrice});
  t.push_back({"ODDFYIELD", 8, 9, FOddFYield});
  t.push_back({"ACCRINT", 6, 8, FAccrInt});
  t.push_back({"ACCRINTM", 3, 5, FAccrIntM});
  t.push_back({"PRICEDISC", 4, 5, FPriceDisc});
  t.push_back({"YIELDDISC", 4, 5, FYieldDisc});
  t.push_back({"DISC", 4, 5, FDisc});
  t.push_back({"INTRATE", 4, 5, FIntRate});
  t.push_back({"RECEIVED", 4, 5, FReceived});
  t.push_back({"PRICEMAT", 5, 6, FPriceMat});
  t.push_back({"YIELDMAT", 5, 6, FYieldMat});
  t.push_back({"TBILLPRICE", 3, 3, FTBillPrice});
  t.push_back({"TBILLYIELD", 3, 3, FTBillYield});
  t.push_back({"TBILLEQ", 3, 3, FTBillEq});
  t.push_back({"SLN", 3, 3, FSln});
  t.push_back({"SYD", 4, 4, FSyd});
  t.push_back({"DB", 4, 5, FDb});
  t.push_back({"DDB", 4, 5, FDdb});
  t.push_back({"VDB", 5, 7, FVdb});
  t.push_back({"AMORLINC", 6, 7, FAmorLinc});
  t.push_back({"AMORDEGRC", 6, 7, FAmorDegrc});
  t.push_back({"EFFECT", 2, 2, FEffect});
  t.push_back({"NOMINAL", 2, 2, FNominal});
  t.push_back({"FVSCHEDULE", 2, 2, FFvSchedule});
  t.push_back({"ISPMT", 4, 4, FIsPmt});
  t.push_back({"MIRR", 3, 3, FMirr});
  t.push_back({"PDURATION", 3, 3, FPDuration});
  t.push_back({"RRI", 3, 3, FRri});
  t.push_back({"CUMIPMT", 6, 6, FCumIPmt});
  t.push_back({"CUMPRINC", 6, 6, FCumPrinc});
  t.push_back({"DOLLARDE", 2, 2, FDollarDe});
  t.push_back({"DOLLARFR", 2, 2, FDollarFr});
}

}  // namespace yc::formula::fn
