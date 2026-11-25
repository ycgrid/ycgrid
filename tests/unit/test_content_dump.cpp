// SPDX-License-Identifier: Apache-2.0
// io::ContentDump, the oracle of scripts\basics.py (Undo gives back what was there, read-only mode changes nothing):
// it must change with every change of content and with none of the window's state.
#include <catch2/catch_test_macros.hpp>

#include "io/content_dump.h"
#include "test_helpers.h"

#include <functional>
#include <string>

using yc::core::Rect;

TEST_CASE("Content dump: the same workbook dumps the same; the window's state doesn't count") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("B2", "=A1*2");
  const std::string a = yc::io::ContentDump(t.d.wb());
  CHECK(a == yc::io::ContentDump(t.d.wb()));
  CHECK(a.find("B2\t=A1*2\t2") != std::string::npos);
  // the selection, scroll and zoom belong to the window
  yc::model::Workbook w = t.d.wb();
  auto& v = w.sheets[0].mut().view;
  v.act_row = 5;
  v.top_row = 3;
  v.zoom = 2.0f;
  w.active_sheet = 0;
  CHECK(yc::io::ContentDump(w) == a);
}

TEST_CASE("Content dump: every kind of change shows, and Undo gives the first dump back") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("A2", "two");
  t.Set("B1", "=A1+1");
  const std::string before = yc::io::ContentDump(t.d.wb());
  auto changes = [&](const char* what, const std::function<void()>& act) {
    INFO(what);
    const size_t depth = t.d.UndoDepth();
    t.d.Begin(what);
    act();
    t.d.Commit();
    t.d.Recalc();
    REQUIRE(t.d.UndoDepth() > depth);
    const std::string after = yc::io::ContentDump(t.d.wb());
    CHECK(after != before);
    while (t.d.UndoDepth() > depth) REQUIRE(t.d.Undo());
    t.d.Recalc();
    CHECK(yc::io::ContentDump(t.d.wb()) == before);
  };
  changes("a value", [&] { t.d.SetInput(sh, 0, 0, "5"); });
  changes("a format", [&] { t.d.ModifyStyle(sh, Rect{0, 0, 0, 0}, [](yc::model::Style& s) { s.bold = true; }); });
  changes("a merge", [&] { t.d.Merge(sh, Rect{3, 0, 4, 1}); });
  changes("hidden rows", [&] { t.d.HideRows(sh, 1, 1, true); });
  changes("inserted rows", [&] { t.d.InsertRows(sh, 0, 2); });
  changes("a name", [&] { t.d.DefineName(yc::model::NameDef{"Price", "=Sheet1!$A$1", -1}, nullptr); });
}
