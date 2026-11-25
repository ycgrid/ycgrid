// SPDX-License-Identifier: Apache-2.0
// Coverage of PivotTables in the Document (engine/document_pivot.cpp) beyond the other pivot tests: every style of the
// gallery painted on a report with banded rows and columns and subtotals, a report refused where it would cover
// another, Refresh All, and a timeline connected to two PivotTables.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "core/i18n.h"
#include "engine/pivot_engine.h"

using yc::core::Rect;
using yc::model::PivotTable;

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

void Fill(yctest::Doc& t) {
  const char* rows[][4] = {{"Region", "Product", "Sales", "Qty"}, {"East", "A", "10", "1"}, {"East", "B", "20", "2"}, {"West", "A", "30", "3"},
                           {"West", "B", "40", "4"},          {"East", "A", "5", "1"},  {"North", "B", "7", "1"}};
  for (int r = 0; r < 7; ++r) {
    for (int c = 0; c < 4; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
  }
}

yc::model::PivotAxisField Axis(int f) {
  yc::model::PivotAxisField a;
  a.field = f;
  return a;
}

yc::model::PivotDataField Data(int f) {
  yc::model::PivotDataField d;
  d.field = f;
  return d;
}

PivotTable At(yctest::Doc& t, uint32_t r0, uint32_t c0, Rect src = Rect{0, 0, 6, 3}) {
  PivotTable p;
  p.src_sheet = t.sheet();
  p.src = src;
  p.r0 = r0;
  p.c0 = c0;
  return p;
}

// does any cell of the report have this fill / this bottom line
bool AnyFill(yctest::Doc& t, const Rect& out, uint32_t fill) {
  for (uint32_t r = out.r0; r <= out.r1; ++r) {
    for (uint32_t c = out.c0; c <= out.c1; ++c) {
      if (t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(r, c)).fill_color == fill) return true;
    }
  }
  return false;
}
bool AnyBottomLine(yctest::Doc& t, const Rect& out, uint32_t color) {
  for (uint32_t r = out.r0; r <= out.r1; ++r) {
    for (uint32_t c = out.c0; c <= out.c1; ++c) {
      const auto& b = t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(r, c)).border[yc::model::kBottom];
      if (b.style != yc::model::LineStyle::None && b.color == color) return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("Pivot document coverage: every gallery style paints bands, subtotals and lines") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = At(t, 0, 6);
  p.rows = {Axis(0), Axis(1)};
  p.cols = {Axis(1)};
  p.data = {Data(2)};
  p.rows[0].sub_top = 0;  // subtotals below each region: rows of their own
  const uint32_t id = t.d.AddPivot(t.sheet(), p);
  REQUIRE(id != 0);
  for (const auto& name : yc::engine::PivotStyleNames()) {
    INFO(name);
    REQUIRE(t.d.UpdatePivot(t.sheet(), id, [&](PivotTable& x) {
      x.style = name;
      x.style_banded_rows = true;
      x.style_banded_cols = true;
    }));
    const auto look = yc::engine::PivotStyleFor(name);
    const Rect out = t.d.PivotById(t.sheet(), id)->output;
    if (look.band_fill) CHECK(AnyFill(t, out, look.band_fill));
    if (look.subtotal_fill) CHECK(AnyFill(t, out, look.subtotal_fill));
    if (look.body_line) CHECK(AnyBottomLine(t, out, look.body_line));
    if (look.header_line) CHECK(AnyBottomLine(t, out, look.header_line));
  }
}

TEST_CASE("Pivot document coverage: a report that would cover another is refused; Refresh All") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = At(t, 0, 6);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  std::string err;
  const uint32_t first = t.d.AddPivot(t.sheet(), p, &err);
  REQUIRE(first != 0);
  auto q = At(t, 8, 6);  // its empty placeholder starts at G9, below the first report
  const uint32_t second = t.d.AddPivot(t.sheet(), q, &err);
  REQUIRE(second != 0);
  const Rect before = t.d.PivotById(t.sheet(), first)->output;
  // regions and products: ten rows, into the second report's place
  CHECK_FALSE(t.d.UpdatePivot(t.sheet(), first, [](PivotTable& x) { x.rows.push_back(Axis(1)); }, &err));
  CHECK(err == "A PivotTable report cannot overlap another PivotTable report.");
  CHECK(t.d.PivotById(t.sheet(), first)->output == before);
  CHECK(t.d.PivotById(t.sheet(), first)->rows.size() == 1);
  // GETPIVOTDATA reads the report as it was refreshed: an edit of the source changes nothing until Refresh, for a
  // formula entered after the edit too (Excel 16, measured); Refresh All reads every source again
  t.Set("Z1", "=GETPIVOTDATA(\"Sales\",G1)");
  CHECK(t.N("Z1") == 112);
  t.Set("C2", "100");
  CHECK(t.N("Z1") == 112);
  t.Set("Z2", "=GETPIVOTDATA(\"Sales\",G1,\"Region\",\"East\")");
  CHECK(t.N("Z2") == 35);
  t.d.RefreshAllPivots();
  t.d.Recalc();
  CHECK(t.N("Z1") == 202);
  CHECK(t.N("Z2") == 125);
  // the next edit waits for its own refresh
  t.Set("C2", "1");
  CHECK(t.N("Z2") == 125);
  REQUIRE(t.d.RefreshPivot(t.sheet(), first));
  t.d.Recalc();
  CHECK(t.N("Z2") == 26);
}

TEST_CASE("Pivot document coverage: a timeline filters every PivotTable it is connected to") {
  English en;
  yctest::Doc t;
  t.Set("A1", "Date");
  t.Set("B1", "Sales");
  const char* dates[] = {"2024-01-05", "2024-02-10", "2024-03-15", "2024-04-20"};
  for (int i = 0; i < 4; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), dates[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string((i + 1) * 10));
  }
  auto p = At(t, 0, 4, Rect{0, 0, 4, 1});
  p.data = {Data(1)};
  const uint32_t one = t.d.AddPivot(t.sheet(), p);
  auto q = At(t, 0, 8, Rect{0, 0, 4, 1});
  q.data = {Data(1)};
  const uint32_t two = t.d.AddPivot(t.sheet(), q);
  REQUIRE((one != 0 && two != 0));
  yc::model::Slicer sl;
  sl.pivot = t.d.PivotById(t.sheet(), one)->name;
  sl.field = 0;
  sl.timeline = true;
  sl.more_pivots = {t.d.PivotById(t.sheet(), two)->name, "NoSuchPivot"};
  const uint32_t sid = t.d.AddSlicer(t.sheet(), sl);
  REQUIRE(sid != 0);
  t.Set("Z1", "=GETPIVOTDATA(\"Sales\",E1)");
  t.Set("Z2", "=GETPIVOTDATA(\"Sales\",I1)");
  CHECK(t.N("Z1") == 100);
  // February 1 to March 31: the second and third records
  std::string err;
  CHECK(t.d.SetSlicerDates(t.sheet(), sid, 45323, 45382, &err));
  t.d.Recalc();
  CHECK(t.N("Z1") == 50);
  CHECK(t.N("Z2") == 50);
  CHECK_FALSE(t.d.SetSlicerDates(t.sheet(), 9999, 45323, 45382, &err));
  CHECK_FALSE(t.d.SetSlicerDates(999, sid, 45323, 45382, &err));
}
