// SPDX-License-Identifier: Apache-2.0
// Round 2, events: the engine reports calculated and added sheets for Worksheet_Calculate and
// Workbook_NewSheet
#include <catch2/catch_test_macros.hpp>

#include "engine/engine.h"
#include "test_helpers.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace engine = yc::engine;

namespace {
bool Has(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }
}  // namespace

TEST_CASE("Round 2 events: a calculation reports its sheet once, also when no value changed") {
  yctest::Doc t;
  std::vector<uint32_t> calc, added;
  t.d.TakeSheetEvents(&calc, &added);
  t.Set("A1", "2");
  t.Set("B1", "=A1*2");
  t.Set("C1", "=A1+1");
  t.d.TakeSheetEvents(&calc, &added);
  CHECK(calc == std::vector<uint32_t>{t.sheet()});  // one entry for two formulas
  CHECK(added.empty());
  t.d.TakeSheetEvents(&calc, &added);
  CHECK(calc.empty());  // taken: nothing until the next calculation
  t.Set("A1", "2");     // the same value again: the formulas are calculated, their values don't change
  t.d.TakeSheetEvents(&calc, &added);
  CHECK(Has(calc, t.sheet()));
  t.Set("Z9", "text");  // nothing depends on it: no calculation
  t.d.TakeSheetEvents(&calc, &added);
  CHECK(calc.empty());
}

TEST_CASE("Round 2 events: added sheets are reported, copies aren't (Excel's NewSheet)") {
  yctest::Doc t;
  std::vector<uint32_t> calc, added;
  t.d.TakeSheetEvents(&calc, &added);
  const uint32_t a = t.d.AddSheet();
  t.d.TakeSheetEvents(&calc, &added);
  CHECK(added == std::vector<uint32_t>{a});
  const uint32_t copy = t.d.CopySheet(a);
  CHECK(copy != 0);
  t.d.TakeSheetEvents(&calc, &added);
  CHECK_FALSE(Has(added, copy));
}

TEST_CASE("Round 2 events: every published snapshot carries its calculated and added sheets to on_changes") {
  engine::Engine eng(yc::model::Workbook::NewDefault("Sheet"));
  std::mutex mu;
  std::vector<uint32_t> calc, added;
  eng.on_changes = [&](const std::vector<engine::CellChange>&, const engine::Snapshot& s) {
    std::lock_guard<std::mutex> lk(mu);
    calc.insert(calc.end(), s.calc_sheets.begin(), s.calc_sheets.end());
    added.insert(added.end(), s.added_sheets.begin(), s.added_sheets.end());
  };
  eng.Start();
  const uint32_t first = eng.Current()->wb.sheets[0]->id;
  eng.Call([&](engine::Document& d) {
    d.SetInput(first, 0, 0, "1");
    d.SetInput(first, 0, 1, "=A1+1");
    return 0;
  });
  eng.Sync();
  const uint32_t added_id = eng.Call([](engine::Document& d) { return d.AddSheet(); });
  eng.Sync();
  eng.Stop();
  std::lock_guard<std::mutex> lk(mu);
  CHECK(Has(calc, first));
  CHECK(Has(added, added_id));
}
