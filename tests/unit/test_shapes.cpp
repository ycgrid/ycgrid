// SPDX-License-Identifier: Apache-2.0
// Pictures, shapes, text boxes and sparklines
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/ycg.h"

using yc::core::Rect;

TEST_CASE("shapes: add, update, delete, save and load") {
  yctest::Doc t;
  yc::model::ShapeObj a;
  a.kind = yc::model::ShapeKind::Oval;
  a.left = 10;
  a.top = 20;
  a.text = "Hello";
  const uint32_t id = t.d.AddShape(t.sheet(), a);
  REQUIRE(id != 0);
  yc::model::ShapeObj b;
  b.kind = yc::model::ShapeKind::Picture;
  b.image = std::string("\x89PNG\r\n\x1a\n\0\x01", 10);
  b.width = 30;
  const uint32_t pid = t.d.AddShape(t.sheet(), b);
  const auto& shapes = *t.d.wb().sheets[0]->shapes;
  REQUIRE(shapes.size() == 2);
  CHECK(shapes[0].name == "Oval " + std::to_string(id));
  CHECK(shapes[1].name == "Picture " + std::to_string(pid));
  CHECK(shapes[1].z > shapes[0].z);
  REQUIRE(t.d.UpdateShape(t.sheet(), id, [](yc::model::ShapeObj& x) {
    x.fill_color = 0xFF00FF00;
    x.bold = true;
    x.halign = 2;
  }));
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& ls = *res.wb.sheets[0]->shapes;
  REQUIRE(ls.size() == 2);
  CHECK(ls[0].kind == yc::model::ShapeKind::Oval);
  CHECK(ls[0].left == 10);
  CHECK(ls[0].top == 20);
  CHECK(ls[0].text == "Hello");
  CHECK(ls[0].fill_color == 0xFF00FF00);
  CHECK(ls[0].bold);
  CHECK(ls[0].halign == 2);
  CHECK(ls[1].image == b.image);
  CHECK(ls[1].width == 30);
  REQUIRE(t.d.DeleteShape(t.sheet(), id));
  CHECK(t.d.wb().sheets[0]->shapes->size() == 1);
}

TEST_CASE("sparklines: groups follow inserted rows and survive a save") {
  yctest::Doc t;
  for (int r = 1; r <= 3; ++r) {
    for (int c = 0; c < 4; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r)).c_str(), std::to_string(r * c - 2));
  }
  yc::model::SparklineGroup g;
  g.type = 1;
  g.high = true;
  // A1:D3 -> E1:E3, one sparkline per row
  REQUIRE(yc::engine::BuildSparklines(t.sheet(), Rect{0, 0, 2, 3}, Rect{0, 4, 2, 4}, &g.items));
  REQUIRE(g.items.size() == 3);
  CHECK(g.items[1].src == Rect{1, 0, 1, 3});
  const uint32_t id = t.d.AddSparklineGroup(t.sheet(), g);
  REQUIRE(id != 0);
  CHECK(t.d.SparklineGroupAt(t.sheet(), 2, 4) != nullptr);
  // a row inserted above row 2 moves the second and third sparklines and their data
  t.d.InsertRows(t.sheet(), 1, 1);
  const auto* sg = t.d.SparklineGroupAt(t.sheet(), 3, 4);
  REQUIRE(sg != nullptr);
  CHECK(sg->items[0].r == 0);
  CHECK(sg->items[1].r == 2);
  CHECK(sg->items[1].src == Rect{2, 0, 2, 3});
  // clearing one cell keeps the others; the group disappears with the last one
  t.d.ClearSparklines(t.sheet(), Rect::Cell(0, 4));
  CHECK(t.d.SparklineGroupAt(t.sheet(), 0, 4) == nullptr);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.sheets[0]->sparklines->size() == 1);
  const auto& lg = res.wb.sheets[0]->sparklines->front();
  CHECK(lg.type == 1);
  CHECK(lg.high);
  CHECK(!lg.low);
  REQUIRE(lg.items.size() == 2);
  CHECK(lg.items[1].r == 3);
  CHECK(lg.items[1].src == Rect{3, 0, 3, 3});
  t.d.ClearSparklines(t.sheet(), Rect{0, 0, 10, 10});
  CHECK(t.d.wb().sheets[0]->sparklines->empty());
}

TEST_CASE("controls: the newer kinds keep their properties through a save") {
  yctest::Doc t;
  yc::model::Control cb;
  cb.kind = yc::model::ControlKind::CheckBox;
  const uint32_t id1 = t.d.AddControl(t.sheet(), cb);
  yc::model::Control sp;
  sp.kind = yc::model::ControlKind::SpinButton;
  sp.min = 5;
  sp.max = 50;
  sp.small_change = 5;
  sp.orientation = 1;
  t.d.AddControl(t.sheet(), sp);
  yc::model::Control lb;
  lb.kind = yc::model::ControlKind::ListBox;
  lb.multi_select = 1;
  lb.selected = {0, 2};
  lb.items = {"a", "b", "c"};
  lb.italic = true;
  lb.list_rows = 5;
  t.d.AddControl(t.sheet(), lb);
  yc::model::Control im;
  im.kind = yc::model::ControlKind::Image;
  im.picture = std::string("\x89PNG\0\x01", 6);
  im.picture_size_mode = 3;
  t.d.AddControl(t.sheet(), im);
  yc::model::Control ob;
  ob.kind = yc::model::ControlKind::OptionButton;
  ob.group_name = "side";
  t.d.AddControl(t.sheet(), ob);
  const auto& ctls = *t.d.wb().sheets[0]->controls;
  REQUIRE(ctls.size() == 5);
  CHECK(ctls[0].name == "CheckBox1");
  CHECK(ctls[0].caption == "CheckBox1");
  CHECK(ctls[0].value == "False");
  CHECK(ctls[1].name == "SpinButton1");
  CHECK(ctls[1].value == "5");
  CHECK(ctls[3].name == "Image1");
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& lc = *res.wb.sheets[0]->controls;
  REQUIRE(lc.size() == 5);
  CHECK(lc[0].kind == yc::model::ControlKind::CheckBox);
  CHECK(lc[0].id == id1);
  CHECK(lc[1].min == 5);
  CHECK(lc[1].max == 50);
  CHECK(lc[1].small_change == 5);
  CHECK(lc[1].orientation == 1);
  CHECK(lc[2].multi_select == 1);
  CHECK(lc[2].selected == std::vector<int>{0, 2});
  CHECK(lc[2].italic);
  CHECK(lc[2].list_rows == 5);
  CHECK(lc[3].picture == im.picture);
  CHECK(lc[3].picture_size_mode == 3);
  CHECK(lc[4].group_name == "side");
}

TEST_CASE("controls: form controls drawn in the UI are named as Excel names them") {
  // Developer > Insert > Form Controls gave ActiveX names (ComboBox1, SpinButton1); Shapes("Drop Down 1") failed
  yctest::Doc t;
  yc::model::Control dd;
  dd.kind = yc::model::ControlKind::ComboBox;
  dd.form = true;
  t.d.AddControl(t.sheet(), dd);
  yc::model::Control sp;
  sp.kind = yc::model::ControlKind::SpinButton;
  sp.form = true;
  t.d.AddControl(t.sheet(), sp);
  t.d.AddControl(t.sheet(), sp);
  yc::model::Control ax;
  ax.kind = yc::model::ControlKind::SpinButton;  // an ActiveX control keeps its own name
  t.d.AddControl(t.sheet(), ax);
  const auto& ctls = *t.d.wb().sheets[0]->controls;
  REQUIRE(ctls.size() == 4);
  CHECK(ctls[0].name == "Drop Down 1");
  CHECK(ctls[1].name == "Spinner 1");
  CHECK(ctls[2].name == "Spinner 2");
  CHECK(ctls[3].name == "SpinButton1");
}
