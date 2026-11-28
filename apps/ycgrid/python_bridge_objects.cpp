// SPDX-License-Identifier: Apache-2.0
// python_bridge_objects.cpp - YCP: controls, charts, custom panels, containers, layout (PLAN §9, §10, §12.9)
//
// Model objects (controls, charts, panels) are written through the engine and saved; containers and layout are view state applied on the UI thread.
#include "python_bridge.h"

#include "app_state.h"

#include "commands/command_registry.h"
#include "engine/shape_geometry.h"
#include "engine/chart_layout.h"
#include "engine/page_layout.h"
#include "engine/pivot_engine.h"
#include "print.h"
#include "dock/dock_layout.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace yc::app {

using engine::Document;
using io::MV;
using model::ChartObj;
using model::ChartType;
using model::Control;
using model::ControlKind;
using model::Placement;

namespace {

std::string LowerStr(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

void SetErr(std::string* code, std::string* msg, const char* c, std::string m) {
  *code = c;
  *msg = std::move(m);
}

MV StrList(const std::vector<std::string>& v) {
  MV a = MV::Array();
  for (const auto& x : v) a.Push(MV::Str(x));
  return a;
}

std::vector<std::string> ToStrList(const MV& v) {
  std::vector<std::string> out;
  for (const auto& x : v.a) {
    if (x.t == MV::T::Str) out.push_back(x.s);
    else if (x.IsNum()) {
      char b[64];
      std::snprintf(b, sizeof(b), "%.15g", x.Num());
      out.push_back(b);
    } else if (x.t == MV::T::Bool) {
      out.push_back(x.b ? "TRUE" : "FALSE");
    } else {
      out.emplace_back();
    }
  }
  return out;
}

// number arrays: a msgpack array, or bytes of little-endian f64 (numpy tobytes())
std::vector<double> ToDoubles(const MV* v) {
  std::vector<double> out;
  if (!v) return out;
  if (v->t == MV::T::Bin) {
    out.resize(v->s.size() / sizeof(double));
    if (!out.empty()) std::memcpy(out.data(), v->s.data(), out.size() * sizeof(double));
    return out;
  }
  out.reserve(v->a.size());
  for (const auto& x : v->a) out.push_back(x.IsNum() ? x.Num() : (x.t == MV::T::Bool ? (x.b ? 1.0 : 0.0) : std::numeric_limits<double>::quiet_NaN()));
  return out;
}

int PlacementInt(Placement p) { return static_cast<int>(p); }
Placement PlacementOf(int64_t v) {
  switch (v) {
    case 2: return Placement::Move;
    case 3: return Placement::FreeFloating;
    case 100: return Placement::FloatWindow;
    default: return Placement::MoveAndSize;
  }
}

const char* const kKindNames[] = {"button", "combobox", "checkbox", "optionbutton", "togglebutton", "textbox", "label", "listbox", "spinbutton", "scrollbar", "image", "groupbox"};

const char* KindStr(ControlKind k) {
  const int i = static_cast<int>(k);
  return i >= 0 && i < model::kControlKinds ? kKindNames[i] : "button";
}

// "checkbox", "CheckBox", "Forms.CheckBox.1", "CommandButton"... -> kind
ControlKind KindOf(std::string s) {
  s = LowerStr(s);
  if (s.rfind("forms.", 0) == 0) s = s.substr(6);
  if (const size_t dot = s.find('.'); dot != std::string::npos) s.resize(dot);
  if (s == "commandbutton") return ControlKind::Button;
  for (int i = 0; i < model::kControlKinds; ++i) {
    if (s == kKindNames[i] || s == LowerStr(model::ControlTypeName(static_cast<ControlKind>(i)))) return static_cast<ControlKind>(i);
  }
  return ControlKind::Button;
}

MV ControlToMV(uint32_t sheet, const Control& c) {
  MV x = MV::Map();
  x.Set("sheet", MV::Int(sheet));
  x.Set("id", MV::Int(c.id));
  x.Set("name", MV::Str(c.name));
  x.Set("kind", MV::Str(KindStr(c.kind)));
  x.Set("prog_id", MV::Str(std::string("Forms.") + model::ControlTypeName(c.kind) + ".1"));
  x.Set("group_name", MV::Str(c.group_name));
  x.Set("min", MV::Int(c.min));
  x.Set("max", MV::Int(c.max));
  x.Set("small_change", MV::Int(c.small_change));
  x.Set("large_change", MV::Int(c.large_change));
  x.Set("orientation", MV::Int(c.orientation));
  x.Set("multi_select", MV::Int(c.multi_select));
  {
    MV sel = MV::Array();
    for (int i : c.selected) sel.Push(MV::Int(i));
    x.Set("selected", std::move(sel));
  }
  x.Set("multi_line", MV::Bool(c.multi_line));
  x.Set("password_char", MV::Str(c.password_char));
  x.Set("has_picture", MV::Bool(!c.picture.empty()));
  x.Set("picture_size_mode", MV::Int(c.picture_size_mode));
  x.Set("caption", MV::Str(c.caption));
  x.Set("left", MV::Double(c.left));
  x.Set("top", MV::Double(c.top));
  x.Set("width", MV::Double(c.width));
  x.Set("height", MV::Double(c.height));
  x.Set("placement", MV::Int(PlacementInt(c.placement)));
  x.Set("enabled", MV::Bool(c.enabled));
  x.Set("visible", MV::Bool(c.visible));
  x.Set("locked", MV::Bool(c.locked));
  x.Set("print_object", MV::Bool(c.print_object));
  x.Set("alt_text", MV::Str(c.alt_text));
  x.Set("text_halign", MV::Int(c.text_halign));
  x.Set("text_valign", MV::Int(c.text_valign));
  x.Set("text_orientation", MV::Int(c.text_orientation));
  x.Set("auto_size", MV::Bool(c.auto_size));
  x.Set("auto_margins", MV::Bool(c.auto_margins));
  x.Set("margin_l", MV::Double(c.margin_l));
  x.Set("margin_t", MV::Double(c.margin_t));
  x.Set("margin_r", MV::Double(c.margin_r));
  x.Set("margin_b", MV::Double(c.margin_b));
  x.Set("shading_3d", MV::Bool(c.shading_3d));
  x.Set("italic", MV::Bool(c.italic));
  x.Set("word_wrap", MV::Bool(c.word_wrap));
  x.Set("back_color", MV::Int(c.back_color));
  x.Set("fore_color", MV::Int(c.fore_color));
  x.Set("font_name", MV::Str(c.font_name));
  x.Set("font_size", MV::Double(c.font_size));
  x.Set("bold", MV::Bool(c.bold));
  x.Set("italic", MV::Bool(c.italic));
  x.Set("items", StrList(c.items));
  x.Set("list_fill_range", MV::Str(c.list_fill_range));
  x.Set("linked_cell", MV::Str(c.linked_cell));
  x.Set("style", MV::Int(c.style));
  x.Set("list_rows", MV::Int(c.list_rows));
  x.Set("value", MV::Str(c.value));
  x.Set("list_index", MV::Int(c.list_index));
  x.Set("z", MV::Int(c.z));
  x.Set("created_by_python", MV::Bool(c.created_by_python));
  x.Set("form", MV::Bool(c.form));
  x.Set("on_action", MV::Str(c.on_action));
  return x;
}

void ApplyControl(Control& c, const MV& p) {
  if (const MV* v = p.Get("name"); v && !v->s.empty()) c.name = v->s;
  if (const MV* v = p.Get("kind")) c.kind = KindOf(v->s);
  if (const MV* v = p.Get("group_name")) c.group_name = v->s;
  if (const MV* v = p.Get("min")) c.min = static_cast<int>(v->Num());
  if (const MV* v = p.Get("max")) c.max = static_cast<int>(v->Num());
  if (const MV* v = p.Get("small_change")) c.small_change = static_cast<int>(v->Num());
  if (const MV* v = p.Get("large_change")) c.large_change = static_cast<int>(v->Num());
  if (const MV* v = p.Get("orientation")) c.orientation = static_cast<int>(v->Num());
  if (const MV* v = p.Get("multi_select")) c.multi_select = std::clamp(static_cast<int>(v->Num()), 0, 2);
  if (const MV* v = p.Get("selected")) {
    c.selected.clear();
    for (const auto& e : v->a) c.selected.push_back(static_cast<int>(e.Num()));
  }
  if (const MV* v = p.Get("multi_line")) c.multi_line = v->b;
  if (const MV* v = p.Get("password_char")) c.password_char = v->s;
  if (const MV* v = p.Get("picture")) c.picture = v->s;
  if (const MV* v = p.Get("picture_size_mode")) c.picture_size_mode = static_cast<int>(v->Num());
  if (const MV* v = p.Get("caption")) c.caption = v->s;
  if (const MV* v = p.Get("form")) c.form = v->b;
  if (const MV* v = p.Get("on_action")) c.on_action = v->s;
  if (const MV* v = p.Get("left")) c.left = v->Num();
  if (const MV* v = p.Get("top")) c.top = v->Num();
  if (const MV* v = p.Get("width")) c.width = std::max(1.0, v->Num());
  if (const MV* v = p.Get("height")) c.height = std::max(1.0, v->Num());
  if (const MV* v = p.Get("placement")) c.placement = PlacementOf(v->i);
  if (const MV* v = p.Get("enabled")) c.enabled = v->b;
  if (const MV* v = p.Get("visible")) c.visible = v->b;
  if (const MV* v = p.Get("locked")) c.locked = v->b;
  if (const MV* v = p.Get("print_object")) c.print_object = v->b;
  if (const MV* v = p.Get("alt_text")) c.alt_text = v->s;
  if (const MV* v = p.Get("text_halign")) c.text_halign = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
  if (const MV* v = p.Get("text_valign")) c.text_valign = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
  if (const MV* v = p.Get("text_orientation")) c.text_orientation = static_cast<int>(std::clamp<int64_t>(v->i, 0, 3));
  if (const MV* v = p.Get("auto_size")) c.auto_size = v->b;
  if (const MV* v = p.Get("auto_margins")) c.auto_margins = v->b;
  if (const MV* v = p.Get("margin_l")) c.margin_l = std::max(0.0, v->Num());
  if (const MV* v = p.Get("margin_t")) c.margin_t = std::max(0.0, v->Num());
  if (const MV* v = p.Get("margin_r")) c.margin_r = std::max(0.0, v->Num());
  if (const MV* v = p.Get("margin_b")) c.margin_b = std::max(0.0, v->Num());
  if (const MV* v = p.Get("shading_3d")) c.shading_3d = v->b;
  if (const MV* v = p.Get("italic")) c.italic = v->b;
  if (const MV* v = p.Get("word_wrap")) c.word_wrap = v->b;
  if (const MV* v = p.Get("back_color")) c.back_color = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("fore_color")) c.fore_color = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("font_name")) c.font_name = v->s;
  if (const MV* v = p.Get("font_size")) c.font_size = static_cast<float>(v->Num());
  if (const MV* v = p.Get("bold")) c.bold = v->b;
  if (const MV* v = p.Get("italic")) c.italic = v->b;
  if (const MV* v = p.Get("items")) c.items = ToStrList(*v);
  if (const MV* v = p.Get("list_fill_range")) c.list_fill_range = v->s;
  if (const MV* v = p.Get("linked_cell")) c.linked_cell = v->s;
  if (const MV* v = p.Get("style")) c.style = static_cast<int>(v->i);
  if (const MV* v = p.Get("list_rows")) c.list_rows = static_cast<int>(v->i);
  if (const MV* v = p.Get("value")) c.value = v->s;
  if (const MV* v = p.Get("list_index")) {
    c.list_index = static_cast<int>(v->i);
    if (c.list_index >= 0 && static_cast<size_t>(c.list_index) < c.items.size()) c.value = c.items[static_cast<size_t>(c.list_index)];
  }
}

// ---- chart types' options: pie of pie, stock elements, 3-D floor and walls, date axis, axis titles' places,
// properties; the series' trendlines, X error bars, treemap / box / map options ----
MV ManualToMV(const ChartObj::ManualRect& m) {
  MV r = MV::Array();
  r.Push(MV::Bool(m.on));
  r.Push(MV::Double(m.x));
  r.Push(MV::Double(m.y));
  r.Push(MV::Double(m.w));
  r.Push(MV::Double(m.h));
  return r;
}

void ChartTypeOptionsToMV(MV& x, const ChartObj& c) {
  x.Set("split_type", MV::Int(c.split_type));
  x.Set("split_value", MV::Double(c.split_value));
  x.Set("second_size", MV::Int(c.second_size));
  x.Set("pie_gap", MV::Int(c.pie_gap));
  MV sp = MV::Array();
  for (int k : c.second_points) sp.Push(MV::Int(k));
  x.Set("second_points", std::move(sp));
  x.Set("up_down_bars", MV::Bool(c.up_down_bars));
  x.Set("hi_lo_lines", MV::Bool(c.hi_lo_lines));
  x.Set("up_fill", MV::Int(c.up_fill));
  x.Set("down_fill", MV::Int(c.down_fill));
  x.Set("hilo_line", MV::Int(c.hilo_line));
  x.Set("updown_gap", MV::Int(c.updown_gap));
  x.Set("floor_fill", MV::Int(c.floor_fill));
  x.Set("wall_fill", MV::Int(c.wall_fill));
  x.Set("cat_type", MV::Int(c.cat_type));
  x.Set("base_unit", MV::Int(c.base_unit));
  x.Set("major_unit_scale", MV::Int(c.major_unit_scale));
  x.Set("cat_title_rect", ManualToMV(c.cat_title_rect));
  x.Set("val_title_rect", ManualToMV(c.val_title_rect));
  x.Set("val2_title_rect", ManualToMV(c.val2_title_rect));
  x.Set("print_object", MV::Bool(c.print_object));
  x.Set("locked", MV::Bool(c.locked));
  // the chart type's Chart Styles set: Chart.ChartStyle is style_base + style - 1 (201-214 for columns...)
  x.Set("style_base", MV::Int(engine::ChartStyleNumber(c.type)));
  x.Set("style_count", MV::Int(engine::ChartStyleCount(c.type)));
}

MV TrendlineToMV(const model::ChartTrendline& t) {
  MV m = MV::Map();
  m.Set("trend", MV::Int(static_cast<int>(t.kind)));
  m.Set("trend_order", MV::Int(t.order));
  m.Set("trend_period", MV::Int(t.period));
  m.Set("trend_forward", MV::Double(t.forward));
  m.Set("trend_backward", MV::Double(t.backward));
  m.Set("trend_eq", MV::Bool(t.eq));
  m.Set("trend_r2", MV::Bool(t.r2));
  m.Set("trend_name", MV::Str(t.name));
  m.Set("trend_icept_on", MV::Bool(t.intercept_on));
  m.Set("trend_icept", MV::Double(t.intercept));
  return m;
}

void SeriesTypeOptionsToMV(MV& m, const model::ChartSeries& se) {
  m.Set("trend_name", MV::Str(se.trend_name));
  m.Set("trend_icept_on", MV::Bool(se.trend_icept_on));
  m.Set("trend_icept", MV::Double(se.trend_icept));
  MV mt = MV::Array();
  for (const auto& t : se.more_trends) mt.Push(TrendlineToMV(t));
  m.Set("more_trends", std::move(mt));
  m.Set("err_cap", MV::Bool(se.err_cap));
  m.Set("xerr", MV::Int(static_cast<int>(se.xerr)));
  m.Set("xerr_amount", MV::Double(se.xerr_amount));
  m.Set("xerr_plus", MV::Str(se.xerr_plus));
  m.Set("xerr_minus", MV::Str(se.xerr_minus));
  m.Set("xerr_include", MV::Int(se.xerr_include));
  m.Set("parent_labels", MV::Int(se.parent_labels));
  m.Set("inclusive_median", MV::Bool(se.inclusive_median));
  m.Set("geo_projection", MV::Int(se.geo_projection));
  m.Set("geo_area", MV::Int(se.geo_area));
  m.Set("geo_labels", MV::Int(se.geo_labels));
  m.Set("geo_colors", MV::Int(se.geo_colors));
  m.Set("geo_min", MV::Int(se.geo_min));
  m.Set("geo_mid", MV::Int(se.geo_mid));
  m.Set("geo_max", MV::Int(se.geo_max));
  m.Set("label_dx", MV::Double(se.label_dx));
  m.Set("label_dy", MV::Double(se.label_dy));
}

void ApplyManual(const MV& p, const char* k, ChartObj::ManualRect& m) {
  const MV* v = p.Get(k);
  if (!v) return;
  if (v->a.size() >= 5) {
    m.on = v->a[0].b;
    m.x = static_cast<float>(std::clamp(v->a[1].Num(), 0.0, 1.0));
    m.y = static_cast<float>(std::clamp(v->a[2].Num(), 0.0, 1.0));
    m.w = static_cast<float>(std::clamp(v->a[3].Num(), 0.0, 1.0));
    m.h = static_cast<float>(std::clamp(v->a[4].Num(), 0.0, 1.0));
  } else {
    m = {};
  }
}

void ApplyChartTypeOptions(ChartObj& c, const MV& p) {
  if (const MV* v = p.Get("split_type")) c.split_type = static_cast<int>(std::clamp<int64_t>(v->i, 1, 4));
  if (const MV* v = p.Get("split_value")) c.split_value = std::max(0.0, v->Num());
  if (const MV* v = p.Get("second_size")) c.second_size = static_cast<int>(std::clamp(v->Num(), 5.0, 200.0));
  if (const MV* v = p.Get("pie_gap")) c.pie_gap = static_cast<int>(std::clamp(v->Num(), 0.0, 500.0));
  if (const MV* v = p.Get("second_points")) {
    c.second_points.clear();
    for (const MV& k : v->a) {
      if (k.i >= 0 && k.i < 1000000) c.second_points.push_back(static_cast<int>(k.i));
    }
    std::sort(c.second_points.begin(), c.second_points.end());
    c.second_points.erase(std::unique(c.second_points.begin(), c.second_points.end()), c.second_points.end());
  }
  if (const MV* v = p.Get("up_down_bars")) c.up_down_bars = v->b;
  if (const MV* v = p.Get("hi_lo_lines")) c.hi_lo_lines = v->b;
  if (const MV* v = p.Get("up_fill")) c.up_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("down_fill")) c.down_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("hilo_line")) c.hilo_line = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("updown_gap")) c.updown_gap = static_cast<int>(std::clamp(v->Num(), 0.0, 500.0));
  if (const MV* v = p.Get("floor_fill")) c.floor_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("wall_fill")) c.wall_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("cat_type")) c.cat_type = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
  if (const MV* v = p.Get("base_unit")) c.base_unit = static_cast<int>(std::clamp<int64_t>(v->i, -1, 2));
  if (const MV* v = p.Get("major_unit_scale")) c.major_unit_scale = static_cast<int>(std::clamp<int64_t>(v->i, -1, 2));
  ApplyManual(p, "cat_title_rect", c.cat_title_rect);
  ApplyManual(p, "val_title_rect", c.val_title_rect);
  ApplyManual(p, "val2_title_rect", c.val2_title_rect);
  if (const MV* v = p.Get("print_object")) c.print_object = v->b;
  if (const MV* v = p.Get("locked")) c.locked = v->b;
}

void ApplyTrendline(model::ChartTrendline& t, const MV& p) {
  if (const MV* v = p.Get("trend")) t.kind = static_cast<model::TrendKind>(std::clamp<int64_t>(v->i, 1, 6));
  if (const MV* v = p.Get("trend_order")) t.order = static_cast<int>(std::clamp<int64_t>(v->i, 2, 6));
  if (const MV* v = p.Get("trend_period")) t.period = static_cast<int>(std::clamp<int64_t>(v->i, 2, 255));
  if (const MV* v = p.Get("trend_forward")) t.forward = std::max(0.0, v->Num());
  if (const MV* v = p.Get("trend_backward")) t.backward = std::max(0.0, v->Num());
  if (const MV* v = p.Get("trend_eq")) t.eq = v->b;
  if (const MV* v = p.Get("trend_r2")) t.r2 = v->b;
  if (const MV* v = p.Get("trend_name")) t.name = v->s;
  if (const MV* v = p.Get("trend_icept_on")) t.intercept_on = v->b;
  if (const MV* v = p.Get("trend_icept")) t.intercept = v->Num();
}

void ApplySeriesTypeOptions(model::ChartSeries& se, const MV& p) {
  if (const MV* v = p.Get("trend_name")) se.trend_name = v->s;
  if (const MV* v = p.Get("trend_icept_on")) se.trend_icept_on = v->b;
  if (const MV* v = p.Get("trend_icept")) se.trend_icept = v->Num();
  if (const MV* v = p.Get("more_trends")) {
    se.more_trends.clear();
    for (const MV& t : v->a) {
      model::ChartTrendline tl;
      ApplyTrendline(tl, t);
      se.more_trends.push_back(std::move(tl));
    }
  }
  if (const MV* v = p.Get("err_cap")) se.err_cap = v->b;
  if (const MV* v = p.Get("xerr")) se.xerr = static_cast<model::ErrorBarKind>(std::clamp<int64_t>(v->i, 0, 5));
  if (const MV* v = p.Get("xerr_amount")) se.xerr_amount = v->Num();
  if (const MV* v = p.Get("xerr_plus")) se.xerr_plus = v->s;
  if (const MV* v = p.Get("xerr_minus")) se.xerr_minus = v->s;
  if (const MV* v = p.Get("xerr_include")) se.xerr_include = static_cast<int>(std::clamp<int64_t>(v->i, 1, 3));
  if (const MV* v = p.Get("parent_labels")) se.parent_labels = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
  if (const MV* v = p.Get("inclusive_median")) se.inclusive_median = v->b;
  if (const MV* v = p.Get("geo_projection")) se.geo_projection = static_cast<int>(std::clamp<int64_t>(v->i, 0, 4));
  if (const MV* v = p.Get("geo_area")) se.geo_area = static_cast<int>(std::clamp<int64_t>(v->i, 0, 7));
  if (const MV* v = p.Get("geo_labels")) se.geo_labels = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
  if (const MV* v = p.Get("geo_colors")) se.geo_colors = static_cast<int>(std::clamp<int64_t>(v->i, 0, 1));
  if (const MV* v = p.Get("geo_min")) se.geo_min = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("geo_mid")) se.geo_mid = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("geo_max")) se.geo_max = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("label_dx")) se.label_dx = static_cast<float>(std::clamp(v->Num(), -1.0, 1.0));
  if (const MV* v = p.Get("label_dy")) se.label_dy = static_cast<float>(std::clamp(v->Num(), -1.0, 1.0));
}

// ---- chart text and Fill & Line (Format pane: Text Options, fonts of each element, axis lines, series fills) ----
void FontToMV(MV& x, const std::string& k, const model::ChartFont& f) {
  x.Set((k + "_size").c_str(), MV::Double(f.size));
  x.Set((k + "_color").c_str(), MV::Int(f.color));
  x.Set((k + "_bold").c_str(), MV::Bool(f.bold));
  x.Set((k + "_name").c_str(), MV::Str(f.name));
  x.Set((k + "_italic").c_str(), MV::Bool(f.italic));
  x.Set((k + "_underline").c_str(), MV::Bool(f.underline));
}

// true when one of the font's keys was given
bool ApplyFont(const MV& p, const std::string& k, model::ChartFont& f) {
  bool any = false;
  if (const MV* v = p.Get((k + "_size").c_str())) f.size = static_cast<float>(std::clamp(v->Num(), 0.0, 400.0)), any = true;
  if (const MV* v = p.Get((k + "_color").c_str())) f.color = static_cast<uint32_t>(v->i), any = true;
  if (const MV* v = p.Get((k + "_bold").c_str())) f.bold = v->b, any = true;
  if (const MV* v = p.Get((k + "_name").c_str())) f.name = v->s, any = true;
  if (const MV* v = p.Get((k + "_italic").c_str())) f.italic = v->b, any = true;
  if (const MV* v = p.Get((k + "_underline").c_str())) f.underline = v->b, any = true;
  return any;
}

void ChartFormatToMV(MV& x, const ChartObj& c) {
  FontToMV(x, "title", c.title_font);
  FontToMV(x, "legend", c.legend_font);
  FontToMV(x, "axis", c.axis_font);
  auto axis = [&](const std::string& k, const model::ChartAxisFormat& a) {
    FontToMV(x, k + "_font", a.own_font ? a.font : c.axis_font);  // the font its labels are drawn in
    FontToMV(x, k + "_tfont", a.title_font == model::ChartFont{} ? c.axis_font : a.title_font);
    x.Set((k + "_line_color").c_str(), MV::Int(a.line_color));
    x.Set((k + "_no_line").c_str(), MV::Bool(a.no_line));
    x.Set((k + "_line_weight").c_str(), MV::Double(a.line_weight));
    x.Set((k + "_label_offset").c_str(), MV::Int(a.label_offset));
    x.Set((k + "_multi_level").c_str(), MV::Bool(a.multi_level));
  };
  axis("cat", c.cat_axis);
  axis("val", c.val_axis);
  axis("val2", c.val_axis2);
}

void ApplyChartFormat(ChartObj& c, const MV& p) {
  ApplyFont(p, "title", c.title_font);
  ApplyFont(p, "legend", c.legend_font);
  ApplyFont(p, "axis", c.axis_font);
  auto axis = [&](const std::string& k, model::ChartAxisFormat& a) {
    model::ChartFont f = a.own_font ? a.font : c.axis_font;
    if (ApplyFont(p, k + "_font", f)) a.font = f, a.own_font = true;
    model::ChartFont tf = a.title_font == model::ChartFont{} ? c.axis_font : a.title_font;
    if (ApplyFont(p, k + "_tfont", tf)) a.title_font = tf;
    if (const MV* v = p.Get((k + "_line_color").c_str())) a.line_color = static_cast<uint32_t>(v->i);
    if (const MV* v = p.Get((k + "_no_line").c_str())) a.no_line = v->b;
    if (const MV* v = p.Get((k + "_line_weight").c_str())) a.line_weight = static_cast<float>(std::clamp(v->Num(), 0.0, 100.0));
    if (const MV* v = p.Get((k + "_label_offset").c_str())) a.label_offset = static_cast<int>(std::clamp<int64_t>(v->i, 0, 1000));
    if (const MV* v = p.Get((k + "_multi_level").c_str())) a.multi_level = v->b;
  };
  axis("cat", c.cat_axis);
  axis("val", c.val_axis);
  axis("val2", c.val_axis2);
}

void SeriesFormatToMV(MV& m, const model::ChartSeries& se) {
  m.Set("fill_type", MV::Int(se.fill_type));
  m.Set("grad_color2", MV::Int(se.grad_color2));
  m.Set("grad_angle", MV::Int(se.grad_angle));
  m.Set("pattern", MV::Int(se.pattern));
  m.Set("pattern_back", MV::Int(se.pattern_back));
  m.Set("picture_tile", MV::Bool(se.picture_tile));
  m.Set("has_picture", MV::Bool(!se.picture.empty()));
  m.Set("compound", MV::Int(se.compound));
  m.Set("cap", MV::Int(se.cap));
  m.Set("join", MV::Int(se.join));
  m.Set("begin_arrow", MV::Int(se.begin_arrow));
  m.Set("end_arrow", MV::Int(se.end_arrow));
  m.Set("leader_lines", MV::Bool(se.leader_lines));
  m.Set("vary_points", MV::Bool(se.vary_points));
  m.Set("legend_hidden", MV::Bool(se.legend_hidden));
  m.Set("tile_ox", MV::Double(se.tile_ox));
  m.Set("tile_oy", MV::Double(se.tile_oy));
  m.Set("tile_sx", MV::Double(se.tile_sx));
  m.Set("tile_sy", MV::Double(se.tile_sy));
  m.Set("tile_align", MV::Int(se.tile_align));
  m.Set("tile_mirror", MV::Int(se.tile_mirror));
  m.Set("legend_entry_font_set", MV::Bool(se.legend_entry_font_set));
  m.Set("label_fill", MV::Int(se.label_fill));
  m.Set("label_line", MV::Int(se.label_line));
  FontToMV(m, "legend_entry_font", se.legend_entry_font);
  FontToMV(m, "label_font", se.label_font);
}

void ApplySeriesFormat(model::ChartSeries& se, const MV& p) {
  if (const MV* v = p.Get("fill_type")) se.fill_type = static_cast<int>(std::clamp<int64_t>(v->i, 0, 3));
  if (const MV* v = p.Get("grad_color2")) se.grad_color2 = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("grad_angle")) se.grad_angle = static_cast<int>(((v->i % 360) + 360) % 360);
  if (const MV* v = p.Get("pattern")) se.pattern = static_cast<int>(std::clamp<int64_t>(v->i, 0, model::kChartPatternCount - 1));
  if (const MV* v = p.Get("pattern_back")) se.pattern_back = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("picture_tile")) se.picture_tile = v->b;
  if (const MV* v = p.Get("picture_file")) {  // Fill.UserPicture / UserTextured: the picture file's bytes
    std::ifstream f(FromUtf8(v->s), std::ios::binary);
    if (f) se.picture.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  if (const MV* v = p.Get("compound")) se.compound = static_cast<int>(std::clamp<int64_t>(v->i, 0, 4));
  if (const MV* v = p.Get("cap")) se.cap = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
  if (const MV* v = p.Get("join")) se.join = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
  if (const MV* v = p.Get("begin_arrow")) se.begin_arrow = static_cast<int>(std::clamp<int64_t>(v->i, 0, 5));
  if (const MV* v = p.Get("end_arrow")) se.end_arrow = static_cast<int>(std::clamp<int64_t>(v->i, 0, 5));
  if (const MV* v = p.Get("leader_lines")) se.leader_lines = v->b;
  if (const MV* v = p.Get("vary_points")) se.vary_points = v->b;
  if (const MV* v = p.Get("legend_hidden")) se.legend_hidden = v->b;
  if (const MV* v = p.Get("tile_ox")) se.tile_ox = static_cast<float>(std::clamp(v->Num(), -2000.0, 2000.0));
  if (const MV* v = p.Get("tile_oy")) se.tile_oy = static_cast<float>(std::clamp(v->Num(), -2000.0, 2000.0));
  if (const MV* v = p.Get("tile_sx")) se.tile_sx = static_cast<float>(std::clamp(v->Num(), 1.0, 1000.0));
  if (const MV* v = p.Get("tile_sy")) se.tile_sy = static_cast<float>(std::clamp(v->Num(), 1.0, 1000.0));
  if (const MV* v = p.Get("tile_align")) se.tile_align = static_cast<int>(std::clamp<int64_t>(v->i, 0, 8));
  if (const MV* v = p.Get("tile_mirror")) se.tile_mirror = static_cast<int>(std::clamp<int64_t>(v->i, 0, 3));
  if (ApplyFont(p, "legend_entry_font", se.legend_entry_font)) se.legend_entry_font_set = true;
  if (const MV* v = p.Get("legend_entry_font_set")) se.legend_entry_font_set = v->b;
  if (const MV* v = p.Get("label_fill")) se.label_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("label_line")) se.label_line = static_cast<uint32_t>(v->i);
  ApplyFont(p, "label_font", se.label_font);
}

MV ChartToMV(uint32_t sheet, const ChartObj& c) {
  MV x = MV::Map();
  x.Set("sheet", MV::Int(sheet));
  x.Set("id", MV::Int(c.id));
  x.Set("name", MV::Str(c.name));
  x.Set("type", MV::Int(static_cast<int>(c.type)));
  x.Set("left", MV::Double(c.left));
  x.Set("top", MV::Double(c.top));
  x.Set("width", MV::Double(c.width));
  x.Set("height", MV::Double(c.height));
  x.Set("placement", MV::Int(PlacementInt(c.placement)));
  x.Set("title", MV::Str(c.title));
  x.Set("has_title", MV::Bool(c.has_title));
  x.Set("has_legend", MV::Bool(c.has_legend));
  x.Set("interactive", MV::Bool(c.interactive));
  x.Set("floating", MV::Bool(c.floating));
  x.Set("pivot", MV::Str(c.pivot));
  // the bounds as older clients read them (both ends automatic or not); each axis has its own below
  x.Set("y_auto", MV::Bool(c.val_axis.min_auto && c.val_axis.max_auto));
  x.Set("x_auto", MV::Bool(c.cat_axis.min_auto && c.cat_axis.max_auto));
  x.Set("y_min", MV::Double(c.val_axis.min));
  x.Set("y_max", MV::Double(c.val_axis.max));
  x.Set("x_min", MV::Double(c.cat_axis.min));
  x.Set("x_max", MV::Double(c.cat_axis.max));
  x.Set("plot_visible_only", MV::Bool(c.plot_visible_only));
  x.Set("display_blanks", MV::Int(c.display_blanks));
  x.Set("na_as_blank", MV::Bool(c.na_as_blank));
  x.Set("title_overlay", MV::Bool(c.title_overlay));
  x.Set("legend_overlay", MV::Bool(c.legend_overlay));
  x.Set("color_scheme", MV::Int(c.color_scheme));
  x.Set("style", MV::Int(c.style));
  x.Set("bins_overflow", MV::Bool(c.bins_overflow));
  x.Set("overflow_at", MV::Double(c.overflow_at));
  x.Set("bins_underflow", MV::Bool(c.bins_underflow));
  x.Set("underflow_at", MV::Double(c.underflow_at));
  x.Set("bins_by_category", MV::Bool(c.bins_by_category));
  x.Set("connectors", MV::Bool(c.connectors));
  x.Set("lock_aspect", MV::Bool(c.lock_aspect));
  x.Set("plot_line", MV::Int(c.plot_line));
  x.Set("legend_fill", MV::Int(c.legend_fill));
  x.Set("legend_line", MV::Int(c.legend_line));
  x.Set("title_fill", MV::Int(c.title_fill));
  x.Set("title_line", MV::Int(c.title_line));
  auto manual = [&](const char* k, const ChartObj::ManualRect& m) {
    MV r = MV::Array();
    r.Push(MV::Bool(m.on));
    r.Push(MV::Double(m.x));
    r.Push(MV::Double(m.y));
    r.Push(MV::Double(m.w));
    r.Push(MV::Double(m.h));
    x.Set(k, std::move(r));
  };
  manual("title_rect", c.title_rect);
  manual("legend_rect", c.legend_rect);
  manual("plot_rect", c.plot_rect);
  x.Set("z", MV::Int(c.z));
  x.Set("bin_width", MV::Double(c.bin_width));
  x.Set("gap_width", MV::Int(c.gap_width));
  x.Set("overlap", MV::Int(c.overlap));
  x.Set("first_slice_angle", MV::Int(c.first_slice_angle));
  x.Set("hole_size", MV::Int(c.hole_size));
  x.Set("bin_count", MV::Int(c.bin_count));
  x.Set("source", MV::Str(c.source));  // the data range (Chart.PlotBy reads it again by rows or columns)
  x.Set("plot_by_rows", MV::Bool(c.source_by_rows));
  // element formatting (0 = automatic)
  auto font = [&](const char* k, const model::ChartFont& f) {
    x.Set((std::string(k) + "_size").c_str(), MV::Double(f.size));
    x.Set((std::string(k) + "_color").c_str(), MV::Int(f.color));
    x.Set((std::string(k) + "_bold").c_str(), MV::Bool(f.bold));
  };
  font("title", c.title_font);
  font("legend", c.legend_font);
  font("axis", c.axis_font);
  x.Set("legend_pos", MV::Int(c.legend_pos));
  x.Set("chart_fill", MV::Int(c.chart_fill));
  x.Set("chart_line", MV::Int(c.chart_line));
  x.Set("chart_line_none", MV::Bool(c.chart_line_none));
  x.Set("plot_fill", MV::Int(c.plot_fill));
  auto axis = [&](const char* k, const model::ChartAxisFormat& a) {
    const std::string p = k;
    x.Set((p + "_visible").c_str(), MV::Bool(a.visible));
    x.Set((p + "_has_title").c_str(), MV::Bool(a.has_title));
    x.Set((p + "_title").c_str(), MV::Str(a.title));
    x.Set((p + "_format").c_str(), MV::Str(a.number_format));
    x.Set((p + "_grid").c_str(), MV::Bool(a.gridlines));
    x.Set((p + "_min_auto").c_str(), MV::Bool(a.min_auto));
    x.Set((p + "_max_auto").c_str(), MV::Bool(a.max_auto));
    x.Set((p + "_min").c_str(), MV::Double(a.min));
    x.Set((p + "_max").c_str(), MV::Double(a.max));
    x.Set((p + "_major").c_str(), MV::Double(a.major));
    x.Set((p + "_minor").c_str(), MV::Double(a.minor));
    x.Set((p + "_log").c_str(), MV::Bool(a.log));
    x.Set((p + "_log_base").c_str(), MV::Double(a.log_base));
    x.Set((p + "_reverse").c_str(), MV::Bool(a.reverse));
    x.Set((p + "_crosses").c_str(), MV::Int(a.crosses));
    x.Set((p + "_crosses_at").c_str(), MV::Double(a.crosses_at));
    x.Set((p + "_display_unit").c_str(), MV::Double(a.display_unit));
    x.Set((p + "_major_tick").c_str(), MV::Int(a.major_tick));
    x.Set((p + "_minor_tick").c_str(), MV::Int(a.minor_tick));
    x.Set((p + "_label_pos").c_str(), MV::Int(a.label_pos));
    x.Set((p + "_minor_grid").c_str(), MV::Bool(a.minor_gridlines));
    x.Set((p + "_between").c_str(), MV::Bool(a.between));
    x.Set((p + "_label_interval").c_str(), MV::Int(a.label_interval));
    x.Set((p + "_mark_interval").c_str(), MV::Int(a.mark_interval));
    x.Set((p + "_unit_label").c_str(), MV::Bool(a.unit_label));
  };
  axis("cat", c.cat_axis);
  axis("val", c.val_axis);
  axis("val2", c.val_axis2);
  x.Set("view3d", MV::Bool(c.view3d));
  const bool pie3d = !c.view3d && c.type == ChartType::Pie3D;  // a 3-D pie's own defaults until a view is set
  x.Set("rotation", MV::Double(pie3d ? 0 : c.rotation));
  x.Set("elevation", MV::Double(pie3d ? 30 : c.elevation));
  x.Set("perspective", MV::Double(pie3d ? 0 : c.perspective));
  x.Set("depth_percent", MV::Double(c.depth_percent));
  MV ss = MV::Array();
  for (const auto& se : c.series) {
    MV m = MV::Map();
    m.Set("name", MV::Str(se.name));
    m.Set("values", MV::Str(se.values));
    m.Set("x_values", MV::Str(se.x_values));
    m.Set("axis_group", MV::Int(se.axis_group));
    m.Set("color", MV::Int(se.color));
    m.Set("python_data", MV::Bool(se.python_data));
    m.Set("count", MV::Int(se.data ? static_cast<int64_t>(se.data->y.size()) : 0));
    m.Set("capacity", MV::Int(se.capacity));
    m.Set("type", MV::Int(se.type));
    m.Set("data_labels", MV::Bool(se.data_labels));
    m.Set("trend", MV::Int(static_cast<int>(se.trend)));
    m.Set("trend_order", MV::Int(se.trend_order));
    m.Set("trend_period", MV::Int(se.trend_period));
    m.Set("trend_forward", MV::Double(se.trend_forward));
    m.Set("trend_backward", MV::Double(se.trend_backward));
    m.Set("trend_eq", MV::Bool(se.trend_eq));
    m.Set("trend_r2", MV::Bool(se.trend_r2));
    m.Set("err", MV::Int(static_cast<int>(se.err)));
    m.Set("err_amount", MV::Double(se.err_amount));
    m.Set("err_plus", MV::Str(se.err_plus));
    m.Set("err_minus", MV::Str(se.err_minus));
    m.Set("err_include", MV::Int(se.err_include));
    MV tot = MV::Array();
    for (int t : se.totals) tot.Push(MV::Int(t));
    m.Set("totals", std::move(tot));
    m.Set("line_weight", MV::Double(se.line_weight));
    m.Set("smooth", MV::Bool(se.smooth));
    m.Set("box_flags", MV::Int(se.box_flags));
    m.Set("marker", MV::Int(se.marker));
    m.Set("marker_size", MV::Double(se.marker_size));
    m.Set("filtered", MV::Bool(se.filtered));
    MV pts = MV::Array();
    for (const auto& pf : se.points) {
      MV q = MV::Map();
      q.Set("index", MV::Int(pf.index));
      q.Set("fill", MV::Int(pf.fill));
      q.Set("line", MV::Int(pf.line));
      q.Set("explosion", MV::Int(pf.explosion));
      q.Set("label", MV::Int(pf.label));
      q.Set("label_dx", MV::Double(pf.label_dx));
      q.Set("label_dy", MV::Double(pf.label_dy));
      q.Set("label_text", MV::Str(pf.label_text));
      q.Set("label_show", MV::Int(pf.label_show));
      q.Set("label_pos", MV::Int(pf.label_pos));
      q.Set("label_font_set", MV::Bool(pf.label_font_set));
      FontToMV(q, "label_font", pf.label_font);
      q.Set("legend_font_set", MV::Bool(pf.legend_font_set));
      q.Set("label_fill", MV::Int(pf.label_fill));
      q.Set("label_line", MV::Int(pf.label_line));
      q.Set("legend_hidden", MV::Bool(pf.legend_hidden));
      FontToMV(q, "legend_font", pf.legend_font);
      pts.Push(std::move(q));
    }
    m.Set("points", std::move(pts));
    m.Set("explosion", MV::Int(se.explosion));
    m.Set("vary_colors", MV::Bool(se.vary_colors));
    m.Set("label_pos", MV::Int(se.label_pos));
    m.Set("label_show", MV::Int(se.label_show));
    m.Set("label_range", MV::Str(se.label_range));
    m.Set("label_format", MV::Str(se.label_format));
    m.Set("label_sep", MV::Str(se.label_sep));
    m.Set("bubble_sizes", MV::Str(se.bubble_sizes));
    m.Set("line_color", MV::Int(se.line_color));
    m.Set("no_fill", MV::Bool(se.no_fill));
    m.Set("no_line", MV::Bool(se.no_line));
    m.Set("dash", MV::Int(se.dash));
    m.Set("transparency", MV::Int(se.transparency));
    m.Set("invert_negative", MV::Bool(se.invert_negative));
    m.Set("invert_color", MV::Int(se.invert_color));
    m.Set("marker_fill", MV::Int(se.marker_fill));
    m.Set("marker_line", MV::Int(se.marker_line));
    m.Set("formula", MV::Str(se.python_data ? std::string() : SeriesFormula(c, static_cast<size_t>(&se - c.series.data()))));
    SeriesTypeOptionsToMV(m, se);
    SeriesFormatToMV(m, se);
    ss.Push(std::move(m));
  }
  x.Set("series", std::move(ss));
  ChartTypeOptionsToMV(x, c);
  ChartFormatToMV(x, c);
  MV hc = MV::Array();
  for (int k : c.hidden_cats) hc.Push(MV::Int(k));
  x.Set("hidden_cats", std::move(hc));
  return x;
}

void ApplyChart(ChartObj& c, const MV& p) {
  if (const MV* v = p.Get("name"); v && !v->s.empty()) c.name = v->s;
  if (const MV* v = p.Get("type")) {
    const ChartType t = static_cast<ChartType>(std::clamp<int64_t>(v->i, 0, model::kChartTypeCount - 1));
    // another kind of chart has its own Chart Styles set: Style 1 of it (Excel's Chart.ChartStyle follows the type)
    if (engine::ChartStyleNumber(t) != engine::ChartStyleNumber(c.type) || engine::ChartStyleCount(t) != engine::ChartStyleCount(c.type)) c.style = 0;
    c.type = t;
  }
  if (const MV* v = p.Get("bin_width")) c.bin_width = std::max(0.0, v->Num());
  if (const MV* v = p.Get("gap_width")) c.gap_width = static_cast<int>(std::clamp(v->Num(), 0.0, 500.0));
  if (const MV* v = p.Get("overlap")) c.overlap = static_cast<int>(std::clamp(v->Num(), -100.0, 100.0));
  if (const MV* v = p.Get("first_slice_angle")) c.first_slice_angle = static_cast<int>(std::clamp(v->Num(), 0.0, 360.0));
  if (const MV* v = p.Get("hole_size")) c.hole_size = static_cast<int>(std::clamp(v->Num(), 10.0, 90.0));
  if (const MV* v = p.Get("bin_count")) c.bin_count = static_cast<int>(std::clamp<int64_t>(v->i, 0, 1000));
  if (const MV* v = p.Get("left")) c.left = v->Num();
  if (const MV* v = p.Get("top")) c.top = v->Num();
  if (const MV* v = p.Get("width")) c.width = std::max(10.0, v->Num());
  if (const MV* v = p.Get("height")) c.height = std::max(10.0, v->Num());
  if (const MV* v = p.Get("placement")) c.placement = PlacementOf(v->i);
  if (const MV* v = p.Get("title")) {
    c.title = v->s;
    if (!p.Has("has_title")) c.has_title = !v->s.empty();
  }
  if (const MV* v = p.Get("has_title")) c.has_title = v->b;
  if (const MV* v = p.Get("has_legend")) c.has_legend = v->b;
  if (const MV* v = p.Get("interactive")) c.interactive = v->b;
  if (const MV* v = p.Get("floating")) c.floating = v->b;
  // the older bound names: both ends at once
  if (const MV* v = p.Get("y_auto")) c.val_axis.min_auto = c.val_axis.max_auto = v->b;
  if (const MV* v = p.Get("x_auto")) c.cat_axis.min_auto = c.cat_axis.max_auto = v->b;
  if (const MV* v = p.Get("y_min")) c.val_axis.min = v->Num(), c.val_axis.min_auto = p.Has("y_auto") ? c.val_axis.min_auto : false;
  if (const MV* v = p.Get("y_max")) c.val_axis.max = v->Num(), c.val_axis.max_auto = p.Has("y_auto") ? c.val_axis.max_auto : false;
  if (const MV* v = p.Get("x_min")) c.cat_axis.min = v->Num(), c.cat_axis.min_auto = p.Has("x_auto") ? c.cat_axis.min_auto : false;
  if (const MV* v = p.Get("x_max")) c.cat_axis.max = v->Num(), c.cat_axis.max_auto = p.Has("x_auto") ? c.cat_axis.max_auto : false;
  if (const MV* v = p.Get("plot_visible_only")) c.plot_visible_only = v->b;
  if (const MV* v = p.Get("display_blanks")) c.display_blanks = static_cast<int>(std::clamp<int64_t>(v->i, 1, 3));
  if (const MV* v = p.Get("na_as_blank")) c.na_as_blank = v->b;
  if (const MV* v = p.Get("title_overlay")) c.title_overlay = v->b;
  if (const MV* v = p.Get("legend_overlay")) c.legend_overlay = v->b;
  if (const MV* v = p.Get("color_scheme")) c.color_scheme = static_cast<int>(std::clamp<int64_t>(v->i, 0, engine::kChartColorSchemes - 1));
  if (const MV* v = p.Get("style")) c.style = static_cast<int>(std::clamp<int64_t>(v->i, 0, 48));
  // Chart Design's galleries: Quick Layout n, Chart Style n (Chart.ApplyLayout, Chart.ChartStyle)
  if (const MV* v = p.Get("apply_layout")) engine::ApplyChartQuickLayout(c, static_cast<int>(std::clamp<int64_t>(v->i, 1, engine::kChartQuickLayouts)));
  if (const MV* v = p.Get("apply_style")) engine::ApplyChartStyle(c, static_cast<int>(std::clamp<int64_t>(v->i, 1, engine::ChartStyleCount(c.type))));
  if (const MV* v = p.Get("bins_overflow")) c.bins_overflow = v->b;
  if (const MV* v = p.Get("overflow_at")) c.overflow_at = v->Num();
  if (const MV* v = p.Get("bins_underflow")) c.bins_underflow = v->b;
  if (const MV* v = p.Get("underflow_at")) c.underflow_at = v->Num();
  if (const MV* v = p.Get("bins_by_category")) c.bins_by_category = v->b;
  if (const MV* v = p.Get("connectors")) c.connectors = v->b;
  if (const MV* v = p.Get("lock_aspect")) c.lock_aspect = v->b;
  if (const MV* v = p.Get("plot_line")) c.plot_line = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("legend_fill")) c.legend_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("legend_line")) c.legend_line = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("title_fill")) c.title_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("title_line")) c.title_line = static_cast<uint32_t>(v->i);
  auto manual = [&](const char* k, ChartObj::ManualRect& m) {
    const MV* v = p.Get(k);
    if (!v) return;
    if (v->a.size() >= 5) {
      m.on = v->a[0].b;
      m.x = static_cast<float>(std::clamp(v->a[1].Num(), 0.0, 1.0));
      m.y = static_cast<float>(std::clamp(v->a[2].Num(), 0.0, 1.0));
      m.w = static_cast<float>(std::clamp(v->a[3].Num(), 0.0, 1.0));
      m.h = static_cast<float>(std::clamp(v->a[4].Num(), 0.0, 1.0));
    } else {
      m = {};
    }
  };
  manual("title_rect", c.title_rect);
  manual("legend_rect", c.legend_rect);
  manual("plot_rect", c.plot_rect);
  auto font = [&](const char* k, model::ChartFont& f) {
    const std::string b = k;
    if (const MV* v = p.Get((b + "_size").c_str())) f.size = static_cast<float>(std::clamp(v->Num(), 0.0, 400.0));
    if (const MV* v = p.Get((b + "_color").c_str())) f.color = static_cast<uint32_t>(v->i);
    if (const MV* v = p.Get((b + "_bold").c_str())) f.bold = v->b;
  };
  font("title", c.title_font);
  font("legend", c.legend_font);
  font("axis", c.axis_font);
  if (const MV* v = p.Get("legend_pos")) c.legend_pos = static_cast<int>(std::clamp<int64_t>(v->i, 0, 4));
  if (const MV* v = p.Get("chart_fill")) c.chart_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("chart_line")) c.chart_line = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("chart_line_none")) c.chart_line_none = v->b;
  if (const MV* v = p.Get("plot_fill")) c.plot_fill = static_cast<uint32_t>(v->i);
  auto axis = [&](const char* k, model::ChartAxisFormat& a) {
    const std::string b = k;
    if (const MV* v = p.Get((b + "_visible").c_str())) a.visible = v->b;
    if (const MV* v = p.Get((b + "_has_title").c_str())) a.has_title = v->b;
    if (const MV* v = p.Get((b + "_title").c_str())) {
      a.title = v->s;
      if (!p.Has((b + "_has_title").c_str())) a.has_title = true;
    }
    if (const MV* v = p.Get((b + "_format").c_str())) a.number_format = v->s;
    if (const MV* v = p.Get((b + "_grid").c_str())) a.gridlines = v->b;
    if (const MV* v = p.Get((b + "_min_auto").c_str())) a.min_auto = v->b;
    if (const MV* v = p.Get((b + "_max_auto").c_str())) a.max_auto = v->b;
    if (const MV* v = p.Get((b + "_min").c_str())) a.min = v->Num(), a.min_auto = p.Has((b + "_min_auto").c_str()) ? a.min_auto : false;
    if (const MV* v = p.Get((b + "_max").c_str())) a.max = v->Num(), a.max_auto = p.Has((b + "_max_auto").c_str()) ? a.max_auto : false;
    if (const MV* v = p.Get((b + "_major").c_str())) a.major = std::max(0.0, v->Num());
    if (const MV* v = p.Get((b + "_minor").c_str())) a.minor = std::max(0.0, v->Num());
    if (const MV* v = p.Get((b + "_log").c_str())) a.log = v->b;
    if (const MV* v = p.Get((b + "_log_base").c_str())) a.log_base = std::clamp(v->Num(), 2.0, 1000.0);
    if (const MV* v = p.Get((b + "_reverse").c_str())) a.reverse = v->b;
    if (const MV* v = p.Get((b + "_crosses").c_str())) a.crosses = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
    if (const MV* v = p.Get((b + "_crosses_at").c_str())) a.crosses_at = v->Num();
    if (const MV* v = p.Get((b + "_display_unit").c_str())) a.display_unit = v->Num() > 0 ? v->Num() : 1;
    if (const MV* v = p.Get((b + "_major_tick").c_str())) a.major_tick = static_cast<int>(std::clamp<int64_t>(v->i, -1, 3));
    if (const MV* v = p.Get((b + "_minor_tick").c_str())) a.minor_tick = static_cast<int>(std::clamp<int64_t>(v->i, 0, 3));
    if (const MV* v = p.Get((b + "_label_pos").c_str())) a.label_pos = static_cast<int>(std::clamp<int64_t>(v->i, 0, 3));
    if (const MV* v = p.Get((b + "_minor_grid").c_str())) a.minor_gridlines = v->b;
    if (const MV* v = p.Get((b + "_between").c_str())) a.between = v->b;
    if (const MV* v = p.Get((b + "_label_interval").c_str())) a.label_interval = static_cast<int>(std::clamp<int64_t>(v->i, 0, 100000));
    if (const MV* v = p.Get((b + "_mark_interval").c_str())) a.mark_interval = static_cast<int>(std::clamp<int64_t>(v->i, 0, 100000));
    if (const MV* v = p.Get((b + "_unit_label").c_str())) a.unit_label = v->b;
  };
  axis("cat", c.cat_axis);
  axis("val", c.val_axis);
  axis("val2", c.val_axis2);
  auto view = [&](const char* k, double* v, double lo, double hi) {
    if (const MV* m = p.Get(k)) {
      *v = std::clamp(m->Num(), lo, hi);
      c.view3d = true;
    }
  };
  view("rotation", &c.rotation, -360, 360);
  view("elevation", &c.elevation, -90, 90);
  view("perspective", &c.perspective, 0, 100);
  view("depth_percent", &c.depth_percent, 20, 2000);
  if (const MV* m = p.Get("view3d")) c.view3d = m->b;
  if (const MV* v = p.Get("hidden_cats")) {
    c.hidden_cats.clear();
    for (const MV& k : v->a) {
      if (k.i >= 0 && k.i < 1000000) c.hidden_cats.push_back(static_cast<int>(k.i));
    }
    std::sort(c.hidden_cats.begin(), c.hidden_cats.end());
    c.hidden_cats.erase(std::unique(c.hidden_cats.begin(), c.hidden_cats.end()), c.hidden_cats.end());
  }
  ApplyChartTypeOptions(c, p);
  ApplyChartFormat(c, p);
}

void ApplySeries(model::ChartSeries& se, const MV& p) {
  if (const MV* v = p.Get("name")) se.name = v->s;
  if (const MV* v = p.Get("line_weight")) se.line_weight = static_cast<float>(std::clamp(v->Num(), 0.0, 100.0));
  if (p.Has("smooth")) se.smooth = p.GetBool("smooth");
  if (const MV* v = p.Get("box_flags")) se.box_flags = static_cast<int>(std::clamp(v->Num(), 0.0, 15.0));
  if (const MV* v = p.Get("marker")) se.marker = static_cast<int>(std::clamp<int64_t>(v->i, -1, 6));
  if (const MV* v = p.Get("marker_size")) se.marker_size = static_cast<float>(std::clamp(v->Num(), 0.0, 200.0));
  if (const MV* v = p.Get("filtered")) se.filtered = v->b;
  if (const MV* v = p.Get("explosion")) se.explosion = static_cast<int>(std::clamp(v->Num(), 0.0, 400.0));
  if (const MV* v = p.Get("vary_colors")) se.vary_colors = v->b;
  if (const MV* v = p.Get("label_pos")) se.label_pos = static_cast<int>(std::clamp<int64_t>(v->i, -1, 8));
  if (const MV* v = p.Get("label_show")) se.label_show = static_cast<int>(std::clamp<int64_t>(v->i, 0, 63));
  if (const MV* v = p.Get("label_range")) se.label_range = v->s;
  if (const MV* v = p.Get("label_format")) se.label_format = v->s;
  if (const MV* v = p.Get("label_sep")) se.label_sep = v->s;
  if (const MV* v = p.Get("bubble_sizes")) se.bubble_sizes = v->s;
  if (const MV* v = p.Get("line_color")) se.line_color = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("no_fill")) se.no_fill = v->b;
  if (const MV* v = p.Get("no_line")) se.no_line = v->b;
  if (const MV* v = p.Get("dash")) se.dash = static_cast<int>(std::clamp<int64_t>(v->i, 0, 5));
  if (const MV* v = p.Get("transparency")) se.transparency = static_cast<int>(std::clamp(v->Num(), 0.0, 100.0));
  if (const MV* v = p.Get("invert_negative")) se.invert_negative = v->b;
  if (const MV* v = p.Get("invert_color")) se.invert_color = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("marker_fill")) se.marker_fill = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("marker_line")) se.marker_line = static_cast<uint32_t>(v->i);
  // one point's own format (Series.Points(i)): only the keys given change
  if (const MV* v = p.Get("point_set")) {
    const int idx = static_cast<int>(v->GetInt("index", -1));
    if (idx >= 0) {
      auto it = std::find_if(se.points.begin(), se.points.end(), [&](const model::ChartPointFormat& q) { return q.index == idx; });
      if (it == se.points.end()) {
        model::ChartPointFormat q;
        q.index = idx;
        se.points.push_back(q);
        it = se.points.end() - 1;
      }
      if (const MV* f = v->Get("fill")) it->fill = static_cast<uint32_t>(f->i);
      if (const MV* f = v->Get("line")) it->line = static_cast<uint32_t>(f->i);
      if (const MV* f = v->Get("explosion")) it->explosion = static_cast<int>(std::clamp<int64_t>(f->i, -1, 400));
      if (const MV* f = v->Get("label")) it->label = static_cast<int>(std::clamp<int64_t>(f->i, -1, 1));
      if (const MV* f = v->Get("label_dx")) it->label_dx = static_cast<float>(std::clamp(f->Num(), -1.0, 1.0));
      if (const MV* f = v->Get("label_dy")) it->label_dy = static_cast<float>(std::clamp(f->Num(), -1.0, 1.0));
      if (const MV* f = v->Get("label_text")) it->label_text = f->s, it->label = 1;
      if (const MV* f = v->Get("label_show")) it->label_show = static_cast<int>(std::clamp<int64_t>(f->i, -1, 63));
      if (const MV* f = v->Get("label_pos")) it->label_pos = static_cast<int>(std::clamp<int64_t>(f->i, -1, 8));
      if (ApplyFont(*v, "label_font", it->label_font)) it->label_font_set = true;
      if (const MV* f = v->Get("label_font_set")) it->label_font_set = f->b;
      if (ApplyFont(*v, "legend_font", it->legend_font)) it->legend_font_set = true;
      if (const MV* f = v->Get("label_fill")) it->label_fill = static_cast<uint32_t>(f->i);
      if (const MV* f = v->Get("label_line")) it->label_line = static_cast<uint32_t>(f->i);
      if (const MV* f = v->Get("legend_hidden")) it->legend_hidden = f->b;
    }
  }
  if (const MV* v = p.Get("formula")) {  // =SERIES(name, X values, values, order)
    std::string name, x, values;
    int order = 0;
    if (ParseSeriesFormula(v->s, &name, &x, &values, &order)) {
      se.name = name;
      se.x_values = x;
      se.values = values;
      se.python_data = false;
      se.data.reset();
    }
  }
  if (const MV* v = p.Get("values")) {
    se.values = v->s;
    se.python_data = false;
    se.data.reset();
  }
  if (const MV* v = p.Get("x_values")) se.x_values = v->s;
  if (const MV* v = p.Get("axis_group")) se.axis_group = static_cast<int>(v->i);
  if (const MV* v = p.Get("color")) se.color = static_cast<uint32_t>(v->i);
  if (const MV* v = p.Get("capacity")) se.capacity = static_cast<uint32_t>(std::max<int64_t>(1, v->i));
  if (const MV* v = p.Get("type")) se.type = static_cast<int>(std::clamp<int64_t>(v->i, -1, model::kChartTypeCount - 1));
  if (const MV* v = p.Get("data_labels")) se.data_labels = v->b;
  if (const MV* v = p.Get("trend")) se.trend = static_cast<model::TrendKind>(std::clamp<int64_t>(v->i, 0, 6));
  if (const MV* v = p.Get("trend_order")) se.trend_order = static_cast<int>(std::clamp<int64_t>(v->i, 2, 6));
  if (const MV* v = p.Get("trend_period")) se.trend_period = static_cast<int>(std::clamp<int64_t>(v->i, 2, 255));
  if (const MV* v = p.Get("trend_forward")) se.trend_forward = std::max(0.0, v->Num());
  if (const MV* v = p.Get("trend_backward")) se.trend_backward = std::max(0.0, v->Num());
  if (const MV* v = p.Get("trend_eq")) se.trend_eq = v->b;
  if (const MV* v = p.Get("trend_r2")) se.trend_r2 = v->b;
  if (const MV* v = p.Get("err")) se.err = static_cast<model::ErrorBarKind>(std::clamp<int64_t>(v->i, 0, 5));
  if (const MV* v = p.Get("err_amount")) se.err_amount = v->Num();
  if (const MV* v = p.Get("err_plus")) se.err_plus = v->s;
  if (const MV* v = p.Get("err_minus")) se.err_minus = v->s;
  if (const MV* v = p.Get("err_include")) se.err_include = static_cast<int>(std::clamp<int64_t>(v->i, 1, 3));
  if (const MV* v = p.Get("totals")) {
    se.totals.clear();
    for (const MV& t : v->a) se.totals.push_back(static_cast<int>(t.i));
  }
  ApplySeriesTypeOptions(se, p);
  ApplySeriesFormat(se, p);
}

MV PanelToMV(const model::Panel& p) {
  MV x = MV::Map();
  x.Set("name", MV::Str(p.name));
  x.Set("caption", MV::Str(p.caption));
  x.Set("dock", MV::Str(p.dock));
  x.Set("width", MV::Double(p.width));
  x.Set("height", MV::Double(p.height));
  x.Set("visible", MV::Bool(p.visible));
  x.Set("form", MV::Bool(p.form));
  x.Set("modal", MV::Bool(p.modal));
  x.Set("back_color", MV::Int(p.back_color));
  x.Set("fore_color", MV::Int(p.fore_color));
  MV cs = MV::Array();
  for (const auto& c : p.controls) {
    MV m = MV::Map();
    m.Set("type", MV::Str(c.type));
    m.Set("name", MV::Str(c.name));
    m.Set("caption", MV::Str(c.caption));
    m.Set("linked_cell", MV::Str(c.linked_cell));
    m.Set("on_action", MV::Str(c.on_action));
    m.Set("list_fill_range", MV::Str(c.list_fill_range));
    m.Set("chart", MV::Str(c.chart));
    m.Set("items", StrList(c.items));
    m.Set("value", MV::Str(c.value));
    m.Set("min", MV::Double(c.min));
    m.Set("max", MV::Double(c.max));
    m.Set("enabled", MV::Bool(c.enabled));
    m.Set("visible", MV::Bool(c.visible));
    m.Set("left", MV::Double(c.left));
    m.Set("top", MV::Double(c.top));
    m.Set("width", MV::Double(c.width));
    m.Set("height", MV::Double(c.height));
    m.Set("group", MV::Str(c.group));
    m.Set("back_color", MV::Int(c.back_color));
    m.Set("fore_color", MV::Int(c.fore_color));
    m.Set("font_size", MV::Double(c.font_size));
    m.Set("bold", MV::Bool(c.bold));
    m.Set("number_format", MV::Str(c.number_format));
    m.Set("small_change", MV::Double(c.small_change));
    cs.Push(std::move(m));
  }
  x.Set("controls", std::move(cs));
  return x;
}

void ApplyPanelControl(model::PanelControl& c, const MV& m) {
  if (const MV* v = m.Get("type")) c.type = LowerStr(v->s);
  if (const MV* v = m.Get("name")) c.name = v->s;
  if (const MV* v = m.Get("caption")) c.caption = v->s;
  if (const MV* v = m.Get("linked_cell")) c.linked_cell = v->s;
  if (const MV* v = m.Get("on_action")) c.on_action = v->s;
  if (const MV* v = m.Get("list_fill_range")) c.list_fill_range = v->s;
  if (const MV* v = m.Get("chart")) c.chart = v->s;
  if (const MV* v = m.Get("items")) c.items = ToStrList(*v);
  if (const MV* v = m.Get("value")) {
    MV one = MV::Array();
    one.Push(*v);
    c.value = ToStrList(one).front();
  }
  if (const MV* v = m.Get("min")) c.min = v->Num();
  if (const MV* v = m.Get("max")) c.max = v->Num();
  if (const MV* v = m.Get("enabled")) c.enabled = v->b;
  if (const MV* v = m.Get("visible")) c.visible = v->b;
  if (const MV* v = m.Get("left")) c.left = v->Num();
  if (const MV* v = m.Get("top")) c.top = v->Num();
  if (const MV* v = m.Get("width")) c.width = v->Num();
  if (const MV* v = m.Get("height")) c.height = v->Num();
  if (const MV* v = m.Get("group")) c.group = v->s;
  if (const MV* v = m.Get("back_color")) c.back_color = static_cast<int64_t>(v->Num());
  if (const MV* v = m.Get("fore_color")) c.fore_color = static_cast<int64_t>(v->Num());
  if (const MV* v = m.Get("font_size")) c.font_size = v->Num();
  if (const MV* v = m.Get("bold")) c.bold = v->b;
  if (const MV* v = m.Get("number_format")) c.number_format = v->s;
  if (const MV* v = m.Get("small_change")) c.small_change = std::max(1e-9, v->Num());
}

void ApplyPanel(model::Panel& p, const MV& m) {
  if (const MV* v = m.Get("name")) p.name = v->s;
  if (const MV* v = m.Get("caption")) p.caption = v->s;
  if (const MV* v = m.Get("dock")) p.dock = LowerStr(v->s);
  if (const MV* v = m.Get("width")) p.width = v->Num();
  if (const MV* v = m.Get("height")) p.height = v->Num();
  if (const MV* v = m.Get("visible")) p.visible = v->b;
  if (const MV* v = m.Get("form")) p.form = v->b;
  if (const MV* v = m.Get("modal")) p.modal = v->b;
  if (const MV* v = m.Get("back_color")) p.back_color = v->t == MV::T::Int ? v->i : static_cast<int64_t>(v->Num());
  if (const MV* v = m.Get("fore_color")) p.fore_color = v->t == MV::T::Int ? v->i : static_cast<int64_t>(v->Num());
  if (const MV* v = m.Get("controls")) {
    // KeepValues: controls with the same name keep their old values (Panel.Rebuild(KeepValues=True), PLAN §10.3)
    const bool keep = m.GetBool("keep_values", true);
    std::vector<model::PanelControl> next;
    for (const auto& cm : v->a) {
      model::PanelControl c;
      ApplyPanelControl(c, cm);
      if (keep && !cm.Has("value")) {
        for (const auto& old : p.controls) {
          if (old.name == c.name && old.type == c.type) c.value = old.value;
        }
      }
      next.push_back(std::move(c));
    }
    p.controls = std::move(next);
  }
}

// index < 0 or out of range: add a series
model::ChartSeries& SeriesAt(ChartObj& x, int64_t index) {
  if (index < 0 || static_cast<size_t>(index) >= x.series.size()) {
    x.series.emplace_back();
    return x.series.back();
  }
  return x.series[static_cast<size_t>(index)];
}

const Control* FindCtl(const model::Sheet& s, const std::string& name) {
  const std::string k = LowerStr(name);
  for (const auto& c : *s.controls) {
    if (LowerStr(c.name) == k) return &c;
  }
  return nullptr;
}

const ChartObj* FindChart(const model::Sheet& s, const std::string& name) {
  const std::string k = LowerStr(name);
  for (const auto& c : *s.charts) {
    if (LowerStr(c.name) == k) return &c;
  }
  // indexes are accepted too ("1", "2"..., ChartObjects(1))
  if (!name.empty() && std::all_of(name.begin(), name.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
    const size_t i = static_cast<size_t>(std::atoi(name.c_str()));
    if (i >= 1 && i <= s.charts->size()) return &(*s.charts)[i - 1];
  }
  return nullptr;
}

std::string WindowNameFor(AppState& s, const std::string& kind, const std::string& name) {
  if (kind == "sheet") {
    const int32_t i = s.snap->wb.FindSheet(name);
    if (i < 0) return {};
    return s.SheetWindowName(s.snap->wb.sheets[static_cast<size_t>(i)]->id);
  }
  if (kind == "panel") {
    for (const auto& p : *s.snap->wb.panels) {
      if (p.name == name) return (p.caption.empty() ? p.name : p.caption) + "###yc_panel_" + p.name;
    }
    return {};
  }
  if (kind == "chart") {
    for (const auto& sh : s.snap->wb.sheets) {
      for (const auto& c : *sh->charts) {
        if (c.name == name) return c.name + "###yc_chartobj_" + std::to_string(c.id);
      }
    }
    return {};
  }
  if (kind == "container") {
    for (const auto& c : s.containers) {
      if (c.name == name) return s.ContainerWindowName(c);
    }
  }
  if (kind == "figure") return FigureWindowName(name);  // yc.plot figures, by key or id
  return {};
}

// ---- shapes and sparklines ----
MV ShapeFxToMV(const model::ShapeEffects& own);  // below
void ApplyShapeFx(model::ShapeEffects& fx, const MV& p);

MV ShapeToMV(uint32_t sheet, const model::ShapeObj& x) {
  MV o = MV::Map();
  o.Set("sheet", MV::Int(sheet));
  o.Set("id", MV::Int(x.id));
  o.Set("kind", MV::Int(static_cast<int>(x.kind)));
  o.Set("name", MV::Str(x.name));
  o.Set("left", MV::Double(x.left));
  o.Set("top", MV::Double(x.top));
  o.Set("width", MV::Double(x.width));
  o.Set("height", MV::Double(x.height));
  o.Set("placement", MV::Int(PlacementInt(x.placement)));
  o.Set("fill_visible", MV::Bool(x.fill_visible));
  o.Set("line_visible", MV::Bool(x.line_visible));
  o.Set("fill_color", MV::Int(x.fill_color));
  o.Set("line_color", MV::Int(x.line_color));
  o.Set("line_weight", MV::Double(x.line_weight));
  o.Set("flip_h", MV::Bool(x.flip_h));
  o.Set("flip_v", MV::Bool(x.flip_v));
  o.Set("text", MV::Str(x.text));
  o.Set("font_name", MV::Str(x.font_name));
  o.Set("font_size", MV::Double(x.font_size));
  o.Set("font_color", MV::Int(x.font_color));
  o.Set("bold", MV::Bool(x.bold));
  o.Set("italic", MV::Bool(x.italic));
  o.Set("halign", MV::Int(x.halign));
  o.Set("valign", MV::Int(x.valign));
  o.Set("has_image", MV::Bool(!x.image.empty()));
  o.Set("on_action", MV::Str(x.on_action));
  o.Set("visible", MV::Bool(x.visible));
  o.Set("z", MV::Int(x.z));
  o.Set("line_dash", MV::Int(x.line_dash));
  o.Set("begin_arrow", MV::Int(x.begin_arrow));
  o.Set("end_arrow", MV::Int(x.kind == model::ShapeKind::Arrow ? 1 : x.end_arrow));
  o.Set("fill_transparency", MV::Double(x.fill_transparency));
  o.Set("margin_l", MV::Double(x.margin_l));
  o.Set("margin_t", MV::Double(x.margin_t));
  o.Set("margin_r", MV::Double(x.margin_r));
  o.Set("margin_b", MV::Double(x.margin_b));
  o.Set("wrap", MV::Bool(x.wrap));
  o.Set("autofit", MV::Bool(x.autofit));
  o.Set("alt_text", MV::Str(x.alt_text));
  o.Set("crop_l", MV::Double(x.crop_l));
  o.Set("crop_t", MV::Double(x.crop_t));
  o.Set("crop_r", MV::Double(x.crop_r));
  o.Set("crop_b", MV::Double(x.crop_b));
  o.Set("locked", MV::Bool(x.locked));
  o.Set("lock_aspect", MV::Bool(x.lock_aspect));
  o.Set("rotation", MV::Double(x.rotation));
  o.Set("group", MV::Int(x.group));
  o.Set("on_chart", MV::Int(x.on_chart));
  o.Set("print_object", MV::Bool(x.print_object));
  o.Set("orig_w", MV::Double(x.orig_w));
  o.Set("orig_h", MV::Double(x.orig_h));
  o.Set("auto_type", MV::Int(x.auto_type));
  o.Set("connector", MV::Int(x.connector));
  MV path = MV::Array();
  for (double v : x.path) path.Push(MV::Double(v));
  o.Set("path", std::move(path));
  o.Set("path_closed", MV::Bool(x.path_closed));
  o.Set("path_smooth", MV::Bool(x.path_smooth));
  // Shape.Adjustments: the values (Excel's defaults where none were set)
  MV adj = MV::Array();
  static const int kKindType[] = {0, 1, 5, 9, 7, 0, 0, 33, 0};
  const int kk = static_cast<int>(x.kind);
  std::vector<double> av = engine::AutoShapeAdjustDefaults(x.auto_type ? x.auto_type : kk >= 0 && kk < 9 ? kKindType[kk] : 0);
  for (size_t i = 0; i < av.size() && i < x.adj.size(); ++i) av[i] = x.adj[i];
  for (double v : av) adj.Push(MV::Double(v));
  o.Set("adj", std::move(adj));
  o.Set("fx", ShapeFxToMV(x.fx));
  return o;
}

void ApplyShape(model::ShapeObj& x, const MV& p) {
  if (const MV* v = p.Get("name"); v && v->t == MV::T::Str) x.name = v->s;
  if (p.Has("kind")) x.kind = static_cast<model::ShapeKind>(std::clamp<int64_t>(p.GetInt("kind"), 0, 8));
  if (p.Has("left")) x.left = p.GetDouble("left");
  if (p.Has("top")) x.top = p.GetDouble("top");
  if (p.Has("width")) x.width = std::max(0.0, p.GetDouble("width"));
  if (p.Has("height")) x.height = std::max(0.0, p.GetDouble("height"));
  if (p.Has("placement")) x.placement = PlacementOf(p.GetInt("placement"));
  if (p.Has("fill_visible")) x.fill_visible = p.GetBool("fill_visible");
  if (p.Has("line_visible")) x.line_visible = p.GetBool("line_visible");
  if (p.Has("fill_color")) x.fill_color = static_cast<uint32_t>(p.GetInt("fill_color"));
  if (p.Has("line_color")) x.line_color = static_cast<uint32_t>(p.GetInt("line_color"));
  if (p.Has("line_weight")) x.line_weight = static_cast<float>(std::max(0.0, p.GetDouble("line_weight")));
  if (p.Has("flip_h")) x.flip_h = p.GetBool("flip_h");
  if (p.Has("flip_v")) x.flip_v = p.GetBool("flip_v");
  if (p.Has("text")) x.text = p.GetStr("text");
  if (p.Has("font_name")) x.font_name = p.GetStr("font_name");
  if (p.Has("font_size")) x.font_size = static_cast<float>(std::max(1.0, p.GetDouble("font_size")));
  if (p.Has("font_color")) x.font_color = static_cast<uint32_t>(p.GetInt("font_color"));
  if (p.Has("bold")) x.bold = p.GetBool("bold");
  if (p.Has("italic")) x.italic = p.GetBool("italic");
  if (p.Has("halign")) x.halign = static_cast<int>(std::clamp<int64_t>(p.GetInt("halign"), 0, 2));
  if (p.Has("valign")) x.valign = static_cast<int>(std::clamp<int64_t>(p.GetInt("valign"), 0, 2));
  if (p.Has("on_action")) x.on_action = p.GetStr("on_action");
  if (p.Has("visible")) x.visible = p.GetBool("visible");
  if (p.Has("z")) x.z = static_cast<int>(p.GetInt("z"));
  if (p.Has("line_dash")) x.line_dash = static_cast<int>(std::clamp<int64_t>(p.GetInt("line_dash"), 0, 5));
  if (p.Has("begin_arrow")) x.begin_arrow = p.GetInt("begin_arrow") ? 1 : 0;
  if (p.Has("end_arrow")) {
    x.end_arrow = p.GetInt("end_arrow") ? 1 : 0;
    if (x.kind == model::ShapeKind::Arrow && !x.end_arrow) x.kind = model::ShapeKind::Line;
  }
  if (p.Has("fill_transparency")) x.fill_transparency = static_cast<float>(std::clamp(p.GetDouble("fill_transparency"), 0.0, 1.0));
  for (const char* k : {"margin_l", "margin_t", "margin_r", "margin_b", "crop_l", "crop_t", "crop_r", "crop_b"}) {
    if (!p.Has(k)) continue;
    const double v = std::max(0.0, p.GetDouble(k));
    const std::string key = k;
    (key == "margin_l" ? x.margin_l : key == "margin_t" ? x.margin_t : key == "margin_r" ? x.margin_r : key == "margin_b" ? x.margin_b
     : key == "crop_l" ? x.crop_l : key == "crop_t" ? x.crop_t : key == "crop_r" ? x.crop_r : x.crop_b) = v;
  }
  if (p.Has("wrap")) x.wrap = p.GetBool("wrap");
  if (p.Has("autofit")) x.autofit = p.GetBool("autofit");
  if (p.Has("alt_text")) x.alt_text = p.GetStr("alt_text");
  if (p.Has("locked")) x.locked = p.GetBool("locked");
  if (p.Has("lock_aspect")) x.lock_aspect = p.GetBool("lock_aspect");
  if (p.Has("rotation")) x.rotation = std::fmod(std::fmod(p.GetDouble("rotation"), 360.0) + 360.0, 360.0);
  if (p.Has("group")) x.group = static_cast<uint32_t>(std::max<int64_t>(0, p.GetInt("group")));
  if (p.Has("on_chart")) x.on_chart = static_cast<uint32_t>(std::max<int64_t>(0, p.GetInt("on_chart")));
  if (p.Has("print_object")) x.print_object = p.GetBool("print_object");
  if (p.Has("auto_type")) x.auto_type = static_cast<int>(std::clamp<int64_t>(p.GetInt("auto_type"), 0, 1000));
  if (p.Has("connector")) x.connector = static_cast<int>(std::clamp<int64_t>(p.GetInt("connector"), 0, 3));
  if (const MV* v = p.Get("path"); v && v->t == MV::T::Array) {
    x.path.clear();
    for (const MV& e : v->a) x.path.push_back(e.t == MV::T::Int ? static_cast<double>(e.i) : e.d);
  }
  if (p.Has("path_closed")) x.path_closed = p.GetBool("path_closed");
  if (p.Has("path_smooth")) x.path_smooth = p.GetBool("path_smooth");
  if (const MV* v = p.Get("adj"); v && v->t == MV::T::Array) {
    // a shape of an older kind takes its AutoShapeType, so its adjustments are drawn
    if (x.auto_type == 0) {
      static const int kType[] = {0, 1, 5, 9, 7, 0, 0, 33, 0};
      const int k = static_cast<int>(x.kind);
      if (k >= 0 && k < 9) x.auto_type = kType[k];
    }
    x.adj.clear();
    for (const MV& e : v->a) x.adj.push_back(e.t == MV::T::Int ? static_cast<double>(e.i) : e.d);
  }
  if (const MV* v = p.Get("fx"); v && v->t == MV::T::Map) ApplyShapeFx(x.fx, *v);  // Shape.Shadow / Glow / SoftEdge / Reflection / ThreeD
}

const model::ShapeObj* FindShapeByName(const model::Sheet& s, const std::string& name) {
  for (const auto& x : *s.shapes) {
    if (LowerStr(x.name) == LowerStr(name)) return &x;
  }
  return nullptr;
}

MV SparkToMV(const model::Workbook& wb, const model::SparklineGroup& g) {
  MV o = MV::Map();
  o.Set("id", MV::Int(g.id));
  o.Set("type", MV::Int(g.type));
  MV items = MV::Array();
  for (const auto& it : g.items) {
    MV i = MV::Map();
    i.Set("location", MV::Str(core::CellA1(it.r, it.c)));
    const model::Sheet* src = wb.SheetById(it.src_sheet);
    std::string a = core::CellA1(it.src.r0, it.src.c0);
    if (!it.src.IsCell()) a += ":" + core::CellA1(it.src.r1, it.src.c1);
    i.Set("source", MV::Str(src ? "'" + src->name + "'!" + a : a));
    items.Push(std::move(i));
  }
  o.Set("items", std::move(items));
  o.Set("color", MV::Int(g.color));
  o.Set("negative_color", MV::Int(g.negative_color));
  o.Set("marker_color", MV::Int(g.marker_color));
  o.Set("high_color", MV::Int(g.high_color));
  o.Set("low_color", MV::Int(g.low_color));
  o.Set("first_color", MV::Int(g.first_color));
  o.Set("last_color", MV::Int(g.last_color));
  o.Set("high", MV::Bool(g.high));
  o.Set("low", MV::Bool(g.low));
  o.Set("first", MV::Bool(g.first));
  o.Set("last", MV::Bool(g.last));
  o.Set("negative", MV::Bool(g.negative));
  o.Set("markers", MV::Bool(g.markers));
  o.Set("axis", MV::Bool(g.axis));
  o.Set("line_weight", MV::Double(g.line_weight));
  o.Set("display_blanks", MV::Int(g.display_blanks));
  o.Set("show_hidden", MV::Bool(g.show_hidden));
  o.Set("min_scale", MV::Int(g.min_scale));
  o.Set("max_scale", MV::Int(g.max_scale));
  o.Set("min_value", MV::Double(g.min_value));
  o.Set("max_value", MV::Double(g.max_value));
  o.Set("right_to_left", MV::Bool(g.right_to_left));
  o.Set("date_range", MV::Str(g.date_range));
  o.Set("axis_color", MV::Int(g.axis_color));
  return o;
}

void ApplySpark(model::SparklineGroup& g, const MV& p) {
  if (p.Has("type")) g.type = static_cast<int>(std::clamp<int64_t>(p.GetInt("type"), 0, 2));
  struct C {
    const char* k;
    uint32_t* v;
  };
  for (const C& c : {C{"color", &g.color}, C{"negative_color", &g.negative_color}, C{"marker_color", &g.marker_color}, C{"high_color", &g.high_color},
                     C{"low_color", &g.low_color}, C{"first_color", &g.first_color}, C{"last_color", &g.last_color}}) {
    if (p.Has(c.k)) *c.v = static_cast<uint32_t>(p.GetInt(c.k));
  }
  struct B {
    const char* k;
    bool* v;
  };
  for (const B& b : {B{"high", &g.high}, B{"low", &g.low}, B{"first", &g.first}, B{"last", &g.last}, B{"negative", &g.negative},
                     B{"markers", &g.markers}, B{"axis", &g.axis}}) {
    if (p.Has(b.k)) *b.v = p.GetBool(b.k);
  }
  if (p.Has("line_weight")) g.line_weight = static_cast<float>(std::max(0.25, p.GetDouble("line_weight")));
  if (p.Has("display_blanks")) g.display_blanks = static_cast<int>(std::clamp<int64_t>(p.GetInt("display_blanks"), 1, 3));
  if (p.Has("show_hidden")) g.show_hidden = p.GetBool("show_hidden");
  if (p.Has("min_scale")) g.min_scale = static_cast<int>(std::clamp<int64_t>(p.GetInt("min_scale"), 1, 3));
  if (p.Has("max_scale")) g.max_scale = static_cast<int>(std::clamp<int64_t>(p.GetInt("max_scale"), 1, 3));
  if (p.Has("min_value")) g.min_value = p.GetDouble("min_value");
  if (p.Has("max_value")) g.max_value = p.GetDouble("max_value");
  if (p.Has("right_to_left")) g.right_to_left = p.GetBool("right_to_left");
  if (p.Has("date_range")) g.date_range = p.GetStr("date_range");
  if (p.Has("axis_color")) g.axis_color = static_cast<uint32_t>(p.GetInt("axis_color"));
}

// ---- Format Shape > Effects (Shape.Shadow, Glow, SoftEdge, Reflection, ThreeD) ----
struct FxField {
  const char* key;
  float model::ShapeEffects::*f;
};
const FxField kFxFloats[] = {{"shadow_transparency", &model::ShapeEffects::shadow_transparency},
                             {"shadow_size", &model::ShapeEffects::shadow_size},
                             {"shadow_blur", &model::ShapeEffects::shadow_blur},
                             {"shadow_angle", &model::ShapeEffects::shadow_angle},
                             {"shadow_distance", &model::ShapeEffects::shadow_distance},
                             {"refl_size", &model::ShapeEffects::refl_size},
                             {"refl_transparency", &model::ShapeEffects::refl_transparency},
                             {"refl_blur", &model::ShapeEffects::refl_blur},
                             {"refl_distance", &model::ShapeEffects::refl_distance},
                             {"glow_size", &model::ShapeEffects::glow_size},
                             {"glow_transparency", &model::ShapeEffects::glow_transparency},
                             {"soft_edge", &model::ShapeEffects::soft_edge},
                             {"bevel_top_w", &model::ShapeEffects::bevel_top_w},
                             {"bevel_top_h", &model::ShapeEffects::bevel_top_h},
                             {"bevel_bottom_w", &model::ShapeEffects::bevel_bottom_w},
                             {"bevel_bottom_h", &model::ShapeEffects::bevel_bottom_h},
                             {"rot_x", &model::ShapeEffects::rot_x},
                             {"rot_y", &model::ShapeEffects::rot_y},
                             {"rot_z", &model::ShapeEffects::rot_z},
                             {"perspective", &model::ShapeEffects::perspective}};
struct FxInt {
  const char* key;
  int model::ShapeEffects::*f;
};
const FxInt kFxInts[] = {{"preset", &model::ShapeEffects::preset},       {"theme_style", &model::ShapeEffects::theme_style},
                         {"shadow", &model::ShapeEffects::shadow},       {"bevel_top", &model::ShapeEffects::bevel_top},
                         {"bevel_bottom", &model::ShapeEffects::bevel_bottom}, {"rot_preset", &model::ShapeEffects::rot_preset}};

MV ShapeFxToMV(const model::ShapeEffects& own) {
  MV o = MV::Map();
  const model::ShapeEffects e = model::EffectiveEffects(own);  // what it is drawn with (a theme style's effects too)
  for (const FxField& f : kFxFloats) o.Set(f.key, MV::Double(e.*f.f));
  for (const FxInt& f : kFxInts) o.Set(f.key, MV::Int(e.*f.f));
  o.Set("theme_style", MV::Int(own.theme_style));
  o.Set("shadow_color", MV::Int(e.shadow_color));
  o.Set("glow_color", MV::Int(e.glow_color));
  return o;
}

void ApplyShapeFx(model::ShapeEffects& fx, const MV& p) {
  if (fx.theme_style >= 0 && !p.Has("theme_style")) {  // a part set by hand: the theme style's effects become its own
    fx = model::EffectiveEffects(fx);
    fx.theme_style = -1;
  }
  for (const FxField& f : kFxFloats) {
    if (p.Has(f.key)) fx.*f.f = static_cast<float>(p.GetDouble(f.key));
  }
  for (const FxInt& f : kFxInts) {
    if (p.Has(f.key)) fx.*f.f = static_cast<int>(p.GetInt(f.key));
  }
  if (p.Has("shadow_color")) fx.shadow_color = 0xFF000000u | static_cast<uint32_t>(p.GetInt("shadow_color"));
  if (p.Has("glow_color")) fx.glow_color = 0xFF000000u | static_cast<uint32_t>(p.GetInt("glow_color"));
  if (p.Has("preset_effect")) fx = model::PresetEffects(static_cast<int>(p.GetInt("preset_effect")));
  if (p.Has("rotation_preset")) {
    const int k = static_cast<int>(p.GetInt("rotation_preset"));
    const auto& rs = model::Rotations3D();
    fx.rot_preset = k >= 1 && k <= static_cast<int>(rs.size()) ? k : 0;
    if (fx.rot_preset) fx.rot_x = rs[static_cast<size_t>(k - 1)].x, fx.rot_y = rs[static_cast<size_t>(k - 1)].y, fx.rot_z = 0, fx.perspective = rs[static_cast<size_t>(k - 1)].perspective;
  }
  for (float* v : {&fx.shadow_transparency, &fx.refl_transparency, &fx.glow_transparency, &fx.refl_blur}) *v = std::clamp(*v, 0.0f, 1.0f);
  if (p.Has("preset_effect") || p.Has("theme_style")) return;
  if (!p.Has("preset")) fx.preset = 0;
}

// ---- page setup ----
std::string SpanText(bool has, uint32_t a, uint32_t b, bool rows) {
  if (!has) return {};
  return rows ? "$" + std::to_string(a + 1) + ":$" + std::to_string(b + 1) : "$" + core::ColumnName(a) + ":$" + core::ColumnName(b);
}

MV PageSetupToMV(const model::PageSetup& ps) {
  MV o = MV::Map();
  o.Set("landscape", MV::Bool(ps.landscape));
  o.Set("paper", MV::Int(ps.paper));
  o.Set("left", MV::Double(ps.left));
  o.Set("right", MV::Double(ps.right));
  o.Set("top", MV::Double(ps.top));
  o.Set("bottom", MV::Double(ps.bottom));
  o.Set("header", MV::Double(ps.header));
  o.Set("footer", MV::Double(ps.footer));
  o.Set("zoom", MV::Int(ps.zoom));
  o.Set("fit_wide", MV::Int(ps.fit_wide));
  o.Set("fit_tall", MV::Int(ps.fit_tall));
  {
    // several print areas read "$A$1:$B$2,$D$5:$E$6" (Excel's PrintArea)
    std::string area;
    if (ps.has_area) {
      std::vector<Rect> all{ps.area};
      all.insert(all.end(), ps.more_areas.begin(), ps.more_areas.end());
      for (const Rect& a : all) {
        if (!area.empty()) area += ",";
        area += core::CellA1(a.r0, a.c0, true, true) + ":" + core::CellA1(a.r1, a.c1, true, true);
      }
    }
    o.Set("print_area", MV::Str(area));
  }
  o.Set("print_comments", MV::Int(ps.print_comments));
  o.Set("print_errors", MV::Int(ps.print_errors));
  o.Set("draft", MV::Bool(ps.draft));
  {
    MV pics = MV::Array();
    for (const auto& pic : ps.hf_pictures) {
      MV m = MV::Map();
      int pw = 0, ph = 0;
      if (!pic.image.empty()) ImageSize(pic.image, &pw, &ph);
      m.Set("has", MV::Bool(!pic.image.empty()));
      m.Set("width", MV::Double(pic.w > 0 ? pic.w : pw * 0.75));  // its printed size (its own at 96 dpi when not set)
      m.Set("height", MV::Double(pic.h > 0 ? pic.h : ph * 0.75));
      pics.Push(std::move(m));
    }
    o.Set("hf_pictures", std::move(pics));
  }
  o.Set("title_rows", MV::Str(SpanText(ps.has_title_rows, ps.title_r0, ps.title_r1, true)));
  o.Set("title_cols", MV::Str(SpanText(ps.has_title_cols, ps.title_c0, ps.title_c1, false)));
  o.Set("gridlines", MV::Bool(ps.gridlines));
  o.Set("headings", MV::Bool(ps.headings));
  o.Set("center_h", MV::Bool(ps.center_h));
  o.Set("center_v", MV::Bool(ps.center_v));
  o.Set("black_and_white", MV::Bool(ps.black_and_white));
  o.Set("over_then_down", MV::Bool(ps.over_then_down));
  o.Set("first_page_number", MV::Int(ps.first_page_number));
  o.Set("header_left", MV::Str(ps.header_left));
  o.Set("header_center", MV::Str(ps.header_center));
  o.Set("header_right", MV::Str(ps.header_right));
  o.Set("footer_left", MV::Str(ps.footer_left));
  o.Set("footer_center", MV::Str(ps.footer_center));
  o.Set("footer_right", MV::Str(ps.footer_right));
  o.Set("different_first", MV::Bool(ps.different_first));
  o.Set("different_odd_even", MV::Bool(ps.different_odd_even));
  o.Set("scale_with_doc", MV::Bool(!ps.no_scale_with_doc));
  o.Set("align_margins", MV::Bool(!ps.no_align_margins));
  {
    MV fa = MV::Array(), ea = MV::Array();
    for (const std::string& t : ps.first_hf) fa.Push(MV::Str(t));
    for (const std::string& t : ps.even_hf) ea.Push(MV::Str(t));
    o.Set("first_hf", std::move(fa));
    o.Set("even_hf", std::move(ea));
  }
  MV rb = MV::Array(), cb = MV::Array();
  for (uint32_t b : ps.row_breaks) rb.Push(MV::Int(b));
  for (uint32_t b : ps.col_breaks) cb.Push(MV::Int(b));
  o.Set("row_breaks", std::move(rb));
  o.Set("col_breaks", std::move(cb));
  return o;
}

// false (with err) when a reference can't be read
bool ApplyPageSetup(const model::Workbook& wb, uint32_t sheet, model::PageSetup& ps, const MV& p, std::string* err) {
  if (p.Has("landscape")) ps.landscape = p.GetBool("landscape");
  if (p.Has("paper")) ps.paper = static_cast<int>(p.GetInt("paper"));
  struct D {
    const char* k;
    double* v;
  };
  for (const D& d : {D{"left", &ps.left}, D{"right", &ps.right}, D{"top", &ps.top}, D{"bottom", &ps.bottom}, D{"header", &ps.header}, D{"footer", &ps.footer}}) {
    if (p.Has(d.k)) *d.v = p.GetDouble(d.k);
  }
  if (p.Has("zoom")) ps.zoom = static_cast<int>(p.GetInt("zoom"));
  if (p.Has("fit_wide")) ps.fit_wide = static_cast<int>(p.GetInt("fit_wide"));
  if (p.Has("fit_tall")) ps.fit_tall = static_cast<int>(p.GetInt("fit_tall"));
  struct B {
    const char* k;
    bool* v;
  };
  for (const B& b : {B{"gridlines", &ps.gridlines}, B{"headings", &ps.headings}, B{"center_h", &ps.center_h}, B{"center_v", &ps.center_v},
                     B{"black_and_white", &ps.black_and_white}, B{"over_then_down", &ps.over_then_down}}) {
    if (p.Has(b.k)) *b.v = p.GetBool(b.k);
  }
  if (p.Has("first_page_number")) ps.first_page_number = static_cast<int>(p.GetInt("first_page_number"));
  struct T {
    const char* k;
    std::string* v;
  };
  for (const T& t : {T{"header_left", &ps.header_left}, T{"header_center", &ps.header_center}, T{"header_right", &ps.header_right},
                     T{"footer_left", &ps.footer_left}, T{"footer_center", &ps.footer_center}, T{"footer_right", &ps.footer_right}}) {
    if (p.Get(t.k)) *t.v = p.GetStr(t.k);
  }
  if (p.Has("different_first")) ps.different_first = p.GetBool("different_first");
  if (p.Has("different_odd_even")) ps.different_odd_even = p.GetBool("different_odd_even");
  if (p.Has("scale_with_doc")) ps.no_scale_with_doc = !p.GetBool("scale_with_doc");
  if (p.Has("align_margins")) ps.no_align_margins = !p.GetBool("align_margins");
  // one text of the first page's / even pages' header and footer: {"first_hf": [index, text]}
  for (const char* k : {"first_hf", "even_hf"}) {
    const MV* v = p.Get(k);
    if (!v || v->a.size() != 2) continue;
    const int64_t i = v->a[0].i;
    if (i >= 0 && i < 6) (std::strcmp(k, "first_hf") == 0 ? ps.first_hf : ps.even_hf)[i] = v->a[1].s;
  }
  auto ref = [&](const char* key, Rect* out, bool* has) {
    const MV* v = p.Get(key);
    if (!v) return true;
    std::string text = v->t == MV::T::Str ? v->s : std::string();
    if (!text.empty() && text[0] == '=') text.erase(0, 1);
    if (text.empty()) {
      *has = false;
      return true;
    }
    uint32_t sid = 0;
    if (!ResolveRangeText(wb, sheet, text, &sid, out) || sid != sheet) {
      *err = std::string("Invalid reference for ") + key + ": " + text;
      return false;
    }
    *has = true;
    return true;
  };
  Rect r;
  if (const MV* pa = p.Get("print_area"); pa && pa->t == MV::T::Str && pa->s.find(',') != std::string::npos) {
    // several ranges: each prints on its own pages
    std::vector<Rect> all;
    std::string text = pa->s;
    if (!text.empty() && text[0] == '=') text.erase(0, 1);
    size_t at = 0;
    while (at <= text.size()) {
      const size_t comma = text.find(',', at);
      const std::string part = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      uint32_t sid = 0;
      Rect a;
      if (!ResolveRangeText(wb, sheet, part, &sid, &a) || sid != sheet) {
        *err = "Invalid reference for print_area: " + part;
        return false;
      }
      all.push_back(a);
      if (comma == std::string::npos) break;
      at = comma + 1;
    }
    ps.has_area = true;
    ps.area = all[0];
    ps.more_areas.assign(all.begin() + 1, all.end());
  } else {
    if (!ref("print_area", &r, &ps.has_area)) return false;
    if (p.Get("print_area") && ps.has_area) ps.area = r;
    if (p.Get("print_area")) ps.more_areas.clear();
  }
  if (p.Has("print_comments")) ps.print_comments = std::clamp(static_cast<int>(p.GetInt("print_comments")), 0, 2);
  if (p.Has("print_errors")) ps.print_errors = std::clamp(static_cast<int>(p.GetInt("print_errors")), 0, 3);
  if (p.Has("draft")) ps.draft = p.GetBool("draft");
  // a header / footer picture: {"hf_picture": [section, path or "", width, height]} (path "" keeps the picture, a size of
  // 0 keeps that size; Graphic.Filename / Width / Height)
  if (const MV* hp = p.Get("hf_picture"); hp && hp->a.size() >= 2) {
    const int64_t k = hp->a[0].i;
    if (k < 0 || k >= 6) {
      *err = "hf_picture: section 0-5";
      return false;
    }
    auto& pic = ps.hf_pictures[k];
    const std::string path = hp->a[1].s;
    if (!path.empty()) {
      std::ifstream f(FromUtf8(path), std::ios::binary);
      if (!f) {
        *err = "The picture can't be read: " + path;
        return false;
      }
      pic.image.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      pic.w = pic.h = 0;
    } else if (hp->a[1].t == MV::T::Nil) {
      pic = model::PageSetup::HfPicture{};
    }
    if (hp->a.size() >= 4) {
      auto num = [](const MV& v) { return v.t == MV::T::Int ? static_cast<double>(v.i) : v.d; };
      const double w = num(hp->a[2]), h = num(hp->a[3]);
      int pw = 0, ph = 0;
      ImageSize(pic.image, &pw, &ph);
      // Excel's LockAspectRatio: a new height takes the width along, and the other way round
      if (w > 0) pic.w = w, pic.h = h > 0 ? h : (pw > 0 ? w * ph / pw : pic.h);
      else if (h > 0) pic.h = h, pic.w = ph > 0 ? h * pw / ph : pic.w;
    }
  }
  if (!ref("title_rows", &r, &ps.has_title_rows)) return false;
  if (p.Get("title_rows") && ps.has_title_rows) ps.title_r0 = r.r0, ps.title_r1 = r.r1;
  if (!ref("title_cols", &r, &ps.has_title_cols)) return false;
  if (p.Get("title_cols") && ps.has_title_cols) ps.title_c0 = r.c0, ps.title_c1 = r.c1;
  return true;
}

// ---- PivotTables ----
MV AxisToMV(const std::vector<model::PivotAxisField>& v) {
  MV a = MV::Array();
  for (const auto& f : v) {
    MV m = MV::Map();
    m.Set("field", MV::Int(f.field));
    m.Set("hidden", StrList(f.hidden));
    m.Set("subtotal", MV::Bool(f.subtotal));
    m.Set("descending", MV::Bool(f.descending));
    m.Set("page_item", MV::Str(f.page_item));
    m.Set("collapsed", StrList(f.collapsed));
    m.Set("collapse_all", MV::Bool(f.collapse_all));
    m.Set("sort_by", MV::Int(f.sort_by));
    m.Set("sort_col", StrList(f.sort_col));
    m.Set("multi_page", MV::Bool(f.multi_page));
    for (const auto& [key, flt] : {std::pair<const char*, const model::PivotFilter*>{"label_filter", &f.label_filter}, {"value_filter", &f.value_filter}}) {
      MV x = MV::Map();
      x.Set("type", MV::Int(flt->type));
      x.Set("data", MV::Int(flt->data));
      x.Set("v1", MV::Str(flt->v1));
      x.Set("v2", MV::Str(flt->v2));
      m.Set(key, std::move(x));
    }
    m.Set("caption", MV::Str(f.caption));
    m.Set("subtotal_funcs", MV::Int(f.subtotal_funcs));
    m.Set("repeat_labels", MV::Bool(f.repeat_labels));
    m.Set("blank_line", MV::Bool(f.blank_line));
    m.Set("show_all_items", MV::Bool(f.show_all_items));
    m.Set("manual_order", StrList(f.manual_order));
    m.Set("include_new", MV::Bool(f.include_new));
    m.Set("known", StrList(f.known));
    m.Set("form", MV::Int(f.form));
    m.Set("sub_top", MV::Int(f.sub_top));
    m.Set("page_break", MV::Bool(f.page_break));
    {
      std::vector<std::string> caps;
      for (const auto& [from, to] : f.item_captions) caps.push_back(from), caps.push_back(to);
      m.Set("item_captions", StrList(caps));
    }
    a.Push(std::move(m));
  }
  return a;
}

std::vector<model::PivotAxisField> AxisFromMV(const MV& v) {
  std::vector<model::PivotAxisField> out;
  for (const auto& m : v.a) {
    model::PivotAxisField f;
    f.field = static_cast<int>(m.GetInt("field", 0));
    if (const MV* h = m.Get("hidden")) f.hidden = ToStrList(*h);
    f.subtotal = m.GetBool("subtotal", true);
    f.descending = m.GetBool("descending");
    f.page_item = m.GetStr("page_item");
    if (const MV* x = m.Get("collapsed")) f.collapsed = ToStrList(*x);
    f.collapse_all = m.GetBool("collapse_all");
    f.sort_by = static_cast<int>(m.GetInt("sort_by", -1));
    if (const MV* x = m.Get("sort_col")) f.sort_col = ToStrList(*x);
    f.multi_page = m.GetBool("multi_page");
    for (const auto& [key, flt] : {std::pair<const char*, model::PivotFilter*>{"label_filter", &f.label_filter}, {"value_filter", &f.value_filter}}) {
      if (const MV* x = m.Get(key); x && x->t == MV::T::Map) {
        flt->type = static_cast<int>(x->GetInt("type"));
        flt->data = static_cast<int>(x->GetInt("data"));
        flt->v1 = x->GetStr("v1");
        flt->v2 = x->GetStr("v2");
      }
    }
    f.caption = m.GetStr("caption");
    f.subtotal_funcs = static_cast<uint32_t>(m.GetInt("subtotal_funcs"));
    f.repeat_labels = m.GetBool("repeat_labels");
    f.blank_line = m.GetBool("blank_line");
    f.show_all_items = m.GetBool("show_all_items");
    if (const MV* x = m.Get("manual_order")) f.manual_order = ToStrList(*x);
    f.include_new = m.GetBool("include_new");
    if (const MV* x = m.Get("known")) f.known = ToStrList(*x);
    f.form = static_cast<int>(std::clamp<int64_t>(m.GetInt("form", -1), -1, 2));
    f.sub_top = static_cast<int>(std::clamp<int64_t>(m.GetInt("sub_top", -1), -1, 1));
    f.page_break = m.GetBool("page_break");
    if (const MV* x = m.Get("item_captions")) {
      const std::vector<std::string> caps = ToStrList(*x);
      for (size_t j = 0; j + 1 < caps.size(); j += 2) f.item_captions.emplace_back(caps[j], caps[j + 1]);
    }
    out.push_back(std::move(f));
  }
  return out;
}

MV PivotToMV(const Document& d, uint32_t sheet, const model::PivotTable& p) {
  MV o = MV::Map();
  o.Set("sheet", MV::Int(sheet));
  o.Set("id", MV::Int(p.id));
  o.Set("name", MV::Str(p.name));
  o.Set("src_sheet", MV::Int(p.src_sheet));
  {
    MV r = MV::Array();
    for (uint32_t v : {p.src.r0, p.src.c0, p.src.r1, p.src.c1}) r.Push(MV::Int(v));
    o.Set("src", std::move(r));
  }
  const model::Sheet* ss = d.S(p.src_sheet);
  o.Set("source", MV::Str(p.from_model ? std::string("ThisWorkbookDataModel") : !p.src_table.empty() ? p.src_table
                                                : (ss ? "'" + ss->name + "'!" : std::string()) + core::CellA1(p.src.r0, p.src.c0, true, true) + ":" +
                                                      core::CellA1(p.src.r1, p.src.c1, true, true)));
  o.Set("src_table", MV::Str(p.src_table));
  o.Set("from_model", MV::Bool(p.from_model));
  o.Set("r0", MV::Int(p.r0));
  o.Set("c0", MV::Int(p.c0));
  o.Set("rows", AxisToMV(p.rows));
  o.Set("cols", AxisToMV(p.cols));
  o.Set("pages", AxisToMV(p.pages));
  MV data = MV::Array();
  for (const auto& df : p.data) {
    MV m = MV::Map();
    m.Set("field", MV::Int(df.field));
    m.Set("func", MV::Int(static_cast<int>(df.func)));
    m.Set("show_as", MV::Int(static_cast<int>(df.show_as)));
    m.Set("caption", MV::Str(df.caption));
    m.Set("number_format", MV::Str(df.number_format));
    m.Set("base_field", MV::Int(df.base_field));
    m.Set("base_item", MV::Str(df.base_item));
    data.Push(std::move(m));
  }
  o.Set("data", std::move(data));
  o.Set("values_on_rows", MV::Bool(p.values_on_rows));
  o.Set("row_grand", MV::Bool(p.row_grand));
  o.Set("col_grand", MV::Bool(p.col_grand));
  o.Set("layout", MV::Int(p.layout));
  o.Set("subtotals", MV::Int(p.subtotals));
  o.Set("style", MV::Str(p.style));
  o.Set("fields", StrList(p.fields));
  {
    // PivotTable Options and Style Options
    MV opt = MV::Map();
    opt.Set("merge_labels", MV::Bool(p.merge_labels));
    opt.Set("indent", MV::Int(p.indent));
    opt.Set("show_error", MV::Bool(p.show_error));
    opt.Set("error_string", MV::Str(p.error_string));
    opt.Set("show_null", MV::Bool(p.show_null));
    opt.Set("null_string", MV::Str(p.null_string));
    opt.Set("autofit", MV::Bool(p.autofit));
    opt.Set("preserve_formatting", MV::Bool(p.preserve_formatting));
    opt.Set("show_buttons", MV::Bool(p.show_buttons));
    opt.Set("field_captions", MV::Bool(p.field_captions));
    opt.Set("refresh_on_open", MV::Bool(p.refresh_on_open));
    opt.Set("enable_drilldown", MV::Bool(p.enable_drilldown));
    opt.Set("field_list_order", MV::Int(p.field_list_order));
    opt.Set("custom_lists", MV::Bool(p.custom_lists));
    opt.Set("chart_buttons", MV::Bool(p.chart_buttons));
    opt.Set("style_row_headers", MV::Bool(p.style_row_headers));
    opt.Set("style_col_headers", MV::Bool(p.style_col_headers));
    opt.Set("style_banded_rows", MV::Bool(p.style_banded_rows));
    opt.Set("style_banded_cols", MV::Bool(p.style_banded_cols));
    opt.Set("general_values", MV::Bool(p.general_values));
    opt.Set("enable_selection", MV::Bool(p.enable_selection));
    opt.Set("grand_name", MV::Str(p.grand_name));
    opt.Set("refreshed_at", MV::Double(p.refreshed_at));
    opt.Set("refreshed_by", MV::Str(p.refreshed_by));
    o.Set("options", std::move(opt));
  }
  {
    MV gs = MV::Array();
    for (const auto& g : p.groups) {
      MV m = MV::Map();
      m.Set("field", MV::Int(g.field));
      m.Set("by", MV::Int(static_cast<int>(g.by)));
      m.Set("start", MV::Double(g.start));
      m.Set("end", MV::Double(g.end));
      m.Set("interval", MV::Double(g.interval));
      m.Set("periods", MV::Int(g.periods));
      m.Set("days", MV::Int(g.days));
      m.Set("auto_start", MV::Bool(g.auto_start));
      m.Set("auto_end", MV::Bool(g.auto_end));
      m.Set("name", MV::Str(g.name));
      MV named = MV::Array();
      for (const auto& [gname, items] : g.named) {
        MV e = MV::Array();
        e.Push(MV::Str(gname));
        e.Push(StrList(items));
        named.Push(std::move(e));
      }
      m.Set("named", std::move(named));
      m.Set("placed", MV::Bool(g.placed));
      gs.Push(std::move(m));
    }
    o.Set("groups", std::move(gs));
    MV cs = MV::Array();
    for (const auto& c : p.calc_fields) {
      MV m = MV::Map();
      m.Set("name", MV::Str(c.name));
      m.Set("formula", MV::Str(c.formula));
      cs.Push(std::move(m));
    }
    o.Set("calc_fields", std::move(cs));
    MV sfs = MV::Array();
    for (const auto& f : p.slicer_filters) {
      MV m = MV::Map();
      m.Set("field", MV::Int(f.field));
      m.Set("hidden", StrList(f.hidden));
      sfs.Push(std::move(m));
    }
    o.Set("slicer_filters", std::move(sfs));
    MV cis = MV::Array();
    for (const auto& c : p.calc_items) {
      MV m = MV::Map();
      m.Set("field", MV::Int(c.field));
      m.Set("name", MV::Str(c.name));
      m.Set("formula", MV::Str(c.formula));
      cis.Push(std::move(m));
    }
    o.Set("calc_items", std::move(cis));
    MV dfs = MV::Array();
    for (const auto& f : p.date_filters) {
      MV m = MV::Map();
      m.Set("field", MV::Int(f.field));
      m.Set("from", MV::Double(f.from));
      m.Set("to", MV::Double(f.to));
      dfs.Push(std::move(m));
    }
    o.Set("date_filters", std::move(dfs));
  }
  {
    // captions of the data fields and the report's bands (for TableRange1 / DataBodyRange)
    engine::PivotCache cache;
    std::string err;
    MV caps = MV::Array();
    int64_t page_rows = 0, header_rows = 0, label_cols = 0, body_cols = 0;
    if (engine::ReadPivotSource(d.wb(), p, &cache, &err)) {
      for (size_t i = 0; i < p.data.size(); ++i) caps.Push(MV::Str(engine::PivotDataCaption(p, cache, i)));
      o.Set("cube_fields", StrList(cache.cube_names));
      const engine::PivotReport rep = engine::ComputePivot(p, cache);
      page_rows = rep.page_rows;
      header_rows = rep.header_rows;
      label_cols = rep.label_cols;
      // the body's width: the row labels and the value columns, without the column the filter area alone needs (a
      // report of one value under a filter is one column wide, its TableRange1 A3:A4 while TableRange2 is A1:B4,
      // Excel 16 measured; gaps_fuzz seed 1019)
      for (uint32_t c = 0; c < rep.cols; ++c) {
        if (c < rep.label_cols || (c < rep.col_kind.size() && rep.col_kind[c] >= 0)) body_cols = c + 1;
      }
    }
    o.Set("captions", std::move(caps));
    o.Set("page_rows", MV::Int(page_rows));
    o.Set("header_rows", MV::Int(header_rows));
    o.Set("label_cols", MV::Int(label_cols));
    o.Set("body_cols", MV::Int(body_cols));
  }
  if (p.has_output) {
    MV r = MV::Array();
    for (uint32_t v : {p.output.r0, p.output.c0, p.output.r1, p.output.c1}) r.Push(MV::Int(v));
    o.Set("output", std::move(r));
  } else {
    o.Set("output", MV());
  }
  return o;
}

void PivotFromMV(model::PivotTable& p, const MV& m) {
  if (m.Has("name")) p.name = m.GetStr("name");
  if (m.Has("src_sheet")) p.src_sheet = static_cast<uint32_t>(m.GetInt("src_sheet"));
  if (const MV* r = m.Get("src"); r && r->a.size() == 4) {
    p.src = Rect{static_cast<uint32_t>(r->a[0].i), static_cast<uint32_t>(r->a[1].i), static_cast<uint32_t>(r->a[2].i), static_cast<uint32_t>(r->a[3].i)};
  }
  if (m.Get("src_table")) p.src_table = m.GetStr("src_table");
  if (m.Has("from_model")) p.from_model = m.GetBool("from_model");
  if (m.Has("r0")) p.r0 = static_cast<uint32_t>(m.GetInt("r0"));
  if (m.Has("c0")) p.c0 = static_cast<uint32_t>(m.GetInt("c0"));
  if (const MV* v = m.Get("rows")) p.rows = AxisFromMV(*v);
  if (const MV* v = m.Get("cols")) p.cols = AxisFromMV(*v);
  if (const MV* v = m.Get("pages")) p.pages = AxisFromMV(*v);
  if (const MV* v = m.Get("data")) {
    p.data.clear();
    for (const auto& e : v->a) {
      model::PivotDataField df;
      df.field = static_cast<int>(e.GetInt("field"));
      df.func = static_cast<model::PivotFunc>(std::clamp<int64_t>(e.GetInt("func"), 0, model::kPivotFuncs - 1));
      df.show_as = static_cast<model::PivotShowAs>(std::clamp<int64_t>(e.GetInt("show_as"), 0, model::kPivotShowAs - 1));
      df.caption = e.GetStr("caption");
      df.number_format = e.GetStr("number_format");
      df.base_field = static_cast<int>(e.GetInt("base_field", -1));
      df.base_item = e.GetStr("base_item");
      p.data.push_back(std::move(df));
    }
  }
  if (m.Has("values_on_rows")) p.values_on_rows = m.GetBool("values_on_rows");
  if (m.Has("row_grand")) p.row_grand = m.GetBool("row_grand");
  if (m.Has("col_grand")) p.col_grand = m.GetBool("col_grand");
  if (m.Has("layout")) p.layout = static_cast<int>(std::clamp<int64_t>(m.GetInt("layout"), 0, 2));
  if (m.Has("subtotals")) p.subtotals = static_cast<int>(std::clamp<int64_t>(m.GetInt("subtotals"), 0, 2));
  if (m.Get("style")) p.style = m.GetStr("style");
  if (const MV* v = m.Get("groups")) {
    p.groups.clear();
    for (const auto& e : v->a) {
      model::PivotGroup g;
      g.field = static_cast<int>(e.GetInt("field"));
      g.by = static_cast<model::PivotGroupBy>(std::clamp<int64_t>(e.GetInt("by"), 0, static_cast<int64_t>(model::PivotGroupBy::Items)));
      g.start = e.GetDouble("start");
      g.end = e.GetDouble("end");
      g.interval = e.GetDouble("interval", 10);
      g.periods = static_cast<uint32_t>(e.GetInt("periods"));
      g.days = static_cast<int>(std::max<int64_t>(1, e.GetInt("days", 1)));
      g.auto_start = e.GetBool("auto_start");
      g.auto_end = e.GetBool("auto_end");
      g.name = e.GetStr("name");
      if (const MV* nv = e.Get("named")) {
        for (const auto& x : nv->a) {
          if (x.a.size() >= 2) g.named.emplace_back(x.a[0].s, ToStrList(x.a[1]));
        }
      }
      g.placed = e.GetBool("placed");
      p.groups.push_back(g);
    }
  }
  if (const MV* v = m.Get("calc_fields")) {
    p.calc_fields.clear();
    for (const auto& e : v->a) p.calc_fields.push_back(model::PivotCalcField{e.GetStr("name"), e.GetStr("formula")});
  }
  if (const MV* v = m.Get("calc_items")) {
    p.calc_items.clear();
    for (const auto& e : v->a) p.calc_items.push_back(model::PivotCalcItem{static_cast<int>(e.GetInt("field")), e.GetStr("name"), e.GetStr("formula")});
  }
  if (const MV* o = m.Get("options"); o && o->t == MV::T::Map) {
    p.merge_labels = o->GetBool("merge_labels", p.merge_labels);
    p.indent = static_cast<int>(std::clamp<int64_t>(o->GetInt("indent", p.indent), 0, 15));
    p.show_error = o->GetBool("show_error", p.show_error);
    if (o->Has("error_string")) p.error_string = o->GetStr("error_string");
    p.show_null = o->GetBool("show_null", p.show_null);
    if (o->Has("null_string")) p.null_string = o->GetStr("null_string");
    p.autofit = o->GetBool("autofit", p.autofit);
    p.preserve_formatting = o->GetBool("preserve_formatting", p.preserve_formatting);
    p.show_buttons = o->GetBool("show_buttons", p.show_buttons);
    p.field_captions = o->GetBool("field_captions", p.field_captions);
    p.refresh_on_open = o->GetBool("refresh_on_open", p.refresh_on_open);
    p.enable_drilldown = o->GetBool("enable_drilldown", p.enable_drilldown);
    p.field_list_order = static_cast<int>(o->GetInt("field_list_order", p.field_list_order));
    p.custom_lists = o->GetBool("custom_lists", p.custom_lists);
    p.chart_buttons = o->GetBool("chart_buttons", p.chart_buttons);
    p.style_row_headers = o->GetBool("style_row_headers", p.style_row_headers);
    p.style_col_headers = o->GetBool("style_col_headers", p.style_col_headers);
    p.style_banded_rows = o->GetBool("style_banded_rows", p.style_banded_rows);
    p.style_banded_cols = o->GetBool("style_banded_cols", p.style_banded_cols);
    p.general_values = o->GetBool("general_values", p.general_values);
    p.enable_selection = o->GetBool("enable_selection", p.enable_selection);
    if (o->Has("grand_name")) p.grand_name = o->GetStr("grand_name");
  }
  if (const MV* v = m.Get("date_filters")) {
    p.date_filters.clear();
    for (const auto& e : v->a) p.date_filters.push_back(model::PivotDateFilter{static_cast<int>(e.GetInt("field")), e.GetDouble("from"), e.GetDouble("to")});
  }
}

const model::PivotTable* FindPivotOn(const Document& d, uint32_t sheet, const std::string& name) {
  const model::Sheet* s = d.S(sheet);
  if (!s) return nullptr;
  for (const auto& p : *s->pivots) {
    if (LowerStr(p.name) == LowerStr(name)) return &p;
  }
  return nullptr;
}

std::vector<uint32_t> SheetIds(const MV& p, uint32_t sheet) {
  std::vector<uint32_t> out;
  if (const MV* v = p.Get("sheets")) {
    for (const auto& x : v->a) out.push_back(static_cast<uint32_t>(x.i));
  }
  if (out.empty()) out.push_back(sheet);
  return out;
}

}  // namespace

MV DispatchRules(engine::Engine& eng, const std::string& m, const MV& p, std::string* ec, std::string* em, bool* handled);

MV PythonBridge::DispatchObjects(const std::string& m, const MV& p, std::string* ec, std::string* em, bool* handled) {
  if (m.rfind("cf.", 0) == 0 || m.rfind("dv.", 0) == 0 || m.rfind("af.", 0) == 0 || m.rfind("note.", 0) == 0 || m.rfind("link.", 0) == 0 ||
      m.rfind("comment.", 0) == 0)
    return DispatchRules(*s_.engine, m, p, ec, em, handled);
  *handled = true;
  auto& eng = *s_.engine;
  const uint32_t sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));

  // ---- Controls (OLEObjects) ----
  if (m == "obj.list") {
    return eng.Call([&](Document& d) {
      MV x = MV::Map();
      MV ctls = MV::Array(), charts = MV::Array(), shapes = MV::Array();
      if (const model::Sheet* s = d.S(sheet)) {
        for (const auto& c : *s->controls) ctls.Push(ControlToMV(sheet, c));
        for (const auto& c : *s->charts) charts.Push(ChartToMV(sheet, c));
        for (const auto& c : *s->shapes) shapes.Push(ShapeToMV(sheet, c));
      }
      x.Set("controls", std::move(ctls));
      x.Set("charts", std::move(charts));
      x.Set("shapes", std::move(shapes));
      return x;
    });
  }
  if (m == "ctl.add") {
    std::string err;
    MV r = eng.Call([&](Document& d) {
      if (!d.S(sheet)) {
        err = "Sheet not found";
        return MV();
      }
      Control c;
      ApplyControl(c, p);
      if (!c.name.empty() && FindCtl(*d.S(sheet), c.name)) {
        err = "Duplicate name:" + c.name;
        return MV();
      }
      c.created_by_python = true;
      const uint32_t id = d.AddControl(sheet, c);
      // a value given with a linked cell goes into the cell (like setting Value afterwards)
      if (p.Has("value") && !c.linked_cell.empty()) {
        for (const auto& x : *d.S(sheet)->controls) {
          if (x.id == id) ApplyControlValue(d, sheet, id, x.value, x.list_index);
        }
      }
      d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->controls) {
        if (x.id == id) return ControlToMV(sheet, x);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "ctl.get" || m == "ctl.set" || m == "ctl.delete") {
    const std::string name = p.GetStr("name");
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      const Control* c = s ? FindCtl(*s, name) : nullptr;
      if (!c) {
        err = "Control not found:" + name;
        return MV();
      }
      const uint32_t id = c->id;
      if (m == "ctl.get") return ControlToMV(sheet, *c);
      if (m == "ctl.delete") {
        d.DeleteControl(sheet, id);
        d.ForeignWrite();
        return MV::Bool(true);
      }
      const MV* props = p.Get("props");
      std::string linked, value;
      bool write_linked = false;
      d.UpdateControl(sheet, id, [&](Control& x) {
        if (props) ApplyControl(x, *props);
        if (props && (props->Has("value") || props->Has("list_index")) && (!x.linked_cell.empty() || x.form)) {
          write_linked = true;
          linked = x.linked_cell;
          value = x.value;
        }
      });
      if (write_linked) {
        int index = -1;
        for (const auto& x : *d.S(sheet)->controls) {
          if (x.id == id) index = x.list_index;
        }
        ApplyControlValue(d, sheet, id, value, index);
      }
      d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->controls) {
        if (x.id == id) return ControlToMV(sheet, x);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, "E_NOT_FOUND", err);
    return r;
  }

  // ---- Charts (ChartObjects) ----
  if (m == "chart.add") {
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) {
        err = "Sheet not found";
        return MV();
      }
      ChartObj c;
      ApplyChart(c, p);
      if (!c.name.empty() && FindChart(*s, c.name)) {
        err = "Duplicate name:" + c.name;
        return MV();
      }
      const std::string src = p.GetStr("source");
      if (!src.empty()) {
        uint32_t sid = 0;
        std::vector<Rect> areas;
        if (!ResolveAreasText(d.wb(), sheet, src, &sid, &areas) || !d.S(sid)) {
          err = "Invalid data range:" + src;
          return MV();
        }
        const bool keep_title = p.Has("title");
        const std::string title = c.title;
        BuildSeriesFromAreas(d.wb(), *d.S(sid), areas, c.type, &c, p.GetInt("plot_by", 2) == 1);
        if (c.series.size() != 1 && c.title.empty()) c.has_title = false;
        if (keep_title) c.title = title, c.has_title = !title.empty();
      }
      c.created_by_python = true;
      const uint32_t id = d.AddChart(sheet, c);
      d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->charts) {
        if (x.id == id) return ChartToMV(sheet, x);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  // Chart.Location: to a new chart sheet (to = 0, named new_name) or onto a worksheet
  if (m == "chart.move") {
    const std::string name = p.GetStr("name"), new_name = p.GetStr("new_name");
    const uint32_t to = static_cast<uint32_t>(p.GetInt("to", 0));
    const double left = p.GetDouble("left", 36), top = p.GetDouble("top", 18);
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      const ChartObj* c = s ? FindChart(*s, name) : nullptr;
      if (!c) {
        err = "Chart not found:" + name;
        return MV();
      }
      uint32_t nid = 0;
      const uint32_t dest = d.MoveChart(sheet, c->id, to, new_name, left, top, &nid);
      d.ForeignWrite();
      if (!dest) {
        err = to ? "The chart can't be moved to that sheet" : "That sheet name can't be used: " + new_name;
        return MV();
      }
      for (const auto& x : *d.S(dest)->charts) {
        if (x.id == nid) return ChartToMV(dest, x);
      }
      return MV();
    });
    if (!err.empty()) {
      SetErr(ec, em, "E_APP", err);
    } else if (to == 0) {  // like Excel: the new chart sheet becomes the active sheet
      const uint32_t dest = static_cast<uint32_t>(r.GetInt("sheet"));
      eng.Sync();
      OnUi([dest](AppState& s) {
        s.snap = s.engine->Current();
        ActivateSheet(s, dest);
        return MV::Bool(true);
      });
    }
    return r;
  }
  // Charts.Add: a chart sheet with the selected data (or a source range)
  if (m == "chartsheet.add") {
    const std::string name = p.GetStr("name"), source = p.GetStr("source");
    const int64_t index = p.GetInt("index", -1);
    const int type = static_cast<int>(p.GetInt("type", static_cast<int>(ChartType::ColumnClustered)));
    eng.Sync();
    MV r = OnUi([&](AppState& s) {
      ChartObj ch;
      ch.type = static_cast<ChartType>(std::clamp(type, 0, model::kChartTypeCount - 1));
      uint32_t sid = 0;
      Rect rr;
      if (!source.empty()) {
        if (ResolveRangeText(s.snap->wb, s.focus_sheet, source, &sid, &rr) && s.snap->Sheet(sid)) BuildSeriesFromRange(s.snap->wb, *s.snap->Sheet(sid), rr, ch.type, &ch, false);
      } else if (const model::Sheet* sh = s.FocusSheet(); sh && !sh->chart_sheet) {
        BuildSeriesFromRange(s.snap->wb, *sh, ExpandToRegion(*sh, SelRect(s)), ch.type, &ch, false);
      }
      const int32_t idx = index >= 0 ? static_cast<int32_t>(index) : s.snap->wb.IndexOfId(s.focus_sheet);
      const uint32_t id = s.engine->Call([&](Document& d) {
        const uint32_t v = d.AddChartSheet(name, idx, ch);
        d.ClearUndo();
        return v;
      });
      s.engine->Sync();
      s.snap = s.engine->Current();
      if (id) ActivateSheet(s, id);
      return MV::Int(id);
    });
    return r;
  }
  // ChartTitle.Select, Legend.Select...: select a chart element in the window (the Format pane follows)
  if (m == "chart.select") {
    const std::string name = p.GetStr("name");
    const int elem = static_cast<int>(p.GetInt("elem", 1)), series = static_cast<int>(p.GetInt("series", 0)), point = static_cast<int>(p.GetInt("point", -1));
    const bool format = p.GetBool("format");
    eng.Sync();
    return OnUi([&](AppState& s) {
      const model::Sheet* sh = s.snap->Sheet(sheet);
      const ChartObj* c = sh ? FindChart(*sh, name) : nullptr;
      if (!c) return MV::Bool(false);
      SelectChartElement(s, sheet, c->id, elem, series, format, point);
      return MV::Bool(true);
    });
  }
  if (m == "chart.get" || m == "chart.set" || m == "chart.delete" || m == "chart.set_source" || m == "chart.series_set" || m == "chart.series_delete" ||
      m == "chart.series_data") {
    const std::string name = p.GetStr("name");
    std::string err, err_code = "E_NOT_FOUND";
    MV r = eng.Call([&](Document& d) {
      if (m == "chart.get" || m == "chart.series_data") d.SettleForRead();  // titles and values linked to fresh formulas
      const model::Sheet* s = d.S(sheet);
      const ChartObj* c = s ? FindChart(*s, name) : nullptr;
      if (!c) {
        err = "Chart not found:" + name;
        return MV();
      }
      const uint32_t id = c->id;
      if (m == "chart.get") {
        MV x = ChartToMV(sheet, *c);
        auto shown = [&](const std::string& t, const char* placeholder) -> std::string {
          if (t.empty()) return placeholder;
          if (t[0] != '=') return t;
          uint32_t sid = 0;
          Rect r;
          const model::Sheet* ts = ResolveRangeText(d.wb(), sheet, t, &sid, &r) ? d.S(sid) : nullptr;
          if (!ts) return t;
          return engine::DisplayTextOf(ts->GetValue(r.r0, r.c0), d.wb().styles->NumberFormat(d.wb().styles->Get(ts->GetStyle(r.r0, r.c0))));
        };
        x.Set("title_text", MV::Str(shown(c->title, "Chart Title")));
        x.Set("cat_title_text", MV::Str(shown(c->cat_axis.title, "Axis Title")));
        x.Set("val_title_text", MV::Str(shown(c->val_axis.title, "Axis Title")));
        x.Set("val2_title_text", MV::Str(shown(c->val_axis2.title, "Axis Title")));
        return x;
      }
      if (m == "chart.delete") {
        d.DeleteChart(sheet, id);
        d.ForeignWrite();
        return MV::Bool(true);
      }
      const int64_t index = p.GetInt("index", -1);
      bool ok = true;
      if (m == "chart.set") {
        if (const MV* props = p.Get("props")) {
          d.UpdateChart(sheet, id, [&](ChartObj& x) {
            const ChartType old = x.type;
            ApplyChart(x, *props);
            if (x.type != old) {
              const ChartType nt = x.type;
              x.type = old;
              ChangeChartType(d.wb(), x, nt);
            }
          });
        }
      } else if (m == "chart.set_source") {
        const std::string src = p.GetStr("source");
        uint32_t sid = 0;
        std::vector<Rect> areas;
        if (!ResolveAreasText(d.wb(), sheet, src, &sid, &areas) || !d.S(sid)) {
          err = "Invalid data range:" + src;
          err_code = "E_APP";
          return MV();
        }
        // a range in a PivotTable report: the chart becomes its PivotChart
        if (const model::PivotTable* pt = d.PivotAt(sid, areas[0].r0, areas[0].c0)) {
          const std::string pname = pt->name;
          d.UpdateChart(sheet, id, [&](ChartObj& x) { x.pivot = pname; });
          d.UpdatePivotCharts();
          d.ForeignWrite();
          for (const auto& x : *d.S(sheet)->charts) {
            if (x.id == id) return ChartToMV(sheet, x);
          }
          return MV();
        }
        d.UpdateChart(sheet, id, [&](ChartObj& x) { x.pivot.clear(); });
        ChartObj tmp = *c;
        BuildSeriesFromAreas(d.wb(), *d.S(sid), areas, tmp.type, &tmp, p.GetInt("plot_by", 2) == 1);
        if (tmp.series.size() != 1 && tmp.title.empty()) tmp.has_title = false;
        KeepSeriesFormat(c->series, &tmp.series);
        d.UpdateChart(sheet, id, [&](ChartObj& x) {
          x.series = tmp.series;
          x.source = tmp.source;
          x.source_by_rows = tmp.source_by_rows;
          if (!x.has_title || x.title.empty()) {
            x.has_title = tmp.has_title;
            x.title = tmp.title;
          }
          x.has_legend = tmp.has_legend;
        });
      } else if (m == "chart.series_set") {
        d.UpdateChart(sheet, id, [&](ChartObj& x) {
          if (const MV* props = p.Get("props")) ApplySeries(SeriesAt(x, index), *props);
        });
      } else if (m == "chart.series_delete") {
        d.UpdateChart(sheet, id, [&](ChartObj& x) {
          if (index >= 0 && static_cast<size_t>(index) < x.series.size()) x.series.erase(x.series.begin() + index);
          else ok = false;
        });
      } else if (m == "chart.series_data") {
        // Series.SetData / AppendData [YC]: data goes straight into the chart (not through cells)
        std::vector<double> xs = ToDoubles(p.Get("x")), ys = ToDoubles(p.Get("y"));
        const bool append = p.GetBool("append");
        d.UpdateChart(sheet, id, [&](ChartObj& x) {
          const bool fresh = index < 0 || static_cast<size_t>(index) >= x.series.size();
          model::ChartSeries& se = SeriesAt(x, index);
          if (fresh) se.name = p.GetStr("series_name", "Series" + std::to_string(x.series.size()));
          if (p.Has("capacity")) se.capacity = static_cast<uint32_t>(std::max<int64_t>(1, p.GetInt("capacity")));
          auto nd = std::make_shared<model::SeriesData>();
          if (append && se.data) {
            nd->x = se.data->x;
            nd->y = se.data->y;
          }
          if (!xs.empty()) {
            if (nd->x.empty() && !nd->y.empty()) {
              for (size_t i = 0; i < nd->y.size(); ++i) nd->x.push_back(static_cast<double>(i + 1));
            }
            nd->x.insert(nd->x.end(), xs.begin(), xs.end());
          } else if (!nd->x.empty()) {
            double last = nd->x.back();
            for (size_t i = 0; i < ys.size(); ++i) nd->x.push_back(++last);
          }
          nd->y.insert(nd->y.end(), ys.begin(), ys.end());
          // ring buffer: drop the oldest points when over capacity
          if (nd->y.size() > se.capacity) {
            const size_t drop = nd->y.size() - se.capacity;
            nd->y.erase(nd->y.begin(), nd->y.begin() + static_cast<std::ptrdiff_t>(drop));
            if (nd->x.size() >= drop) nd->x.erase(nd->x.begin(), nd->x.begin() + static_cast<std::ptrdiff_t>(drop));
          }
          nd->version = (se.data ? se.data->version : 0) + 1;
          se.data = std::move(nd);
          se.python_data = true;
          if (const MV* nm = p.Get("series_name")) se.name = nm->s;
        });
      }
      if (!ok) {
        err = "Series index out of range";
        err_code = "E_RANGE";
        return MV();
      }
      if (m != "chart.series_data") d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->charts) {
        if (x.id == id) return ChartToMV(sheet, x);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, err_code.c_str(), err);
    return r;
  }

  // ---- Pictures, shapes, text boxes (Shapes.AddPicture / AddShape / AddTextbox / AddLine) ----
  if (m == "shape.add") {
    std::string err, code = "E_APP";
    model::ShapeObj sh;
    ApplyShape(sh, p);
    if (sh.kind == model::ShapeKind::Picture) {
      sh.image = p.GetStr("image");
      int w = 0, h = 0;
      if (sh.image.empty() || !ImageSize(sh.image, &w, &h)) {
        SetErr(ec, em, "E_APP", "The picture can't be read");
        return MV();
      }
      // -1 = the picture's own size (pixels x 3/4); one given side keeps the aspect ratio
      const double ow = w * 0.75, oh = h * 0.75;
      const bool has_w = p.GetDouble("width", -1) >= 0, has_h = p.GetDouble("height", -1) >= 0;
      if (!has_w && !has_h) sh.width = ow, sh.height = oh;
      else if (!has_w) sh.width = oh > 0 ? sh.height * ow / oh : ow;
      else if (!has_h) sh.height = ow > 0 ? sh.width * oh / ow : oh;
    }
    if (sh.kind == model::ShapeKind::TextBox && !p.Has("fill_color")) {
      sh.fill_color = 0xFFFFFFFF;
      sh.line_color = 0xFF000000;
      sh.font_color = 0xFF000000;
      if (!p.Has("halign")) sh.halign = 0;
      if (!p.Has("valign")) sh.valign = 0;
    }
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) {
        err = "Sheet not found";
        return MV();
      }
      if (!sh.name.empty() && FindShapeByName(*s, sh.name)) {
        err = "Duplicate name:" + sh.name;
        return MV();
      }
      const uint32_t id = d.AddShape(sheet, sh);
      d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->shapes) {
        if (x.id == id) return ShapeToMV(sheet, x);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, code.c_str(), err);
    return r;
  }
  if (m == "shape.select") {
    // Shape.Select / ShapeRange.Select [YC]: names (several: one selection), Replace=False adds to the selection
    std::vector<std::string> names;
    if (const MV* a = p.Get("names")) {
      for (const MV& n : a->a) names.push_back(n.s);
    } else {
      names.push_back(p.GetStr("name"));
    }
    const bool replace = p.GetBool("replace", true);
    return OnUi([sheet, names, replace](AppState& s) {
      const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
      if (!sh) return MV::Bool(false);
      ActivateSheet(s, sheet);
      bool first = replace, any = false;
      for (const std::string& n : names) {
        int kind = 0;
        uint32_t id = 0;
        if (const model::ShapeObj* x = FindShapeByName(*sh, n)) kind = 3, id = x->id;
        for (const auto& c : *sh->charts) {
          if (!kind && LowerStr(c.name) == LowerStr(n)) kind = 2, id = c.id;
        }
        for (const auto& c : *sh->controls) {
          if (!kind && LowerStr(c.name) == LowerStr(n)) kind = 1, id = c.id;
        }
        if (!kind) continue;
        SelectObject(s, sheet, kind, id, first);
        first = false;
        any = true;
      }
      return MV::Bool(any);
    });
  }
  if (m == "shape.paste") {
    // Worksheet.Paste: the shapes copied last (Shape.Copy), when nothing was copied after them
    return OnUi([sheet](AppState& s) {
      if (SlicerClipboardPending()) {
        // a copied slicer: at the active cell (Paste's own way)
        ActivateSheet(s, sheet);
        ui::Commands().Execute("Paste");
        return MV::Bool(true);
      }
      if (!ObjectClipboardPending()) return MV::Bool(false);
      ActivateSheet(s, sheet);
      ui::Commands().Execute("ObjectPaste");
      return MV::Bool(true);
    });
  }
  if (m == "slicer.copy") {
    // Slicer.Copy / Slicer.Cut: Paste (Worksheet.Paste, Ctrl+V) pastes it
    const std::string name = p.GetStr("name");
    const bool cut = p.GetBool("cut");
    return OnUi([sheet, name, cut](AppState& s) { return MV::Bool(CopySlicerNamed(s, sheet, name, cut)); });
  }
  if (m == "shape.selection") {
    // the selected objects' names, the primary first
    return OnUi([](AppState& s) {
      MV a = MV::Array();
      const auto ids = SelectedObjectIds();
      const model::Sheet* sh = s.snap ? s.snap->Sheet(s.focus_sheet) : nullptr;
      if (!sh) return a;
      for (const auto& [kind, id] : ids) {
        if (kind == 3) {
          for (const auto& x : *sh->shapes) {
            if (x.id == id) a.Push(MV::Str(x.name));
          }
        } else if (kind == 2) {
          for (const auto& x : *sh->charts) {
            if (x.id == id) a.Push(MV::Str(x.name));
          }
        } else {
          for (const auto& x : *sh->controls) {
            if (x.id == id) a.Push(MV::Str(x.name));
          }
        }
      }
      return a;
    });
  }
  if (m == "shape.get" || m == "shape.set" || m == "shape.delete") {
    const std::string name = p.GetStr("name");
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      const model::ShapeObj* x = s ? FindShapeByName(*s, name) : nullptr;
      if (!x) {
        err = "Shape not found:" + name;
        return MV();
      }
      const uint32_t id = x->id;
      if (m == "shape.get") return ShapeToMV(sheet, *x);
      if (m == "shape.delete") {
        d.DeleteShape(sheet, id);
        d.ForeignWrite();
        return MV::Bool(true);
      }
      if (const MV* props = p.Get("props")) d.UpdateShape(sheet, id, [&](model::ShapeObj& o) { ApplyShape(o, *props); });
      d.ForeignWrite();
      for (const auto& o : *d.S(sheet)->shapes) {
        if (o.id == id) return ShapeToMV(sheet, o);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, "E_NOT_FOUND", err);
    return r;
  }

  // ---- Sparklines (Range.SparklineGroups) ----
  if (m == "spark.list") {
    // the groups with a sparkline in "range" (all groups when it is empty)
    const std::string range = p.GetStr("range");
    return eng.Call([&](Document& d) {
      MV a = MV::Array();
      const model::Sheet* s = d.S(sheet);
      if (!s) return a;
      uint32_t sid = sheet;
      Rect rr{0, 0, core::kRowEnd - 1, core::kColEnd - 1};
      if (!range.empty() && !ResolveRangeText(d.wb(), sheet, range, &sid, &rr)) return a;
      for (const auto& g : *s->sparklines) {
        const bool hit = std::any_of(g.items.begin(), g.items.end(), [&](const model::Sparkline& it) { return rr.Contains(it.r, it.c); });
        if (hit) a.Push(SparkToMV(d.wb(), g));
      }
      return a;
    });
  }
  if (m == "spark.add") {
    std::string err;
    MV r = eng.Call([&](Document& d) {
      uint32_t ds = 0, ls = 0;
      Rect data{}, loc{};
      std::vector<model::Sparkline> items;
      if (!ResolveRangeText(d.wb(), sheet, p.GetStr("source"), &ds, &data) || !ResolveRangeText(d.wb(), sheet, p.GetStr("location"), &ls, &loc) ||
          ls != sheet) {
        err = "The reference isn't valid";
        return MV();
      }
      if (!engine::BuildSparklines(ds, data, loc, &items)) {
        err = "The data doesn't match the location";
        return MV();
      }
      model::SparklineGroup g;
      ApplySpark(g, p);
      g.items = std::move(items);
      const uint32_t id = d.AddSparklineGroup(sheet, g);
      d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->sparklines) {
        if (x.id == id) return SparkToMV(d.wb(), x);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "spark.set") {
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id"));
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const MV* props = p.Get("props");
      if (!d.UpdateSparklineGroup(sheet, id, [&](model::SparklineGroup& g) {
            if (props) ApplySpark(g, *props);
          })) {
        err = "Sparkline group not found";
        return MV();
      }
      d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->sparklines) {
        if (x.id == id) return SparkToMV(d.wb(), x);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, "E_NOT_FOUND", err);
    return r;
  }
  if (m == "spark.modify") {
    // SparklineGroup.Modify / ModifyLocation / ModifySourceData: the group laid out again from its location and data
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id"));
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      const model::SparklineGroup* g0 = nullptr;
      if (s) {
        for (const auto& x : *s->sparklines) {
          if (x.id == id) g0 = &x;
        }
      }
      if (!g0) {
        err = "Sparkline group not found";
        return MV();
      }
      std::string loc = p.GetStr("location"), src = p.GetStr("source");
      MV cur = SparkToMV(d.wb(), *g0);
      if (loc.empty() || src.empty()) {
        // the part not given stays: the group's bounding location / its sources joined
        const model::SparklineGroup& g = *g0;
        uint32_t r0 = UINT32_MAX, c0 = UINT32_MAX, r1 = 0, c1 = 0, sr0 = UINT32_MAX, sc0 = UINT32_MAX, sr1 = 0, sc1 = 0;
        for (const auto& it : g.items) {
          r0 = std::min(r0, it.r), c0 = std::min(c0, it.c), r1 = std::max(r1, it.r), c1 = std::max(c1, it.c);
          sr0 = std::min(sr0, it.src.r0), sc0 = std::min(sc0, it.src.c0), sr1 = std::max(sr1, it.src.r1), sc1 = std::max(sc1, it.src.c1);
        }
        if (loc.empty()) loc = core::RectA1(Rect{r0, c0, r1, c1});
        if (src.empty() && !g.items.empty()) {
          const model::Sheet* ss = d.wb().SheetById(g.items[0].src_sheet);
          src = (ss ? "'" + ss->name + "'!" : std::string()) + core::RectA1(Rect{sr0, sc0, sr1, sc1});
        }
      }
      uint32_t ds = 0, ls = 0;
      Rect data{}, lr{};
      std::vector<model::Sparkline> items;
      if (!ResolveRangeText(d.wb(), sheet, src, &ds, &data) || !ResolveRangeText(d.wb(), sheet, loc, &ls, &lr) || ls != sheet ||
          !engine::BuildSparklines(ds, data, lr, &items)) {
        err = "Location reference is not valid because the cells are not all in the same column or same row; or the data doesn't match the location";
        return MV();
      }
      d.UpdateSparklineGroup(sheet, id, [&](model::SparklineGroup& g) { g.items = items; });
      d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->sparklines) {
        if (x.id == id) return SparkToMV(d.wb(), x);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "spark.group" || m == "spark.ungroup") {
    uint32_t sid = 0;
    Rect rr{};
    const std::string range = p.GetStr("range");
    return eng.Call([&](Document& d) {
      if (!ResolveRangeText(d.wb(), sheet, range, &sid, &rr)) return MV::Int(0);
      std::vector<Rect> areas{rr};
      Rect lead = rr;
      uint32_t lsid = 0;
      if (p.Has("lead") && ResolveRangeText(d.wb(), sheet, p.GetStr("lead"), &lsid, &lead)) areas.push_back(lead);
      const uint32_t id = m == "spark.group" ? d.GroupSparklines(sheet, areas, lead.r0, lead.c0) : (d.UngroupSparklines(sheet, {rr}), 0u);
      d.ForeignWrite();
      return MV::Int(id);
    });
  }
  if (m == "spark.clear") {
    // "range": clear the sparklines in it; "id": delete that whole group
    const std::string range = p.GetStr("range");
    const int64_t gid = p.GetInt("id", -1);
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV::Bool(false);
      if (gid >= 0) {
        for (const auto& g : *s->sparklines) {
          if (g.id != gid) continue;
          for (const auto& it : std::vector<model::Sparkline>(g.items)) d.ClearSparklines(sheet, Rect::Cell(it.r, it.c));
          break;
        }
      } else {
        uint32_t sid = 0;
        Rect rr;
        if (ResolveRangeText(d.wb(), sheet, range, &sid, &rr)) d.ClearSparklines(sheet, rr);
      }
      d.ForeignWrite();
      return MV::Bool(true);
    });
  }

  // ---- Page setup, page breaks, printing, PDF ----
  if (m == "page.get" || m == "page.set") {
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* sh = d.S(sheet);
      if (!sh) {
        err = "Sheet not found";
        return MV();
      }
      if (m == "page.set") {
        model::PageSetup ps = *sh->page_setup;
        if (const MV* props = p.Get("props"); props && !ApplyPageSetup(d.wb(), sheet, ps, *props, &err)) return MV();
        d.UpdatePageSetup(sheet, [&ps](model::PageSetup& x) { x = ps; });
        d.ForeignWrite();
      }
      return PageSetupToMV(*d.S(sheet)->page_setup);
    });
    if (!err.empty()) SetErr(ec, em, "E_BAD_REF", err);
    else if (m == "page.set") OnUi([sheet](AppState& app) {
      ShowPageBreaks(app, sheet);
      return MV();
    });
    return r;
  }
  if (m == "page.breaks") {
    // rows: true for HPageBreaks; add / remove an index, or reset
    const bool rows = p.GetBool("rows", true);
    const int64_t add = p.GetInt("add", -1), remove = p.GetInt("remove", -1);
    const bool reset = p.GetBool("reset");
    MV r = eng.Call([&](Document& d) {
      if (!d.S(sheet)) return MV();
      if (add > 0 || remove >= 0 || reset) {
        d.UpdatePageSetup(sheet, [&](model::PageSetup& ps) {
          for (auto* v : {&ps.row_breaks, &ps.col_breaks}) {
            const bool mine = v == (rows ? &ps.row_breaks : &ps.col_breaks);
            if (reset) v->clear();
            if (!mine) continue;
            if (add > 0) v->push_back(static_cast<uint32_t>(add));
            if (remove >= 0) v->erase(std::remove(v->begin(), v->end(), static_cast<uint32_t>(remove)), v->end());
          }
        });
        d.ForeignWrite();
      }
      // every break: manual ones and the automatic ones of the current layout
      const model::Sheet& sh = *d.S(sheet);
      const engine::PrintLayout L = engine::LayoutPages(sh, *sh.page_setup);
      const auto& manual = rows ? sh.page_setup->row_breaks : sh.page_setup->col_breaks;
      MV a = MV::Array();
      const auto& segs = rows ? L.row_segs : L.col_segs;
      for (size_t i = 1; i < segs.size(); ++i) {
        MV b = MV::Array();
        b.Push(MV::Int(segs[i].first));
        b.Push(MV::Bool(std::find(manual.begin(), manual.end(), segs[i].first) != manual.end()));
        a.Push(std::move(b));
      }
      // manual breaks outside the printed range still count (Excel lists them too)
      for (uint32_t mb : manual) {
        bool listed = false;
        for (size_t i = 1; i < segs.size(); ++i) listed = listed || segs[i].first == mb;
        if (listed) continue;
        MV b = MV::Array();
        b.Push(MV::Int(mb));
        b.Push(MV::Bool(true));
        a.Push(std::move(b));
      }
      return a;
    });
    OnUi([sheet](AppState& app) {
      ShowPageBreaks(app, sheet);
      return MV();
    });
    return r;
  }
  if (m == "page.count") {
    return eng.Call([&](Document& d) {
      const model::Sheet* sh = d.S(sheet);
      if (!sh) return MV::Int(0);
      PrintSheetJob j;  // the pages printing makes (with Comments: At end of sheet, the pages of notes)
      j.ps = *sh->page_setup;
      j.layout = engine::LayoutPages(*sh, j.ps);
      AddNotesPages(*sh, &j);
      return MV::Int(j.layout.empty ? 0 : static_cast<int64_t>(j.layout.pages.size()));
    });
  }
  if (m == "print.pdf" || m == "print.out") {
    eng.Sync();
    std::string err;
    const std::vector<uint32_t> sheets = SheetIds(p, sheet);
    const std::wstring path = FromUtf8(p.GetStr("path"));
    const int from = static_cast<int>(p.GetInt("from", 1)), to = static_cast<int>(p.GetInt("to", 0));
    const bool ignore = p.GetBool("ignore_area");
    const bool has_area = p.Has("area");
    Rect area{};
    if (has_area) {
      const MV* a = p.Get("area");
      if (a->a.size() == 4) area = Rect{static_cast<uint32_t>(a->a[0].i), static_cast<uint32_t>(a->a[1].i), static_cast<uint32_t>(a->a[2].i), static_cast<uint32_t>(a->a[3].i)};
    }
    MV r = OnUi([&](AppState& s) {
      bool ok;
      if (m == "print.pdf") {
        ok = ExportPdf(s, path, sheets, ignore, from, to, &err, has_area ? &area : nullptr);
      } else {
        ok = PrintSheets(s, sheets, FromUtf8(p.GetStr("printer")), from, to, static_cast<int>(p.GetInt("copies", 1)), p.GetBool("collate", true), path, &err);
      }
      return MV::Bool(ok);
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "print.preview") {
    return OnUi([](AppState&) {
      ui::Commands().Execute("FilePrint");
      return MV::Bool(true);
    });
  }
  if (m == "print.printers") {
    return OnUi([](AppState&) {
      std::wstring def;
      MV o = MV::Map();
      MV a = MV::Array();
      for (const auto& n : PrinterNames(&def)) a.Push(MV::Str(ToUtf8(n)));
      o.Set("printers", std::move(a));
      o.Set("default", MV::Str(ToUtf8(def)));
      return o;
    });
  }

  // ---- PivotTables ----
  if (m == "pivot.list") {
    return eng.Call([&](Document& d) {
      MV a = MV::Array();
      if (const model::Sheet* s = d.S(sheet)) {
        for (const auto& pt : *s->pivots) a.Push(PivotToMV(d, sheet, pt));
      }
      return a;
    });
  }
  if (m == "pivot.add") {
    std::string err;
    MV r = eng.Call([&](Document& d) {
      model::PivotTable pt;
      const std::string src = p.GetStr("source"), dest = p.GetStr("dest");
      uint32_t sid = 0;
      Rect rr;
      if (model::FindTable(d.wb(), src, &sid)) {
        pt.src_table = src;
        pt.src_sheet = sid;
      } else if (ResolveRangeText(d.wb(), sheet, src, &sid, &rr)) {
        pt.src_sheet = sid;
        pt.src = rr;
      } else {
        err = "Invalid source: " + src;
        return MV();
      }
      uint32_t dsid = sheet;
      Rect dr{0, 0, 0, 0};
      if (!dest.empty() && !ResolveRangeText(d.wb(), sheet, dest, &dsid, &dr)) {
        err = "Invalid destination: " + dest;
        return MV();
      }
      pt.r0 = dr.r0;
      pt.c0 = dr.c0;
      pt.name = p.GetStr("name");
      if (!pt.name.empty() && d.FindPivot(pt.name)) {
        err = "A PivotTable with this name already exists: " + pt.name;
        return MV();
      }
      d.pivot_overwrite = p.GetBool("alerts") ? 1 : 0;
      const uint32_t id = d.AddPivot(dsid, pt, &err);
      d.pivot_overwrite = 0;
      if (!id) return MV();
      err.clear();
      d.ForeignWrite();
      return PivotToMV(d, dsid, *d.PivotById(dsid, id));
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "pivot.get" || m == "pivot.set" || m == "pivot.refresh" || m == "pivot.delete" || m == "pivot.items") {
    const std::string name = p.GetStr("name");
    std::string err, code = "E_NOT_FOUND";
    MV r = eng.Call([&](Document& d) {
      const model::PivotTable* pt = FindPivotOn(d, sheet, name);
      if (!pt) {
        err = "PivotTable not found: " + name;
        return MV();
      }
      const uint32_t id = pt->id;
      if (m == "pivot.get") return PivotToMV(d, sheet, *pt);
      if (m == "pivot.delete") {
        d.DeletePivot(sheet, id, p.GetBool("clear", true));
        d.ForeignWrite();
        return MV::Bool(true);
      }
      if (m == "pivot.items") {
        engine::PivotCache cache;
        const int field = static_cast<int>(p.GetInt("field", -1));
        if (!engine::ReadPivotSource(d.wb(), *pt, &cache, &err) || field < 0 || static_cast<size_t>(field) >= cache.items.size()) {
          code = "E_APP";
          if (err.empty()) err = "No such field";
          return MV();
        }
        if (p.GetBool("order")) {
          // the items in the order the report shows them (manual order, Position)
          const engine::PivotReport rep = engine::ComputePivot(*pt, cache);
          std::vector<std::string> order;
          std::unordered_set<std::string> seen;  // each item once: a find over the list so far was rows times items
          for (const auto* filters : {&rep.row_filter, &rep.col_filter}) {
            for (const auto& f : *filters) {
              for (const auto& [fld, item] : f) {
                if (fld != field) continue;
                const std::string& t = cache.items[static_cast<size_t>(fld)][static_cast<size_t>(item)];
                if (seen.insert(t).second) order.push_back(t);
              }
            }
          }
          for (const auto& t : cache.items[static_cast<size_t>(field)]) {
            if (seen.insert(t).second) order.push_back(t);
          }
          return StrList(order);
        }
        if (!p.GetBool("display")) return StrList(cache.items[static_cast<size_t>(field)]);
        MV a = MV::Array();
        for (size_t i = 0; i < cache.items[static_cast<size_t>(field)].size(); ++i) {
          MV e = MV::Array();
          e.Push(MV::Str(cache.items[static_cast<size_t>(field)][i]));
          e.Push(MV::Str(cache.display[static_cast<size_t>(field)][i]));
          a.Push(std::move(e));
        }
        return a;
      }
      bool ok = true;
      struct Alerts {
        Document& d;
        Alerts(Document& doc, bool on) : d(doc) { d.pivot_overwrite = on ? 1 : 0; }
        ~Alerts() { d.pivot_overwrite = 0; }
      } alerts(d, p.GetBool("alerts"));
      if (m == "pivot.refresh") {
        ok = d.RefreshPivot(sheet, id, &err);
      } else if (const MV* def = p.Get("def")) {
        // calculated fields: their formulas may only use the source's fields
        if (const MV* cfs = def->Get("calc_fields")) {
          engine::PivotCache cache;
          std::string why;
          if (engine::ReadPivotSource(d.wb(), *pt, &cache, &why)) {
            const std::vector<std::string> src(cache.names.begin(), cache.names.begin() + static_cast<std::ptrdiff_t>(cache.source_fields));
            for (const auto& e : cfs->a) {
              if (!engine::CheckPivotFormula(src, e.GetStr("formula"), &why)) {
                err = e.GetStr("name") + ": " + why;
                code = "E_APP";
                return MV();
              }
            }
          }
        }
        if (def->Has("name") && LowerStr(def->GetStr("name")) != LowerStr(name)) {
          if (d.FindPivot(def->GetStr("name"))) {
            err = "A PivotTable with this name already exists: " + def->GetStr("name");
            code = "E_APP";
            return MV();
          }
        }
        ok = d.UpdatePivot(sheet, id, [&](model::PivotTable& x) { PivotFromMV(x, *def); }, &err);
      }
      d.ForeignWrite();
      if (!ok) {
        code = "E_APP";
        if (err.empty()) err = "The PivotTable couldn't be updated";
        return MV();
      }
      const model::PivotTable* now = d.PivotById(sheet, id);
      return now ? PivotToMV(d, sheet, *now) : MV();
    });
    if (!err.empty() && r.IsNil()) SetErr(ec, em, code.c_str(), err);
    return r;
  }
  if (m == "pivot.group") {
    // -1: the cells aren't a PivotTable's labels; otherwise Group Selection (an error says why not)
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const MV* rv = p.Get("rect");
      if (!rv || rv->a.size() != 4) return MV::Int(-1);
      const Rect rect{static_cast<uint32_t>(rv->a[0].i), static_cast<uint32_t>(rv->a[1].i), static_cast<uint32_t>(rv->a[2].i), static_cast<uint32_t>(rv->a[3].i)};
      bool on = false;
      if (const model::Sheet* s = d.S(sheet)) {
        for (const auto& pt : *s->pivots) on = on || (pt.has_output && pt.output.Intersects(rect));
      }
      if (!on) return MV::Int(-1);
      const bool ok = PivotGroupCells(d, sheet, rect, &err);
      d.ForeignWrite();
      return MV::Int(ok ? 1 : 0);
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "pivot.cell_info") {
    // Range.PivotCell / PivotField / PivotItem: what a cell of a report is (an item's label, a value, a subtotal, a
    // grand total, a filter's name or value, a caption), with the (field, item) pairs of its row and column and its
    // data field; nil outside every report
    const uint32_t row = static_cast<uint32_t>(p.GetInt("row")), col = static_cast<uint32_t>(p.GetInt("col"));
    return eng.Call([&](Document& d) {
      const model::PivotTable* pt = d.PivotAt(sheet, row, col);
      if (!pt || !pt->has_output) return MV();
      engine::PivotCache cache;
      std::string why;
      if (!engine::ReadPivotSource(d.wb(), *pt, &cache, &why)) return MV();
      const engine::PivotReport rep = engine::ComputePivot(*pt, cache);
      const uint32_t rr = row - pt->output.r0, cc = col - pt->output.c0;
      if (rr >= rep.rows || cc >= rep.cols) return MV();
      const auto& cell = rep.at(rr, cc);
      auto item_text = [&](int f, int i) {
        return f >= 0 && static_cast<size_t>(f) < cache.display.size() && i >= 0 && static_cast<size_t>(i) < cache.display[static_cast<size_t>(f)].size()
                   ? cache.display[static_cast<size_t>(f)][static_cast<size_t>(i)] : std::string();
      };
      auto field_name = [&](int f) { return f >= 0 && static_cast<size_t>(f) < cache.names.size() ? cache.names[static_cast<size_t>(f)] : std::string(); };
      auto pairs = [&](const std::vector<std::pair<int, int>>& v) {
        MV a = MV::Array();
        for (const auto& [f, i] : v) {
          MV x = MV::Array();
          x.Push(MV::Str(field_name(f)));
          x.Push(MV::Str(item_text(f, i)));
          a.Push(std::move(x));
        }
        return a;
      };
      using K = engine::PivotCell;
      const char* kind = cell.kind == K::Data ? "value" : cell.kind == K::Subtotal ? "subtotal" : cell.kind == K::Grand ? "grand"
                         : cell.kind == K::RowLabel || cell.kind == K::ColLabel ? "item" : cell.kind == K::PageName ? "page_name"
                         : cell.kind == K::PageValue ? "page_value" : cell.kind == K::Header ? "header" : "blank";
      // a caption: a data field's (Sum of Sales) or a field's (tabular form), else Row / Column Labels (Excel 16 reads
      // xlPivotCellDataField, xlPivotCellPivotField and xlPivotCellBlankCell, measured)
      if (cell.kind == K::Header) {
        const std::string t = cell.v.IsString() ? std::string(cell.v.str()) : std::string();
        bool data_cap = false, field_cap = false;
        for (size_t k = 0; k < pt->data.size() && !t.empty(); ++k) data_cap = data_cap || engine::PivotDataCaption(*pt, cache, k) == t;
        for (const auto& e : pt->rows) field_cap = field_cap || (!t.empty() && field_name(e.field) == t);
        for (const auto& e : pt->cols) field_cap = field_cap || (!t.empty() && field_name(e.field) == t);
        if (data_cap) kind = "data_caption";
        else if (field_cap) kind = "field_caption";
      }
      // Excel 16, measured (gaps_fuzz seed 1238): an empty cell of the body is a value cell (no record for that row
      // and column), and the column fields' Grand Total heading is a grand total cell, not an item
      const bool body_row = rr >= rep.page_rows + rep.header_rows && rr < rep.row_kind.size() && rep.row_kind[rr] >= 0 && rep.row_kind[rr] != 5;
      const bool body_col = cc >= rep.label_cols && cc < rep.col_kind.size() && rep.col_kind[cc] >= 0;
      if (cell.kind == K::Empty && body_row && body_col) kind = "value";
      if (cell.kind == K::ColLabel && cc < rep.col_kind.size() && rep.col_kind[cc] == 2) kind = "grand";
      MV x = MV::Map();
      x.Set("name", MV::Str(pt->name));
      x.Set("kind", MV::Str(kind));
      const bool in_body = rr >= rep.page_rows;
      x.Set("row_items", in_body && rr < rep.row_filter.size() ? pairs(rep.row_filter[rr]) : MV::Array());
      x.Set("col_items", in_body && cc < rep.col_filter.size() ? pairs(rep.col_filter[cc]) : MV::Array());
      x.Set("row_kind", MV::Int(rr < rep.row_kind.size() ? rep.row_kind[rr] : -1));
      x.Set("col_kind", MV::Int(cc < rep.col_kind.size() ? rep.col_kind[cc] : -1));
      // the label's own field and item: the level of the label in its row or column
      if (cell.kind == K::RowLabel && rr < rep.row_filter.size() && !rep.row_filter[rr].empty()) {
        const auto& rf = rep.row_filter[rr];
        const int lv = engine::PivotLabelLevel(rep, rr, cc);
        const auto& fi = lv >= 0 && static_cast<size_t>(lv) < rf.size() ? rf[static_cast<size_t>(lv)] : rf.back();
        x.Set("field", MV::Str(field_name(fi.first)));
        x.Set("item", MV::Str(item_text(fi.first, fi.second)));
      } else if (cell.kind == K::ColLabel && rr > rep.page_rows && cc < rep.col_filter.size()) {
        const uint32_t lvl = rr - rep.page_rows - 1;
        if (lvl < rep.col_filter[cc].size()) {
          x.Set("field", MV::Str(field_name(rep.col_filter[cc][lvl].first)));
          x.Set("item", MV::Str(item_text(rep.col_filter[cc][lvl].first, rep.col_filter[cc][lvl].second)));
        }
      }
      int df = cc < rep.col_data.size() ? rep.col_data[cc] : -1;
      if (df < 0 && rr < rep.row_data.size()) df = rep.row_data[rr];
      if (df < 0 && pt->data.size() == 1) df = 0;
      if (df >= 0 && static_cast<size_t>(df) < pt->data.size() && (cell.kind == K::Data || cell.kind == K::Subtotal || cell.kind == K::Grand)) {
        x.Set("data_field", MV::Str(engine::PivotDataCaption(*pt, cache, static_cast<size_t>(df))));
      }
      MV at = MV::Array();
      at.Push(MV::Int(rr));
      at.Push(MV::Int(cc));
      x.Set("at", std::move(at));
      return x;
    });
  }
  if (m == "pivot.field_at") {
    // the PivotTable and the field whose item label is at (row, col): nil for any other cell (Range.Group on a label)
    const uint32_t row = static_cast<uint32_t>(p.GetInt("row")), col = static_cast<uint32_t>(p.GetInt("col"));
    return eng.Call([&](Document& d) {
      const model::PivotTable* pt = d.PivotAt(sheet, row, col);
      if (!pt || !pt->has_output) return MV();
      engine::PivotCache cache;
      std::string why;
      if (!engine::ReadPivotSource(d.wb(), *pt, &cache, &why)) return MV();
      const engine::PivotReport rep = engine::ComputePivot(*pt, cache);
      const uint32_t rr = row - pt->output.r0, cc = col - pt->output.c0;
      if (rr >= rep.rows || cc >= rep.cols) return MV();
      const auto& cell = rep.at(rr, cc);
      int field = -1;
      if (cell.kind == engine::PivotCell::RowLabel && rr < rep.row_filter.size() && !rep.row_filter[rr].empty() && rep.row_kind[rr] != 4) {
        const auto& rf = rep.row_filter[rr];
        const int lv = engine::PivotLabelLevel(rep, rr, cc);
        field = (lv >= 0 && static_cast<size_t>(lv) < rf.size() ? rf[static_cast<size_t>(lv)] : rf.back()).first;
      } else if (cell.kind == engine::PivotCell::ColLabel && rr > rep.page_rows && cc < rep.col_filter.size()) {
        const uint32_t lvl = rr - rep.page_rows - 1;
        if (lvl < rep.col_filter[cc].size()) field = rep.col_filter[cc][lvl].first;
      }
      if (field < 0 || static_cast<size_t>(field) >= cache.names.size()) return MV();
      MV x = MV::Map();
      x.Set("name", MV::Str(pt->name));
      x.Set("field", MV::Str(cache.names[static_cast<size_t>(field)]));
      return x;
    });
  }
  if (m == "pivot.item_ranges") {
    // PivotItem.LabelRange / DataRange: where an item of a row or column field is on the report: its label cell, and
    // the values of the rows (columns) it holds, its subtotal included and the grand totals left out
    const std::string name = p.GetStr("name"), item = p.GetStr("item");
    const int field = static_cast<int>(p.GetInt("field", -1));
    return eng.Call([&](Document& d) {
      const model::PivotTable* pt = FindPivotOn(d, sheet, name);
      if (!pt || !pt->has_output || field < 0) return MV();
      engine::PivotCache cache;
      std::string why;
      if (!engine::ReadPivotSource(d.wb(), *pt, &cache, &why) || static_cast<size_t>(field) >= cache.items.size()) return MV();
      const auto& items = cache.items[static_cast<size_t>(field)];
      const auto found = std::find(items.begin(), items.end(), item);
      if (found == items.end()) return MV();
      const int idx = static_cast<int>(found - items.begin());
      const engine::PivotReport rep = engine::ComputePivot(*pt, cache);
      const uint32_t R0 = pt->output.r0, C0 = pt->output.c0, body0 = rep.page_rows + rep.header_rows;
      auto at = [&](uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
        MV a = MV::Array();
        for (uint32_t v : {R0 + r0, C0 + c0, R0 + r1, C0 + c1}) a.Push(MV::Int(v));
        return a;
      };
      auto holds = [&](const std::vector<std::pair<int, int>>& v) {
        return std::any_of(v.begin(), v.end(), [&](const auto& fi) { return fi.first == field && fi.second == idx; });
      };
      // the value area without the grand totals
      uint32_t b0 = UINT32_MAX, b1 = 0, v0 = UINT32_MAX, v1 = 0;
      for (uint32_t r = body0; r < rep.rows && r < rep.row_kind.size(); ++r) {
        if (rep.row_kind[r] != 2) b0 = std::min(b0, r), b1 = std::max(b1, r);
      }
      for (uint32_t c = rep.label_cols; c < rep.cols && c < rep.col_kind.size(); ++c) {
        if (rep.col_kind[c] != 2) v0 = std::min(v0, c), v1 = std::max(v1, c);
      }
      MV x = MV::Map();
      for (size_t i = 0; i < pt->rows.size() && i < rep.row_field_col.size(); ++i) {
        if (pt->rows[i].field != field) continue;
        uint32_t r0 = UINT32_MAX, r1 = 0;
        for (uint32_t r = body0; r < rep.rows && r < rep.row_filter.size(); ++r) {
          if (r < rep.row_kind.size() && rep.row_kind[r] == 2) continue;
          if (holds(rep.row_filter[r])) r0 = std::min(r0, r), r1 = std::max(r1, r);
        }
        if (r0 == UINT32_MAX) return MV();
        const uint32_t c = rep.row_field_col[i];
        x.Set("label", at(r0, c, r0, c));
        if (v0 != UINT32_MAX) x.Set("data", at(r0, v0, r1, v1));
        return x;
      }
      for (size_t j = 0; j < pt->cols.size(); ++j) {
        if (pt->cols[j].field != field) continue;
        uint32_t c0 = UINT32_MAX, c1 = 0;
        for (uint32_t c = rep.label_cols; c < rep.cols && c < rep.col_filter.size(); ++c) {
          if (c < rep.col_kind.size() && rep.col_kind[c] == 2) continue;
          if (holds(rep.col_filter[c])) c0 = std::min(c0, c), c1 = std::max(c1, c);
        }
        if (c0 == UINT32_MAX) return MV();
        const uint32_t r = rep.page_rows + 1 + static_cast<uint32_t>(j);
        x.Set("label", at(r, c0, r, c0));
        if (b0 != UINT32_MAX) x.Set("data", at(b0, c0, b1, c1));
        return x;
      }
      return MV();
    });
  }
  if (m == "pivot.field_ranges") {
    // PivotField.DataRange / LabelRange: where a field's items and its caption are on the report, as Excel gives them
    // (measured): a row field's labels down its label column (subtotal rows too, not the grand total) under its
    // caption; a column field's labels along its header row right of its caption; a filter's value right of its
    // name; a data field's values (no grand totals) under its caption
    const std::string name = p.GetStr("name");
    const int field = static_cast<int>(p.GetInt("field", -1)), data = static_cast<int>(p.GetInt("data", -1));
    return eng.Call([&](Document& d) {
      const model::PivotTable* pt = FindPivotOn(d, sheet, name);
      if (!pt || !pt->has_output) return MV();
      engine::PivotCache cache;
      std::string why;
      if (!engine::ReadPivotSource(d.wb(), *pt, &cache, &why)) return MV();
      const engine::PivotReport rep = engine::ComputePivot(*pt, cache);
      if (rep.rows == 0 || rep.cols == 0) return MV();
      const uint32_t R0 = pt->output.r0, C0 = pt->output.c0;
      auto at = [&](uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
        MV a = MV::Array();
        for (uint32_t v : {R0 + r0, C0 + c0, R0 + r1, C0 + c1}) a.Push(MV::Int(v));
        return a;
      };
      const uint32_t h0 = rep.page_rows, body0 = rep.page_rows + rep.header_rows, last_header = body0 > 0 ? body0 - 1 : 0;
      uint32_t b0 = UINT32_MAX, b1 = 0, v0 = UINT32_MAX, v1 = 0;  // body rows and value columns, grand totals left out
      for (uint32_t r = body0; r < rep.rows && r < rep.row_kind.size(); ++r) {
        if (rep.row_kind[r] == 2 || (data >= 0 && r < rep.row_data.size() && rep.row_data[r] >= 0 && rep.row_data[r] != data)) continue;
        b0 = std::min(b0, r);
        b1 = std::max(b1, r);
      }
      for (uint32_t c = rep.label_cols; c < rep.cols && c < rep.col_kind.size(); ++c) {
        if (rep.col_kind[c] == 2 || (data >= 0 && c < rep.col_data.size() && rep.col_data[c] >= 0 && rep.col_data[c] != data)) continue;
        v0 = std::min(v0, c);
        v1 = std::max(v1, c);
      }
      MV x = MV::Map();
      if (data >= 0) {
        if (static_cast<size_t>(data) >= pt->data.size() || b0 == UINT32_MAX || v0 == UINT32_MAX) return MV();
        x.Set("data", at(b0, v0, b1, v1));
        // the caption: over the values' column without column fields, else the report's corner (one data field)
        x.Set("label", pt->cols.empty() || pt->data.size() > 1 ? at(last_header, v0, last_header, v0) : at(h0, 0, h0, 0));
        return x;
      }
      for (size_t i = 0; i < pt->rows.size() && i < rep.row_field_col.size(); ++i) {
        if (pt->rows[i].field != field) continue;
        const uint32_t c = rep.row_field_col[i];
        if (b0 != UINT32_MAX) x.Set("data", at(b0, c, b1, c));
        x.Set("label", at(last_header, c, last_header, c));
        return x;
      }
      for (size_t j = 0; j < pt->cols.size(); ++j) {
        if (pt->cols[j].field != field) continue;
        const uint32_t r = h0 + 1 + static_cast<uint32_t>(j);
        if (v0 != UINT32_MAX && r < rep.rows) x.Set("data", at(r, v0, r, v1));
        x.Set("label", at(h0, rep.label_cols + static_cast<uint32_t>(j), h0, rep.label_cols + static_cast<uint32_t>(j)));
        return x;
      }
      for (size_t k = 0; k < pt->pages.size(); ++k) {
        if (pt->pages[k].field != field) continue;
        x.Set("data", at(static_cast<uint32_t>(k), 1, static_cast<uint32_t>(k), 1));
        x.Set("label", at(static_cast<uint32_t>(k), 0, static_cast<uint32_t>(k), 0));
        return x;
      }
      return MV();  // not on the report
    });
  }
  if (m == "pivot.details") {
    // -1: the cell isn't in a PivotTable report; 0: not a value; otherwise the new sheet's id
    const uint32_t row = static_cast<uint32_t>(p.GetInt("row")), col = static_cast<uint32_t>(p.GetInt("col"));
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::PivotTable* pt = d.PivotAt(sheet, row, col);
      if (!pt || !pt->has_output) return MV::Int(-1);
      const uint32_t ns = d.PivotShowDetails(sheet, row, col, &err);
      d.ForeignWrite();
      return MV::Int(ns);
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  // ---- Slicers ----
  if (m == "slicer.list") {
    return eng.Call([&](Document& d) {
      MV a = MV::Array();
      if (const model::Sheet* s = d.S(sheet)) {
        for (const auto& sl : *s->slicers) {
          MV o = MV::Map();
          o.Set("sheet", MV::Int(sheet));
          o.Set("id", MV::Int(sl.id));
          o.Set("name", MV::Str(sl.name));
          o.Set("caption", MV::Str(sl.caption));
          o.Set("pivot", MV::Str(sl.pivot));
          o.Set("field", MV::Int(sl.field));
          o.Set("left", MV::Double(sl.left));
          o.Set("top", MV::Double(sl.top));
          o.Set("width", MV::Double(sl.width));
          o.Set("height", MV::Double(sl.height));
          o.Set("columns", MV::Int(sl.columns));
          o.Set("timeline", MV::Bool(sl.timeline));
          o.Set("level", MV::Int(sl.level));
          o.Set("show_header", MV::Bool(sl.show_header));
          o.Set("descending", MV::Bool(sl.descending));
          o.Set("hide_no_data", MV::Bool(sl.hide_no_data));
          o.Set("indicate_no_data", MV::Bool(sl.indicate_no_data));
          o.Set("no_data_last", MV::Bool(sl.no_data_last));
          o.Set("more_pivots", StrList(sl.more_pivots));
          o.Set("table", MV::Str(sl.table));
          o.Set("tl_hide", MV::Int(sl.tl_hide));
          a.Push(std::move(o));
        }
      }
      return a;
    });
  }
  if (m == "slicer.add") {
    std::string err;
    MV r = eng.Call([&](Document& d) {
      model::Slicer sl;
      if (p.Has("table")) {
        // a table's slicer: its column (0-based within the table)
        uint32_t ts = 0;
        const model::Table* t = model::FindTable(d.wb(), p.GetStr("table"), &ts);
        if (!t) {
          err = "Table not found: " + p.GetStr("table");
          return MV();
        }
        sl.table = t->name;
        sl.field = static_cast<int>(p.GetInt("field", -1));
        if (sl.field < 0 || static_cast<uint32_t>(sl.field) >= t->range.Cols()) {
          err = "No such column";
          return MV();
        }
        sl.caption = p.GetStr("caption", static_cast<size_t>(sl.field) < t->columns.size() ? t->columns[static_cast<size_t>(sl.field)].name : std::string("Slicer"));
        sl.name = p.GetStr("name");
        if (p.Has("left")) sl.left = p.GetDouble("left");
        if (p.Has("top")) sl.top = p.GetDouble("top");
        if (p.Has("width")) sl.width = p.GetDouble("width");
        if (p.Has("height")) sl.height = p.GetDouble("height");
        const uint32_t id = d.AddSlicer(sheet, sl);
        d.ForeignWrite();
        for (const auto& x : *d.S(sheet)->slicers) {
          if (x.id == id) return MV::Str(x.name);
        }
        return MV();
      }
      sl.pivot = p.GetStr("pivot");
      const model::PivotTable* pt = d.FindPivot(sl.pivot);
      if (!pt) {
        err = "PivotTable not found: " + sl.pivot;
        return MV();
      }
      sl.pivot = pt->name;
      sl.field = static_cast<int>(p.GetInt("field", -1));
      if (sl.field < 0 || static_cast<size_t>(sl.field) >= pt->fields.size()) {
        err = "No such field";
        return MV();
      }
      sl.caption = p.GetStr("caption", pt->fields[static_cast<size_t>(sl.field)]);
      sl.name = p.GetStr("name");
      if (p.Has("left")) sl.left = p.GetDouble("left");
      if (p.Has("top")) sl.top = p.GetDouble("top");
      if (p.Has("width")) sl.width = p.GetDouble("width");
      if (p.Has("height")) sl.height = p.GetDouble("height");
      sl.timeline = p.GetBool("timeline");
      if (sl.timeline) {
        if (!p.Has("width")) sl.width = 330;
        if (!p.Has("height")) sl.height = 110;
      }
      sl.level = static_cast<int>(std::clamp<int64_t>(p.GetInt("level", 2), 0, 3));
      const uint32_t id = d.AddSlicer(sheet, sl);
      d.ForeignWrite();
      for (const auto& x : *d.S(sheet)->slicers) {
        if (x.id == id) return MV::Str(x.name);
      }
      return MV();
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "slicer.set" || m == "slicer.delete") {
    const std::string name = p.GetStr("name");
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      uint32_t id = 0;
      for (const auto& x : s ? *s->slicers : std::vector<model::Slicer>{}) {
        if (LowerStr(x.name) == LowerStr(name)) id = x.id;
      }
      if (!id) {
        err = "Slicer not found: " + name;
        return MV();
      }
      if (m == "slicer.delete") {
        d.DeleteSlicer(sheet, id);
      } else if (const MV* props = p.Get("props")) {
        d.UpdateSlicer(sheet, id, [&](model::Slicer& x) {
          if (props->Has("name")) x.name = props->GetStr("name");
          if (props->Has("caption")) x.caption = props->GetStr("caption");
          if (props->Has("left")) x.left = props->GetDouble("left");
          if (props->Has("top")) x.top = props->GetDouble("top");
          if (props->Has("width")) x.width = std::max(40.0, props->GetDouble("width"));
          if (props->Has("height")) x.height = std::max(40.0, props->GetDouble("height"));
          if (props->Has("columns")) x.columns = static_cast<int>(props->GetInt("columns"));
          if (props->Has("level")) x.level = static_cast<int>(std::clamp<int64_t>(props->GetInt("level"), 0, 3));
          if (props->Has("show_header")) x.show_header = props->GetBool("show_header");
          if (props->Has("descending")) x.descending = props->GetBool("descending");
          if (props->Has("hide_no_data")) x.hide_no_data = props->GetBool("hide_no_data");
          if (props->Has("indicate_no_data")) x.indicate_no_data = props->GetBool("indicate_no_data");
          if (props->Has("no_data_last")) x.no_data_last = props->GetBool("no_data_last");
          if (props->Has("tl_hide")) x.tl_hide = static_cast<int>(props->GetInt("tl_hide")) & 7;
        });
      }
      d.ForeignWrite();
      return MV::Bool(true);
    });
    if (!err.empty()) SetErr(ec, em, "E_NOT_FOUND", err);
    return r;
  }
  if (m == "slicer.filter") {
    std::string err;
    MV r = eng.Call([&](Document& d) {
      std::vector<std::string> hidden;
      if (const MV* h = p.Get("hidden")) hidden = ToStrList(*h);
      d.pivot_overwrite = p.GetBool("alerts") ? 1 : 0;
      const bool ok = d.SetSlicerFilter(p.GetStr("pivot"), static_cast<int>(p.GetInt("field")), hidden, &err);
      d.pivot_overwrite = 0;
      d.ForeignWrite();
      if (!ok && err.empty()) err = "PivotTable not found";
      return MV::Bool(ok);
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "slicer.select" || m == "slicer.dates" || m == "slicer.connect" || m == "slicer.items") {
    // one slicer by its name on the sheet: its selection (every PivotTable it is connected to, or its table), a
    // timeline's dates, its Report Connections, a table slicer's items
    const std::string name = p.GetStr("name");
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      const model::Slicer* sl = nullptr;
      if (!s) {
        err = "Sheet not found";
        return MV();
      }
      for (const auto& x : *s->slicers) {
        if (LowerStr(x.name) == LowerStr(name)) sl = &x;
      }
      if (!sl) {
        err = "Slicer not found: " + name;
        return MV();
      }
      const uint32_t id = sl->id;
      if (m == "slicer.items") {
        std::vector<std::string> items, hidden;
        std::vector<char> data;
        engine::TableSlicerState(d.wb(), *sl, nullptr, &items, &hidden, &data);
        MV o = MV::Map();
        o.Set("items", StrList(items));
        o.Set("hidden", StrList(hidden));
        MV hd = MV::Array();
        for (char c : data) hd.Push(MV::Bool(c != 0));
        o.Set("has_data", std::move(hd));
        return o;
      }
      d.pivot_overwrite = p.GetBool("alerts") ? 1 : 0;
      bool ok = false;
      if (m == "slicer.select") {
        std::vector<std::string> hidden;
        if (const MV* h = p.Get("hidden")) hidden = ToStrList(*h);
        ok = d.SetSlicerSelection(sheet, id, hidden, &err);
      } else if (m == "slicer.dates") {
        ok = d.SetSlicerDates(sheet, id, p.GetDouble("from", 1), p.GetDouble("to", 0), &err);
      } else {
        std::vector<std::string> pivots;
        if (const MV* v = p.Get("pivots")) pivots = ToStrList(*v);
        ok = d.ConnectSlicer(sheet, id, pivots, &err);
      }
      d.pivot_overwrite = 0;
      d.ForeignWrite();
      if (!ok && err.empty()) err = "The slicer couldn't be changed";
      return MV::Bool(ok);
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "timeline.filter") {
    std::string err;
    MV r = eng.Call([&](Document& d) {
      const bool ok = d.SetTimelineFilter(p.GetStr("pivot"), static_cast<int>(p.GetInt("field")), p.GetDouble("from", 1), p.GetDouble("to", 0), &err);
      d.ForeignWrite();
      if (!ok && err.empty()) err = "PivotTable not found";
      return MV::Bool(ok);
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "pivot.refresh_all") {
    return eng.Call([&](Document& d) {
      d.RefreshAllPivots();
      d.ForeignWrite();
      return MV::Bool(true);
    });
  }

  // ---- Custom panels (Application.Panels) ----
  if (m == "panel.list") {
    return eng.Call([&](Document& d) {
      MV a = MV::Array();
      for (const auto& x : *d.wb().panels) a.Push(PanelToMV(x));
      return a;
    });
  }
  if (m == "panel.set") {
    const MV* pm = p.Get("panel");
    if (!pm || pm->GetStr("name").empty()) return SetErr(ec, em, "E_ARG", "A panel needs a name"), MV();
    return eng.Call([&](Document& d) {
      const std::string name = pm->GetStr("name");
      model::Panel next;
      bool found = false;
      for (const auto& x : *d.wb().panels) {
        if (x.name == name) next = x, found = true;
      }
      if (!found) next.name = name;
      ApplyPanel(next, *pm);
      d.UpsertPanel(next);
      d.ForeignWrite();
      return PanelToMV(next);
    });
  }
  if (m == "panel.input") {  // tests and agents: click a panel button or set a control's value like the user
    std::string e;
    MV r = OnUi([&e, p](AppState& s) { return PanelInput(s, p, &e); });
    if (!e.empty()) return SetErr(ec, em, "E_NOT_FOUND", e), MV();
    return r;
  }
  if (m == "panel.delete") {
    const std::string name = p.GetStr("name");
    return eng.Call([&](Document& d) {
      const bool ok = d.DeletePanel(name);
      d.ForeignWrite();
      return MV::Bool(ok);
    });
  }
  if (m == "panel.ctl_set") {
    const std::string pn = p.GetStr("panel"), cn = p.GetStr("name");
    std::string err;
    MV r = eng.Call([&](Document& d) {
      MV out;
      std::string linked, value;
      bool write = false;
      const bool ok = d.UpdatePanel(pn, [&](model::Panel& x) {
        for (auto& c : x.controls) {
          if (c.name != cn) continue;
          if (const MV* props = p.Get("props")) {
            ApplyPanelControl(c, *props);
            if (props->Has("value") && !c.linked_cell.empty()) {
              write = true;
              linked = c.linked_cell;
              value = c.value;
            }
          }
          out = MV::Bool(true);
        }
      });
      if (!ok || out.IsNil()) {
        err = "Panel control not found:" + pn + "." + cn;
        return MV();
      }
      if (write) {
        uint32_t sid = 0;
        Rect rr;
        const uint32_t def = d.S(sheet) ? sheet : d.wb().sheets.front()->id;
        if (ResolveRangeText(d.wb(), def, linked, &sid, &rr)) {
          d.SetInput(sid, rr.r0, rr.c0, value, nullptr, engine::kSrcPython);
        }
      }
      d.ForeignWrite();
      return out;
    });
    if (!err.empty()) SetErr(ec, em, "E_NOT_FOUND", err);
    return r;
  }

  // ---- Containers and layout (view state, UI thread) ----
  if (m == "container.list") {
    return OnUi([](AppState& s) {
      MV a = MV::Array();
      for (const auto& c : s.containers) {
        MV x = MV::Map();
        x.Set("id", MV::Int(c.id));
        x.Set("name", MV::Str(c.name));
        a.Push(std::move(x));
      }
      return a;
    });
  }
  if (m == "container.add") {
    const std::string name = p.GetStr("name");
    const double l = p.GetDouble("left", -1), t = p.GetDouble("top", -1), w = p.GetDouble("width", 640), h = p.GetDouble("height", 420);
    return OnUi([name, l, t, w, h](AppState& s) {
      for (const auto& c : s.containers) {
        if (c.name == name) {
          MV x = MV::Map();
          x.Set("id", MV::Int(c.id));
          x.Set("name", MV::Str(c.name));
          return x;
        }
      }
      Container c;
      c.id = s.next_container_id++;
      c.name = name.empty() ? "Container" + std::to_string(c.id) : name;
      s.containers.push_back(c);
      const std::string wn = s.ContainerWindowName(c);
      const float dpi = ImGui::GetStyle().FontScaleDpi;
      const ImVec2 o = ImGui::GetMainViewport()->Pos;
      if (l >= 0 && t >= 0) ImGui::SetWindowPos(wn.c_str(), ImVec2(o.x + static_cast<float>(l * 4.0 / 3.0) * dpi, o.y + static_cast<float>(t * 4.0 / 3.0) * dpi));
      ImGui::SetWindowSize(wn.c_str(), ImVec2(static_cast<float>(w * 4.0 / 3.0) * dpi, static_cast<float>(h * 4.0 / 3.0) * dpi));
      MV x = MV::Map();
      x.Set("id", MV::Int(c.id));
      x.Set("name", MV::Str(c.name));
      return x;
    });
  }
  if (m == "container.delete") {
    const std::string name = p.GetStr("name");
    return OnUi([name](AppState& s) {
      for (auto it = s.containers.begin(); it != s.containers.end(); ++it) {
        if (it->name == name) {
          ImGui::DockBuilderRemoveNode(ui::dock::ContainerDockspaceId(it->id));
          s.containers.erase(it);
          return MV::Bool(true);
        }
      }
      return MV::Bool(false);
    });
  }
  if (m == "container.dock" || m == "window.float") {
    // docking: item = {kind: sheet|panel|chart|container, name}; position: left/right/top/bottom/tab
    const MV* item = p.Get("item");
    const std::string kind = item ? item->GetStr("kind") : std::string(), iname = item ? item->GetStr("name") : std::string();
    const std::string target = p.GetStr("name"), pos = p.GetStr("position", "tab");
    const double ratio = p.GetDouble("ratio", 0.3);
    const double l = p.GetDouble("left", -1), t = p.GetDouble("top", -1), w = p.GetDouble("width", -1), h = p.GetDouble("height", -1);
    std::string err;
    MV r = OnUi([&, kind, iname, target, pos, ratio](AppState& s) {
      if (kind == "sheet") {
        const int32_t i = s.snap->wb.FindSheet(iname);
        if (i >= 0) s.Win(s.snap->wb.sheets[static_cast<size_t>(i)]->id).floating = true;  // pull it out of the main window first
      }
      if (kind == "chart") {
        for (const auto& sh : s.snap->wb.sheets) {
          for (const auto& c : *sh->charts) {
            if (c.name == iname && !c.floating) {
              const uint32_t sid = sh->id, cid = c.id;
              s.engine->Post([sid, cid](Document& d) { d.UpdateChart(sid, cid, [](ChartObj& x) { x.floating = true; }); });
            }
          }
        }
      }
      const std::string wn = WindowNameFor(s, kind, iname);
      if (wn.empty()) {
        err = "Item to dock not found:" + kind + " " + iname;
        return MV();
      }
      if (m == "window.float") {
        ui::dock::UndockWindow(wn.c_str());
        const float dpi = ImGui::GetStyle().FontScaleDpi;
        const ImVec2 o = ImGui::GetMainViewport()->Pos;
        if (l >= 0 && t >= 0) ImGui::SetWindowPos(wn.c_str(), ImVec2(o.x + static_cast<float>(l * 4.0 / 3.0) * dpi, o.y + static_cast<float>(t * 4.0 / 3.0) * dpi));
        if (w > 0 && h > 0) ImGui::SetWindowSize(wn.c_str(), ImVec2(static_cast<float>(w * 4.0 / 3.0) * dpi, static_cast<float>(h * 4.0 / 3.0) * dpi));
        return MV::Bool(true);
      }
      const Container* ct = nullptr;
      for (const auto& c : s.containers) {
        if (c.name == target) ct = &c;
      }
      if (!ct) {
        err = "Container not found:" + target;
        return MV();
      }
      const ImGuiID ds = ui::dock::ContainerDockspaceId(ct->id);
      // out of its old place first: the node it leaves is deleted at once and may be the leaf split below
      if (ImGuiWindow* dw = ImGui::FindWindowByName(wn.c_str()); dw && (dw->DockNode || dw->DockId)) ui::dock::UndockWindow(wn.c_str());
      ImGuiDockNode* node = ImGui::DockBuilderGetNode(ds);
      if (!node) {
        ImGui::DockBuilderAddNode(ds, ImGuiDockNodeFlags_DockSpace);
        node = ImGui::DockBuilderGetNode(ds);
      }
      ImGuiID dst = ds;
      // split by position when there is content; dock straight in when empty or for tab
      if (pos != "tab" && node && !node->IsEmpty()) {
        ImGuiDockNode* leaf = node;
        while (leaf->IsSplitNode()) leaf = leaf->ChildNodes[1] ? leaf->ChildNodes[1] : leaf->ChildNodes[0];
        const ImGuiDir dir = pos == "left" ? ImGuiDir_Left : pos == "right" ? ImGuiDir_Right : pos == "top" ? ImGuiDir_Up : ImGuiDir_Down;
        dst = ui::dock::SplitOrTab(leaf, dir, static_cast<float>(ratio));  // a leaf too small to split takes it as a tab
      }
      ui::dock::DockWindow(wn.c_str(), dst);
      ImGui::DockBuilderFinish(ds);
      return MV::Bool(true);
    });
    if (!err.empty()) SetErr(ec, em, "E_NOT_FOUND", err);
    return r;
  }
  if (m == "layout.reset") {  // [YC] Application.Layout.Reset(): also while the user can't (windows protected)
    return OnUi([](AppState& s) {
      s.req_layout_reset = true;
      return MV::Bool(true);
    });
  }
  if (m == "ui.lock") {  // [YC] Panel.Locked, Container.Locked, Figure.Locked (kind: panel / container / figure)
    const std::string kind = p.GetStr("kind"), name = p.GetStr("name");
    const bool set = p.Has("locked"), on = p.GetBool("locked");
    std::string err;
    MV r = OnUi([&, kind, name, set, on](AppState& s) {
      std::string key;
      if (kind == "panel" || kind == "figure") {
        key = kind + ":" + name;
      } else if (kind == "container") {
        for (const Container& c : s.containers) {
          if (c.name == name) key = "container:" + std::to_string(c.id);
        }
        if (key.empty()) err = "No container " + name;
      } else {
        err = "kind is panel, container or figure";
      }
      if (key.empty()) return MV::Bool(false);
      if (set && on) s.locked_windows.insert(key);  // saved with the workbook's layout
      else if (set) s.locked_windows.erase(key);
      return MV::Bool(s.locked_windows.count(key) != 0);
    });
    if (!err.empty()) SetErr(ec, em, "E_ARG", err);
    return r;
  }
  if (m == "layout.capture") {
    return OnUi([](AppState& s) { return MV::Str(CaptureLayoutJson(s)); });
  }
  if (m == "layout.apply") {
    const std::string json = p.GetStr("json");
    std::string err;
    MV r = OnUi([&, json](AppState& s) {
      if (!ApplyLayoutJson(s, json, p.GetBool("main_window"))) err = "Invalid layout data";
      ForgetPanelDocking();
      return MV::Bool(err.empty());
    });
    if (!err.empty()) SetErr(ec, em, "E_APP", err);
    return r;
  }
  if (m == "ui.design_mode_set") {
    const bool on = p.GetBool("on");
    return OnUi([on](AppState& s) {
      s.design_mode = on;
      if (auto* c = ui::Commands().Find("DesignMode")) c->pressed = on;
      return MV::Bool(on);
    });
  }
  *handled = false;
  return MV();
}

}  // namespace yc::app
