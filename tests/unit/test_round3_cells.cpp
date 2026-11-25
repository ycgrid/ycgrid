// SPDX-License-Identifier: Apache-2.0
// Round 3 (cells): the fill handle's tip while it is dragged, Insert / Delete on every area, and linked pictures saved
// in .ycg
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/ycg.h"

using yc::core::Rect;
namespace engine = yc::engine;
namespace io = yc::io;

namespace {

Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}

std::string Tip(yctest::Doc& t, const char* src, const char* dest, int type = 0) {
  return engine::Document::AutoFillTip(t.d.wb(), *t.d.S(t.sheet()), R(src), R(dest), type);
}

}  // namespace

TEST_CASE("Round 3 cells: the fill handle's tip shows what the last cell gets") {
  yctest::Doc t;
  t.Set("A1", "1");
  CHECK(Tip(t, "A1", "A1:A5") == "1");      // one number is copied
  CHECK(Tip(t, "A1", "A1:A5", 2) == "5");   // Ctrl: a series
  t.Set("B1", "2");
  t.Set("B2", "4");
  CHECK(Tip(t, "B1:B2", "B1:B6") == "12");  // 2, 4, 6... the sixth is 12
  t.Set("C1", "Jan");
  CHECK(Tip(t, "C1", "C1:C5") == "May");
  t.Set("D1", "Item1");
  CHECK(Tip(t, "D1", "D1:H1") == "Item5");  // sideways
  t.Set("E5", "10");
  t.Set("E6", "20");
  CHECK(Tip(t, "E5:E6", "E2:E6") == "-20");  // upwards: 10, 0, -10, -20
  t.Set("F1", "=A1*2");
  CHECK(Tip(t, "F1", "F1:F4").empty());       // a formula: no value (Excel)
}

TEST_CASE("Round 3 cells: a date's tip is in its number format") {
  yctest::Doc t;
  t.Set("A1", "2026-11-02");
  const std::string first = t.T("A1");
  t.Set("A2", "2026-11-04");
  const std::string third = t.T("A2");
  // the default fill of one date steps by days: two cells on is the 4th, shown as the cell shows dates
  CHECK(Tip(t, "A1", "A1:A3") == third);
  CHECK(first != third);
}

TEST_CASE("Round 3 cells: linked pictures are saved in .ycg (sheet key 70) and loaded back") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  yc::model::ShapeObj pic;
  pic.kind = yc::model::ShapeKind::Picture;
  pic.image = std::string("\x89PNG", 4);
  const uint32_t id = t.d.AddShape(sh, pic);
  REQUIRE(id != 0);
  t.d.SetLinkedPicture(sh, id, sh, R("A1:C4"));
  REQUIRE(t.d.S(sh)->linked_pictures->size() == 1);
  const std::string bytes = io::SaveYcgToBytes(t.d.wb(), {});
  auto res = io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  const auto& lps = *res.wb.sheets[0]->linked_pictures;
  REQUIRE(lps.size() == 1);
  CHECK(lps[0].shape == id);
  CHECK(lps[0].sheet == sh);
  CHECK(lps[0].rect == R("A1:C4"));
  // unlinked: nothing is written
  t.d.SetLinkedPicture(sh, id, sh, Rect{1, 0, 0, 0});
  CHECK(t.d.S(sh)->linked_pictures->empty());
  auto res2 = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res2.ok);
  CHECK(res2.wb.sheets[0]->linked_pictures->empty());
}

TEST_CASE("Round 3 cells: Filter by Cell Icon shows the rows whose icon matches, and is saved") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "n");
  for (int i = 1; i <= 9; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), std::to_string(i * 10));  // 10..90
  t.Set("A11", "x");  // text: no icon
  yc::model::CfRule rule;
  rule.type = yc::model::CfType::IconSet;
  rule.icon_style = yc::model::kIcon3Arrows;
  rule.icon_thresholds = yc::model::DefaultIconThresholds(rule.icon_style);
  rule.ranges = {R("A2:A11")};
  t.d.AddCondFormat(sh, rule);
  REQUIRE(t.d.SetAutoFilter(sh, R("A1:A11")));
  yc::model::FilterColumn fc;
  fc.kind = 7;
  fc.icon_set = yc::model::kIcon3Arrows;
  fc.icon = 2;  // the up arrow: the highest third
  t.d.SetFilterColumn(sh, fc);
  const auto* s = t.d.S(sh);
  CHECK(s->rows->IsHidden(1));    // 10
  CHECK(s->rows->IsHidden(4));    // 40
  CHECK_FALSE(s->rows->IsHidden(9));  // 90
  CHECK(s->rows->IsHidden(10));   // "x" has no icon
  // No Cell Icon: only the text row
  fc.icon_set = 0;
  fc.icon = -1;
  t.d.SetFilterColumn(sh, fc);
  CHECK(t.d.S(sh)->rows->IsHidden(9));
  CHECK_FALSE(t.d.S(sh)->rows->IsHidden(10));
  // saved in the AutoFilter map (keys 14, 15) and read back
  fc.icon_set = yc::model::kIcon3Arrows;
  fc.icon = 1;
  t.d.SetFilterColumn(sh, fc);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& af = *res.wb.sheets[0]->autofilter;
  REQUIRE(af.columns.size() == 1);
  CHECK(af.columns[0].kind == 7);
  CHECK(af.columns[0].icon_set == yc::model::kIcon3Arrows);
  CHECK(af.columns[0].icon == 1);
}

TEST_CASE("Round 3 cells: grouped sheets' tabs are saved (sheet key 71) with linked pictures (70) and a background (100)") {
  yctest::Doc t;
  const uint32_t a = t.sheet();
  const uint32_t b = t.d.AddSheet("Two");
  const uint32_t c = t.d.AddSheet("Three");
  t.d.SetTabsSelected({a, c});
  yc::model::ShapeObj pic;
  pic.kind = yc::model::ShapeKind::Picture;
  pic.image = std::string("\x89PNG", 4);
  const uint32_t id = t.d.AddShape(a, pic);
  t.d.SetLinkedPicture(a, id, b, R("B2:C3"));
  REQUIRE(t.d.SetSheetBackground(a, std::string("\xFF\xD8\xFF", 3) + "jpeg"));
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets.size() == 3);
  CHECK(res.wb.sheets[0]->tab_selected);
  CHECK_FALSE(res.wb.sheets[1]->tab_selected);
  CHECK(res.wb.sheets[2]->tab_selected);
  REQUIRE(res.wb.sheets[0]->linked_pictures->size() == 1);
  CHECK((*res.wb.sheets[0]->linked_pictures)[0].sheet == b);
  CHECK((*res.wb.sheets[0]->linked_pictures)[0].rect == R("B2:C3"));
  CHECK(*res.wb.sheets[0]->background == std::string("\xFF\xD8\xFF", 3) + "jpeg");
  // a sheet after it (keys read in order) is whole too
  CHECK(res.wb.sheets[1]->name == "Two");
}
