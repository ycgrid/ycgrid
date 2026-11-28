// SPDX-License-Identifier: Apache-2.0
// AutoFill options (XlAutoFillType), custom lists, sorting by a custom list
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/custom_lists.h"
#include "formula/datetime.h"

#include <cmath>

using yc::core::Rect;
namespace engine = yc::engine;

TEST_CASE("fill: Excel's AutoFill types") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "5");
  t.d.AutoFill(sh, Rect{0, 0, 0, 0}, Rect{0, 0, 3, 0}, 0);  // default: one number is copied
  CHECK(t.N("A4") == 5);
  t.d.AutoFill(sh, Rect{0, 0, 0, 0}, Rect{0, 0, 3, 0}, 2);  // Fill Series: steps of 1
  CHECK(t.N("A4") == 8);
  t.Set("B1", "1");
  t.Set("B2", "3");
  t.d.AutoFill(sh, Rect{0, 1, 1, 1}, Rect{0, 1, 4, 1}, 1);  // Copy Cells repeats the source
  CHECK(t.N("B3") == 1);
  CHECK(t.N("B4") == 3);
  t.d.AutoFill(sh, Rect{0, 1, 1, 1}, Rect{0, 1, 4, 1}, 9);  // linear trend
  CHECK(t.N("B5") == 9);
  t.Set("C1", "2");
  t.Set("C2", "6");
  t.d.AutoFill(sh, Rect{0, 2, 1, 2}, Rect{0, 2, 3, 2}, 10);  // growth trend
  CHECK(std::abs(t.N("C4") - 54) < 1e-9);
  // dates: months keep the day (or the month's last day), weekdays skip weekends, years
  t.Set("D1", "2024-01-31");
  t.d.AutoFill(sh, Rect{0, 3, 0, 3}, Rect{0, 3, 2, 3}, 7);
  CHECK(t.N("D2") == yc::formula::DateSerial(2024, 2, 29));
  CHECK(t.N("D3") == yc::formula::DateSerial(2024, 3, 31));
  t.Set("E1", "2024-01-05");  // Friday
  t.d.AutoFill(sh, Rect{0, 4, 0, 4}, Rect{0, 4, 2, 4}, 6);
  CHECK(t.N("E2") == yc::formula::DateSerial(2024, 1, 8));
  t.d.AutoFill(sh, Rect{0, 4, 0, 4}, Rect{0, 4, 1, 4}, 8);
  CHECK(t.N("E2") == yc::formula::DateSerial(2025, 1, 5));
  // upwards: the series goes back
  t.Set("F5", "2024-03-10");
  t.d.AutoFill(sh, Rect{4, 5, 4, 5}, Rect{2, 5, 4, 5}, 5);
  CHECK(t.N("F3") == yc::formula::DateSerial(2024, 3, 8));
  // formats only / values only
  t.Set("G1", "x");
  t.d.ModifyStyle(sh, Rect::Cell(0, 6), [](yc::model::Style& s) { s.bold = true; });
  t.d.AutoFill(sh, Rect::Cell(0, 6), Rect{0, 6, 2, 6}, 3);
  CHECK(t.V("G3").IsEmpty());
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(2, 6)).bold);
  t.Set("H1", "y");
  t.d.ModifyStyle(sh, Rect::Cell(0, 7), [](yc::model::Style& s) { s.italic = true; });
  t.d.AutoFill(sh, Rect::Cell(0, 7), Rect{0, 7, 2, 7}, 4);
  CHECK(t.V("H3").str() == "y");
  CHECK_FALSE(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(2, 7)).italic);
}

TEST_CASE("fill: custom lists continue and order sorting") {
  engine::SetUserFillLists({{"Low", "Medium", "High"}});
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "medium");
  t.d.AutoFill(sh, Rect::Cell(0, 0), Rect{0, 0, 3, 0});
  CHECK(t.V("A2").str() == "High");
  CHECK(t.V("A3").str() == "Low");
  int list = -1, idx = -1;
  REQUIRE(engine::FindInFillLists("HIGH", &list, &idx));
  CHECK(list == static_cast<int>(engine::BuiltinFillLists().size()));
  CHECK(idx == 2);
  // sort by the list; values not in it go last
  t.Set("B1", "High");
  t.Set("B2", "zzz");
  t.Set("B3", "Low");
  t.Set("B4", "Medium");
  engine::SortKey k{1, false};
  k.custom_list = list;
  t.d.Sort(sh, Rect{0, 1, 3, 1}, {k}, false);
  CHECK(t.V("B1").str() == "Low");
  CHECK(t.V("B2").str() == "Medium");
  CHECK(t.V("B3").str() == "High");
  CHECK(t.V("B4").str() == "zzz");
  // descending reverses the whole order, the values not in the list first (Excel 16, measured 2026-11-28); CustomOrder
  // items work the same way
  engine::SortKey k2{1, true};
  k2.custom_items = {"Low", "Medium", "High"};
  t.d.Sort(sh, Rect{0, 1, 3, 1}, {k2}, false);
  CHECK(t.V("B1").str() == "zzz");
  CHECK(t.V("B2").str() == "High");
  CHECK(t.V("B4").str() == "Low");
  // Case sensitive: an item written in another case isn't the list's (it goes with the values not in it)
  t.Set("B2", "high");
  engine::SortKey k3{1, false};
  k3.custom_items = {"Low", "Medium", "High"};
  t.d.Sort(sh, Rect{0, 1, 3, 1}, {k3}, false, false, /*match_case=*/true);
  CHECK(t.V("B1").str() == "Low");
  CHECK(t.V("B2").str() == "Medium");
  CHECK(t.V("B3").str() == "high");
  CHECK(t.V("B4").str() == "zzz");
  engine::SetUserFillLists({});
}
