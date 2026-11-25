// SPDX-License-Identifier: Apache-2.0
#include "core/decimal.h"

#include <algorithm>
#include <cmath>

namespace yc::core {
namespace {

const Int128 kMax112 = Int128::FromParts((int64_t{1} << 47) - 1, ~uint64_t{0});
const Int128 kMin112 = -Int128::FromParts(int64_t{1} << 47, 0);

// unsigned comparison and shifts
bool ULess(const Int128& a, const Int128& b) {
  const uint64_t ah = static_cast<uint64_t>(a.hi), bh = static_cast<uint64_t>(b.hi);
  return ah != bh ? ah < bh : a.lo < b.lo;
}
Int128 Shl1(const Int128& a) {
  return Int128::FromParts(static_cast<int64_t>((static_cast<uint64_t>(a.hi) << 1) | (a.lo >> 63)), a.lo << 1);
}

}  // namespace

bool DecFits(const Int128& m) { return !(m > kMax112) && !(m < kMin112); }

void DivMod128(const Int128& n, const Int128& d, Int128* q, Int128* r) {
  Int128 quo, rem;
  for (int i = 127; i >= 0; --i) {
    rem = Shl1(rem);
    const uint64_t bit = i >= 64 ? (static_cast<uint64_t>(n.hi) >> (i - 64)) & 1 : (n.lo >> i) & 1;
    rem.lo |= bit;
    if (!ULess(rem, d)) {
      rem = rem - d;
      if (i >= 64) quo.hi = static_cast<int64_t>(static_cast<uint64_t>(quo.hi) | (uint64_t{1} << (i - 64)));
      else quo.lo |= uint64_t{1} << i;
    }
  }
  *q = quo;
  *r = rem;
}

bool DecParse(std::string_view s, Dec* out) {
  while (!s.empty() && (s.front() == ' ')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ')) s.remove_suffix(1);
  if (s.empty()) return false;
  bool neg = false;
  size_t i = 0;
  if (s[0] == '-' || s[0] == '+') {
    neg = s[0] == '-';
    i = 1;
  }
  Int128 m;
  int scale = 0;
  bool dot = false, digits = false;
  const Int128 ten(10);
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '.') {
      if (dot) return false;
      dot = true;
      continue;
    }
    if (c < '0' || c > '9') return false;
    digits = true;
    if (dot) {
      if (scale >= kDecMaxScale) continue;  // extra decimal places are truncated
      ++scale;
    }
    Int128 t;
    if (Int128::MulOverflow(m, ten, &t)) return false;
    m = t + Int128(c - '0');
    if (!DecFits(m)) return false;
  }
  if (!digits) return false;
  out->m = neg ? -m : m;
  out->scale = scale;
  return true;
}

std::string DecToString(const Dec& d) {
  std::string s = d.m.Abs().ToString();
  if (d.scale > 0) {
    if (static_cast<int>(s.size()) <= d.scale) s.insert(0, static_cast<size_t>(d.scale - static_cast<int>(s.size()) + 1), '0');
    s.insert(s.size() - static_cast<size_t>(d.scale), 1, '.');
  }
  if (d.m.IsNegative()) s.insert(0, 1, '-');
  return s;
}

Dec DecFromValue(const Value& v) { return Dec{v.dec_mantissa(), v.dec_scale()}; }

Value DecToValue(const Dec& d) {
  if (!DecFits(d.m) || d.scale < 0 || d.scale > kDecMaxScale) return Value::Error(Err::Num);
  return Value::DecimalRaw(d.m, d.scale);
}

double DecToDouble(const Dec& d) { return d.m.ToDouble() / Pow10(d.scale).ToDouble(); }

bool DecFromDouble(double v, int scale, Dec* out) {
  const double scaled = std::round(v * std::pow(10.0, scale));
  Int128 m;
  if (!Int128::FromDouble(scaled, &m) || !DecFits(m)) return false;
  out->m = m;
  out->scale = scale;
  return true;
}

bool DecRescale(const Dec& d, int ns, Dec* out) {
  if (ns == d.scale) {
    *out = d;
    return true;
  }
  if (ns > d.scale) {
    Int128 m;
    if (Int128::MulOverflow(d.m, Pow10(ns - d.scale), &m) || !DecFits(m)) return false;
    *out = Dec{m, ns};
    return true;
  }
  // scale down: divide by 10^k, round half away from zero
  Int128 m = d.m;
  for (int k = d.scale - ns; k > 0;) {
    const int step = std::min(k, 18);
    int64_t rem = 0;
    const uint64_t div = static_cast<uint64_t>(Pow10(step).lo);
    Int128 q = m.DivSmall(div, &rem);
    k -= step;
    if (k == 0) {
      const uint64_t arem = static_cast<uint64_t>(rem < 0 ? -rem : rem);
      if (arem * 2 >= div) q = q + Int128(m.IsNegative() ? -1 : 1);
    }
    m = q;
  }
  *out = Dec{m, ns};
  return true;
}

bool DecAdd(const Dec& a, const Dec& b, Dec* out) {
  const int s = std::max(a.scale, b.scale);
  Dec x, y;
  if (!DecRescale(a, s, &x) || !DecRescale(b, s, &y)) return false;
  const Int128 m = x.m + y.m;
  if (!DecFits(m)) return false;
  *out = Dec{m, s};
  return true;
}

bool DecSub(const Dec& a, const Dec& b, Dec* out) { return DecAdd(a, Dec{-b.m, b.scale}, out); }

bool DecMul(const Dec& a, const Dec& b, Dec* out) {
  Int128 m;
  if (Int128::MulOverflow(a.m, b.m, &m)) return false;
  Dec r{m, a.scale + b.scale};
  if (r.scale > kDecMaxScale) {
    if (!DecRescale(r, kDecMaxScale, &r)) return false;
  }
  if (!DecFits(r.m)) return false;
  *out = r;
  return true;
}

bool DecDiv(const Dec& a, const Dec& b, Dec* out) {
  if (b.m.IsZero()) return false;
  const int target = std::max({a.scale, b.scale, 9});
  // result = a.m / b.m x 10^(b.scale - a.scale); compute one more digit, then round
  const int shift = target + 1 + b.scale - a.scale;
  Int128 num = a.m.Abs();
  Int128 den = b.m.Abs();
  if (shift >= 0) {
    if (Int128::MulOverflow(num, Pow10(shift), &num)) return false;
  } else {
    if (Int128::MulOverflow(den, Pow10(-shift), &den)) return false;
  }
  Int128 q, r;
  DivMod128(num, den, &q, &r);
  int64_t last = 0;
  q = q.DivSmall(10, &last);
  if (last >= 5) q = q + Int128(1);
  if (a.m.IsNegative() != b.m.IsNegative()) q = -q;
  if (!DecFits(q)) return false;
  *out = Dec{q, target};
  return true;
}

int DecCompare(const Dec& a, const Dec& b) {
  Dec x, y;
  const int s = std::max(a.scale, b.scale);
  if (DecRescale(a, s, &x) && DecRescale(b, s, &y)) return x.m < y.m ? -1 : (y.m < x.m ? 1 : 0);
  const double da = DecToDouble(a), db = DecToDouble(b);
  return da < db ? -1 : (db < da ? 1 : 0);
}

int SignificantDigits(std::string_view s) {
  int n = 0;
  bool leading = true;
  for (char c : s) {
    if (c == 'e' || c == 'E') break;
    if (c < '0' || c > '9') continue;
    if (leading && c == '0') continue;
    leading = false;
    ++n;
  }
  // trailing zeros of the fraction count as significant digits (1.500 is 4 digits), the same way Excel judges precision
  return n;
}

}  // namespace yc::core
