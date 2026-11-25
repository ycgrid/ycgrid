// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/decimal.h"
#include "core/int128.h"

using namespace yc::core;

TEST_CASE("Int128 基本運算與字串轉換") {
  Int128 a(1234567890123456789LL), b(987654321);
  Int128 p;
  REQUIRE_FALSE(Int128::MulOverflow(a, b, &p));
  CHECK(p.ToString() == "1219326311248285321112635269");
  CHECK((a - a).IsZero());
  CHECK((-a).ToString() == "-1234567890123456789");
  Int128 q;
  REQUIRE(Int128::Parse("-170141183460469231731687303715884105727", 40, &q));
  CHECK(q.ToString() == "-170141183460469231731687303715884105727");
  int64_t rem = 0;
  CHECK(Int128(1000000007).DivSmall(10, &rem).ToString() == "100000000");
  CHECK(rem == 7);
  CHECK(Pow10(20).ToString() == "100000000000000000000");
}

TEST_CASE("Decimal 精確運算（10 位整數＋9 位小數）") {
  Dec a, b, c;
  REQUIRE(DecParse("1729658400.123456789", &a));
  REQUIRE(DecParse("1729658399.000000001", &b));
  REQUIRE(DecSub(a, b, &c));
  CHECK(DecToString(c) == "1.123456788");
  REQUIRE(DecAdd(a, b, &c));
  CHECK(DecToString(c) == "3459316799.123456790");
  REQUIRE(DecParse("0.1", &a));
  REQUIRE(DecParse("0.2", &b));
  REQUIRE(DecAdd(a, b, &c));
  CHECK(DecToString(c) == "0.3");
  REQUIRE(DecParse("10", &a));
  REQUIRE(DecParse("3", &b));
  REQUIRE(DecDiv(a, b, &c));
  CHECK(DecToString(c) == "3.333333333");
  REQUIRE(DecMul(a, b, &c));
  CHECK(DecToString(c) == "30");
  CHECK_FALSE(DecDiv(a, Dec{Int128(0), 0}, &c));
  Value v = DecToValue(a);
  CHECK(v.IsDecimal());
  CHECK(DecToString(DecFromValue(v)) == "10");
  Dec r;
  REQUIRE(DecParse("2.5", &a));
  REQUIRE(DecRescale(a, 0, &r));
  CHECK(DecToString(r) == "3");
  REQUIRE(DecParse("-2.5", &a));
  REQUIRE(DecRescale(a, 0, &r));
  CHECK(DecToString(r) == "-3");
  CHECK(SignificantDigits("1729658400.123456789") == 19);
  CHECK(SignificantDigits("0.00012") == 2);
}
