// SPDX-License-Identifier: Apache-2.0
// Coverage of the MessagePack reader and writer (.ycg chunks, KV, the YCP protocol): every integer width both ways,
// long strings, binaries, arrays and maps past their short forms, float32 from other writers, uint64 past int64,
// what Skip and RawValue walk over, and the refusals of a damaged or short buffer.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "io/msgpack.h"

using yc::io::MsgReader;
using yc::io::MsgType;
using yc::io::MsgWriter;

TEST_CASE("MessagePack integers of every width read back as written") {
  const int64_t values[] = {0, 1, 127, 128, 255, 256, 65535, 65536, 4294967295LL, 4294967296LL, -1, -32, -33, -128, -129,
                            -32768, -32769, -2147483648LL, -2147483649LL, std::numeric_limits<int64_t>::min(),
                            std::numeric_limits<int64_t>::max()};
  MsgWriter w;
  for (int64_t v : values) w.Int(v);
  MsgReader r(w.buf());
  for (int64_t v : values) {
    int64_t got = 0;
    REQUIRE(r.Int(&got));
    CHECK(got == v);
  }
  CHECK(r.AtEnd());
  // an unsigned past int64 reads as UInt, and Int refuses it
  MsgWriter u;
  u.UInt(std::numeric_limits<uint64_t>::max());
  u.UInt(5);
  MsgReader ru(u.buf());
  CHECK(ru.Peek() == MsgType::UInt);
  int64_t i = 0;
  CHECK_FALSE(ru.Int(&i));
  uint64_t big = 0;
  REQUIRE(ru.UInt(&big));
  CHECK(big == std::numeric_limits<uint64_t>::max());
  REQUIRE(ru.UInt(&big));
  CHECK(big == 5);
  // a negative number is no UInt
  MsgWriter n;
  n.Int(-5);
  MsgReader rn(n.buf());
  CHECK_FALSE(rn.UInt(&big));
}

TEST_CASE("MessagePack doubles, float32 from other writers and integers read as doubles") {
  MsgWriter w;
  w.Double(1.5);
  w.Int(7);
  w.Int(-300000);
  MsgReader r(w.buf());
  double d = 0;
  REQUIRE(r.Double(&d));
  CHECK(d == 1.5);
  REQUIRE(r.Double(&d));
  CHECK(d == 7);
  REQUIRE(r.Double(&d));
  CHECK(d == -300000);
  // float32 (0xca) as Python's msgpack may send it
  std::string f32 = "\xca";
  const float f = 2.25f;
  uint32_t bits = 0;
  std::memcpy(&bits, &f, 4);
  for (int k = 3; k >= 0; --k) f32 += static_cast<char>((bits >> (k * 8)) & 0xFF);
  MsgReader rf(f32);
  CHECK(rf.Peek() == MsgType::Float);
  REQUIRE(rf.Double(&d));
  CHECK(d == 2.25);
  // a string is no number; a cut-off float isn't read
  MsgWriter s;
  s.Str("x");
  MsgReader rs(s.buf());
  CHECK_FALSE(rs.Double(&d));
  MsgReader cut(std::string_view("\xcb\x00\x00", 3));
  CHECK_FALSE(cut.Double(&d));
}

TEST_CASE("MessagePack strings, binaries, arrays and maps past their short forms") {
  for (size_t len : {size_t{0}, size_t{31}, size_t{32}, size_t{255}, size_t{256}, size_t{65535}, size_t{65536}, size_t{70000}}) {
    const std::string text(len, 'a');
    MsgWriter w;
    w.Str(text);
    w.Bin(text);
    MsgReader r(w.buf());
    std::string a, b;
    REQUIRE(r.Str(&a));
    REQUIRE(r.Bin(&b));
    CHECK(a == text);
    CHECK(b == text);
    // Bin accepts a str and StrView a binary
    MsgReader r2(w.buf());
    std::string any;
    REQUIRE(r2.Bin(&any));
    std::string_view view;
    CHECK(r2.StrView(&view));
    CHECK(view == text);
  }
  for (uint32_t n : {0u, 15u, 16u, 65535u, 65536u}) {
    MsgWriter w;
    w.Array(n);
    for (uint32_t k = 0; k < n; ++k) w.Nil();
    w.Map(n);
    for (uint32_t k = 0; k < n; ++k) w.Int(k), w.Bool(k % 2 == 0);
    MsgReader r(w.buf());
    uint32_t got = 0;
    REQUIRE(r.Array(&got));
    CHECK(got == n);
    for (uint32_t k = 0; k < n; ++k) REQUIRE(r.Nil());
    REQUIRE(r.Map(&got));
    CHECK(got == n);
    for (uint32_t k = 0; k < n; ++k) {
      REQUIRE(r.Skip());
      REQUIRE(r.Skip());
    }
    CHECK(r.AtEnd());
  }
}

TEST_CASE("MessagePack extensions of every length, Skip and RawValue over every kind") {
  MsgWriter w;
  for (size_t len : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}, size_t{3}, size_t{300}, size_t{70000}}) w.Ext(5, std::string(len, 'x'));
  w.Double(3.5);
  w.UInt(1ULL << 40);
  w.Int(-1LL << 40);
  w.Bool(true);
  w.Array(2);
  w.Str("a");
  w.Map(1);
  w.Int(1);
  w.Bin("b");
  {
    MsgReader r(w.buf());
    for (size_t len : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}, size_t{3}, size_t{300}, size_t{70000}}) {
      int8_t type = 0;
      std::string_view data;
      REQUIRE(r.Ext(&type, &data));
      CHECK(type == 5);
      CHECK(data.size() == len);
    }
  }
  MsgReader r(w.buf());
  size_t values = 0;
  while (!r.AtEnd()) {
    std::string_view raw;
    REQUIRE(r.RawValue(&raw));
    MsgReader one(raw);
    REQUIRE(one.Skip());
    CHECK(one.AtEnd());
    ++values;
  }
  CHECK(values == 13);
}

TEST_CASE("MessagePack refuses damaged and cut-off input instead of reading past it") {
  // every prefix of a valid value fails cleanly
  MsgWriter w;
  w.Map(2);
  w.KStr(1, std::string(300, 'z'));
  w.Int(2);
  w.Array(3);
  w.Double(1.25);
  w.Int(-70000);
  w.Ext(1, "abc");
  const std::string& full = w.buf();
  for (size_t cut = 0; cut < full.size(); ++cut) {
    MsgReader r(std::string_view(full.data(), cut));
    CHECK_FALSE(r.Skip());
  }
  MsgReader ok(full);
  CHECK(ok.Skip());
  // a reserved byte is no value
  MsgReader bad(std::string_view("\xc1", 1));
  CHECK(bad.Peek() == MsgType::Invalid);
  CHECK_FALSE(bad.Skip());
  // nesting past 64 levels is refused (a damaged file can't overflow the stack)
  std::string deep(100, '\x91');
  deep += '\xc0';
  MsgReader rd(deep);
  CHECK_FALSE(rd.Skip());
  // a string cut off is refused and the cursor stays (a reader can tell what is there)
  MsgReader short_str(std::string_view("\xa5" "ab", 3));
  std::string_view sv;
  CHECK_FALSE(short_str.StrView(&sv));
  CHECK(short_str.pos() == 0);
  // the wrong kind is refused and the cursor stays
  MsgWriter s;
  s.Str("x");
  MsgReader rs(s.buf());
  bool b = false;
  uint32_t n = 0;
  CHECK_FALSE(rs.Bool(&b));
  CHECK_FALSE(rs.Array(&n));
  CHECK_FALSE(rs.Map(&n));
  CHECK_FALSE(rs.Nil());
  std::string out;
  CHECK(rs.Str(&out));
  CHECK(out == "x");
}
