// SPDX-License-Identifier: Apache-2.0
// python_bridge_rules.cpp - YCP: conditional formatting (FormatConditions), data validation (Validation), AutoFilter
#include "app_state.h"

#include "engine/navigate.h"
#include "engine/rule_eval.h"
#include "formula/numfmt.h"
#include "io/msgvalue.h"

#include <ctime>

namespace yc::app {

using engine::Document;
using io::MV;

namespace {

// a note or comment thread as sent to Python
MV ThreadToMV(const model::CellNote& n) {
  MV x = MV::Map();
  x.Set("author", MV::Str(n.author));
  x.Set("text", MV::Str(n.text));
  x.Set("threaded", MV::Bool(n.threaded));
  x.Set("visible", MV::Bool(n.visible));
  if (!n.threaded) {
    const auto& lk = n.look;
    MV b = MV::Map();
    b.Set("w", MV::Double(lk.w));
    b.Set("h", MV::Double(lk.h));
    b.Set("dx", MV::Double(lk.dx));
    b.Set("dy", MV::Double(lk.dy));
    b.Set("font_name", MV::Str(lk.font_name));
    b.Set("font_size", MV::Double(lk.font_size));
    b.Set("bold", MV::Bool(lk.bold));
    b.Set("italic", MV::Bool(lk.italic));
    b.Set("font_color", MV::Int(lk.font_color));
    b.Set("fill_color", MV::Int(lk.fill_color));
    b.Set("line_color", MV::Int(lk.line_color));
    b.Set("halign", MV::Int(lk.halign));
    b.Set("valign", MV::Int(lk.valign));
    x.Set("look", std::move(b));
  }
  if (n.threaded) {
    x.Set("date", MV::Double(n.date));
    x.Set("resolved", MV::Bool(n.resolved));
    MV rs = MV::Array();
    for (const auto& rp : n.replies) {
      MV y = MV::Map();
      y.Set("author", MV::Str(rp.author));
      y.Set("text", MV::Str(rp.text));
      y.Set("date", MV::Double(rp.date));
      rs.Push(std::move(y));
    }
    x.Set("replies", std::move(rs));
  }
  return x;
}

MV RectMV(const Rect& r) {
  MV x = MV::Array();
  x.Push(MV::Int(r.r0));
  x.Push(MV::Int(r.c0));
  x.Push(MV::Int(r.r1));
  x.Push(MV::Int(r.c1));
  return x;
}
Rect RectFromMV(const MV& a) {
  if (a.a.size() < 4) return Rect{};
  return Rect{static_cast<uint32_t>(a.a[0].i), static_cast<uint32_t>(a.a[1].i), static_cast<uint32_t>(a.a[2].i), static_cast<uint32_t>(a.a[3].i)};
}
MV Rects(const std::vector<Rect>& v) {
  MV a = MV::Array();
  for (const Rect& r : v) a.Push(RectMV(r));
  return a;
}

// colors: Python passes RGB() values (0xBBGGRR, like Excel); the model stores ARGB (0xAARRGGBB)
uint32_t FromExcelColor(int64_t v) {
  const uint32_t x = static_cast<uint32_t>(v) & 0xFFFFFF;
  return 0xFF000000u | ((x & 0xFF) << 16) | (x & 0xFF00) | ((x >> 16) & 0xFF);
}
int64_t ToExcelColor(uint32_t argb) { return ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16); }

MV CfToMV(const model::CfRule& c) {
  MV x = MV::Map();
  x.Set("id", MV::Int(c.id));
  x.Set("ranges", Rects(c.ranges));
  x.Set("type", MV::Int(static_cast<int>(c.type)));
  x.Set("op", MV::Int(static_cast<int>(c.op)));
  x.Set("formula1", MV::Str(c.formula1));
  x.Set("formula2", MV::Str(c.formula2));
  x.Set("text", MV::Str(c.text));
  x.Set("fill", c.fmt.has_fill ? MV::Int(ToExcelColor(c.fmt.fill_color)) : MV());
  x.Set("font_color", c.fmt.has_font_color ? MV::Int(ToExcelColor(c.fmt.font_color)) : MV());
  x.Set("bold", c.fmt.bold < 0 ? MV() : MV::Bool(c.fmt.bold != 0));
  x.Set("italic", c.fmt.italic < 0 ? MV() : MV::Bool(c.fmt.italic != 0));
  x.Set("strike", c.fmt.strike < 0 ? MV() : MV::Bool(c.fmt.strike != 0));
  x.Set("border", (c.fmt.border_color >> 24) ? MV::Int(ToExcelColor(c.fmt.border_color)) : MV());
  x.Set("rank", MV::Int(c.rank));
  x.Set("bottom", MV::Bool(c.bottom));
  x.Set("percent", MV::Bool(c.percent));
  x.Set("below", MV::Bool(c.below));
  x.Set("duplicate", MV::Bool(c.duplicate));
  x.Set("points", MV::Int(c.points));
  MV cols = MV::Array();
  for (uint32_t col : c.colors) cols.Push(MV::Int(ToExcelColor(col)));
  x.Set("colors", std::move(cols));
  x.Set("bar_color", MV::Int(ToExcelColor(c.bar_color)));
  x.Set("priority", MV::Int(c.priority + 1));
  x.Set("stop_if_true", MV::Bool(c.stop_if_true));
  x.Set("icon_style", MV::Int(c.icon_style));
  x.Set("icon_reverse", MV::Bool(c.icon_reverse));
  x.Set("icon_only", MV::Bool(c.icon_only));
  MV th = MV::Array();
  for (const auto& t : (c.icon_thresholds.empty() ? model::DefaultIconThresholds(c.icon_style) : c.icon_thresholds)) {
    MV e = MV::Array();
    e.Push(MV::Int(t.type));
    e.Push(MV::Double(t.value));
    e.Push(MV::Str(t.formula));
    e.Push(MV::Bool(t.gte));
    th.Push(std::move(e));
  }
  x.Set("icon_thresholds", std::move(th));
  x.Set("text_op", MV::Int(c.text_op));
  x.Set("time_period", MV::Int(c.time_period));
  x.Set("equal_average", MV::Bool(c.equal_average));
  x.Set("std_dev", MV::Int(c.std_dev));
  MV pts = MV::Array();
  for (int i = 0; i < 3; ++i) {
    MV e = MV::Array();
    e.Push(MV::Int(c.point_type[i]));
    e.Push(MV::Double(c.point_value[i]));
    e.Push(MV::Str(c.point_formula[i]));
    pts.Push(std::move(e));
  }
  x.Set("scale_points", std::move(pts));
  // the format's number format, underline, pattern and lines per side; the data bar's look and bounds
  x.Set("numfmt", c.fmt.numfmt.empty() ? MV() : MV::Str(c.fmt.numfmt));
  x.Set("underline", c.fmt.underline < 0 ? MV() : MV::Int(c.fmt.underline));
  x.Set("pattern", c.fmt.pattern < 0 ? MV() : MV::Int(c.fmt.pattern));
  x.Set("pattern_color", MV::Int(ToExcelColor(c.fmt.pattern_color)));
  MV sides = MV::Array();
  for (int k = 0; k < 4; ++k) {
    MV e = MV::Array();
    e.Push(MV::Int(c.fmt.side_style[k]));
    e.Push(MV::Int(ToExcelColor(c.fmt.side_color[k])));
    sides.Push(std::move(e));
  }
  x.Set("sides", std::move(sides));
  x.Set("bar_gradient", MV::Bool(c.bar_gradient));
  x.Set("bar_show_value", MV::Bool(c.bar_show_value));
  x.Set("bar_border", c.bar_border ? MV::Int(ToExcelColor(c.bar_border)) : MV());
  x.Set("bar_direction", MV::Int(c.bar_direction));
  MV bounds = MV::Array();
  for (int i = 0; i < 2; ++i) {
    MV e = MV::Array();
    e.Push(MV::Int(i ? c.bar_max_type : c.bar_min_type));
    e.Push(MV::Double(i ? c.bar_max_value : c.bar_min_value));
    e.Push(MV::Str(i ? c.bar_max_formula : c.bar_min_formula));
    bounds.Push(std::move(e));
  }
  x.Set("bar_bounds", std::move(bounds));
  return x;
}

void ApplyCf(model::CfRule& c, const MV& p) {
  if (const MV* v = p.Get("ranges")) {
    c.ranges.clear();
    for (const MV& r : v->a) c.ranges.push_back(RectFromMV(r));
  }
  if (const MV* v = p.Get("type")) c.type = static_cast<model::CfType>(v->i);
  if (const MV* v = p.Get("op")) c.op = static_cast<model::CmpOp>(v->i);
  // a rule keeps its formulas without the =, as the dialogs write them: FormatConditions.Add(..., "=10") kept it, and
  // Edit Rule showed ==10 (Formula1 reads the = back either way)
  auto bare = [](const std::string& f) { return !f.empty() && f[0] == '=' ? f.substr(1) : f; };
  if (const MV* v = p.Get("formula1")) c.formula1 = bare(v->s);
  if (const MV* v = p.Get("formula2")) c.formula2 = bare(v->s);
  if (const MV* v = p.Get("text")) c.text = v->s;
  if (const MV* v = p.Get("fill")) {
    c.fmt.has_fill = !v->IsNil();
    if (c.fmt.has_fill) c.fmt.fill_color = FromExcelColor(v->i);
  }
  if (const MV* v = p.Get("font_color")) {
    c.fmt.has_font_color = !v->IsNil();
    if (c.fmt.has_font_color) c.fmt.font_color = FromExcelColor(v->i);
  }
  if (const MV* v = p.Get("bold")) c.fmt.bold = v->IsNil() ? -1 : (v->b ? 1 : 0);
  if (const MV* v = p.Get("italic")) c.fmt.italic = v->IsNil() ? -1 : (v->b ? 1 : 0);
  if (const MV* v = p.Get("strike")) c.fmt.strike = v->IsNil() ? -1 : (v->b ? 1 : 0);
  if (const MV* v = p.Get("border")) c.fmt.border_color = v->IsNil() ? 0u : (FromExcelColor(v->i) | 0xFF000000u);
  if (const MV* v = p.Get("rank")) c.rank = static_cast<int>(v->i);
  if (const MV* v = p.Get("bottom")) c.bottom = v->b;
  if (const MV* v = p.Get("percent")) c.percent = v->b;
  if (const MV* v = p.Get("below")) c.below = v->b;
  if (const MV* v = p.Get("duplicate")) c.duplicate = v->b;
  if (const MV* v = p.Get("points")) c.points = static_cast<int>(v->i);
  if (const MV* v = p.Get("colors")) {
    for (size_t i = 0; i < 3 && i < v->a.size(); ++i) c.colors[i] = FromExcelColor(v->a[i].i);
    if (v->a.size() == 2) c.colors[2] = c.colors[1];
  }
  if (const MV* v = p.Get("bar_color")) c.bar_color = FromExcelColor(v->i);
  if (const MV* v = p.Get("stop_if_true")) c.stop_if_true = v->b;
  if (const MV* v = p.Get("icon_style")) {
    c.icon_style = static_cast<int>(v->i);
    if (c.icon_thresholds.size() + 1 != static_cast<size_t>(model::IconSetCount(c.icon_style))) c.icon_thresholds = model::DefaultIconThresholds(c.icon_style);
  }
  if (const MV* v = p.Get("icon_reverse")) c.icon_reverse = v->b;
  if (const MV* v = p.Get("icon_only")) c.icon_only = v->b;
  if (const MV* v = p.Get("icon_thresholds"); v && v->t == MV::T::Array) {
    c.icon_thresholds.clear();
    for (const MV& e : v->a) {
      if (e.a.size() < 4) continue;
      model::IconThreshold t;
      t.type = static_cast<int>(e.a[0].i);
      t.value = e.a[1].Num();
      t.formula = e.a[2].s;
      t.gte = e.a[3].b;
      c.icon_thresholds.push_back(std::move(t));
    }
  }
  if (const MV* v = p.Get("text_op")) c.text_op = static_cast<int>(std::clamp<int64_t>(v->i, 0, 3));
  if (const MV* v = p.Get("time_period")) c.time_period = static_cast<int>(std::clamp<int64_t>(v->i, 0, 9));
  if (const MV* v = p.Get("equal_average")) c.equal_average = v->b;
  if (const MV* v = p.Get("std_dev")) c.std_dev = static_cast<int>(std::clamp<int64_t>(v->i, 0, 3));
  if (const MV* v = p.Get("scale_points"); v && v->t == MV::T::Array) {
    for (size_t i = 0; i < 3 && i < v->a.size(); ++i) {
      const MV& e = v->a[i];
      if (e.a.size() < 3) continue;
      c.point_type[i] = static_cast<int>(e.a[0].i);
      c.point_value[i] = e.a[1].Num();
      c.point_formula[i] = e.a[2].s;
    }
  }
  if (const MV* v = p.Get("numfmt")) c.fmt.numfmt = v->IsNil() ? std::string() : v->s;
  if (const MV* v = p.Get("underline")) c.fmt.underline = v->IsNil() ? -1 : static_cast<int8_t>(std::clamp<int64_t>(v->i, 0, 4));
  if (const MV* v = p.Get("pattern")) c.fmt.pattern = v->IsNil() ? -1 : static_cast<int8_t>(std::clamp<int64_t>(v->i, 0, static_cast<int64_t>(model::FillPattern::Count) - 1));
  if (const MV* v = p.Get("pattern_color")) c.fmt.pattern_color = FromExcelColor(v->i);
  if (const MV* v = p.Get("sides"); v && v->t == MV::T::Array) {
    for (size_t k = 0; k < 4 && k < v->a.size(); ++k) {
      if (v->a[k].a.size() < 2) continue;
      c.fmt.side_style[k] = static_cast<uint8_t>(std::clamp<int64_t>(v->a[k].a[0].i, 0, 13));
      c.fmt.side_color[k] = c.fmt.side_style[k] ? FromExcelColor(v->a[k].a[1].i) : 0u;
    }
  }
  if (const MV* v = p.Get("bar_gradient")) c.bar_gradient = v->b;
  if (const MV* v = p.Get("bar_show_value")) c.bar_show_value = v->b;
  if (const MV* v = p.Get("bar_border")) c.bar_border = v->IsNil() ? 0u : FromExcelColor(v->i);
  if (const MV* v = p.Get("bar_direction")) c.bar_direction = static_cast<int>(std::clamp<int64_t>(v->i, 0, 2));
  if (const MV* v = p.Get("bar_bounds"); v && v->t == MV::T::Array) {
    for (size_t i = 0; i < 2 && i < v->a.size(); ++i) {
      const MV& e = v->a[i];
      if (e.a.size() < 3) continue;
      (i ? c.bar_max_type : c.bar_min_type) = static_cast<int>(std::clamp<int64_t>(e.a[0].i, -1, 7));
      (i ? c.bar_max_value : c.bar_min_value) = e.a[1].Num();
      (i ? c.bar_max_formula : c.bar_min_formula) = e.a[2].s;
    }
  }
  if (c.type == model::CfType::IconSet && c.icon_thresholds.empty()) c.icon_thresholds = model::DefaultIconThresholds(c.icon_style);
}

MV DvToMV(const model::DvRule& d) {
  MV x = MV::Map();
  x.Set("ranges", Rects(d.ranges));
  x.Set("type", MV::Int(static_cast<int>(d.type)));
  x.Set("op", MV::Int(static_cast<int>(d.op)));
  x.Set("formula1", MV::Str(d.formula1));
  x.Set("formula2", MV::Str(d.formula2));
  x.Set("ignore_blank", MV::Bool(d.ignore_blank));
  x.Set("in_cell_dropdown", MV::Bool(d.in_cell_dropdown));
  x.Set("show_input", MV::Bool(d.show_input));
  x.Set("show_error", MV::Bool(d.show_error));
  x.Set("alert", MV::Int(static_cast<int>(d.alert)));
  x.Set("input_title", MV::Str(d.input_title));
  x.Set("input_message", MV::Str(d.input_message));
  x.Set("error_title", MV::Str(d.error_title));
  x.Set("error_message", MV::Str(d.error_message));
  return x;
}

void ApplyDv(model::DvRule& d, const MV& p) {
  if (const MV* v = p.Get("type")) d.type = static_cast<model::DvType>(v->i);
  if (const MV* v = p.Get("op")) d.op = static_cast<model::CmpOp>(v->i);
  if (const MV* v = p.Get("formula1")) d.formula1 = v->s;
  if (const MV* v = p.Get("formula2")) d.formula2 = v->s;
  if (const MV* v = p.Get("ignore_blank")) d.ignore_blank = v->b;
  if (const MV* v = p.Get("in_cell_dropdown")) d.in_cell_dropdown = v->b;
  if (const MV* v = p.Get("show_input")) d.show_input = v->b;
  if (const MV* v = p.Get("show_error")) d.show_error = v->b;
  if (const MV* v = p.Get("alert")) d.alert = static_cast<model::DvAlert>(std::clamp<int64_t>(v->i, 1, 3));
  if (const MV* v = p.Get("input_title")) d.input_title = v->s;
  if (const MV* v = p.Get("input_message")) d.input_message = v->s;
  if (const MV* v = p.Get("error_title")) d.error_title = v->s;
  if (const MV* v = p.Get("error_message")) d.error_message = v->s;
}

MV AfToMV(const model::AutoFilter& a) {
  MV x = MV::Map();
  x.Set("on", MV::Bool(a.on));
  x.Set("range", RectMV(a.range));
  MV cols = MV::Array();
  for (const auto& c : a.columns) {
    MV m = MV::Map();
    m.Set("field", MV::Int(c.field + 1));
    m.Set("kind", MV::Int(c.kind));
    MV vals = MV::Array();
    for (const auto& v : c.values) vals.Push(MV::Str(v));
    m.Set("values", std::move(vals));
    m.Set("blanks", MV::Bool(c.blanks));
    m.Set("crit1", MV::Str(c.crit1));
    m.Set("crit2", MV::Str(c.crit2));
    m.Set("or", MV::Bool(c.or_op));
    m.Set("top", MV::Bool(c.top));
    m.Set("percent", MV::Bool(c.percent));
    m.Set("count", MV::Int(c.count));
    if (c.kind == 4 || c.kind == 5) m.Set("color", MV::Int(c.color));
    if (c.kind == 6) m.Set("dynamic", MV::Int(c.dynamic));
    if (c.kind == 7) {  // Filter by Cell Icon: the icon set and the icon (0 = the lowest values' icon)
      m.Set("icon_set", MV::Int(c.icon_set));
      m.Set("icon", MV::Int(c.icon));
    }
    cols.Push(std::move(m));
  }
  x.Set("columns", std::move(cols));
  x.Set("hidden_rows", MV::Int(static_cast<int64_t>(a.hidden.size())));
  MV sort = MV::Array();  // AutoFilter.Sort: [column within the range, descending] per level
  for (const auto& l : a.sort_levels) {
    MV lv = MV::Array();
    lv.Push(MV::Int(l.col));
    lv.Push(MV::Bool(l.desc));
    sort.Push(std::move(lv));
  }
  x.Set("sort", std::move(sort));
  return x;
}

}  // namespace

MV DispatchRules(engine::Engine& eng, const std::string& m, const MV& p, std::string* ec, std::string* em, bool* handled) {
  *handled = true;
  const uint32_t sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));
  auto err = [&](const char* code, std::string msg) {
    *ec = code;
    *em = std::move(msg);
    return MV();
  };
  // a format code Excel refuses (formula::ValidFormatCode) raises, as Range.NumberFormat does
  auto bad_code = [](const MV* props) {
    const MV* v = props ? props->Get("numfmt") : nullptr;
    return v && !v->IsNil() && !formula::ValidFormatCode(v->s);
  };
  if ((m == "cf.add" && bad_code(p.Get("rule"))) || (m == "cf.set" && bad_code(p.Get("props"))))
    return err("E_ARG", "Unable to set the NumberFormat property of the FormatCondition class");
  if (m == "cf.list") {
    return eng.Call([&](Document& d) {
      MV a = MV::Array();
      if (const model::Sheet* s = d.S(sheet)) {
        std::vector<const model::CfRule*> v;
        for (const auto& r : *s->cond_formats) v.push_back(&r);
        std::stable_sort(v.begin(), v.end(), [](auto* x, auto* y) { return x->priority < y->priority; });
        const MV* filter = p.Get("rect");
        const Rect fr = filter ? RectFromMV(*filter) : Rect{};
        for (auto* r : v) {
          bool hit = !filter;
          for (const Rect& x : r->ranges) hit = hit || x.Intersects(fr);
          if (hit) a.Push(CfToMV(*r));
        }
      }
      return a;
    });
  }
  // a structured reference ([@Qty], Trades[Qty]) in a rule's formula: Excel 16 refuses it in conditional formatting
  // and in a validation's list or formula (measured through FormatConditions.Add and Validation.Add: error 1004;
  // INDIRECT("Trades[Qty]") and a name defined on it are taken)
  auto names_table = [](Document&, const std::string& text, core::CellPos) { return engine::HasStructuredRef(text); };
  if (m == "cf.add") {
    const MV* rule = p.Get("rule");
    if (!rule) return err("E_ARG", "rule is required");
    bool refused_table_ref = false;
    MV out = eng.Call([&](Document& d) {
      model::CfRule c;
      ApplyCf(c, *rule);
      if (c.ranges.empty() || d.RefuseProtectedSheet(sheet)) return MV();
      const core::CellPos at{c.ranges.front().r0, c.ranges.front().c0};
      if (names_table(d, c.formula1, at) || names_table(d, c.formula2, at)) {
        refused_table_ref = true;
        return MV();
      }
      // "last": after the sheet's other rules, as Excel's FormatConditions.Add (the UI puts a new rule on top)
      if (p.GetBool("last")) c.priority = -1;
      const uint32_t id = d.AddCondFormat(sheet, c);
      d.ForeignWrite();
      for (const auto& r : *d.S(sheet)->cond_formats) {
        if (r.id == id) return CfToMV(r);
      }
      return MV();
    });
    if (refused_table_ref) return err("E_APP", "This type of reference can't be used in a conditional formatting formula: use a cell reference, or a function such as INDIRECT(\"Table1[Qty]\")");
    return out;
  }
  if (m == "cf.set" || m == "cf.delete") {
    const uint32_t id = static_cast<uint32_t>(p.GetInt("id"));
    bool ok = false;
    MV r = eng.Call([&](Document& d) {
      MV out;
      if (m == "cf.delete") {
        ok = d.DeleteCondFormat(sheet, id);
      } else {
        ok = d.UpdateCondFormat(sheet, id, [&](model::CfRule& c) {
          if (const MV* props = p.Get("props")) ApplyCf(c, *props);
          if (p.Has("priority")) {
            // move to the given position (1-based): the other rules shift down
            const int want = static_cast<int>(p.GetInt("priority")) - 1;
            c.priority = want;
          }
        });
        if (ok && p.Has("priority")) {
          auto& v = d.M(sheet)->cond_formats.mut();
          std::stable_sort(v.begin(), v.end(), [&](const model::CfRule& a, const model::CfRule& b) {
            const int pa = a.priority * 2 + (a.id == id ? 0 : 1), pb = b.priority * 2 + (b.id == id ? 0 : 1);
            return pa < pb;
          });
          for (size_t i = 0; i < v.size(); ++i) v[i].priority = static_cast<int>(i);
        }
        for (const auto& c : *d.S(sheet)->cond_formats) {
          if (c.id == id) out = CfToMV(c);
        }
      }
      d.ForeignWrite();
      return out.IsNil() ? MV::Bool(ok) : out;
    });
    if (!ok) return err("E_NOT_FOUND", "Conditional formatting rule not found");
    return r;
  }
  if (m == "cf.clear") {
    const MV* rr = p.Get("rect");
    eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      d.ClearCondFormats(sheet, rr ? RectFromMV(*rr) : Rect{0, 0, s ? s->row_limit - 1 : 0, core::kMaxCols - 1});
      d.ForeignWrite();
    });
    return MV::Bool(true);
  }
  if (m == "dv.get") {
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      const model::DvRule* r = s ? engine::FindValidation(*s, static_cast<uint32_t>(p.GetInt("row")), static_cast<uint32_t>(p.GetInt("col"))) : nullptr;
      return r ? DvToMV(*r) : MV();
    });
  }
  if (m == "dv.formula_fails") {  // Validation.Add / Modify: the formula gives an error at the rule's first cell
    const std::string text = p.GetStr("formula");
    const core::CellPos at{static_cast<uint32_t>(p.GetInt("row")), static_cast<uint32_t>(p.GetInt("col"))};
    const bool list = p.GetBool("list", false);
    return MV::Bool(eng.Call([&](Document& d) { return d.S(sheet) && d.ValidationFormulaFails(sheet, at, text, list); }));
  }
  if (m == "dv.set" || m == "dv.clear" || m == "dv.modify") {
    const MV* rr = p.Get("rect");
    if (!rr) return err("E_ARG", "rect is required");
    const Rect rect = RectFromMV(*rr);
    bool refused_table_ref = false;
    eng.Call([&](Document& d) {
      if (m == "dv.clear") {
        d.ClearValidation(sheet, rect);
      } else {
        model::DvRule rule;
        if (m == "dv.modify") {
          if (const model::Sheet* s = d.S(sheet)) {
            if (const model::DvRule* old = engine::FindValidation(*s, rect.r0, rect.c0)) rule = *old;
          }
        }
        if (const MV* props = p.Get("rule")) ApplyDv(rule, *props);
        const core::CellPos at{rect.r0, rect.c0};
        if (names_table(d, rule.formula1, at) || names_table(d, rule.formula2, at)) {
          refused_table_ref = true;
          return;
        }
        d.SetValidation(sheet, rect, rule);
      }
      d.ForeignWrite();
    });
    if (refused_table_ref) return err("E_APP", "This type of reference can't be used in a data validation formula: use a cell reference, or a function such as INDIRECT");
    return MV::Bool(true);
  }
  if (m == "dv.check") {
    // test whether an input passes validation (without writing it)
    const uint32_t r = static_cast<uint32_t>(p.GetInt("row")), c = static_cast<uint32_t>(p.GetInt("col"));
    const std::string text = p.GetStr("text");
    eng.Sync();  // the rule and the value Python just wrote are in the snapshot read here
    auto snap = eng.Current();
    const model::Sheet* s = snap->Sheet(sheet);
    const model::DvRule* rule = s ? engine::FindValidation(*s, r, c) : nullptr;
    MV x = MV::Map();
    std::string msg;
    std::string_view f1 = rule ? std::string_view(rule->formula1) : std::string_view();
    while (!f1.empty() && f1.front() == ' ') f1.remove_prefix(1);
    if (rule && rule->type == model::DvType::List && !f1.empty() && f1.front() != '=' && text.size() > 1 && text[0] == '\'') {
      // Validation.Value of a typed list (Excel, measured): the text without the spaces around it is one of the items
      // exactly; case counts and an item keeps its own spaces (in a,b,c 'A' fails, ' a' and 'b ' pass; in a, b ,c 'b'
      // fails). A list of cells, and what a person types (ValidateInput), match without case
      std::string_view v = std::string_view(text).substr(1);
      while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
      while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
      bool found = false;
      std::string_view rest = rule->formula1;
      while (!found) {
        const size_t comma = rest.find(',');
        found = !v.empty() && rest.substr(0, comma) == v;
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
      }
      x.Set("valid", MV::Bool(found));
      return x;
    }
    x.Set("valid", MV::Bool(!rule || engine::ValidateInput(snap->wb, sheet, r, c, text, *rule, &msg)));
    if (!msg.empty()) x.Set("message", MV::Str(msg));
    return x;
  }
  // a table's own AutoFilter (ListObject.AutoFilter): "table" names it; the sheet's otherwise
  const uint32_t table = static_cast<uint32_t>(p.GetInt("table", 0));
  if (m == "af.owner") {  // the table whose own filter covers (row, col), else 0
    const uint32_t row = static_cast<uint32_t>(p.GetInt("row")), col = static_cast<uint32_t>(p.GetInt("col"));
    return MV::Int(eng.Call([&](Document& d) -> int64_t {
      const model::Sheet* s = d.S(sheet);
      if (!s) return 0;
      for (const auto& t : *s->tables) {
        if (engine::TableHasOwnFilter(*s, t) && t.range.Contains(row, col)) return t.id;
      }
      return 0;
    }));
  }
  if (m == "af.get") {
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV();
      if (!table) {
        // the sheet's AutoFilter that is a table's buttons: Excel's model sees no AutoFilter on the sheet then
        MV x = AfToMV(*s->autofilter);
        bool owned = false;
        for (const auto& t : *s->tables) owned = owned || engine::TableOwnsSheetFilter(*s, t);
        if (x.t == MV::T::Map) x.Set("table_owned", MV::Bool(owned));
        return x;
      }
      for (const auto& t : *s->tables) {
        if (t.id != table) continue;
        if (engine::TableOwnsSheetFilter(*s, t)) return AfToMV(*s->autofilter);
        model::AutoFilter a = t.filter;
        a.on = engine::TableHasOwnFilter(*s, t);
        a.range = engine::TableFilterRange(t);
        return AfToMV(a);
      }
      return MV();
    });
  }
  if (m == "af.set") {
    const MV* rr = p.Get("rect");
    eng.Call([&](Document& d) {
      if (!rr || rr->IsNil()) d.ClearAutoFilter(sheet);
      else d.SetAutoFilter(sheet, RectFromMV(*rr));
      d.ForeignWrite();
    });
    return MV::Bool(true);
  }
  if (m == "af.column") {
    std::string e;
    eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s || (!s->autofilter->on && !table)) {
        e = "This sheet has no AutoFilter; create one with Range.AutoFilter() first";
        return;
      }
      const model::Table* own = nullptr;
      if (table) {
        for (const auto& t : *s->tables) {
          if (t.id == table && engine::TableHasOwnFilter(*s, t)) own = &t;
        }
      }
      model::FilterColumn fc;
      fc.field = static_cast<uint32_t>(std::max<int64_t>(1, p.GetInt("field", 1)) - 1);
      fc.kind = static_cast<int>(p.GetInt("kind", 0));
      if (const MV* v = p.Get("values")) {
        for (const MV& x : v->a) {
          if (x.t == MV::T::Str) fc.values.push_back(x.s);
          else if (x.IsNum()) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.15g", x.Num());
            fc.values.push_back(b);
          }
        }
      }
      fc.blanks = p.GetBool("blanks", false);
      fc.crit1 = p.GetStr("crit1");
      fc.crit2 = p.GetStr("crit2");
      fc.or_op = p.GetBool("or");
      fc.top = p.GetBool("top", true);
      fc.percent = p.GetBool("percent");
      fc.count = static_cast<int>(p.GetInt("count", 10));
      fc.color = static_cast<uint32_t>(p.GetInt("color", 0));
      fc.dynamic = static_cast<int>(p.GetInt("dynamic", 0));
      fc.icon_set = static_cast<int>(p.GetInt("icon_set", 0));
      fc.icon = static_cast<int>(p.GetInt("icon", -1));
      const Rect fr = own ? engine::TableFilterRange(*own) : s->autofilter->range;
      if (fc.field > fr.c1 - fr.c0) {
        e = "Field is outside the filter range";
        return;
      }
      if (own) d.SetTableFilterColumn(sheet, table, fc);
      else d.SetFilterColumn(sheet, fc);
      d.ForeignWrite();
    });
    if (!e.empty()) return err("E_APP", e);
    return MV::Bool(true);
  }
  if (m == "af.sort") {  // AutoFilter.Sort.Apply: records the levels and sorts the list's visible rows
    std::vector<model::SortLevel> levels;
    if (const MV* lv = p.Get("levels")) {
      for (const MV& x : lv->a) {
        if (x.a.size() >= 2) levels.push_back(model::SortLevel{static_cast<uint32_t>(x.a[0].IsNum() ? x.a[0].Num() : 0), x.a[1].b});
      }
    }
    eng.Call([&](Document& d) {
      d.Begin("Sort");
      const model::Sheet* s = d.S(sheet);
      const model::Table* own = nullptr;
      if (s && table) {
        for (const auto& t : *s->tables) {
          if (t.id == table && engine::TableHasOwnFilter(*s, t)) own = &t;
        }
      }
      if (own) {
        Rect body = engine::TableFilterRange(*own);
        std::vector<engine::SortKey> keys;
        for (const auto& l : levels) {
          if (body.c0 + l.col <= body.c1) keys.push_back(engine::SortKey{body.c0 + l.col, l.desc});
        }
        d.SetTableFilterSort(sheet, table, levels);
        if (!keys.empty()) d.Sort(sheet, body, keys, true);
      } else {
        d.SetFilterSort(sheet, levels);
        d.ApplyAutoFilter(sheet, true);
      }
      d.Commit();
    });
    return MV::Bool(true);
  }
  if (m == "af.show_all" || m == "af.reapply") {
    eng.Call([&](Document& d) {
      if (m == "af.show_all" && table) {
        const model::Sheet* s = d.S(sheet);
        const model::Table* t = d.TableById(sheet, table);
        if (s && t && engine::TableOwnsSheetFilter(*s, *t)) d.ShowAllData(sheet);
        else d.ClearTableFilter(sheet, table);
      } else if (m == "af.show_all") {
        d.ShowAllData(sheet);
      } else {
        d.ApplyAutoFilter(sheet);
      }
      d.ForeignWrite();
    });
    return MV::Bool(true);
  }
  // ---- Notes and hyperlinks ----
  if (m == "note.get" || m == "link.get") {
    const uint64_t key = (static_cast<uint64_t>(p.GetInt("row")) << 16) | static_cast<uint64_t>(p.GetInt("col"));
    return eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return MV();
      if (m == "note.get") {
        auto it = s->notes->find(key);
        if (it == s->notes->end()) return MV();
        return ThreadToMV(it->second);
      }
      auto it = s->links->find(key);
      if (it == s->links->end()) return MV();
      MV x = MV::Map();
      x.Set("address", MV::Str(it->second.address));
      x.Set("sub_address", MV::Str(it->second.sub_address));
      x.Set("tip", MV::Str(it->second.tip));
      return x;
    });
  }
  if (m == "link.list" || m == "note.list") {
    const MV* rr = p.Get("rect");
    const Rect area = rr ? RectFromMV(*rr) : Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1};
    return eng.Call([&](Document& d) {
      MV a = MV::Array();
      const model::Sheet* s = d.S(sheet);
      if (!s) return a;
      auto add = [&](uint64_t k, MV x) {
        const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
        if (!area.Contains(r, c)) return;
        x.Set("row", MV::Int(r));
        x.Set("col", MV::Int(c));
        a.Push(std::move(x));
      };
      if (m == "link.list") {
        for (const auto& [k, l] : *s->links) {
          MV x = MV::Map();
          x.Set("address", MV::Str(l.address));
          x.Set("sub_address", MV::Str(l.sub_address));
          x.Set("tip", MV::Str(l.tip));
          add(k, std::move(x));
        }
      } else {
        for (const auto& [k, n] : *s->notes) add(k, ThreadToMV(n));
      }
      return a;
    });
  }
  // ---- Threaded comments ----
  if (m == "comment.set" || m == "comment.reply" || m == "comment.edit" || m == "comment.reply_delete" || m == "comment.resolve") {
    const uint32_t r = static_cast<uint32_t>(p.GetInt("row")), c = static_cast<uint32_t>(p.GetInt("col"));
    const double now = static_cast<double>(std::time(nullptr));
    const std::string author = p.GetStr("author", "Python"), text = p.GetStr("text");
    const int index = static_cast<int>(p.GetInt("index", -1));
    const bool ok = eng.Call([&](Document& d) {
      d.ForeignWrite();
      if (m == "comment.set") {
        d.SetComment(sheet, r, c, author, text, now);
        return true;
      }
      if (m == "comment.reply") return d.AddCommentReply(sheet, r, c, author, text, now);
      if (m == "comment.edit") return d.EditCommentPost(sheet, r, c, index, text);
      if (m == "comment.reply_delete") return d.DeleteCommentReply(sheet, r, c, index);
      return d.ResolveComment(sheet, r, c, p.GetBool("resolved", true));
    });
    if (!ok) return err("E_NOT_FOUND", "No comment thread (or reply) there");
    return MV::Bool(true);
  }
  if (m == "comment.convert") {
    const MV* rr = p.Get("rect");
    const Rect area = rr ? RectFromMV(*rr) : Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1};
    const double now = static_cast<double>(std::time(nullptr));
    return MV::Int(eng.Call([&](Document& d) {
      d.ForeignWrite();
      return d.ConvertNotesToComments(sheet, area, now);
    }));
  }
  if (m == "note.look") {  // Comment.Shape: the note box's size, place, font, fill and line
    const uint32_t r = static_cast<uint32_t>(p.GetInt("row")), c = static_cast<uint32_t>(p.GetInt("col"));
    const MV* props = p.Get("props");
    const bool ok = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      const auto it = s ? s->notes->find((uint64_t{r} << 16) | c) : decltype(s->notes->end()){};
      if (!s || it == s->notes->end() || it->second.threaded) return false;
      model::CellNote::Look lk = it->second.look;
      if (props) {
        const MV& q = *props;
        if (q.Has("w")) lk.w = q.GetDouble("w");
        if (q.Has("h")) lk.h = q.GetDouble("h");
        if (q.Has("dx")) lk.dx = q.GetDouble("dx");
        if (q.Has("dy")) lk.dy = q.GetDouble("dy");
        if (q.Has("font_name")) lk.font_name = q.GetStr("font_name");
        if (q.Has("font_size")) lk.font_size = static_cast<float>(q.GetDouble("font_size"));
        if (q.Has("bold")) lk.bold = q.GetBool("bold");
        if (q.Has("italic")) lk.italic = q.GetBool("italic");
        if (q.Has("font_color")) lk.font_color = static_cast<uint32_t>(q.GetInt("font_color"));
        if (q.Has("fill_color")) lk.fill_color = static_cast<uint32_t>(q.GetInt("fill_color"));
        if (q.Has("line_color")) lk.line_color = static_cast<uint32_t>(q.GetInt("line_color"));
        if (q.Has("halign")) lk.halign = static_cast<int>(std::clamp<int64_t>(q.GetInt("halign"), 0, 2));
        if (q.Has("valign")) lk.valign = static_cast<int>(std::clamp<int64_t>(q.GetInt("valign"), 0, 2));
      }
      const bool done = d.SetNoteLook(sheet, r, c, lk);
      d.ForeignWrite();
      return done;
    });
    return MV::Bool(ok);
  }
  if (m == "note.visible") {  // Comment.Visible: the note stays on the sheet (Show/Hide Note)
    const uint32_t r = static_cast<uint32_t>(p.GetInt("row")), c = static_cast<uint32_t>(p.GetInt("col"));
    const bool v = p.GetBool("visible");
    eng.Call([&](Document& d) {
      d.SetNotesVisible(sheet, Rect::Cell(r, c), v);
      d.ForeignWrite();
    });
    return MV::Bool(true);
  }
  if (m == "note.set" || m == "link.set") {
    const uint32_t r = static_cast<uint32_t>(p.GetInt("row")), c = static_cast<uint32_t>(p.GetInt("col"));
    eng.Call([&](Document& d) {
      if (m == "note.set") {
        d.SetNote(sheet, r, c, p.GetStr("author", "Python"), p.GetStr("text"));
      } else {
        model::Hyperlink l{p.GetStr("address"), p.GetStr("sub_address"), p.GetStr("tip")};
        if (p.Has("text")) d.SetValue(sheet, r, c, core::Value::String(p.GetStr("text")), engine::kSrcPython);
        // Hyperlinks.Add gives the cell the Hyperlink style: its font (a red or bold one too), its number format kept
        // (Excel 16, measured 2026-11-28)
        if (p.GetBool("style", true)) d.ApplyCellStyle(sheet, Rect::Cell(r, c), "Hyperlink");
        d.SetHyperlink(sheet, r, c, l);
      }
      d.ForeignWrite();
    });
    return MV::Bool(true);
  }
  if (m == "note.clear" || m == "link.clear") {
    const MV* rr = p.Get("rect");
    if (!rr) return err("E_ARG", "rect is required");
    const Rect area = RectFromMV(*rr);
    eng.Call([&](Document& d) {
      if (m == "note.clear") {
        d.ClearNotes(sheet, area);
      } else {
        // Hyperlink(s).Delete is Remove Hyperlinks: the cells that had a link lose their formats too, where
        // Range.ClearHyperlinks keeps them (Excel 16, measured 2026-11-28)
        std::vector<Rect> cells;
        if (const model::Sheet* sh = p.GetBool("formats") ? d.S(sheet) : nullptr) {
          for (const auto& [k, l] : *sh->links) {
            const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
            if (area.Contains(r, c)) cells.push_back(Rect::Cell(r, c));
          }
        }
        d.ClearHyperlinks(sheet, area);
        for (const Rect& cell : cells) d.ClearFormats(sheet, cell, false);
      }
      d.ForeignWrite();
    });
    return MV::Bool(true);
  }
  *handled = false;
  return MV();
}

}  // namespace yc::app
