// SPDX-License-Identifier: Apache-2.0
// Coverage of the Document's rules and filters (engine/document_rules.cpp) beyond the other tests: every dynamic date
// filter around today, quarters and months, custom criteria joined by Or, a table's filter button and sort, and
// conditional formats changed and deleted by id.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <algorithm>

using yc::core::Rect;
using yc::model::FilterColumn;

namespace {

// a list of dates around today in A1:A12 (A1 is the header): each row's date is a formula of TODAY()
const char* kDates[] = {"=TODAY()",           "=TODAY()-1",          "=TODAY()+1",           "=TODAY()-7",
                        "=TODAY()+7",         "=EDATE(TODAY(),-1)",  "=EDATE(TODAY(),1)",    "=EDATE(TODAY(),-3)",
                        "=EDATE(TODAY(),3)",  "=EDATE(TODAY(),-12)", "=EDATE(TODAY(),12)"};
enum Row { kToday = 2, kYesterday, kTomorrow, kLastWeek, kNextWeek, kLastMonth, kNextMonth, kLastQuarter, kNextQuarter, kLastYear, kNextYear };

}  // namespace

TEST_CASE("Rules coverage: dynamic date filters around today") {
  yctest::Doc t;
  t.Set("A1", "When");
  for (int i = 0; i < 11; ++i) t.Set(("A" + std::to_string(i + 2)).c_str(), kDates[i]);
  REQUIRE(t.d.SetAutoFilter(t.sheet(), Rect{0, 0, 11, 0}));
  auto shown = [&](int dynamic) {
    FilterColumn fc;
    fc.kind = 6;
    fc.dynamic = dynamic;
    t.d.SetFilterColumn(t.sheet(), fc);
    std::vector<int> rows;
    for (int r = 2; r <= 12; ++r) {
      if (!t.d.wb().sheets[0]->rows->IsHidden(static_cast<uint32_t>(r - 1))) rows.push_back(r);
    }
    return rows;
  };
  auto has = [](const std::vector<int>& v, int r) { return std::find(v.begin(), v.end(), r) != v.end(); };
  using V = std::vector<int>;
  CHECK(shown(1) == V{kToday});
  CHECK(shown(2) == V{kYesterday});
  CHECK(shown(3) == V{kTomorrow});
  auto week = shown(4);  // this week: today, never a week away
  CHECK(has(week, kToday));
  CHECK_FALSE(has(week, kLastWeek));
  CHECK_FALSE(has(week, kNextWeek));
  auto last_week = shown(5);
  CHECK(has(last_week, kLastWeek));
  CHECK_FALSE(has(last_week, kToday));
  auto next_week = shown(6);
  CHECK(has(next_week, kNextWeek));
  CHECK_FALSE(has(next_week, kToday));
  auto month = shown(7);
  CHECK(has(month, kToday));
  CHECK_FALSE(has(month, kLastMonth));
  CHECK_FALSE(has(month, kNextMonth));
  CHECK(has(shown(8), kLastMonth));
  CHECK_FALSE(has(shown(8), kToday));
  CHECK(has(shown(9), kNextMonth));
  auto quarter = shown(10);
  CHECK(has(quarter, kToday));
  CHECK_FALSE(has(quarter, kLastQuarter));
  CHECK_FALSE(has(quarter, kNextQuarter));
  CHECK(has(shown(11), kLastQuarter));
  CHECK(has(shown(12), kNextQuarter));
  auto year = shown(13);
  CHECK(has(year, kToday));
  CHECK_FALSE(has(year, kLastYear));
  CHECK_FALSE(has(year, kNextYear));
  CHECK(has(shown(14), kLastYear));  // in January last month is last year's too: membership, not the whole list
  CHECK_FALSE(has(shown(14), kToday));
  CHECK(has(shown(15), kNextYear));
  CHECK_FALSE(has(shown(15), kToday));
  auto ytd = shown(16);  // year to date: today, never tomorrow or next year
  CHECK(has(ytd, kToday));
  CHECK_FALSE(has(ytd, kTomorrow));
  CHECK_FALSE(has(ytd, kNextYear));
  CHECK(shown(99).empty());  // a code YCGrid doesn't know shows nothing
}

TEST_CASE("Rules coverage: quarter and month filters, custom criteria joined by Or") {
  yctest::Doc t;
  t.Set("A1", "When");
  t.Set("B1", "N");
  const char* dates[] = {"2024-02-10", "2024-05-01", "2023-02-28", "x"};
  for (int i = 0; i < 4; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), dates[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(i * 3));  // 0, 3, 6, 9
  }
  REQUIRE(t.d.SetAutoFilter(t.sheet(), Rect{0, 0, 4, 1}));
  auto hidden = [&](int row1) { return t.d.wb().sheets[0]->rows->IsHidden(static_cast<uint32_t>(row1 - 1)); };
  FilterColumn fc;
  fc.kind = 6;
  fc.dynamic = 17;  // the first quarter of any year
  t.d.SetFilterColumn(t.sheet(), fc);
  CHECK_FALSE(hidden(2));
  CHECK(hidden(3));
  CHECK_FALSE(hidden(4));
  CHECK(hidden(5));  // text isn't a date
  fc.dynamic = 25;  // May
  t.d.SetFilterColumn(t.sheet(), fc);
  CHECK(hidden(2));
  CHECK_FALSE(hidden(3));
  fc.kind = 0;
  t.d.SetFilterColumn(t.sheet(), fc);
  FilterColumn n;
  n.field = 1;
  n.kind = 2;
  n.crit1 = "<2";
  n.crit2 = ">7";
  n.or_op = true;
  t.d.SetFilterColumn(t.sheet(), n);
  CHECK_FALSE(hidden(2));
  CHECK(hidden(3));
  CHECK(hidden(4));
  CHECK_FALSE(hidden(5));
  n.or_op = false;
  n.crit2 = ">=0";
  t.d.SetFilterColumn(t.sheet(), n);
  CHECK_FALSE(hidden(2));
  CHECK(hidden(5));
}

TEST_CASE("Rules coverage: a table's filter button and sort levels; rules changed and deleted by id; notes removed") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "k");
  t.Set("A2", "b");
  t.Set("A3", "a");
  const uint32_t id = t.d.AddTable(sh, Rect{0, 0, 2, 0}, true, "TableStyleMedium2");
  REQUIRE(id != 0);
  REQUIRE(t.d.wb().sheets[0]->autofilter->on);  // the first table's buttons are the sheet's
  t.d.SetTableFilterButton(sh, id, false);
  CHECK_FALSE(t.d.TableById(sh, id)->filter_button);
  CHECK_FALSE(t.d.wb().sheets[0]->autofilter->on);
  t.d.SetTableFilterButton(sh, id, true);
  CHECK(t.d.TableById(sh, id)->filter_button);
  CHECK(t.d.wb().sheets[0]->autofilter->on);
  t.d.SetTableFilterButton(sh, 999, true);  // no such table: nothing
  yc::model::SortLevel lv;
  lv.col = 0;
  lv.desc = true;
  t.d.SetTableFilterSort(sh, id, {lv});
  REQUIRE(t.d.TableById(sh, id)->filter.sort_levels.size() == 1);
  CHECK(t.d.TableById(sh, id)->filter.sort_levels[0].desc);
  t.d.SetTableFilterSort(sh, id, {lv});  // the same levels: no change
  // conditional formats by id
  yc::model::CfRule r;
  r.ranges = {Rect{0, 3, 4, 3}};
  r.formula1 = "1";
  const uint32_t rid = t.d.AddCondFormat(sh, r);
  REQUIRE(rid != 0);
  CHECK(t.d.UpdateCondFormat(sh, rid, [](yc::model::CfRule& x) {
    x.formula1 = "5";
    x.fmt.numfmt = "0.000";
  }));
  CHECK(t.d.wb().sheets[0]->cond_formats->front().formula1 == "5");
  CHECK_FALSE(t.d.UpdateCondFormat(sh, 999, [](yc::model::CfRule&) {}));
  CHECK_FALSE(t.d.UpdateCondFormat(999, rid, [](yc::model::CfRule&) {}));
  CHECK(t.d.DeleteCondFormat(sh, rid));
  CHECK_FALSE(t.d.DeleteCondFormat(sh, rid));
  CHECK_FALSE(t.d.DeleteCondFormat(999, rid));
  CHECK(t.d.wb().sheets[0]->cond_formats->empty());
  // a note and a comment removed by empty text
  t.d.SetNote(sh, 5, 5, "me", "hello");
  t.d.SetComment(sh, 6, 6, "me", "thread", 45000);
  CHECK(t.d.wb().sheets[0]->notes->size() == 2);
  t.d.SetNote(sh, 5, 5, "me", "");
  t.d.SetComment(sh, 6, 6, "me", "", 0);
  CHECK(t.d.wb().sheets[0]->notes->empty());
}
