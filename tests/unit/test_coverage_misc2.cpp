// SPDX-License-Identifier: Apache-2.0
// Coverage of smaller corners: Go To Special's notes, last cell, conditional formats and validation, and row
// differences of text and logicals (engine/navigate.cpp); the workbook's default table style as the store keeps it
// (model/table_style.cpp); decimals converted from doubles and compared past the mantissa (core/decimal.cpp).
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <algorithm>

#include "core/decimal.h"
#include "engine/navigate.h"
#include "model/table_style.h"

using yc::core::Rect;
using yc::engine::Special;

TEST_CASE("Go To Special coverage: notes, the last cell, rules and validation, differences of text and logicals") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "x");
  t.Set("B1", "x");
  t.Set("C1", "y");
  t.Set("A2", "TRUE");
  t.Set("B2", "TRUE");
  t.Set("C2", "FALSE");
  t.Set("A3", "x");
  t.Set("B3", "TRUE");
  t.Set("E9", "1");
  t.d.SetNote(sh, 1, 1, "me", "inside");
  t.d.SetNote(sh, 7, 7, "me", "outside");
  yc::model::CfRule cf;
  cf.ranges = {Rect{0, 0, 9, 1}};
  cf.formula1 = "0";
  t.d.AddCondFormat(sh, cf);
  yc::model::DvRule dv;
  dv.type = yc::model::DvType::WholeNumber;
  dv.formula1 = "1";
  t.d.SetValidation(sh, Rect{2, 2, 3, 3}, dv);
  const auto& s = *t.d.S(sh);
  const Rect within{0, 0, 4, 3};
  CHECK(yc::engine::SpecialCells(s, within, Special::Comments) == std::vector<Rect>{Rect::Cell(1, 1)});
  CHECK(yc::engine::SpecialCells(s, within, Special::LastCell) == std::vector<Rect>{Rect::Cell(8, 4)});
  CHECK(yc::engine::SpecialCells(s, within, Special::AllFormatConditions) == std::vector<Rect>{Rect{0, 0, 4, 1}});  // clipped
  CHECK(yc::engine::SpecialCells(s, within, Special::AllValidation) == std::vector<Rect>{Rect{2, 2, 3, 3}});
  // each row against column A: text and logicals compare by their values, a text against a logical differs
  const auto diff = yc::engine::DifferenceCells(s, Rect{0, 0, 2, 2}, 0, 0, true);
  std::vector<Rect> want = {Rect::Cell(0, 2), Rect::Cell(1, 2), Rect::Cell(2, 1), Rect::Cell(2, 2)};  // C3 is empty
  std::vector<yc::core::CellPos> got;
  for (const auto& r : diff) {
    for (uint32_t rr = r.r0; rr <= r.r1; ++rr) {
      for (uint32_t cc = r.c0; cc <= r.c1; ++cc) got.push_back({rr, cc});
    }
  }
  CHECK(got.size() == 4);
  for (const auto& w : want) CHECK(std::find(got.begin(), got.end(), yc::core::CellPos{w.r0, w.c0}) != got.end());
  yctest::Doc empty;
  CHECK(yc::engine::SpecialCells(*empty.d.S(empty.sheet()), within, Special::LastCell).empty());
}

TEST_CASE("Table styles coverage: the default style as the store keeps it, in every msgpack string form") {
  using yc::model::DefaultTableStyle;
  const std::string key = yc::model::kDefaultTableStyleKey;
  CHECK(DefaultTableStyle({}) == "TableStyleMedium2");
  CHECK(DefaultTableStyle({{key, ""}}) == "TableStyleMedium2");
  CHECK(DefaultTableStyle({{key, yc::model::EncodeStoreString("TableStyleLight9")}}) == "TableStyleLight9");
  CHECK(DefaultTableStyle({{key, std::string(1, static_cast<char>(0xA0 | 6)) + "Mine 1"}}) == "Mine 1");  // fixstr
  const std::string long_name(40, 'n');
  CHECK(DefaultTableStyle({{key, std::string(1, static_cast<char>(0xD9)) + std::string(1, 40) + long_name}}) == long_name);  // str8
  CHECK(DefaultTableStyle({{key, std::string("\xDA\x00\x03", 3) + "abc"}}) == "abc");  // str16
  CHECK(DefaultTableStyle({{key, std::string("\x01x", 2)}}) == "TableStyleMedium2");  // not a string
  CHECK(DefaultTableStyle({{key, std::string("\xDB\x00", 2)}}) == "TableStyleMedium2");  // cut short
  CHECK(DefaultTableStyle({{key, std::string(1, static_cast<char>(0xA0))}}) == "TableStyleMedium2");  // an empty name
  const std::string enc = yc::model::EncodeStoreString("ab");
  CHECK(enc == std::string("\xDB\x00\x00\x00\x02" "ab", 7));
}

TEST_CASE("Decimals coverage: from doubles, and compared when one scale can't hold the other") {
  using namespace yc::core;
  Dec d;
  REQUIRE(DecFromDouble(0.125, 2, &d));  // exact in binary: rounds half away from zero
  CHECK(DecToString(d) == "0.13");
  CHECK(DecToDouble(d) == 0.13);
  REQUIRE(DecFromDouble(-0.5, 0, &d));
  CHECK(DecToString(d) == "-1");  // half away from zero
  CHECK_FALSE(DecFromDouble(1e40, 2, &d));
  Dec big, tiny;
  REQUIRE(DecParse("1000000000000000000000", &big));
  REQUIRE(DecParse("0.0000000000000000000000000001", &tiny));
  CHECK(DecCompare(big, tiny) == 1);  // 10^21 at scale 28 overflows: compared as doubles
  CHECK(DecCompare(tiny, big) == -1);
  Dec neg;
  REQUIRE(DecParse("-1000000000000000000000", &neg));
  CHECK(DecCompare(neg, tiny) == -1);
}
