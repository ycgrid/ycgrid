// SPDX-License-Identifier: Apache-2.0
#include "engine/rule_eval.h"

#include "core/i18n.h"

#include "engine/document.h"
#include "engine/external.h"
#include "engine/tile_iter.h"
#include "formula/datetime.h"
#include "formula/functions.h"
#include "formula/numfmt.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

namespace yc::engine {

using core::Rect;
using core::Value;
using model::CfType;
using model::CmpOp;

// ---- SnapshotEvalHost ----

const Value& SnapshotEvalHost::Cell(int32_t sheet, uint32_t r, uint32_t c) {
  static const Value kEmpty;
  if (IsExternalSheet(sheet)) return ExternalValue(wb_, sheet, r, c);
  if (has_ov_ && sheet == ov_sheet_ && r == ov_r_ && c == ov_c_) return ov_v_;
  const model::Sheet* s = wb_.SheetById(static_cast<uint32_t>(sheet));
  return s ? s->GetValue(r, c) : kEmpty;
}

void SnapshotEvalHost::ForEachCell(int32_t sheet, const Rect& rect, const std::function<bool(uint32_t, uint32_t, const Value&)>& fn) {
  if (!has_ov_ || sheet != ov_sheet_ || !rect.Contains(ov_r_, ov_c_)) {
    ForEachRaw(sheet, rect, fn);
    return;
  }
  // the overridden cell takes its place in row-major order (also when the snapshot has it empty)
  bool done = false, stopped = false;
  auto emit = [&]() {
    done = true;
    return ov_v_.IsEmpty() || fn(ov_r_, ov_c_, ov_v_);
  };
  ForEachRaw(sheet, rect, [&](uint32_t r, uint32_t c, const Value& v) {
    if (!done && (r > ov_r_ || (r == ov_r_ && c >= ov_c_))) {
      if (!emit()) return !(stopped = true);
      if (r == ov_r_ && c == ov_c_) return true;
    }
    if (!fn(r, c, v)) return !(stopped = true);
    return true;
  });
  if (!done && !stopped) emit();
}

void SnapshotEvalHost::ForEachRaw(int32_t sheet, const Rect& rect_in, const std::function<bool(uint32_t, uint32_t, const Value&)>& fn) {
  if (IsExternalSheet(sheet)) {
    const size_t li = ExternalLinkOf(sheet), si = ExternalSheetOf(sheet);
    const auto& links = *wb_.external_links;
    if (li >= links.size() || si >= links[li].values.size()) return;
    for (const auto& [k, v] : links[li].values[si]) {
      const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
      if (r < rect_in.r0 || r > rect_in.r1 || c < rect_in.c0 || c > rect_in.c1) continue;
      if (!fn(r, c, v)) return;
    }
    return;
  }
  const model::Sheet* s = wb_.SheetById(static_cast<uint32_t>(sheet));
  if (!s || !s->has_used) return;
  Rect rect = rect_in;
  rect.r1 = std::min(rect.r1, s->used.r1);
  rect.c1 = std::min(rect.c1, s->used.c1);
  if (rect.r0 > rect.r1 || rect.c0 > rect.c1) return;
  ForEachValueRowMajor(*s, rect, fn);
}

uint32_t SnapshotEvalHost::RowLimit(int32_t sheet) {
  if (IsExternalSheet(sheet)) return 1048576;
  const model::Sheet* s = wb_.SheetById(static_cast<uint32_t>(sheet));
  return s ? s->row_limit : 0;
}

Rect SnapshotEvalHost::UsedRect(int32_t sheet) {
  if (IsExternalSheet(sheet)) {
    bool any = false;
    return ExternalUsed(wb_, sheet, &any);
  }
  const model::Sheet* s = wb_.SheetById(static_cast<uint32_t>(sheet));
  return s && s->has_used ? s->used : Rect{0, 0, 0, 0};
}

int32_t SnapshotEvalHost::FindSheet(std::string_view name) const {
  const int32_t i = wb_.FindSheet(name);
  if (i < 0 && name.find('[') != std::string_view::npos) return FindExternalSheet(wb_, name);
  return i < 0 ? -1 : static_cast<int32_t>(wb_.sheets[static_cast<size_t>(i)]->id);
}

std::string SnapshotEvalHost::SheetName(int32_t sheet) const {
  if (IsExternalSheet(sheet)) return ExternalSheetName(wb_, sheet);
  const model::Sheet* s = sheet >= 0 ? wb_.SheetById(static_cast<uint32_t>(sheet)) : nullptr;
  return s ? s->name : std::string();
}

bool SnapshotEvalHost::RowHidden(int32_t sheet, uint32_t r) {
  const model::Sheet* s = wb_.SheetById(static_cast<uint32_t>(sheet));
  return s && s->rows->IsHidden(r);
}

namespace {
bool IEq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}
std::string Lower(const std::string& s) { return formula::FoldCase(s); }  // as every other comparison
}  // namespace

bool SnapshotEvalHost::ResolveName(std::string_view name, int32_t current_sheet, core::CellPos origin, formula::EV* out) {
  const model::NameDef* found = nullptr;
  for (const auto& n : *wb_.names) {
    if (!IEq(n.name, name)) continue;
    if (n.sheet_scope == current_sheet) {
      found = &n;
      break;
    }
    if (n.sheet_scope < 0 && !found) found = &n;
  }
  if (!found) return false;
  if (depth_ > 16) {
    *out = formula::EV::Err(core::Err::Ref);
    return true;
  }
  std::string_view text = found->refers_to;
  if (!text.empty() && text[0] == '=') text.remove_prefix(1);
  // relative references are kept relative to A1: they reach the same offsets from the cell that uses the name
  formula::ParseResult pr = formula::Parse(text, core::CellPos{0, 0}, this);
  if (!pr.ok) {
    *out = formula::EV::Err(core::Err::Name);
    return true;
  }
  if (formula::HasRelativeRef(pr.root)) formula::RelativeTo(pr.root, origin);
  ++depth_;
  formula::Evaluator ev(*this, current_sheet, origin);
  *out = ev.Eval(pr.root);
  --depth_;
  return true;
}

Value SnapshotEvalHost::Evaluate(uint32_t sheet, const formula::Node& root, uint32_t r, uint32_t c) {
  try {
    return formula::EvaluateCell(root, *this, static_cast<int32_t>(sheet), core::CellPos{r, c});
  } catch (const formula::NeedCell&) {
    return Value::Error(core::Err::NA);
  } catch (const std::exception&) {
    return Value::Error(core::Err::Value);
  }
}

// ---- Conditional formatting ----

namespace {

Rect Bounds(const std::vector<Rect>& v) {
  Rect b = v.front();
  for (const Rect& r : v) b = b.Union(r);
  return b;
}

bool InRanges(const std::vector<Rect>& v, uint32_t r, uint32_t c) {
  for (const Rect& x : v) {
    if (x.Contains(r, c)) return true;
  }
  return false;
}

bool CompareOp(CmpOp op, const Value& v, const Value& a, const Value& b) {
  bool ok = true;
  const int x = formula::Evaluator::Compare(v, a, &ok);
  if (!ok) return false;
  switch (op) {
    case CmpOp::Equal: return x == 0;
    case CmpOp::NotEqual: return x != 0;
    case CmpOp::Greater: return x > 0;
    case CmpOp::Less: return x < 0;
    case CmpOp::GreaterEqual: return x >= 0;
    case CmpOp::LessEqual: return x <= 0;
    case CmpOp::Between:
    case CmpOp::NotBetween: {
      bool ok2 = true;
      const int lo_hi = formula::Evaluator::Compare(a, b, &ok2);
      const Value& lo = lo_hi <= 0 ? a : b;
      const Value& hi = lo_hi <= 0 ? b : a;
      bool o1 = true, o2 = true;
      const bool in = formula::Evaluator::Compare(v, lo, &o1) >= 0 && formula::Evaluator::Compare(v, hi, &o2) <= 0;
      if (!o1 || !o2) return false;
      return op == CmpOp::Between ? in : !in;
    }
  }
  return false;
}

uint32_t Lerp(uint32_t a, uint32_t b, double t) {
  t = std::clamp(t, 0.0, 1.0);
  uint32_t out = 0xFF000000u;
  for (int sh = 0; sh < 24; sh += 8) {
    const double x = ((a >> sh) & 0xFF) * (1 - t) + ((b >> sh) & 0xFF) * t;
    out |= static_cast<uint32_t>(std::lround(x)) << sh;
  }
  return out;
}

void Apply(const model::CfFormat& f, CfVisual* out) {
  if (f.has_fill && !out->has_fill) {
    out->has_fill = true;
    out->fill = f.fill_color;
  }
  if (f.has_font_color && !out->has_font) {
    out->has_font = true;
    out->font = f.font_color;
  }
  if (f.bold >= 0 && out->bold < 0) out->bold = f.bold;
  if (f.italic >= 0 && out->italic < 0) out->italic = f.italic;
  if (f.strike >= 0 && out->strike < 0) out->strike = f.strike;
  if ((f.border_color >> 24) && !out->border) out->border = f.border_color;
  if (!f.numfmt.empty() && out->numfmt.empty()) out->numfmt = f.numfmt;
  if (f.underline >= 0 && out->underline < 0) out->underline = f.underline;
  if (f.pattern >= 0 && out->pattern < 0) {
    out->pattern = f.pattern;
    out->pattern_color = f.pattern_color;
  }
  for (int k = 0; k < 4; ++k) {
    if (f.side_style[k] && !out->side_style[k]) {
      out->side_style[k] = f.side_style[k];
      out->side_color[k] = f.side_color[k];
    }
  }
}

}  // namespace

CondFormatEval::CondFormatEval(model::Workbook wb, uint32_t sheet) : wb_(std::move(wb)), sheet_(sheet), host_(wb_) {
  sh_ = wb_.SheetById(sheet);
  if (!sh_) return;
  std::vector<const model::CfRule*> sorted;
  for (const auto& r : *sh_->cond_formats) {
    if (!r.ranges.empty()) sorted.push_back(&r);
  }
  std::stable_sort(sorted.begin(), sorted.end(), [](const model::CfRule* a, const model::CfRule* b) { return a->priority < b->priority; });
  for (const model::CfRule* r : sorted) {
    Prepared p;
    p.rule = r;
    p.bounds = Bounds(r->ranges);
    const core::CellPos base{r->ranges.front().r0, r->ranges.front().c0};
    auto parse = [&](const std::string& f) -> std::optional<formula::ParseResult> {
      if (f.empty()) return std::nullopt;
      std::string_view t = f;
      if (t[0] == '=') t.remove_prefix(1);
      formula::ParseResult pr = formula::Parse(t, base, &host_);
      if (!pr.ok) return std::nullopt;
      return pr;
    };
    p.f1 = parse(r->formula1);
    p.f2 = parse(r->formula2);
    rules_.push_back(std::move(p));
  }
}

std::string CondFormatEval::KeyOf(const Value& v) const {
  auto number = [](double x) {
    char b[40];
    std::snprintf(b, sizeof(b), "n%.15g", x);
    return std::string(b);
  };
  if (v.IsString()) {
    // text that reads as a number is that number (Excel: '1, '01, ' 1 and 1 are duplicates; '46000 and a date)
    const Value n = formula::ParseInput(v.str()).value;
    if (n.IsNumeric()) return number(n.AsDouble());
    return "s" + Lower(std::string(v.str()));
  }
  if (v.IsNumeric()) return number(v.AsDouble());
  if (v.IsBool()) return v.boolean() ? "bT" : "bF";
  if (v.IsError()) return "e" + std::to_string(static_cast<int>(v.error()));
  return {};
}

void CondFormatEval::KeepStatsFrom(const CondFormatEval& prev) {
  if (!sh_ || !prev.sh_ || prev.sheet_ != sheet_ || &prev.sh_->cond_formats.get() != &sh_->cond_formats.get() || prev.rules_.size() != rules_.size())
    return;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  for (size_t i = 0; i < rules_.size(); ++i) {
    const Stats& old = *prev.rules_[i].stats;
    if (old.ready && old.cells >= 100000 && now - old.built_at < 0.5) rules_[i].stats = prev.rules_[i].stats;
  }
}

void CondFormatEval::BuildStats(Prepared& p) {
  Stats& st = *p.stats;
  st.ready = true;
  st.built_at = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const model::CfRule& rule = *p.rule;
  const bool need_counts = rule.type == CfType::Unique;
  for (const Rect& rr : rule.ranges) {
    Rect r = rr;
    if (!sh_->has_used) break;
    r.r1 = std::min(r.r1, sh_->used.r1);
    r.c1 = std::min(r.c1, sh_->used.c1);
    if (r.r0 > r.r1 || r.c0 > r.c1) continue;
    ForEachValueRowMajor(*sh_, r, [&](uint32_t, uint32_t, const Value& v) {
      ++st.cells;
      if (v.IsNumeric()) st.nums.push_back(v.AsDouble());
      else if (v.IsError()) ++st.errors;
      if (need_counts && !v.IsEmpty()) ++st.counts[KeyOf(v)];
      return true;
    });
  }
  std::sort(st.nums.begin(), st.nums.end());
  if (!st.nums.empty()) {
    double sum = 0;
    for (double x : st.nums) sum += x;
    st.avg = sum / static_cast<double>(st.nums.size());
    st.lo = st.nums.front();
    st.hi = st.nums.back();
    const size_t n = st.nums.size();
    st.mid = n % 2 ? st.nums[n / 2] : (st.nums[n / 2 - 1] + st.nums[n / 2]) / 2;
    if (rule.type == CfType::Top10) {
      size_t k = rule.percent ? static_cast<size_t>(std::floor(n * std::clamp(rule.rank, 0, 100) / 100.0)) : static_cast<size_t>(std::max(0, rule.rank));
      k = std::clamp<size_t>(k, 1, n);
      st.threshold = rule.bottom ? st.nums[k - 1] : st.nums[n - k];
    }
  }
}

bool CondFormatEval::Match(Prepared& p, uint32_t r, uint32_t c, const Value& v, CfVisual* out) {
  const model::CfRule& rule = *p.rule;
  auto stats = [&]() -> Stats& {
    if (!p.stats->ready) BuildStats(p);
    return *p.stats;
  };
  switch (rule.type) {
    case CfType::CellValue: {
      if (!p.f1) return false;
      const Value a = host_.Evaluate(sheet_, p.f1->root, r, c);
      const Value b = p.f2 ? host_.Evaluate(sheet_, p.f2->root, r, c) : Value();
      // an operand that is an error (=1/0) matches no cell, not even a blank one (Excel)
      const bool two = rule.op == CmpOp::Between || rule.op == CmpOp::NotBetween;
      if (a.IsError() || (two && b.IsError())) return false;
      // blank cells compare as 0 with numbers (Excel behavior) and as "" with text
      if (!CompareOp(rule.op, v.IsEmpty() ? (a.IsString() ? Value::String("") : Value::Number(0)) : v, a, b)) return false;
      Apply(rule.fmt, out);
      return true;
    }
    case CfType::Expression: {
      if (!p.f1) return false;
      const Value a = host_.Evaluate(sheet_, p.f1->root, r, c);
      bool t = false;
      core::Err e;
      if (!formula::Evaluator::ToBool(a, &t, &e) || !t) return false;
      Apply(rule.fmt, out);
      return true;
    }
    case CfType::TextContains: {
      // the value as text (as SEARCH, LEFT and RIGHT read it: 1234 in #,##0 is "1234", a date its serial), not as
      // shown; an error cell matches only Does Not Contain, which is ISERROR(SEARCH(...)) (Excel 16, measured
      // 2026-11-28: #DIV/0! is highlighted by "does not contain apple")
      if (v.IsError()) {
        if (rule.text_op != 1) return false;
        Apply(rule.fmt, out);
        return true;
      }
      const std::string text = v.IsEmpty() ? std::string() : Lower(formula::Evaluator::ToText(v));
      const std::string want = Lower(rule.text);
      // contains / does not contain search as Excel's SEARCH does: * ? and ~ are wildcards ("a*c" finds "abc")
      const bool wild = want.find_first_of("*?~") != std::string::npos;
      auto contains = [&] {
        size_t at = 0, len = 0;
        return wild ? WildFind(want, text, 0, false, &at, &len) : text.find(want) != std::string::npos;
      };
      bool hit;
      switch (rule.text_op) {
        case 1: hit = !contains(); break;  // does not contain (blank cells too, as Excel)
        case 2: hit = !v.IsEmpty() && text.compare(0, want.size(), want) == 0; break;
        case 3: hit = !v.IsEmpty() && text.size() >= want.size() && text.compare(text.size() - want.size(), want.size(), want) == 0; break;
        default: hit = !v.IsEmpty() && contains(); break;
      }
      if (!hit) return false;
      Apply(rule.fmt, out);
      return true;
    }
    case CfType::TimePeriod: {
      if (!v.IsNumeric()) return false;
      const double today = std::floor(formula::NowSerial());
      const double d = std::floor(v.AsDouble());
      const double week = today - (formula::Weekday(today) - 1);  // Sunday of this week, as Excel's weeks
      int ty = 0, tm = 0, td = 0, y = 0, m = 0, dd = 0;
      if (!formula::SerialToYmd(today, &ty, &tm, &td) || !formula::SerialToYmd(d, &y, &m, &dd)) return false;
      const int months = (y * 12 + m) - (ty * 12 + tm);
      bool hit = false;
      switch (rule.time_period) {
        case 0: hit = d == today; break;
        case 1: hit = d == today - 1; break;
        case 2: hit = d >= today - 6 && d <= today; break;
        case 3: hit = d >= week && d <= week + 6; break;
        case 4: hit = d >= week - 7 && d <= week - 1; break;
        case 5: hit = months == -1; break;
        case 6: hit = d == today + 1; break;
        case 7: hit = d >= week + 7 && d <= week + 13; break;
        case 8: hit = months == 1; break;
        case 9: hit = months == 0; break;
        default: break;
      }
      if (!hit) return false;
      Apply(rule.fmt, out);
      return true;
    }
    case CfType::Blanks:
    case CfType::NoBlanks: {
      // Excel's rule is LEN(TRIM(A1))=0: text of spaces only is blank too
      const bool blank = v.IsEmpty() || (v.IsString() && v.str().find_first_not_of(' ') == std::string_view::npos);
      if (blank != (rule.type == CfType::Blanks)) return false;
      Apply(rule.fmt, out);
      return true;
    }
    case CfType::Errors:
    case CfType::NoErrors:
      if (v.IsError() != (rule.type == CfType::Errors)) return false;
      Apply(rule.fmt, out);
      return true;
    case CfType::Top10: {
      if (!v.IsNumeric()) return false;
      const Stats& st = stats();
      if (st.nums.empty()) return false;
      const double x = v.AsDouble();
      if (rule.bottom ? x > st.threshold : x < st.threshold) return false;
      Apply(rule.fmt, out);
      return true;
    }
    case CfType::AboveAverage: {
      if (!v.IsNumeric()) return false;
      const Stats& st = stats();
      if (st.nums.empty()) return false;
      const double x = v.AsDouble();
      double at = st.avg;
      // a standard deviation over a range holding an error is an error: the rule highlights nothing, where the average
      // passes errors over (Excel 16, measured 2026-11-28)
      if (rule.std_dev > 0 && st.errors) return false;
      if (rule.std_dev > 0 && st.nums.size() > 1) {
        // Excel: the population standard deviation of the range (STDEV.P, divided by n)
        double ss = 0;
        for (double y : st.nums) ss += (y - st.avg) * (y - st.avg);
        const double sd = std::sqrt(ss / static_cast<double>(st.nums.size()));
        at = st.avg + (rule.below ? -1 : 1) * sd * rule.std_dev;
      }
      const bool eq = rule.equal_average && rule.std_dev == 0;
      if (rule.below ? !(eq ? x <= at : x < at) : !(eq ? x >= at : x > at)) return false;
      Apply(rule.fmt, out);
      return true;
    }
    case CfType::Unique: {
      if (v.IsEmpty()) return false;
      const Stats& st = stats();
      auto it = st.counts.find(KeyOf(v));
      const int n = it == st.counts.end() ? 0 : it->second;
      if (rule.duplicate ? n < 2 : n != 1) return false;
      Apply(rule.fmt, out);
      return true;
    }
    case CfType::ColorScale: {
      if (!v.IsNumeric() || out->has_fill) return false;
      const Stats& st = stats();
      if (st.nums.empty()) return false;
      const double x = v.AsDouble();
      // the value of each point by its type (Excel's ColorScaleCriteria): lowest, highest, number, percent,
      // percentile or formula; by default lowest - 50th percentile - highest
      auto point = [&](int i) {
        const int def = i == 0 ? 1 : i == 2 ? 2 : 5;
        const int type = rule.point_type[i] < 0 ? def : rule.point_type[i];
        const double val = rule.point_type[i] < 0 ? 50.0 : rule.point_value[i];
        switch (type) {
          case 1: return st.lo;
          case 2: return st.hi;
          case 0: return val;
          case 3: return st.lo + (st.hi - st.lo) * val / 100.0;
          case 4: {
            if (rule.point_formula[i].empty() || rule.ranges.empty()) return i == 0 ? st.lo : i == 2 ? st.hi : st.mid;
            std::string_view t = rule.point_formula[i];
            if (t[0] == '=') t.remove_prefix(1);
            formula::ParseResult pr = formula::Parse(t, core::CellPos{rule.ranges.front().r0, rule.ranges.front().c0}, &host_);
            if (!pr.ok) return i == 0 ? st.lo : i == 2 ? st.hi : st.mid;
            const Value fv = host_.Evaluate(sheet_, pr.root, r, c);
            return fv.IsNumeric() ? fv.AsDouble() : (i == 0 ? st.lo : i == 2 ? st.hi : st.mid);
          }
          default: {  // percentile (PERCENTILE.INC)
            const double pos = std::clamp(val, 0.0, 100.0) / 100.0 * static_cast<double>(st.nums.size() - 1);
            const size_t a = static_cast<size_t>(std::floor(pos));
            const size_t b = std::min(a + 1, st.nums.size() - 1);
            return st.nums[a] + (st.nums[b] - st.nums[a]) * (pos - static_cast<double>(a));
          }
        }
      };
      double lo = point(0), hi = point(2);
      if (lo > hi) std::swap(lo, hi);  // a minimum above the maximum (Number 8 to 2): the same gradient over 2..8 (Excel)
      // the ends first, as Excel: at or above the maximum the maximum's color (all values equal, one value, a midpoint
      // equal to the maximum), at or below the minimum the minimum's color (a midpoint equal to the minimum)
      uint32_t col;
      if (x >= hi) {
        col = rule.colors[2];
      } else if (x <= lo) {
        col = rule.colors[0];
      } else if (rule.points >= 3) {
        const double mid = point(1);
        if (x <= mid) col = Lerp(rule.colors[0], rule.colors[1], mid > lo ? std::clamp((x - lo) / (mid - lo), 0.0, 1.0) : 1.0);
        else col = Lerp(rule.colors[1], rule.colors[2], hi > mid ? std::clamp((x - mid) / (hi - mid), 0.0, 1.0) : 0.0);
      } else {
        col = Lerp(rule.colors[0], rule.colors[2], std::clamp((x - lo) / (hi - lo), 0.0, 1.0));
      }
      out->has_fill = true;
      out->fill = col;
      return true;
    }
    case CfType::IconSet: {
      if (!v.IsNumeric() || out->icon >= 0) return false;
      const Stats& st = stats();
      if (st.nums.empty()) return false;
      const int n = model::IconSetCount(rule.icon_style);
      const auto th = rule.icon_thresholds.size() + 1 == static_cast<size_t>(n) ? rule.icon_thresholds : model::DefaultIconThresholds(rule.icon_style);
      const double x = v.AsDouble();
      int idx = 0;
      for (const auto& t : th) {
        double at = t.value;
        if (t.type == 0) {
          at = st.lo + (st.hi - st.lo) * t.value / 100.0;
        } else if (t.type == 2) {
          // PERCENTILE.INC of the range
          const double pos = std::clamp(t.value, 0.0, 100.0) / 100.0 * static_cast<double>(st.nums.size() - 1);
          const size_t lo = static_cast<size_t>(std::floor(pos));
          const size_t hi = std::min(lo + 1, st.nums.size() - 1);
          at = st.nums[lo] + (st.nums[hi] - st.nums[lo]) * (pos - static_cast<double>(lo));
        } else if (t.type == 3 && !t.formula.empty()) {
          const core::CellPos base = rule.ranges.empty() ? core::CellPos{r, c} : core::CellPos{rule.ranges.front().r0, rule.ranges.front().c0};
          formula::ParseResult pr = formula::Parse(t.formula, base, &host_);
          if (pr.ok) {
            const Value fv = host_.Evaluate(sheet_, pr.root, r, c);
            if (fv.IsNumeric()) at = fv.AsDouble();
          }
        }
        if (t.gte ? x >= at : x > at) ++idx;
      }
      if (rule.icon_reverse) idx = n - 1 - idx;
      out->icon_style = static_cast<int8_t>(rule.icon_style);
      out->icon = static_cast<int8_t>(idx);
      out->icon_only = rule.icon_only;
      return true;
    }
    case CfType::DataBar: {
      if (!v.IsNumeric() || out->bar >= 0) return false;
      const Stats& st = stats();
      if (st.nums.empty()) return false;
      // like Excel 2010+: the bars start at an axis at 0; with negative values it sits inside the cell
      // and negative values go left from it in red. MinPoint / MaxPoint set the shortest and longest bar.
      auto bound = [&](int type, double val, const std::string& fx, bool is_max) {
        switch (type) {
          case 0: return val;
          case 3: return st.lo + (st.hi - st.lo) * val / 100.0;
          case 5: {
            const double pos = std::clamp(val, 0.0, 100.0) / 100.0 * static_cast<double>(st.nums.size() - 1);
            const size_t a = static_cast<size_t>(std::floor(pos));
            const size_t b = std::min(a + 1, st.nums.size() - 1);
            return st.nums[a] + (st.nums[b] - st.nums[a]) * (pos - static_cast<double>(a));
          }
          case 4: {
            if (fx.empty() || rule.ranges.empty()) break;
            std::string_view t = fx;
            if (t[0] == '=') t.remove_prefix(1);
            formula::ParseResult pr = formula::Parse(t, core::CellPos{rule.ranges.front().r0, rule.ranges.front().c0}, &host_);
            if (!pr.ok) break;
            const Value fv = host_.Evaluate(sheet_, pr.root, r, c);
            if (fv.IsNumeric()) return fv.AsDouble();
            break;
          }
          case 1: return st.lo;
          case 2: return st.hi;
          default: break;
        }
        return is_max ? std::max(0.0, st.hi) : std::min(0.0, st.lo);
      };
      const double lo = rule.bar_min_type < 0 ? std::min(0.0, st.lo) : bound(rule.bar_min_type, rule.bar_min_value, rule.bar_min_formula, false);
      double hi = rule.bar_max_type < 0 ? std::max(0.0, st.hi) : bound(rule.bar_max_type, rule.bar_max_value, rule.bar_max_formula, true);
      if (hi - lo < 1e-12) hi = lo + 1e-12;
      const double x = v.AsDouble();
      if (lo >= 0) {
        // a minimum above zero: the bar's length runs from it (the shortest bar at the minimum)
        out->bar_axis = 0.0f;
        out->bar_negative = false;
        out->bar = static_cast<float>(std::clamp((x - lo) / (hi - lo), 0.0, 1.0));
      } else {
        out->bar_axis = static_cast<float>(std::clamp(-lo / (hi - lo), 0.0, 1.0));
        out->bar_negative = x < 0;
        out->bar = static_cast<float>(std::clamp(std::fabs(x) / (hi - lo), 0.0, 1.0));
      }
      out->bar_color = rule.bar_color;
      out->bar_gradient = rule.bar_gradient;
      out->bar_border = rule.bar_border;
      out->bar_rtl = rule.bar_direction == 2;
      out->bar_only = !rule.bar_show_value;
      return true;
    }
  }
  return false;
}


int SnapshotEvalHost::TableRef(int32_t sheet, const formula::Node& n, core::CellPos origin, formula::RangeRef* out) {
  uint32_t ts = static_cast<uint32_t>(sheet);
  core::Rect r;
  const int e = model::ResolveTableRef(wb_, static_cast<uint32_t>(sheet), n.str, static_cast<uint8_t>(n.op), n.kids.size() > 0 ? n.kids[0].str : std::string(),
                                       n.kids.size() > 1 ? n.kids[1].str : std::string(), origin.row, origin.col, &ts, &r);
  if (e) return e;
  out->sheet = static_cast<int32_t>(ts);
  out->rect = r;
  return 0;
}

bool CondFormatEval::Eval(uint32_t r, uint32_t c, CfVisual* out) {
  if (!sh_) return false;
  bool any = false;
  const Value& v = sh_->GetValue(r, c);
  for (Prepared& p : rules_) {
    if (!p.bounds.Contains(r, c) || !InRanges(p.rule->ranges, r, c)) continue;
    if (Match(p, r, c, v, out)) {
      any = true;
      if (p.rule->stop_if_true) break;
    }
  }
  return any && out->Any();
}

// ---- Data validation ----

const model::DvRule* FindValidation(const model::Sheet& sh, uint32_t r, uint32_t c) {
  for (const auto& d : *sh.validations) {
    if (InRanges(d.ranges, r, c)) return &d;
  }
  return nullptr;
}

std::vector<std::string> ValidationList(const model::Workbook& wb, uint32_t sheet, const model::DvRule& rule, uint32_t r, uint32_t c,
                                        std::vector<Value>* values) {
  std::vector<std::string> out;
  std::vector<Value> vals;
  std::string_view f = rule.formula1;
  while (!f.empty() && f.front() == ' ') f.remove_prefix(1);
  if (!f.empty() && f[0] == '=') {
    SnapshotEvalHost host(wb);
    const core::CellPos base{rule.ranges.front().r0, rule.ranges.front().c0};
    formula::ParseResult pr = formula::Parse(f.substr(1), base, &host);
    if (!pr.ok) return out;
    formula::Evaluator ev(host, static_cast<int32_t>(sheet), core::CellPos{r, c});
    try {
      const formula::EV e = ev.Eval(pr.root);
      const auto& tbl = *wb.styles;
      if (e.k == formula::EV::K::Range) {
        const model::Sheet* s = wb.SheetById(static_cast<uint32_t>(e.r.sheet < 0 ? static_cast<int32_t>(sheet) : e.r.sheet));
        if (!s) return out;
        Rect rr = e.r.rect;
        if (s->has_used) rr.r1 = std::min(rr.r1, std::max(rr.r0, s->used.r1));
        for (uint32_t y = rr.r0; y <= rr.r1 && out.size() < 10000; ++y) {
          for (uint32_t x = rr.c0; x <= rr.c1; ++x) {
            out.push_back(DisplayTextOf(s->GetValue(y, x), tbl.NumberFormat(tbl.Get(s->GetStyle(y, x)))));
            vals.push_back(s->GetValue(y, x));
          }
        }
      } else {
        auto arr = ev.ToArray(e);
        if (arr) {
          for (const Value& v : arr->v) {
            out.push_back(formula::Evaluator::ToText(v));
            vals.push_back(v);
          }
        }
      }
    } catch (...) {
    }
    // drop trailing blank items from the list
    while (!out.empty() && out.back().empty()) {
      out.pop_back();
      if (!vals.empty()) vals.pop_back();
    }
    if (values) *values = std::move(vals);
    return out;
  }
  // comma-separated list of constants
  std::string cur;
  for (char ch : f) {
    if (ch == ',') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty() || !out.empty()) out.push_back(cur);
  for (auto& s : out) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
  }
  if (values) {
    // an item with spaces around it is that text, not a number: 5 isn't in "1, 5 ,a b" (Excel 16, measured)
    values->clear();
    std::string_view rest = f;
    for (const auto& s : out) {
      const size_t comma = rest.find(',');
      const std::string_view raw = rest.substr(0, comma);
      rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
      values->push_back(raw.size() != s.size() ? Value::String(s) : formula::ParseInput(s).value);
    }
  }
  return out;
}

namespace {
// the length LEN gives: UTF-16 code units (a CJK character counts 1, a character outside the BMP 2)
size_t Utf16Units(const std::string& s) {
  size_t n = 0;
  for (unsigned char ch : s) {
    if ((ch & 0xC0) != 0x80) n += ch >= 0xF0 ? 2 : 1;
  }
  return n;
}
}  // namespace

bool ValidateInput(const model::Workbook& wb, uint32_t sheet, uint32_t r, uint32_t c, const std::string& text, const model::DvRule& rule, std::string* message) {
  auto fail = [&](const std::string& why) {
    if (message) *message = rule.error_message.empty() ? why : rule.error_message;
    return false;
  };
  if (rule.type == model::DvType::Any) return true;
  // a blank with Ignore blank off is checked as 0 (no text): decimal < 5 passes, > 5 fails, a length of 0, a list fails
  // (Excel 16, measured)
  if (text.empty() && rule.ignore_blank) return true;
  SnapshotEvalHost host(wb);
  const core::CellPos base{rule.ranges.front().r0, rule.ranges.front().c0};
  // the entered value: evaluate it first if it is a formula
  Value v;
  if (text.empty()) {
    v = Value();
  } else if (text[0] == '=' && text.size() > 1) {
    formula::ParseResult pr = formula::Parse(std::string_view(text).substr(1), core::CellPos{r, c}, &host);
    v = pr.ok ? host.Evaluate(sheet, pr.root, r, c) : Value::Error(core::Err::Name);
  } else {
    v = formula::ParseInput(text).value;
  }
  // formulas see the cell holding the entered value (=ISNUMBER(A1), =COUNTIF($A:$A,A1)=1)
  host.SetOverride(sheet, r, c, v);
  auto operand = [&](const std::string& f) -> Value {
    if (f.empty()) return Value();
    // a bound without = is a constant read as typed: 2024/1/1 is a date, 9:00 a time (not 2024 / 1 / 1 or a row
    // range: Excel 16, measured); with = it is a formula
    if (f[0] != '=') return formula::ParseInput(f).value;
    std::string_view t = f;
    t.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(t, base, &host);
    if (pr.ok) return host.Evaluate(sheet, pr.root, r, c);
    return formula::ParseInput(f).value;
  };
  switch (rule.type) {
    case model::DvType::List: {
      std::vector<Value> item_values;
      const auto items = ValidationList(wb, sheet, rule, r, c, &item_values);
      // the value is checked, not the text that produced it (="b" is b, '1 is the text 1)
      if (v.IsError()) return fail(TR("The value isn't in the list."));
      std::string_view f1 = rule.formula1;
      while (!f1.empty() && f1.front() == ' ') f1.remove_prefix(1);
      const bool from_cells = !f1.empty() && f1.front() == '=';
      if (!v.IsNumeric() && !from_cells) {
        // a typed list matches with case, its items as written: spaces around the entry don't count, spaces around an
        // item do (Excel 16, measured: "aapl" isn't in "AAPL,MSFT" and " AAPL" is; nothing is in " AAPL , MSFT ")
        const std::string entry = formula::Evaluator::ToText(v);
        std::string_view t = entry;
        while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
        while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
        std::string_view rest = rule.formula1;
        for (;;) {
          const size_t comma = rest.find(',');
          if (rest.substr(0, comma) == t) return true;
          if (comma == std::string_view::npos) break;
          rest.remove_prefix(comma + 1);
        }
        return fail(TR("The value isn't in the list."));
      }
      if (!v.IsNumeric()) {
        // a list of cells matches without case, each cell's text as it shows (Excel 16, measured: "aapl" is in a
        // list of cells holding AAPL, "MSFT " isn't in one holding MSFT)
        const std::string t = Lower(formula::Evaluator::ToText(v));
        for (size_t i = 0; i < items.size(); ++i) {
          // a list of cells: text matches text (the text 3 isn't the number 3) and TRUE matches TRUE; a typed list
          // ("1,2,3", "TRUE,FALSE") holds texts: '1 and the text TRUE are in it (Excel)
          if (from_cells && i < item_values.size() && item_values[i].IsNumeric()) continue;
          if (from_cells && i < item_values.size() && v.IsBool() != item_values[i].IsBool()) continue;
          if (Lower(items[i]) == t) return true;
        }
        return fail(TR("The value isn't in the list."));
      }
      // a number or date matches an item of the same value, whatever either looks like (Excel: 1 is in a list
      // showing 1.00, 2024-01-05 in one showing 2024/1/5)
      for (const Value& iv : item_values) {
        if (iv.IsNumeric() && iv.AsDouble() == v.AsDouble()) return true;
      }
      return fail(TR("The value isn't in the list."));
    }
    case model::DvType::Custom: {
      if (rule.ignore_blank && !rule.formula1.empty()) {
        // Ignore blank: a formula that refers to an empty cell lets the value in (Excel: =A5<A6 with A6 empty)
        std::string_view t = rule.formula1;
        if (t[0] == '=') t.remove_prefix(1);
        formula::ParseResult pr = formula::Parse(t, base, &host);
        if (pr.ok) {
          std::vector<formula::ResolvedRef> refs;
          formula::CollectRefs(pr.root, core::CellPos{r, c}, &refs);
          for (const auto& ref : refs) {
            if (ref.rect.r0 != ref.rect.r1 || ref.rect.c0 != ref.rect.c1) continue;  // single cells only, not ranges
            const int32_t rs = ref.sheet < 0 ? static_cast<int32_t>(sheet) : ref.sheet;
            if (host.Cell(rs, ref.rect.r0, ref.rect.c0).IsEmpty()) return true;
          }
        }
      }
      const Value a = operand(rule.formula1);
      bool t = false;
      core::Err e;
      if (formula::Evaluator::ToBool(a, &t, &e) && t) return true;
      return fail(TR("The value doesn't satisfy this cell's validation formula."));
    }
    case model::DvType::TextLength: {
      if (v.IsError()) return fail(TR("The text length doesn't meet the restriction."));  // #N/A isn't 4 characters long
      const Value len = Value::Number(static_cast<double>(Utf16Units(formula::Evaluator::ToText(v))));
      if (CompareOp(rule.op, len, operand(rule.formula1), operand(rule.formula2))) return true;
      return fail(TR("The text length doesn't meet the restriction."));
    }
    default: {
      if (v.IsEmpty()) v = Value::Number(0);
      if (v.IsBool()) {
        // TRUE and FALSE count as above every number, as Excel compares them: not between, not equal to, greater than
        // (or equal to) any bounds pass, the others fail
        const CmpOp op = rule.op;
        if (op == CmpOp::NotBetween || op == CmpOp::NotEqual || op == CmpOp::Greater || op == CmpOp::GreaterEqual) return true;
        return fail(TR("The value is outside the allowed range."));
      }
      if (!v.IsNumeric()) return fail(TR("You must enter a number."));
      if (rule.type == model::DvType::WholeNumber && std::floor(v.AsDouble()) != v.AsDouble()) return fail(TR("You must enter a whole number."));
      if (CompareOp(rule.op, v, operand(rule.formula1), operand(rule.formula2))) return true;
      return fail(TR("The value is outside the allowed range."));
    }
  }
}

}  // namespace yc::engine
