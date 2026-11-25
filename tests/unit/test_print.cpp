// SPDX-License-Identifier: Apache-2.0
// Page setup and splitting a sheet into printed pages
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "engine/page_layout.h"
#include "io/ycg.h"

#include <utility>

using yc::core::Rect;
using Seg = std::pair<uint32_t, uint32_t>;

TEST_CASE("page layout: automatic pages, order, fit to page, titles and breaks") {
  yctest::Doc t;
  t.Set("A1", "top");
  t.Set("T100", "end");  // 20 columns x 100 rows
  const yc::model::Sheet& sh = *t.d.wb().sheets[0];
  yc::model::PageSetup ps;  // A4 portrait, normal margins: 494.2 x 734 pt; columns 48 pt, rows 15 pt
  auto L = yc::engine::LayoutPages(sh, ps);
  REQUIRE(!L.empty);
  CHECK(L.area == Rect{0, 0, 99, 19});
  REQUIRE(L.col_segs.size() == 2);
  CHECK(L.col_segs[0] == Seg{0, 9});
  REQUIRE(L.row_segs.size() == 3);
  CHECK(L.row_segs[0] == Seg{0, 47});
  CHECK(L.row_segs[2] == Seg{96, 99});
  REQUIRE(L.pages.size() == 6);
  // down, then over
  CHECK(L.pages[1].r0 == 48);
  CHECK(L.pages[1].c0 == 0);
  CHECK(L.pages[3].c0 == 10);
  ps.over_then_down = true;
  L = yc::engine::LayoutPages(sh, ps);
  CHECK(L.pages[1].r0 == 0);
  CHECK(L.pages[1].c0 == 10);
  // landscape: wider pages
  ps = {};
  ps.landscape = true;
  L = yc::engine::LayoutPages(sh, ps);
  CHECK(L.col_segs.size() == 2);
  CHECK(L.col_segs[0].second == 14);  // 741.6 pt wide
  // fit to one page
  ps = {};
  ps.zoom = 0;
  L = yc::engine::LayoutPages(sh, ps);
  CHECK(L.pages.size() == 1);
  CHECK(L.scale == 0.48);
  // fit all columns on one page: as many pages tall as needed
  ps.fit_tall = 0;
  L = yc::engine::LayoutPages(sh, ps);
  CHECK(L.col_segs.size() == 1);
  CHECK(L.row_segs.size() == 2);
  // print titles take room on the pages after the first
  ps = {};
  ps.has_title_rows = true;
  L = yc::engine::LayoutPages(sh, ps);
  CHECK(L.row_segs[1] == Seg{48, 94});
  CHECK(!L.pages[0].title_rows);
  CHECK(L.pages[1].title_rows);
  // manual page break and print area
  ps = {};
  ps.row_breaks = {10};
  ps.has_area = true;
  ps.area = Rect{0, 0, 29, 3};
  L = yc::engine::LayoutPages(sh, ps);
  REQUIRE(L.row_segs.size() == 2);
  CHECK(L.row_segs[0] == Seg{0, 9});
  CHECK(L.col_segs.size() == 1);
  // headings
  ps = {};
  ps.headings = true;
  L = yc::engine::LayoutPages(sh, ps);
  CHECK(L.heading_h == 15);
  CHECK(L.row_segs[0] == Seg{0, 46});
}

TEST_CASE("page layout: header and footer codes") {
  using yc::engine::ExpandHeaderFooter;
  CHECK(ExpandHeaderFooter("Page &P of &N - &A&&", 2, 5, "Book.ycg", "C:\\", "Sheet1", "2026/1/2", "10:00") == "Page 2 of 5 - Sheet1&");
  CHECK(ExpandHeaderFooter("&\"Arial,Bold\"&14&BTitle&B &F", 1, 1, "Book.ycg", "", "S", "", "") == "Title Book.ycg");
  CHECK(ExpandHeaderFooter("&D &T &P+10", 3, 9, "", "", "", "D", "T") == "D T 13");
}

TEST_CASE("page setup: saved, and the print area follows inserted rows") {
  yctest::Doc t;
  t.Set("A1", "1");
  REQUIRE(t.d.UpdatePageSetup(t.sheet(), [](yc::model::PageSetup& p) {
    p.landscape = true;
    p.paper = 1;
    p.left = 36;
    p.zoom = 0;
    p.fit_wide = 1;
    p.fit_tall = 0;
    p.has_area = true;
    p.area = Rect{1, 0, 9, 3};
    p.has_title_rows = true;
    p.title_r0 = p.title_r1 = 0;
    p.gridlines = true;
    p.center_h = true;
    p.footer_center = "Page &P of &N";
    p.row_breaks = {5, 3};
  }));
  CHECK(t.d.wb().sheets[0]->page_setup->row_breaks == std::vector<uint32_t>{3, 5});
  t.d.InsertRows(t.sheet(), 2, 2);
  const auto& ps = *t.d.wb().sheets[0]->page_setup;
  CHECK(ps.area == Rect{1, 0, 11, 3});
  CHECK(ps.row_breaks == std::vector<uint32_t>{5, 7});
  CHECK(ps.title_r1 == 0);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& lp = *res.wb.sheets[0]->page_setup;
  CHECK(lp == *t.d.wb().sheets[0]->page_setup);
  CHECK(lp.landscape);
  CHECK(lp.zoom == 0);
  CHECK(lp.fit_tall == 0);
  CHECK(lp.footer_center == "Page &P of &N");
  CHECK(lp.gridlines);
  CHECK(!lp.headings);
}
