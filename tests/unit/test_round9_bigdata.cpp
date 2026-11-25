// SPDX-License-Identifier: Apache-2.0
// Round 9: big lists stay smooth. What an action or a frame no longer waits for: undo entries a new action drops are
// freed elsewhere, and a conditional format over a long range keeps its statistics across snapshots for a moment.
#include <catch2/catch_test_macros.hpp>

#include "engine/rule_eval.h"
#include "formula/eval.h"
#include "test_helpers.h"

#include <iterator>
#include <memory>
#include <vector>

using yc::core::Rect;
using yc::core::Value;
namespace engine = yc::engine;
namespace model = yc::model;

TEST_CASE("Undo entries a new action drops go to the release hook instead of being freed inside the action") {
  // typing after undoing a paste of six million cells waited 0.8 s while the redo list's tiles were freed
  yctest::Doc t;
  const uint32_t s = t.sheet();
  std::vector<std::shared_ptr<void>> released;
  t.d.release_later = [&](std::shared_ptr<void> p) { released.push_back(std::move(p)); };
  auto act = [&](const char* label, uint32_t r, const char* text) {
    t.d.Begin(label);
    t.d.SetInput(s, r, 0, text);
    t.d.Commit();
  };
  act("one", 0, "1");
  act("two", 1, "2");
  REQUIRE(t.d.Undo());
  REQUIRE(t.d.CanRedo());
  CHECK(released.empty());
  act("three", 2, "3");  // ends the redo list
  CHECK_FALSE(t.d.CanRedo());
  REQUIRE(released.size() == 1);
  CHECK(released[0] != nullptr);
  // past the undo limit the oldest entry goes the same way
  t.d.undo_limit = 2;
  act("four", 3, "4");
  CHECK(t.d.UndoDepth() == 2);
  CHECK(released.size() == 2);
  // what stays still undoes
  REQUIRE(t.d.Undo());
  CHECK(t.d.GetValue(s, 3, 0).IsEmpty());
  CHECK(t.d.GetValue(s, 2, 0).AsDouble() == 3);
  // without the hook (a document of its own, no engine) they are freed in place, as before
  yctest::Doc plain;
  plain.d.Begin("a");
  plain.d.SetInput(plain.sheet(), 0, 0, "1");
  plain.d.Commit();
  REQUIRE(plain.d.Undo());
  plain.d.Begin("b");
  plain.d.SetInput(plain.sheet(), 0, 1, "2");
  plain.d.Commit();
  CHECK_FALSE(plain.d.CanRedo());
}

TEST_CASE("A conditional format over 100,000 cells keeps its statistics across snapshots for half a second") {
  // a new snapshot every frame (live data, a recalculation in slices) gathered and sorted a million numbers a frame
  yctest::Doc t;
  const uint32_t s = t.sheet();
  constexpr uint32_t n = 100000;
  std::vector<Value> vals(n);
  for (uint32_t i = 0; i < n; ++i) vals[i] = Value::Number(i + 1.0);
  t.d.SetValues(s, Rect{0, 0, n - 1, 0}, vals);
  model::CfRule bar;
  bar.type = model::CfType::DataBar;
  bar.ranges = {Rect{0, 0, n - 1, 0}};
  t.d.AddCondFormat(s, bar);
  t.d.Recalc();
  const model::Workbook before = t.d.wb();  // what a snapshot holds
  engine::CondFormatEval ev(before, s);
  engine::CfVisual v;
  REQUIRE(ev.Eval(n / 2 - 1, 0, &(v = {})));
  const float half = v.bar;
  // live data raises the largest value: a new snapshot with the same rules
  t.d.SetValue(s, n - 1, 0, Value::Number(1e9));
  const model::Workbook after = t.d.wb();
  engine::CondFormatEval kept(after, s);
  kept.KeepStatsFrom(ev);
  REQUIRE(kept.Eval(n / 2 - 1, 0, &(v = {})));
  CHECK(v.bar == half);  // the statistics gathered a moment ago
  engine::CondFormatEval fresh(after, s);
  REQUIRE(fresh.Eval(n / 2 - 1, 0, &(v = {})));
  CHECK(v.bar < half);  // gathered again: the middle value is now a short bar
  // edited rules keep nothing
  model::CfRule other = bar;
  other.ranges = {Rect{0, 2, 9, 2}};
  t.d.AddCondFormat(s, other);
  const model::Workbook edited = t.d.wb();
  engine::CondFormatEval changed(edited, s);
  changed.KeepStatsFrom(ev);
  REQUIRE(changed.Eval(n / 2 - 1, 0, &(v = {})));
  CHECK(v.bar < half);
  // a short range is gathered again at once (its colors follow every change)
  yctest::Doc small;
  small.Set("A1", "1");
  small.Set("A2", "2");
  small.Set("A3", "3");
  model::CfRule sb;
  sb.type = model::CfType::DataBar;
  sb.ranges = {Rect{0, 0, 2, 0}};
  small.d.AddCondFormat(small.sheet(), sb);
  small.d.Recalc();
  const model::Workbook s0 = small.d.wb();
  engine::CondFormatEval e0(s0, small.sheet());
  REQUIRE(e0.Eval(1, 0, &(v = {})));
  const float mid = v.bar;
  small.d.SetValue(small.sheet(), 2, 0, Value::Number(1000));
  const model::Workbook s1 = small.d.wb();
  engine::CondFormatEval e1(s1, small.sheet());
  e1.KeepStatsFrom(e0);
  REQUIRE(e1.Eval(1, 0, &(v = {})));
  CHECK(v.bar < mid);
}

TEST_CASE("Sort works out each line's key once and still orders as the comparison of two values, stably") {
  // a comparison lowered and copied both texts every time: a million rows took four seconds. The order must stay
  // Evaluator::Compare's (texts in Windows' word sort with case ignored, numbers at 15 digits), types in Excel's
  // order (numbers, texts, logicals, errors, blanks last), equal keys in the order they had
  yctest::Doc t;
  const uint32_t s = t.sheet();
  const char* words[] = {"co-op", "coop", "cop", "Coop", "CO-OP", "apple", "Apple", "_x", "1a", "ete", "f", "zeta", "Zeta", "a'b", "ab", "AB"};
  uint32_t seed = 12345;
  auto rnd = [&] {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) & 0x7FFF;
  };
  constexpr uint32_t n = 3000;
  std::vector<Value> cells;  // two columns, row by row: the key, then where the line was
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t pick = rnd() % 12;
    Value v;
    if (pick < 5) v = Value::String(words[rnd() % std::size(words)]);
    else if (pick < 9) v = Value::Number((rnd() % 50) / 7.0 + (rnd() % 2 ? 1e-15 : 0.0));
    else if (pick == 9) v = Value::Bool(rnd() % 2 != 0);
    else if (pick == 10) v = Value::Error(yc::core::Err::Div0);
    cells.push_back(v);
    cells.push_back(Value::Number(i));
  }
  t.d.SetValues(s, Rect{0, 0, n - 1, 1}, cells);
  auto rank = [](const Value& v) { return v.IsNumeric() ? 0 : v.IsString() ? 1 : v.IsBool() ? 2 : v.IsError() ? 3 : 4; };
  auto compare = [&](const Value& x, const Value& y) {
    const int rx = rank(x), ry = rank(y);
    if (rx != ry) return rx < ry ? -1 : 1;
    if (rx == 4) return 0;
    bool ok;
    return yc::formula::Evaluator::Compare(x, y, &ok);
  };
  for (const bool descending : {false, true}) {
    t.d.Sort(s, Rect{0, 0, n - 1, 1}, {engine::SortKey{0, descending}}, false);
    uint32_t bad = 0;
    for (uint32_t r = 0; r + 1 < n; ++r) {
      const Value& x = t.d.GetValue(s, r, 0);
      const Value& y = t.d.GetValue(s, r + 1, 0);
      int c = compare(x, y);
      if (descending && rank(x) != 4 && rank(y) != 4) c = -c;  // blanks stay last either way
      if (c > 0) ++bad;
      if (c == 0 && !descending && t.d.GetValue(s, r, 1).AsDouble() > t.d.GetValue(s, r + 1, 1).AsDouble()) ++bad;  // stable
    }
    CHECK(bad == 0);
  }
}
