// SPDX-License-Identifier: Apache-2.0
// Round 3 (insert): Excel's Shapes gallery, WordArt, controls, notes, sparklines and protection options, saved and
// drawn as Excel does. The UI parts are in python/tests/test_round3_insert.py.
#include "engine/document.h"
#include "engine/shape_geometry.h"
#include "engine/sparkline.h"
#include "io/ycg.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>

namespace eng = yc::engine;
namespace model = yc::model;
using yc::core::Rect;

namespace {

model::Workbook Reload(const model::Workbook& wb) {
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(wb, {}));
  REQUIRE(res.ok);
  return std::move(res.wb);
}

}  // namespace

TEST_CASE("insert: every preset shape of the gallery has a drawing inside its box") {
  int presets = 0;
  std::set<std::string> groups;
  for (const auto& e : eng::ShapeGallery()) {
    groups.insert(e.group);
    if (e.type <= 0) continue;
    ++presets;
    eng::ShapeGeom g;
    INFO(e.name);
    REQUIRE(eng::AutoShapeGeometry(e.type, 100, 60, &g));
    CHECK((!g.fills.empty() || !g.strokes.empty()));
    // the box's center is on the shape for the solid ones (not for lines, brackets, frames and the like)
    for (const auto& f : g.fills) CHECK(f.pts.size() >= 3);
  }
  CHECK(presets >= 140);
  CHECK(groups.size() == 8);  // Lines, Rectangles, Basic Shapes, Block Arrows, Equation Shapes, Flowchart, Stars and Banners, Callouts
  eng::ShapeGeom star;
  REQUIRE(eng::AutoShapeGeometry(92, 100, 100, &star));
  CHECK(eng::InShapeGeometry(star, {50, 50}));
  CHECK_FALSE(eng::InShapeGeometry(star, {2, 98}));  // a corner of the box is outside the star
  CHECK(std::string(eng::AutoShapeName(34)) == "Arrow: Left");
}

TEST_CASE("insert: a gallery shape is named as Excel names it and keeps its type, connector and points when saved") {
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  const uint32_t sheet = d.wb().sheets[0]->id;
  model::ShapeObj star;
  star.auto_type = 92;
  star.text_outline = 0xFF4472C4;
  star.text_shadow = true;
  const uint32_t a = d.AddShape(sheet, star);
  model::ShapeObj elbow;
  elbow.kind = model::ShapeKind::Arrow;
  elbow.connector = 2;
  d.AddShape(sheet, elbow);
  model::ShapeObj ff;
  ff.auto_type = eng::kFreeformType;
  ff.path = {0, 0, 1, 0.5, 0.2, 1};
  ff.path_closed = true;
  ff.path_smooth = true;
  d.AddShape(sheet, ff);
  const auto& shapes = *d.wb().sheets[0]->shapes;
  CHECK(shapes[0].name == "Star: 5 Points " + std::to_string(a));
  CHECK(shapes[1].name.rfind("Connector: Elbow", 0) == 0);
  CHECK(shapes[2].name.rfind("Freeform: Shape", 0) == 0);
  const model::Workbook wb = Reload(d.wb());
  const auto& s2 = *wb.sheets[0]->shapes;
  REQUIRE(s2.size() == 3);
  CHECK(s2[0].auto_type == 92);
  CHECK(s2[0].text_outline == 0xFF4472C4);
  CHECK(s2[0].text_shadow);
  CHECK(s2[1].connector == 2);
  CHECK(s2[2].path == std::vector<double>{0, 0, 1, 0.5, 0.2, 1});
  CHECK(s2[2].path_closed);
  CHECK(s2[2].path_smooth);
  eng::ShapeGeom g;
  CHECK(eng::ShapeObjGeometry(s2[2], 100, 100, &g));
  CHECK(g.fills.size() == 1);
}

TEST_CASE("insert: a control's Print object and Alt Text are saved") {
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  const uint32_t sheet = d.wb().sheets[0]->id;
  model::Control c;
  c.kind = model::ControlKind::CheckBox;
  c.form = true;
  c.print_object = false;
  c.alt_text = "Agree";
  d.AddControl(sheet, c);
  const model::Workbook wb = Reload(d.wb());
  REQUIRE(wb.sheets[0]->controls->size() == 1);
  CHECK_FALSE((*wb.sheets[0]->controls)[0].print_object);
  CHECK((*wb.sheets[0]->controls)[0].alt_text == "Agree");
}

TEST_CASE("insert: a note keeps its box (size, place, Format Comment) when edited and saved") {
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  const uint32_t sheet = d.wb().sheets[0]->id;
  d.SetNote(sheet, 2, 1, "me", "hello");
  model::CellNote::Look lk;
  lk.w = 210;
  lk.h = 40;
  lk.dx = 12;
  lk.dy = -6;
  lk.bold = true;
  lk.fill_color = 0xFFCCE5FF;
  lk.halign = 1;
  REQUIRE(d.SetNoteLook(sheet, 2, 1, lk));
  d.SetNote(sheet, 2, 1, "me", "hello again");  // typing keeps the box
  const auto key = (uint64_t{2} << 16) | 1;
  CHECK(d.wb().sheets[0]->notes->at(key).look == lk);
  const model::Workbook wb = Reload(d.wb());
  const auto& n = wb.sheets[0]->notes->at(key);
  CHECK(n.text == "hello again");
  CHECK(n.look == lk);
  CHECK_FALSE(n.threaded);
}

TEST_CASE("insert: sparkline Hidden & Empty Cells, axis options, Group and Ungroup") {
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  const uint32_t sheet = d.wb().sheets[0]->id;
  // A1:C1 = 1, (empty), 3; A2:C2 = 10, 20, 30
  d.SetValue(sheet, 0, 0, yc::core::Value::Number(1), eng::kSrcUser);
  d.SetValue(sheet, 0, 2, yc::core::Value::Number(3), eng::kSrcUser);
  for (uint32_t c = 0; c < 3; ++c) d.SetValue(sheet, 1, c, yc::core::Value::Number(10.0 * (c + 1)), eng::kSrcUser);
  model::SparklineGroup g1, g2;
  g1.items = {model::Sparkline{0, 4, sheet, Rect{0, 0, 0, 2}}};
  g2.items = {model::Sparkline{1, 4, sheet, Rect{1, 0, 1, 2}}};
  g2.color = 0xFF00FF00;
  const uint32_t id1 = d.AddSparklineGroup(sheet, g1);
  d.AddSparklineGroup(sheet, g2);
  auto group_of = [&](uint32_t id) -> const model::SparklineGroup& {
    const auto& all = *d.wb().sheets[0]->sparklines;
    const auto it = std::find_if(all.begin(), all.end(), [id](const model::SparklineGroup& g) { return g.id == id; });
    REQUIRE(it != all.end());  // (FAIL followed by a return is unreachable code: a warning the debug build stops on)
    return *it;
  };
  std::vector<double> v = eng::SparklineValues(d.wb(), group_of(id1), group_of(id1).items[0]);
  REQUIRE(v.size() == 3);
  CHECK(std::isnan(v[1]));  // gaps
  d.UpdateSparklineGroup(sheet, id1, [](model::SparklineGroup& g) { g.display_blanks = 2; });
  CHECK(eng::SparklineValues(d.wb(), group_of(id1), group_of(id1).items[0])[1] == 0);  // zero
  d.UpdateSparklineGroup(sheet, id1, [](model::SparklineGroup& g) { g.display_blanks = 3; });
  CHECK(eng::SparklineValues(d.wb(), group_of(id1), group_of(id1).items[0])[1] == 2);  // connected
  d.UpdateSparklineGroup(sheet, id1, [](model::SparklineGroup& g) { g.right_to_left = true; });
  CHECK(eng::SparklineValues(d.wb(), group_of(id1), group_of(id1).items[0])[0] == 3);
  // Group: both sparklines in the group at A... (the first one's look), Same for All: the group's bounds
  const uint32_t gid = d.GroupSparklines(sheet, {Rect{0, 4, 1, 4}}, 0, 4);
  CHECK(gid == id1);
  REQUIRE(d.wb().sheets[0]->sparklines->size() == 1);
  CHECK(group_of(gid).items.size() == 2);
  CHECK(group_of(gid).color != 0xFF00FF00);
  d.UpdateSparklineGroup(sheet, gid, [](model::SparklineGroup& g) {
    g.min_scale = 1;
    g.max_scale = 3;
    g.max_value = 50;
  });
  double lo = 0, hi = 0;
  eng::SparklineAxis(d.wb(), group_of(gid), &lo, &hi);
  CHECK(lo == 1);
  CHECK(hi == 50);
  // saved
  const model::Workbook wb = Reload(d.wb());
  const auto& sg = wb.sheets[0]->sparklines->front();
  CHECK(sg.display_blanks == 3);
  CHECK(sg.right_to_left);
  CHECK(sg.min_scale == 1);
  CHECK(sg.max_scale == 3);
  CHECK(sg.max_value == 50);
  d.UngroupSparklines(sheet, {Rect{0, 4, 1, 4}});
  CHECK(d.wb().sheets[0]->sparklines->size() == 2);
}

TEST_CASE("insert: Protect Sheet's Edit scenarios is kept and guards the scenarios") {
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  const uint32_t sheet = d.wb().sheets[0]->id;
  model::SheetProtection p;
  p.scenarios = false;
  d.ProtectSheet(sheet, p, "");
  model::Scenario sc;
  sc.name = "Best";
  sc.cells = {yc::core::CellPos{0, 0}};
  std::string err;
  CHECK_FALSE(d.AddScenario(sheet, sc, &err));
  d.UnprotectSheet(sheet, "");
  p.scenarios = true;
  d.ProtectSheet(sheet, p, "");
  CHECK(d.AddScenario(sheet, sc, &err));
  const model::Workbook wb = Reload(d.wb());
  CHECK(wb.sheets[0]->protection->scenarios);
}

TEST_CASE("insert: a preset shape's adjustments move its handles, change its drawing and are saved") {
  // a rounded rectangle: the corner handle dragged to a third of the shorter side
  std::vector<double> adj;
  REQUIRE(eng::AutoShapeAdjustDefaults(5).size() == 1);
  auto h0 = eng::AutoShapeHandles(5, 120, 60, adj);
  REQUIRE(h0.size() == 1);
  CHECK(std::abs(h0[0].x - 0.16667 * 60) < 0.01);
  REQUIRE(eng::AutoShapeDragHandle(5, 0, 120, 60, {20, 0}, &adj));
  REQUIRE(adj.size() == 1);
  CHECK(std::abs(adj[0] - 20.0 / 60) < 1e-9);
  // a right arrow: two handles (shaft, head)
  std::vector<double> arrow;
  CHECK(eng::AutoShapeHandles(33, 100, 40, arrow).size() == 2);
  eng::AutoShapeDragHandle(33, 1, 100, 40, {70, 0}, &arrow);
  CHECK(std::abs(arrow[1] - 30.0 / 40) < 1e-9);
  eng::ShapeGeom a, b;
  eng::AutoShapeGeometry(33, 100, 40, &a);
  eng::AutoShapeGeometry(33, 100, 40, &b, &arrow);
  CHECK(a.fills[0].pts[1].x != b.fills[0].pts[1].x);  // the head starts elsewhere
  // saved
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  model::ShapeObj sh;
  sh.auto_type = 33;
  sh.adj = arrow;
  d.AddShape(d.wb().sheets[0]->id, sh);
  const model::Workbook wb = Reload(d.wb());
  CHECK((*wb.sheets[0]->shapes)[0].adj == arrow);
}

TEST_CASE("insert: a form button's alignment, margins and 3-D shading are saved") {
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  model::Control c;
  c.kind = model::ControlKind::Button;
  c.form = true;
  c.text_halign = 0;
  c.text_valign = 2;
  c.text_orientation = 2;
  c.auto_size = true;
  c.auto_margins = false;
  c.margin_l = 9;
  c.shading_3d = true;
  d.AddControl(d.wb().sheets[0]->id, c);
  const model::Workbook wb = Reload(d.wb());
  const auto& x = (*wb.sheets[0]->controls)[0];
  CHECK(x.text_halign == 0);
  CHECK(x.text_valign == 2);
  CHECK(x.text_orientation == 2);
  CHECK(x.auto_size);
  CHECK_FALSE(x.auto_margins);
  CHECK(x.margin_l == 9);
  CHECK(x.shading_3d);
}

TEST_CASE("insert: Allow Edit Ranges' Permissions let an account edit without the password, and are saved") {
  eng::Document d(model::Workbook::NewDefault("Sheet"));
  const uint32_t sheet = d.wb().sheets[0]->id;
  model::AllowEditRange er;
  er.title = "Range1";
  er.rect = Rect{0, 0, 1, 1};
  er.hash = model::ProtectionHash("pw");
  er.users = {{"PC\\ann", true}, {"PC\\bob", false}};
  d.SetAllowEditRanges(sheet, {er});
  d.ProtectSheet(sheet, model::SheetProtection{}, "");
  d.current_user = "pc\\ANN";
  d.SetValue(sheet, 0, 0, yc::core::Value::Number(1), eng::kSrcUser);
  CHECK(d.wb().sheets[0]->GetValue(0, 0).AsDouble() == 1);  // allowed: no password asked
  d.current_user = "PC\\bob";
  d.SetValue(sheet, 1, 1, yc::core::Value::Number(2), eng::kSrcUser);
  CHECK(d.wb().sheets[0]->GetValue(1, 1).IsEmpty());  // denied: the password is needed
  const model::Workbook wb = Reload(d.wb());
  REQUIRE(wb.sheets[0]->protection->ranges.size() == 1);
  CHECK(wb.sheets[0]->protection->ranges[0].users == er.users);
}

TEST_CASE("insert: angles, arrows, ribbons, scrolls and sun have their handles") {
  for (int type : {142, 161, 25, 20, 60, 41, 42, 44, 45, 46, 47, 48, 97, 98, 99, 100, 101, 102, 23, 160, 159, 17, 29, 31, 57, 59}) {
    INFO(type);
    const auto defs = eng::AutoShapeAdjustDefaults(type);
    REQUIRE_FALSE(defs.empty());
    const auto hs = eng::AutoShapeHandles(type, 120, 80, {});
    CHECK_FALSE(hs.empty());
    eng::ShapeGeom g;
    CHECK(eng::AutoShapeGeometry(type, 120, 80, &g, &defs));
  }
  // the pie's first handle dragged to 12 o'clock: the slice starts at 270 degrees
  std::vector<double> pie;
  REQUIRE(eng::AutoShapeDragHandle(142, 0, 100, 100, {50, 0}, &pie));
  CHECK(pie[0] == 270);
  // a curved arrow's thickness follows its handle
  std::vector<double> cur;
  eng::AutoShapeDragHandle(45, 0, 100, 100, {30, 50}, &cur);
  CHECK(std::abs(cur[0] - 0.3) < 1e-9);
}
