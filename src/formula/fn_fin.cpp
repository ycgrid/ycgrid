// SPDX-License-Identifier: Apache-2.0
// fn_fin.cpp - financial functions (PLAN §6.5): PV FV PMT NPER RATE NPV XNPV IRR XIRR IPMT PPMT
//
// Cash flows follow Excel's sign convention (money paid out is negative). RATE follows Excel's own secant search (see
// FRate); IRR and XIRR are solved with Newton's method from the guess; no convergence -> #NUM! (like Excel).
#include "formula/fn_util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace yc::formula::fn {
namespace {

// ---- time value of money (type 0 = payments at the end of each period, 1 = at the beginning) ----

bool WholePower(double n) { return n >= -9007199254740992.0 && n <= 9007199254740992.0 && n == std::floor(n); }

// x^k by repeated squaring, as Excel's ^ and POWER compute whole powers
double SquarePow(double x, unsigned long long k) {
  double res = 1, base = x;
  while (k != 0) {
    if (k & 1) res *= base;
    base *= base;
    k >>= 1;
  }
  return res;
}

// (1 + r)^n as Excel's financial functions compute it, measured to the last bit in Excel 16 (FV, PV and RATE): a
// whole power by repeated squaring (a negative one as 1 over it), any other as exp(n ln x)
double Growth(double x, double n) {
  if (WholePower(n)) {
    if (n >= 0) return SquarePow(x, static_cast<unsigned long long>(n));
    return 1 / SquarePow(x, static_cast<unsigned long long>(-n));
  }
  if (x > 0) return std::exp(n * std::log(x));
  return std::pow(x, n);
}

// (1 + r)^n - 1 without losing the digits of a rate near 0, for PMT: a whole power squares 1 + u as u' = 2u + u*u,
// any other is expm1(n ln(1 + r)). PMT(1E-12,12,1200) is -100.00000000065; Excel's PMT is within 4 units in the
// last place of this one (measured: 80 cases, 32 to the last bit; std::pow was up to 300 units away)
double GrowthM1(double r, double n) {
  if (WholePower(n) && n >= 0) {
    auto k = static_cast<unsigned long long>(n);
    double res = 0, base = r;
    bool first = true;
    while (k != 0) {
      if (k & 1) {
        res = first ? base : res + base + res * base;
        first = false;
      }
      k >>= 1;
      if (k != 0) base = 2 * base + base * base;
    }
    return res;
  }
  return r > -1 ? std::expm1(n * std::log1p(r)) : Growth(1 + r, n) - 1;
}

// FV and PV in Excel's order of operations (measured to the last bit: FV(0.05/12,360,-100), whole and fractional
// periods, negative ones; FV(1E-10,12,-100) is 1200.0000992884452)
double Fv(double r, double n, double pmt, double pv, double type) {
  if (r == 0) return -(pv + pmt * n);
  const double g = Growth(1 + r, n);
  return -(pv * g + pmt * ((1 + r * type) * ((g - 1) / r)));
}

double Pv(double r, double n, double pmt, double fv, double type) {
  if (r == 0) return -(fv + pmt * n);
  const double g = Growth(1 + r, n);
  return -(fv + pmt * ((1 + r * type) * ((g - 1) / r))) / g;
}

double Pmt(double r, double n, double pv, double fv, double type) {
  if (r == 0) return -(pv + fv) / n;
  const double gm1 = GrowthM1(r, n);
  // so many periods that (1 + r)^n overflows: the payment is the interest alone (PMT(0.05/12,1E6,1000), Excel)
  if (std::isinf(gm1) && gm1 > 0) return -(r * pv) / (1 + r * type);
  return -(r * (fv + pv * (1 + gm1))) / ((1 + r * type) * gm1);
}

// IPMT is the rate times the balance after per - 1 periods, with (1 + r)^k - 1 kept to its digits (GrowthM1): Fv's
// own g - 1 lost them over few periods at a small rate, and IPMT(0.003,10,24.5,8.564E6,-1E4) ended 113 units in
// the last place from Excel's, which is within 2 of the exact value (measured: 21 cases, now within 3 of Excel's)
double Ipmt(double r, double per, double n, double pv, double fv, double type) {
  const double pmt = Pmt(r, n, pv, fv, type);
  if (type != 0 && per == 1) return 0;
  double ip;
  if (r == 0) {
    ip = Fv(r, per - 1, pmt, pv, type) * r;
  } else {
    const double gm1 = GrowthM1(r, per - 1);
    ip = -(pv * (1 + gm1) + pmt * ((1 + r * type) * (gm1 / r))) * r;
  }
  if (type != 0) ip /= 1 + r;
  return ip;
}

// PPMT is PMT - IPMT while that keeps its digits; where the two nearly cancel (the first periods of a long loan at a
// high rate) it is the first period's principal grown by (1 + r) a period, which the principal parts do exactly:
// PPMT(0.08,10,360,8.172E3,0,1) is -1.1226811053165241E-09 in Excel (within 6 units in the last place of the exact
// value) and was -1.1228848961764015E-09 as a difference (measured: 19 cases, now within 23 units of Excel's)
double Ppmt(double r, double per, double n, double pv, double fv, double type) {
  const double pmt = Pmt(r, n, pv, fv, type);
  const double diff = pmt - Ipmt(r, per, n, pv, fv, type);
  if (r == 0 || (type != 0 && per == 1) || std::fabs(diff) >= 0.25 * std::fabs(pmt)) return diff;
  const double grown = -(r * (pv + fv)) / GrowthM1(r, n) * Growth(1 + r, per - 1 - type);
  return std::isfinite(grown) ? grown : diff;
}

// Newton's method with a numeric derivative; keeps the rate above -1
bool Solve(const std::function<double(double)>& f, double guess, double* out) {
  double x = guess;
  for (int i = 0; i < 100; ++i) {
    const double fx = f(x);
    if (!std::isfinite(fx)) return false;
    const double h = std::max(1e-9, std::fabs(x) * 1e-7);
    const double d = (f(x + h) - f(x - h)) / (2 * h);
    if (!std::isfinite(d) || d == 0) return false;
    double nx = x - fx / d;
    if (nx <= -1) nx = (x - 1) / 2;  // stay in the domain of (1 + r)^n
    if (std::fabs(nx - x) <= 1e-10 * std::max(1.0, std::fabs(x))) {
      if (!std::isfinite(nx) || std::fabs(f(nx)) > 1e-6 * std::max(1.0, std::fabs(f(guess)))) return false;
      *out = nx;
      return true;
    }
    x = nx;
  }
  return false;
}

EV Result(double v) { return std::isfinite(v) ? Num(v) : Er(Err::Num); }

// numbers of one argument (range, array or value), in order; text / logicals / blanks inside ranges are skipped
bool Numbers(Evaluator& ev, const Node& n, std::vector<double>* out, Value* err) {
  const auto a = ev.ToArray(ev.Eval(n));
  if (!a) {
    *err = Value::Error(Err::Value);
    return false;
  }
  for (const Value& v : a->v) {
    if (v.IsError()) {
      *err = v;
      return false;
    }
    if (v.IsNumeric()) out->push_back(v.AsDouble());
  }
  return true;
}

// every cell must be a number (XNPV / XIRR values and dates)
bool StrictNumbers(Evaluator& ev, const Node& n, std::vector<double>* out, Value* err) {
  const auto a = ev.ToArray(ev.Eval(n));
  if (!a) {
    *err = Value::Error(Err::Value);
    return false;
  }
  for (const Value& v : a->v) {
    if (v.IsError()) {
      *err = v;
      return false;
    }
    if (!v.IsNumeric()) {
      *err = Value::Error(Err::Value);
      return false;
    }
    out->push_back(v.AsDouble());
  }
  return true;
}

EV FPv(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(n, 1);
  YC_NUM(pmt, 2);
  YC_NUM_OPT(fv, 3, 0);
  YC_NUM_OPT(type, 4, 0);
  if (r == -1) return Er(Err::Div0);  // (1 + r)^n is 0 (Excel)
  return Result(Pv(r, n, pmt, fv, type != 0 ? 1 : 0));
}

EV FFv(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(n, 1);
  YC_NUM(pmt, 2);
  YC_NUM_OPT(pv, 3, 0);
  YC_NUM_OPT(type, 4, 0);
  return Result(Fv(r, n, pmt, pv, type != 0 ? 1 : 0));
}

EV FPmt(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(n, 1);
  YC_NUM(pv, 2);
  YC_NUM_OPT(fv, 3, 0);
  YC_NUM_OPT(type, 4, 0);
  if (n == 0 || r <= -1) return Er(Err::Num);  // PMT(-1,10,100) is #NUM!, as in Excel
  return Result(Pmt(r, n, pv, fv, type != 0 ? 1 : 0));
}

EV FNper(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(pmt, 1);
  YC_NUM(pv, 2);
  YC_NUM_OPT(fv, 3, 0);
  YC_NUM_OPT(type_in, 4, 0);
  const double type = type_in != 0 ? 1 : 0;
  if (r == 0) {
    if (pmt == 0) return Er(Err::Div0);  // NPER(0,0,100) is #DIV/0!, as in Excel
    return Result(-(pv + fv) / pmt);
  }
  const double a = pmt * (1 + r * type);
  const double num = a - fv * r, den = a + pv * r;
  if (den == 0 || num / den <= 0 || 1 + r <= 0) return Er(Err::Num);
  return Result(std::log(num / den) / std::log(1 + r));
}

EV FIpmt(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(per, 1);
  YC_NUM(n, 2);
  YC_NUM(pv, 3);
  YC_NUM_OPT(fv, 4, 0);
  YC_NUM_OPT(type, 5, 0);
  if (per < 1 || per > n || n == 0) return Er(Err::Num);
  return Result(Ipmt(r, per, n, pv, fv, type != 0 ? 1 : 0));
}

EV FPpmt(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  YC_NUM(per, 1);
  YC_NUM(n, 2);
  YC_NUM(pv, 3);
  YC_NUM_OPT(fv, 4, 0);
  YC_NUM_OPT(type_in, 5, 0);
  if (per < 1 || per > n || n == 0) return Er(Err::Num);
  const double type = type_in != 0 ? 1 : 0;
  return Result(Ppmt(r, per, n, pv, fv, type));
}

// RATE searches as Excel does, measured to the last bit in Excel 16 (1,925 of 1,953 cases): the secant method from
// the guess (point 0) and a second point 0.001 away (point 1, toward the root when f(guess) > 0), at most 200 steps;
// it stops when a step moves less than 1E-7 and |f| is below 1E-7. A search that ends otherwise, after 200 steps or
// where the last two points have the same f (large amounts: the cash flows' rounding keeps |f| a few units in the
// last place above 1E-7, and the points step back and forth by one unit), returns the last point whose number is a
// multiple of 3 (Excel keeps the points in three slots and reads the first) if it moved less than 1E-4 from the
// point before it, |f| there is below 1E-4 and the rate is above -0.9999: RATE(360,-1E7,1E9) is 0.0096892458225819218
// and RATE(20,0,-100,0) (no root) -0.99911207191253926; RATE(360,-1E10,1E12), where |f| stays at 0.004, is #NUM!.
// A rate at or below -1 + 1E-7 is #NUM!; a guess at or below -1 is #VALUE!.
EV FRate(Evaluator& ev, const Node& call) {
  YC_NUM(n, 0);
  YC_NUM(pmt, 1);
  YC_NUM(pv, 2);
  YC_NUM_OPT(fv, 3, 0);
  YC_NUM_OPT(type_in, 4, 0);
  YC_NUM_OPT(guess, 5, Argc(call) > 5 ? 0 : 0.1);  // RATE(12,-100,1000,,,): an empty guess is 0
  if (n <= 0) return Er(Err::Num);
  if (guess <= -1) return Er(Err::Value);
  const double type = type_in != 0 ? 1 : 0;
  // future value of everything at rate r, in Excel's order of operations; RATE is the r that makes it zero
  auto f = [&](double r) {
    if (r == 0) return pv + pmt * n + fv;
    const double g = Growth(1 + r, n);
    return pv * g + pmt * ((1 + r * type) * ((g - 1) / r)) + fv;
  };
  constexpr double kTol = 1e-7;
  constexpr int kSteps = 200;
  std::array<double, kSteps + 2> x{}, fx{};  // point k of the search and f there
  x[0] = guess;
  fx[0] = f(guess);
  x[1] = guess + (fx[0] > 0 ? -1e-3 : 1e-3);
  fx[1] = f(x[1]);
  int last = 1;
  for (int k = 2; k < kSteps + 2; ++k) {
    const double a = x[k - 2], fa = fx[k - 2], b = x[k - 1], fb = fx[k - 1];
    if (fb == fa) break;  // no slope left
    const double c = b - fb * (b - a) / (fb - fa), fc = f(c);
    x[k] = c;
    fx[k] = fc;
    last = k;
    if (std::fabs(c - b) < kTol && std::fabs(fc) < kTol) return c > -1 + kTol ? Result(c) : Er(Err::Num);
  }
  const int m = last - last % 3;
  const double r = x[m], before = m > 0 ? x[m - 1] : x[1];
  if (!(std::fabs(r - before) < 1e-4 && 1 + r > 1e-4 && std::fabs(fx[m]) < 1e-4 && r > -1 + kTol)) {
    return Er(Err::Num);
  }
  return Result(r);
}

EV FNpv(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  if (r == -1) return Er(Err::Div0);
  std::vector<double> vals;
  Value err;
  if (!CollectNumbers(ev, call, &vals, &err, 1)) return EV::S(err);
  double s = 0, g = 1;
  for (double v : vals) {
    g *= 1 + r;
    s += v / g;
  }
  return Result(s);
}

EV FIrr(Evaluator& ev, const Node& call) {
  std::vector<double> vals;
  Value err;
  if (!Numbers(ev, call.kids[0], &vals, &err)) return EV::S(err);
  YC_NUM_OPT(guess, 1, 0.1);
  const bool pos = std::any_of(vals.begin(), vals.end(), [](double v) { return v > 0; });
  const bool neg = std::any_of(vals.begin(), vals.end(), [](double v) { return v < 0; });
  if (!pos || !neg) return Er(Err::Num);
  auto npv = [&](double r) {
    double s = 0, g = 1;
    for (double v : vals) {
      s += v / g;
      g *= 1 + r;
    }
    return s;
  };
  double r = 0;
  if (!Solve(npv, guess, &r)) return Er(Err::Num);
  return Result(r);
}

// XNPV / XIRR: values and dates of the same size; every date on or after the first
bool CashFlows(Evaluator& ev, const Node& values, const Node& dates, std::vector<double>* v, std::vector<double>* d, Value* err) {
  if (!StrictNumbers(ev, values, v, err) || !StrictNumbers(ev, dates, d, err)) return false;
  if (v->size() != d->size() || v->empty()) {
    *err = Value::Error(Err::Num);
    return false;
  }
  for (double& x : *d) {
    x = std::floor(x);
    if (x < (*d)[0]) {
      *err = Value::Error(Err::Num);
      return false;
    }
  }
  return true;
}

double Xnpv(double r, const std::vector<double>& v, const std::vector<double>& d) {
  double s = 0;
  for (size_t i = 0; i < v.size(); ++i) s += v[i] / std::pow(1 + r, (d[i] - d[0]) / 365.0);
  return s;
}

EV FXnpv(Evaluator& ev, const Node& call) {
  YC_NUM(r, 0);
  if (r <= -1 || r == 0) return Er(Err::Num);  // XNPV(0,...) is #NUM! in Excel
  std::vector<double> v, d;
  Value err;
  if (!CashFlows(ev, call.kids[1], call.kids[2], &v, &d, &err)) return EV::S(err);
  return Result(Xnpv(r, v, d));
}

EV FXirr(Evaluator& ev, const Node& call) {
  std::vector<double> v, d;
  Value err;
  if (!CashFlows(ev, call.kids[0], call.kids[1], &v, &d, &err)) return EV::S(err);
  YC_NUM_OPT(guess, 2, 0.1);
  const bool pos = std::any_of(v.begin(), v.end(), [](double x) { return x > 0; });
  const bool neg = std::any_of(v.begin(), v.end(), [](double x) { return x < 0; });
  if (!pos || !neg) return Er(Err::Num);
  double r = 0;
  if (!Solve([&](double x) { return Xnpv(x, v, d); }, guess, &r)) return Er(Err::Num);
  return Result(r);
}

}  // namespace

void RegisterFinancial(std::vector<FuncInfo>& t) {
  t.push_back({"PV", 3, 5, FPv});
  t.push_back({"FV", 3, 5, FFv});
  t.push_back({"PMT", 3, 5, FPmt});
  t.push_back({"NPER", 3, 5, FNper});
  t.push_back({"RATE", 3, 6, FRate});
  t.push_back({"IPMT", 4, 6, FIpmt});
  t.push_back({"PPMT", 4, 6, FPpmt});
  t.push_back({"NPV", 2, 255, FNpv});
  t.push_back({"IRR", 1, 2, FIrr});
  t.push_back({"XNPV", 3, 3, FXnpv});
  t.push_back({"XIRR", 2, 3, FXirr});
}

}  // namespace yc::formula::fn
