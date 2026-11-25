// SPDX-License-Identifier: Apache-2.0
// document_shapes.cpp - pictures, shapes, text boxes and sparklines
#include "engine/document.h"
#include "engine/shape_geometry.h"

#include <algorithm>

namespace yc::engine {

using model::Sheet;

uint32_t Document::AddShape(uint32_t sheet, model::ShapeObj sh) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return 0;
  Sheet* s = M(sheet);
  if (!s) return 0;
  NoteTouched();
  sh.id = wb_.next_object_id++;
  if (sh.name.empty()) {
    static const char* kBase[] = {"Picture", "Rectangle", "Rounded Rectangle", "Oval", "Isosceles Triangle", "Straight Connector",
                                  "Straight Arrow Connector", "Right Arrow", "TextBox"};
    const int k = static_cast<int>(sh.kind);
    // a gallery shape is named as Excel's gallery calls it ("Arrow: Right 4", "Connector: Elbow 5", "Freeform: Shape 6")
    const char* base = AutoShapeName(sh.auto_type);
    if (!sh.path.empty()) base = sh.path_closed || sh.path_smooth ? "Freeform: Shape" : "Freeform: Scribble";
    else if (sh.connector == 2) base = "Connector: Elbow";
    else if (sh.connector == 3) base = "Connector: Curved";
    sh.name = std::string(base ? base : kBase[k >= 0 && k < 9 ? k : 1]) + " " + std::to_string(sh.id);
  }
  int z = 0;
  for (const auto& x : *s->controls) z = std::max(z, x.z + 1);
  for (const auto& x : *s->charts) z = std::max(z, x.z + 1);
  for (const auto& x : *s->shapes) z = std::max(z, x.z + 1);
  sh.z = z;
  // like Excel: a new picture keeps its proportions; the scale is measured from the size it was inserted at
  // (a copy keeps the settings of the one it was copied from)
  if (sh.orig_w <= 0 || sh.orig_h <= 0) {
    if (sh.kind == model::ShapeKind::Picture) sh.lock_aspect = true;
    sh.orig_w = sh.width, sh.orig_h = sh.height;
  }
  const uint32_t id = sh.id;
  s->shapes.mut().push_back(std::move(sh));
  wb_.dirty = true;
  return id;
}

bool Document::UpdateShape(uint32_t sheet, uint32_t id, const std::function<void(model::ShapeObj&)>& fn) {
  // read-only mode: no shape moves or changes, locked or not (a linked picture's image follows its cells: app_refresh)
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  for (const auto& x : *s->shapes) {
    // a locked shape on a protected sheet (without "Edit objects") can't be moved, sized or formatted (Excel)
    if (x.id == id && x.locked && Refuse(sheet, nullptr, Allow::Objects)) return false;
  }
  for (auto& x : s->shapes.mut()) {
    if (x.id != id) continue;
    NoteTouched();
    fn(x);
    x.id = id;
    wb_.dirty = true;
    return true;
  }
  return false;
}

bool Document::DeleteShape(uint32_t sheet, uint32_t id) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  auto& v = s->shapes.mut();
  const auto it = std::find_if(v.begin(), v.end(), [&](const model::ShapeObj& x) { return x.id == id; });
  if (it == v.end()) return false;
  NoteTouched();
  v.erase(it);
  wb_.dirty = true;
  return true;
}

uint32_t Document::AddSparklineGroup(uint32_t sheet, model::SparklineGroup g) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return 0;
  if (!S(sheet) || g.items.empty()) return 0;
  Rect area = Rect::Cell(g.items.front().r, g.items.front().c);
  for (const auto& it : g.items) area = area.Union(Rect::Cell(it.r, it.c));
  ClearSparklines(sheet, area);
  Sheet* s = M(sheet);
  NoteTouched();
  g.id = wb_.next_object_id++;
  const uint32_t id = g.id;
  s->sparklines.mut().push_back(std::move(g));
  changes_.push_back(CellChange{sheet, area, kSrcUser});
  wb_.dirty = true;
  return id;
}

bool Document::UpdateSparklineGroup(uint32_t sheet, uint32_t id, const std::function<void(model::SparklineGroup&)>& fn) {
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  for (auto& g : s->sparklines.mut()) {
    if (g.id != id) continue;
    NoteTouched();
    fn(g);
    g.id = id;
    for (const auto& it : g.items) changes_.push_back(CellChange{sheet, Rect::Cell(it.r, it.c), kSrcUser});
    wb_.dirty = true;
    return true;
  }
  return false;
}

uint32_t Document::GroupSparklines(uint32_t sheet, const std::vector<Rect>& areas, uint32_t r, uint32_t c) {
  if (RefuseReadOnly(sheet)) return 0;
  Sheet* s = M(sheet);
  if (!s) return 0;
  auto touches = [&](const model::SparklineGroup& g) {
    for (const auto& it : g.items) {
      for (const Rect& a : areas) {
        if (a.Contains(it.r, it.c)) return true;
      }
    }
    return false;
  };
  std::vector<size_t> hit;
  size_t lead = SIZE_MAX;
  for (size_t i = 0; i < s->sparklines->size(); ++i) {
    const auto& g = (*s->sparklines)[i];
    if (!touches(g)) continue;
    hit.push_back(i);
    for (const auto& it : g.items) {
      if (it.r == r && it.c == c) lead = i;
    }
  }
  if (hit.size() < 2) return hit.size() == 1 ? (*s->sparklines)[hit[0]].id : 0;
  if (lead == SIZE_MAX) lead = hit.front();
  NoteTouched();
  auto& v = s->sparklines.mut();
  model::SparklineGroup merged = v[lead];
  merged.items.clear();
  for (size_t i : hit) merged.items.insert(merged.items.end(), v[i].items.begin(), v[i].items.end());
  for (size_t k = hit.size(); k-- > 0;) {
    if (hit[k] != lead) v.erase(v.begin() + static_cast<std::ptrdiff_t>(hit[k]));
  }
  for (auto& g : v) {
    if (g.id == merged.id) g = merged;
  }
  for (const auto& it : merged.items) changes_.push_back(CellChange{sheet, Rect::Cell(it.r, it.c), kSrcUser});
  wb_.dirty = true;
  return merged.id;
}

void Document::UngroupSparklines(uint32_t sheet, const std::vector<Rect>& areas) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  std::vector<model::SparklineGroup> out;
  bool any = false;
  for (const auto& g : *s->sparklines) {
    bool hit = false;
    for (const auto& it : g.items) {
      for (const Rect& a : areas) hit = hit || a.Contains(it.r, it.c);
    }
    if (!hit || g.items.size() < 2) {
      out.push_back(g);
      continue;
    }
    any = true;
    for (size_t k = 0; k < g.items.size(); ++k) {
      model::SparklineGroup one = g;
      one.items = {g.items[k]};
      if (k) one.id = wb_.next_object_id++;
      out.push_back(std::move(one));
      changes_.push_back(CellChange{sheet, Rect::Cell(g.items[k].r, g.items[k].c), kSrcUser});
    }
  }
  if (!any) return;
  NoteTouched();
  s->sparklines.mut() = std::move(out);
  wb_.dirty = true;
}

void Document::ClearSparklines(uint32_t sheet, const Rect& area) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s || s->sparklines->empty()) return;
  bool any = false;
  for (const auto& g : *s->sparklines) {
    for (const auto& it : g.items) any = any || area.Contains(it.r, it.c);
  }
  if (!any) return;
  NoteTouched();
  auto& v = s->sparklines.mut();
  for (auto& g : v) {
    g.items.erase(std::remove_if(g.items.begin(), g.items.end(), [&](const model::Sparkline& it) { return area.Contains(it.r, it.c); }), g.items.end());
  }
  v.erase(std::remove_if(v.begin(), v.end(), [](const model::SparklineGroup& g) { return g.items.empty(); }), v.end());
  changes_.push_back(CellChange{sheet, area, kSrcUser});
  wb_.dirty = true;
}

bool Document::UpdatePageSetup(uint32_t sheet, const std::function<void(model::PageSetup&)>& fn) {
  Sheet* s = M(sheet);
  if (!s) return false;
  model::PageSetup ps = *s->page_setup;
  fn(ps);
  ps.zoom = ps.zoom == 0 ? 0 : std::clamp(ps.zoom, 10, 400);
  ps.fit_wide = std::max(0, ps.fit_wide);
  ps.fit_tall = std::max(0, ps.fit_tall);
  for (double* m : {&ps.left, &ps.right, &ps.top, &ps.bottom, &ps.header, &ps.footer}) *m = std::clamp(*m, 0.0, 720.0);
  std::sort(ps.row_breaks.begin(), ps.row_breaks.end());
  ps.row_breaks.erase(std::unique(ps.row_breaks.begin(), ps.row_breaks.end()), ps.row_breaks.end());
  std::sort(ps.col_breaks.begin(), ps.col_breaks.end());
  ps.col_breaks.erase(std::unique(ps.col_breaks.begin(), ps.col_breaks.end()), ps.col_breaks.end());
  if (ps == *s->page_setup) return true;
  if (RefuseReadOnly(sheet)) return false;  // read-only mode: only a change is refused (printing as set up works)
  NoteTouched();
  s->page_setup.mut() = std::move(ps);
  wb_.dirty = true;
  return true;
}

bool Document::SetSheetBackground(uint32_t sheet, std::string image) {
  Sheet* s = M(sheet);
  if (!s) return false;
  if (*s->background == image) return true;
  if (RefuseReadOnly(sheet)) return false;
  NoteTouched();
  s->background.reset(std::move(image));
  wb_.dirty = true;
  return true;
}

const model::SparklineGroup* Document::SparklineGroupAt(uint32_t sheet, uint32_t r, uint32_t c) const {
  const Sheet* s = S(sheet);
  if (!s) return nullptr;
  for (const auto& g : *s->sparklines) {
    for (const auto& it : g.items) {
      if (it.r == r && it.c == c) return &g;
    }
  }
  return nullptr;
}

// one sparkline per data row (location is a column) or per data column (location is a row); a single cell takes a 1-D range
bool BuildSparklines(uint32_t data_sheet, const Rect& data, const Rect& loc, std::vector<model::Sparkline>* out) {
  out->clear();
  if (loc.IsCell() && (data.r0 == data.r1 || data.c0 == data.c1)) {
    out->push_back({loc.r0, loc.c0, data_sheet, data});
    return true;
  }
  if (loc.c0 == loc.c1 && loc.Rows() == data.Rows()) {
    for (uint32_t i = 0; i < data.Rows(); ++i) out->push_back({loc.r0 + i, loc.c0, data_sheet, Rect{data.r0 + i, data.c0, data.r0 + i, data.c1}});
    return true;
  }
  if (loc.r0 == loc.r1 && loc.Cols() == data.Cols()) {
    for (uint32_t j = 0; j < data.Cols(); ++j) out->push_back({loc.r0, loc.c0 + j, data_sheet, Rect{data.r0, data.c0 + j, data.r1, data.c0 + j}});
    return true;
  }
  return false;
}

}  // namespace yc::engine
