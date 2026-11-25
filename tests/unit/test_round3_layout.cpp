// SPDX-License-Identifier: Apache-2.0
// Round 3 (layout): Page Layout, Page Setup and printing as in Excel
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/page_layout.h"
#include "io/ycg.h"
#include "model/sheet.h"

using yc::core::Rect;

TEST_CASE("round3 layout: the Sheet tab's Comments / Cell errors as / Draft quality, more print areas and header pictures survive a save") {
  yctest::Doc t;
  REQUIRE(t.d.UpdatePageSetup(t.sheet(), [](yc::model::PageSetup& ps) {
    ps.print_comments = 1;
    ps.print_errors = 2;
    ps.draft = true;
    ps.has_area = true;
    ps.area = Rect{0, 0, 4, 2};
    ps.more_areas = {Rect{10, 0, 12, 1}, Rect{20, 3, 21, 3}};
    ps.header_center = "&G";
    ps.hf_pictures[1].image = std::string("\x89PNG\r\n\x1a\n", 8) + "data";
    ps.hf_pictures[1].w = 36;
    ps.hf_pictures[1].h = 18;
  }));
  REQUIRE(t.d.SetSheetBackground(t.sheet(), std::string("\xFF\xD8\xFF", 3) + "jpeg"));
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const yc::model::PageSetup& ps = *res.wb.sheets[0]->page_setup;
  CHECK(ps.print_comments == 1);
  CHECK(ps.print_errors == 2);
  CHECK(ps.draft);
  REQUIRE(ps.more_areas.size() == 2);
  CHECK(ps.more_areas[1] == Rect{20, 3, 21, 3});
  CHECK(ps.hf_pictures[1].image == std::string("\x89PNG\r\n\x1a\n", 8) + "data");
  CHECK(ps.hf_pictures[1].w == 36);
  CHECK(ps.hf_pictures[1].h == 18);
  CHECK(*res.wb.sheets[0]->background == std::string("\xFF\xD8\xFF", 3) + "jpeg");
  // a page setup without them stays Excel's 14 elements, and a sheet without a background has none
  yctest::Doc u;
  auto r2 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(u.d.wb(), {}));
  REQUIRE(r2.ok);
  CHECK_FALSE(r2.wb.sheets[0]->page_setup->HasExtras());
  CHECK(r2.wb.sheets[0]->background->empty());
}

TEST_CASE("round3 layout: several print areas print each on its own pages, in order") {
  yctest::Doc t;
  t.Set("A1", "a");
  yc::model::PageSetup ps = *t.d.wb().sheets[0]->page_setup;
  ps.has_area = true;
  ps.area = Rect{0, 0, 3, 1};
  ps.more_areas = {Rect{50, 4, 52, 5}};
  const auto L = yc::engine::LayoutPages(*t.d.wb().sheets[0], ps);
  REQUIRE(L.pages.size() == 2);
  CHECK(L.pages[0].area == 0);
  CHECK(L.pages[0].r0 == 0);
  CHECK(L.pages[1].area == 1);
  CHECK(L.pages[1].r0 == 50);
  CHECK(L.pages[1].c0 == 4);
  CHECK(L.pages[1].c1 == 5);
  REQUIRE(L.areas.size() == 2);
}

TEST_CASE("round3 layout: Format Text codes split a header into runs of one font; &G stands for the picture") {
  const auto runs = yc::engine::HeaderFooterRuns("&\"Arial,Bold Italic\"&14Big&\"-,Regular\"&10small &G", 1, 1, "f", "p", "s", "d", "t");
  REQUIRE(runs.size() >= 3);
  CHECK(runs[0].text == "Big");
  CHECK(runs[0].font == "Arial");
  CHECK(runs[0].bold);
  CHECK(runs[0].italic);
  CHECK(runs[0].size == 14);
  const auto& small = runs[runs.size() - 2];
  CHECK(small.text == "small ");
  CHECK_FALSE(small.bold);
  CHECK(small.size == 10);
  CHECK(small.font == "Arial");
  CHECK(runs.back().picture);
  // the colour code
  const auto red = yc::engine::HeaderFooterRuns("&KFF0000Red", 1, 1, "", "", "", "", "");
  REQUIRE(red.size() == 1);
  CHECK(red[0].color == 0xFFFF0000u);
  // fields shown while typing: &[Picture] is &G
  CHECK(yc::model::HeaderCodesToDisplay("&G") == "&[Picture]");
  CHECK(yc::model::HeaderCodesFromDisplay("&[Picture]") == "&G");
}

TEST_CASE("round3 layout: paper sizes the printer adds are used for the page") {
  {
    std::mutex* mu = nullptr;
    auto& m = yc::model::PrinterPaperSizes(&mu);
    std::lock_guard<std::mutex> g(*mu);
    m[256] = {300.0, 500.0};
  }
  double w = 0, h = 0;
  yc::model::PaperSize(256, &w, &h);
  CHECK(w == 300.0);
  CHECK(h == 500.0);
  yc::model::PaperSize(70, &w, &h);  // A6
  CHECK(w == 298.0);
}

TEST_CASE("round3 layout: Format Shape > Effects survive a save (shape extra key 30) and a theme style takes the scheme's effects") {
  yctest::Doc t;
  yc::model::ShapeObj a;
  a.fx.shadow = 1;
  a.fx.shadow_angle = 135;
  a.fx.shadow_distance = 7;
  a.fx.glow_size = 5;
  a.fx.glow_color = 0xFFFF0000;
  a.fx.soft_edge = 2.5f;
  a.fx.refl_size = 55;
  a.fx.bevel_top = 3;
  a.fx.bevel_top_w = 9;
  a.fx.rot_x = 45;
  a.fx.perspective = 30;
  a.fx.preset = 4;
  REQUIRE(t.d.AddShape(t.sheet(), a) != 0);
  yc::model::ShapeObj b;
  b.fx.theme_style = 2;
  REQUIRE(t.d.AddShape(t.sheet(), b) != 0);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& shapes = *res.wb.sheets[0]->shapes;
  REQUIRE(shapes.size() == 2);
  CHECK(shapes[0].fx == a.fx);
  CHECK(shapes[1].fx.theme_style == 2);
  // a shape without effects writes none (older readers see Excel's record as before)
  CHECK_FALSE(yc::model::ShapeEffects{}.Any());
  // the theme style's effects follow the scheme (Page Layout > Themes > Effects)
  const auto& all = yc::model::BuiltinThemeEffects();
  REQUIRE(all.size() >= 14);
  yc::model::CurrentThemeEffects().store(5);  // Glow Edge
  CHECK(yc::model::EffectiveEffects(b.fx).glow_size > 0);
  yc::model::CurrentThemeEffects().store(0);  // Office: the intense style is a shadow
  CHECK(yc::model::EffectiveEffects(b.fx).shadow == 1);
  CHECK(yc::model::EffectiveEffects(b.fx).glow_size == 0);
  CHECK(yc::model::PresetEffects(2).bevel_top == 1);
  CHECK(yc::model::PresetEffects(2).shadow == 1);
}
