// SPDX-License-Identifier: Apache-2.0
// int128.h - signed 128-bit integer (PLAN §5.2a)
//
// MSVC has no __int128; implemented with two 64-bit words and intrinsics. Only the operations Decimal needs:
// add, subtract, multiply, divide by small integers (10^k), compare, to string, parse from string.
#pragma once

#include <cstdint>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace yc::core {

struct Int128 {
  uint64_t lo = 0;
  int64_t hi = 0;

  constexpr Int128() = default;
  constexpr Int128(int64_t v) : lo(static_cast<uint64_t>(v)), hi(v < 0 ? -1 : 0) {}  // NOLINT(google-explicit-constructor)
  static constexpr Int128 FromParts(int64_t h, uint64_t l) {
    Int128 r;
    r.hi = h;
    r.lo = l;
    return r;
  }

  bool IsNegative() const { return hi < 0; }
  bool IsZero() const { return hi == 0 && lo == 0; }

  friend bool operator==(const Int128& a, const Int128& b) { return a.hi == b.hi && a.lo == b.lo; }
  friend bool operator!=(const Int128& a, const Int128& b) { return !(a == b); }
  friend bool operator<(const Int128& a, const Int128& b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
  friend bool operator>(const Int128& a, const Int128& b) { return b < a; }
  friend bool operator<=(const Int128& a, const Int128& b) { return !(b < a); }
  friend bool operator>=(const Int128& a, const Int128& b) { return !(a < b); }

  friend Int128 operator+(const Int128& a, const Int128& b) {
    Int128 r;
    r.lo = a.lo + b.lo;
    r.hi = static_cast<int64_t>(static_cast<uint64_t>(a.hi) + static_cast<uint64_t>(b.hi) + (r.lo < a.lo ? 1u : 0u));
    return r;
  }
  friend Int128 operator-(const Int128& a) {
    Int128 r;
    r.lo = ~a.lo + 1;
    r.hi = static_cast<int64_t>(~static_cast<uint64_t>(a.hi) + (r.lo == 0 ? 1u : 0u));
    return r;
  }
  friend Int128 operator-(const Int128& a, const Int128& b) { return a + (-b); }

  Int128 Abs() const { return IsNegative() ? -*this : *this; }

  // unsigned 128x128 -> low 128 bits; returns whether it overflowed (judged by the signed range)
  static bool MulOverflow(const Int128& a, const Int128& b, Int128* out);
  // divide by a positive 64-bit integer, return the quotient and output the remainder (the dividend may be negative; the remainder has its sign)
  Int128 DivSmall(uint64_t d, int64_t* rem) const;

  double ToDouble() const;
  static bool FromDouble(double v, Int128* out);  // only integral values within range
  std::string ToString() const;
  static bool Parse(const char* s, size_t n, Int128* out);
};

Int128 Pow10(int k);  // 0 ≤ k ≤ 38

}  // namespace yc::core
