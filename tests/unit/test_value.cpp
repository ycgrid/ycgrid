// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/decimal.h"
#include "core/value.h"

using namespace yc::core;

TEST_CASE("Value 16 bytes 與字串參考計數") {
  STATIC_REQUIRE(sizeof(Value) == 16);
  Value a = Value::String("short");
  CHECK(a.type() == VType::StrInline);
  Value b = Value::String("這是一個超過十四個位元組的字串");
  CHECK(b.type() == VType::StrHeap);
  Value c = b;
  CHECK(c.str() == b.str());
  b = Value::Number(3);
  CHECK(c.str() == "這是一個超過十四個位元組的字串");
  CHECK(Value::Number(std::numeric_limits<double>::quiet_NaN()).IsError());
  CHECK(Value::Error(Err::NA).error() == Err::NA);
  CHECK(Value::Bool(true).boolean());
  CHECK(Value::String("abc").SameAs(Value::String("abc")));
  CHECK_FALSE(Value::String("abc").SameAs(Value::String("abd")));
  Err e;
  CHECK(ParseErrorText("#N/A", &e));
  CHECK(e == Err::NA);
  CHECK(std::string(ErrorText(Err::Div0)) == "#DIV/0!");
}
