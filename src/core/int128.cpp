// SPDX-License-Identifier: Apache-2.0
#include "core/int128.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace yc::core {
namespace {

struct U128 {
  uint64_t lo, hi;
};

U128 Mul64(uint64_t a, uint64_t b) {
  U128 r;
#if defined(_MSC_VER)
  r.lo = _umul128(a, b, &r.hi);
#else
  const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
  r.lo = static_cast<uint64_t>(p);
  r.hi = static_cast<uint64_t>(p >> 64);
#endif
  return r;
}

uint64_t Div128By64(uint64_t hi, uint64_t lo, uint64_t d, uint64_t* rem) {
#if defined(_MSC_VER)
  return _udiv128(hi, lo, d, rem);
#else
  const unsigned __int128 n = (static_cast<unsigned __int128>(hi) << 64) | lo;
  *rem = static_cast<uint64_t>(n % d);
  return static_cast<uint64_t>(n / d);
#endif
}

}  // namespace

bool Int128::MulOverflow(const Int128& a, const Int128& b, Int128* out) {
  const bool neg = a.IsNegative() != b.IsNegative();
  const Int128 ua = a.Abs(), ub = b.Abs();
  const uint64_t ahi = static_cast<uint64_t>(ua.hi), bhi = static_cast<uint64_t>(ub.hi);
  if (ahi != 0 && bhi != 0) return true;
  U128 ll = Mul64(ua.lo, ub.lo);
  U128 cross{0, 0};
  if (ahi) cross = Mul64(ahi, ub.lo);
  else if (bhi) cross = Mul64(ua.lo, bhi);
  if (cross.hi != 0) return true;
  const uint64_t hi = ll.hi + cross.lo;
  if (hi < ll.hi) return true;
  if (hi >> 63) return true;  // outside the signed range
  Int128 r = Int128::FromParts(static_cast<int64_t>(hi), ll.lo);
  *out = neg ? -r : r;
  return false;
}

Int128 Int128::DivSmall(uint64_t d, int64_t* rem) const {
  const bool neg = IsNegative();
  const Int128 u = Abs();
  uint64_t r1 = 0, r2 = 0;
  const uint64_t qhi = static_cast<uint64_t>(u.hi) / d;
  r1 = static_cast<uint64_t>(u.hi) % d;
  const uint64_t qlo = Div128By64(r1, u.lo, d, &r2);
  Int128 q = Int128::FromParts(static_cast<int64_t>(qhi), qlo);
  if (rem) *rem = neg ? -static_cast<int64_t>(r2) : static_cast<int64_t>(r2);
  return neg ? -q : q;
}

double Int128::ToDouble() const {
  if (IsNegative()) return -(-*this).ToDouble();
  return static_cast<double>(static_cast<uint64_t>(hi)) * 18446744073709551616.0 + static_cast<double>(lo);
}

bool Int128::FromDouble(double v, Int128* out) {
  if (!std::isfinite(v) || std::floor(v) != v || std::fabs(v) >= 1.7e38) return false;
  const bool neg = v < 0;
  double a = std::fabs(v);
  const double h = std::floor(a / 18446744073709551616.0);
  const double l = a - h * 18446744073709551616.0;
  Int128 r = Int128::FromParts(static_cast<int64_t>(h), static_cast<uint64_t>(l));
  *out = neg ? -r : r;
  return true;
}

std::string Int128::ToString() const {
  if (IsZero()) return "0";
  std::string s;
  Int128 u = Abs();
  while (!u.IsZero()) {
    int64_t r = 0;
    u = u.DivSmall(10, &r);
    s.push_back(static_cast<char>('0' + r));
  }
  if (IsNegative()) s.push_back('-');
  std::reverse(s.begin(), s.end());
  return s;
}

bool Int128::Parse(const char* s, size_t n, Int128* out) {
  size_t i = 0;
  bool neg = false;
  if (i < n && (s[i] == '-' || s[i] == '+')) {
    neg = s[i] == '-';
    ++i;
  }
  if (i >= n) return false;
  Int128 r;
  const Int128 ten(10);
  for (; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    Int128 t;
    if (MulOverflow(r, ten, &t)) return false;
    r = t + Int128(s[i] - '0');
    if (r.IsNegative()) return false;
  }
  *out = neg ? -r : r;
  return true;
}

Int128 Pow10(int k) {
  // initialization of function-local statics is thread-safe (C++11 magic statics)
  static const auto table = [] {
    std::array<Int128, 39> t{};
    t[0] = Int128(1);
    for (size_t i = 1; i < t.size(); ++i) Int128::MulOverflow(t[i - 1], Int128(10), &t[i]);
    return t;
  }();
  return table[static_cast<size_t>(std::clamp(k, 0, 38))];
}

}  // namespace yc::core
