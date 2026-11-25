// SPDX-License-Identifier: Apache-2.0
// decimal.h - exact decimal (PLAN §5.2a): value = mantissa / 10^scale, 112-bit mantissa
#pragma once

#include "core/int128.h"
#include "core/value.h"

#include <string>
#include <string_view>

namespace yc::core {

struct Dec {
  Int128 m;
  int scale = 0;
};

constexpr int kDecMaxScale = 28;

bool DecFits(const Int128& m);                 // whether it fits the signed 112-bit range
bool DecParse(std::string_view s, Dec* out);   // "-1729658400.123456789", "12", "0.5"
std::string DecToString(const Dec& d);         // prints every decimal place of the scale
Dec DecFromValue(const Value& v);              // the Value must be a Decimal
Value DecToValue(const Dec& d);                // out of range -> #NUM!
double DecToDouble(const Dec& d);
bool DecFromDouble(double v, int scale, Dec* out);  // round to scale (used when converting back in mixed arithmetic)

bool DecRescale(const Dec& d, int new_scale, Dec* out);  // scaling up is exact; scaling down rounds half away from zero
bool DecAdd(const Dec& a, const Dec& b, Dec* out);
bool DecSub(const Dec& a, const Dec& b, Dec* out);
bool DecMul(const Dec& a, const Dec& b, Dec* out);
bool DecDiv(const Dec& a, const Dec& b, Dec* out);  // division by 0 returns false
int DecCompare(const Dec& a, const Dec& b);

// general unsigned 128-bit division, used by DecDiv
void DivMod128(const Int128& n, const Int128& d, Int128* q, Int128* r);

// significant digits of a number string (no leading zeros, decimal point, sign or exponent); used to detect more than 15 digits
int SignificantDigits(std::string_view s);

}  // namespace yc::core
