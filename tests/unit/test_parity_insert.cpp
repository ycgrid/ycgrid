// SPDX-License-Identifier: Apache-2.0
// Excel parity: shapes (format, protection, groups), page setup header / footer
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/ycg.h"

using yc::model::ShapeKind;
using yc::model::ShapeObj;

TEST_CASE("parity insert: the shape's Format Shape fields survive a save (element #12 of the shape record)") {
  yctest::Doc t;
  ShapeObj a;
  a.kind = ShapeKind::TextBox;
  a.text = "abc";
  a.line_dash = 4;
  a.begin_arrow = 1;
  a.end_arrow = 1;
  a.fill_transparency = 0.25f;
  a.margin_l = 1;
  a.margin_t = 2;
  a.margin_r = 3;
  a.margin_b = 4;
  a.wrap = false;
  a.autofit = true;
  a.alt_text = "a box";
  a.locked = false;
  a.lock_aspect = true;
  a.rotation = 90;
  a.group = 77;
  a.print_object = false;
  const uint32_t id = t.d.AddShape(t.sheet(), a);
  REQUIRE(id != 0);
  ShapeObj p;
  p.kind = ShapeKind::Picture;
  p.image = std::string("\x89PNG\r\n\x1a\n\0\x01", 10);
  p.width = 200;
  p.height = 100;
  p.crop_l = 5;
  p.crop_b = 6;
  const uint32_t pid = t.d.AddShape(t.sheet(), p);
  REQUIRE(pid != 0);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& ls = *res.wb.sheets[0]->shapes;
  REQUIRE(ls.size() == 2);
  const ShapeObj& x = ls[0];
  CHECK(x.text == "abc");
  CHECK(x.line_dash == 4);
  CHECK(x.begin_arrow == 1);
  CHECK(x.end_arrow == 1);
  CHECK(x.fill_transparency == 0.25f);
  CHECK(x.margin_l == 1);
  CHECK(x.margin_t == 2);
  CHECK(x.margin_r == 3);
  CHECK(x.margin_b == 4);
  CHECK_FALSE(x.wrap);
  CHECK(x.autofit);
  CHECK(x.alt_text == "a box");
  CHECK_FALSE(x.locked);
  CHECK(x.lock_aspect);
  CHECK(x.rotation == 90);
  CHECK(x.group == 77);
  CHECK_FALSE(x.print_object);
  // a picture keeps its proportions by default, and remembers the size it was inserted at (Scale Height / Width)
  const ShapeObj& y = ls[1];
  CHECK(y.lock_aspect);
  CHECK(y.orig_w == 200);
  CHECK(y.orig_h == 100);
  CHECK(y.crop_l == 5);
  CHECK(y.crop_b == 6);
  CHECK(y.locked);
  CHECK(y.print_object);
  // saved again, nothing changes (the reader leaves the stream where the next record starts)
  auto res2 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(res.wb, {}));
  REQUIRE(res2.ok);
  CHECK((*res2.wb.sheets[0]->shapes)[0].alt_text == "a box");
  CHECK((*res2.wb.sheets[0]->shapes)[1].orig_w == 200);
}

TEST_CASE("parity insert: a locked shape on a protected sheet can't be moved or formatted (H4)") {
  yctest::Doc t;
  ShapeObj a;
  a.left = 100;
  const uint32_t locked = t.d.AddShape(t.sheet(), a);
  ShapeObj b;
  b.left = 100;
  b.locked = false;
  const uint32_t unlocked = t.d.AddShape(t.sheet(), b);
  yc::model::SheetProtection pr;
  pr.on = true;
  REQUIRE(t.d.ProtectSheet(t.sheet(), pr, ""));
  CHECK_FALSE(t.d.UpdateShape(t.sheet(), locked, [](ShapeObj& x) { x.left = 150; }));
  CHECK((*t.d.wb().sheets[0]->shapes)[0].left == 100);
  // an unlocked one moves
  CHECK(t.d.UpdateShape(t.sheet(), unlocked, [](ShapeObj& x) { x.left = 150; }));
  CHECK((*t.d.wb().sheets[0]->shapes)[1].left == 150);
  // "Edit objects" allowed: the locked one moves too
  REQUIRE(t.d.UnprotectSheet(t.sheet(), ""));
  pr.objects = true;
  REQUIRE(t.d.ProtectSheet(t.sheet(), pr, ""));
  CHECK(t.d.UpdateShape(t.sheet(), locked, [](ShapeObj& x) { x.left = 150; }));
  CHECK((*t.d.wb().sheets[0]->shapes)[0].left == 150);
}

TEST_CASE("parity insert: different first page and odd / even headers survive a save and pick the page's texts (M13)") {
  yctest::Doc t;
  REQUIRE(t.d.UpdatePageSetup(t.sheet(), [](yc::model::PageSetup& ps) {
    ps.header_center = "Every page";
    ps.different_first = true;
    ps.different_odd_even = true;
    ps.no_scale_with_doc = true;
    ps.no_align_margins = true;
    ps.first_hf[1] = "Cover";
    ps.even_hf[4] = "Even &P";
  }));
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const yc::model::PageSetup& ps = *res.wb.sheets[0]->page_setup;
  CHECK(ps.different_first);
  CHECK(ps.different_odd_even);
  CHECK(ps.no_scale_with_doc);
  CHECK(ps.no_align_margins);
  CHECK(ps.first_hf[1] == "Cover");
  CHECK(ps.even_hf[4] == "Even &P");
  CHECK(ps.header_center == "Every page");
  // page 1: the first page's header; page 2 (even): the even pages' footer and no header text; page 3: the usual one
  CHECK(yc::model::HeaderFooterText(ps, 1, 0, 1) == "Cover");
  CHECK(yc::model::HeaderFooterText(ps, 1, 1, 2).empty());
  CHECK(yc::model::HeaderFooterText(ps, 4, 1, 2) == "Even &P");
  CHECK(yc::model::HeaderFooterText(ps, 1, 2, 3) == "Every page");
  // without the extra texts the record keeps Excel's six (old builds read it as before)
  yctest::Doc u;
  REQUIRE(u.d.UpdatePageSetup(u.sheet(), [](yc::model::PageSetup& p) { p.footer_right = "&N"; }));
  auto r2 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(u.d.wb(), {}));
  REQUIRE(r2.ok);
  CHECK(r2.wb.sheets[0]->page_setup->footer_right == "&N");
  CHECK_FALSE(r2.wb.sheets[0]->page_setup->different_first);
}
